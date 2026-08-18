// viewer.cpp
//
// SPDX-License-Identifier: AGPL-3.0-or-later
// GNU Affero General Public License v3.0 (https://www.gnu.org/licenses/agpl-3.0.txt)
// Copyright (c) 2026 Manuel FLURY
// All rights reserved.
//
// This file is part of slaptrack - an OpenLDAP Log Viewer (ANSI edition).
//
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0-or-later).
// See the LICENSE file distributed with this work for full license text.
//
// ANSI implementation following the TUI-ANSI-METHODOLOGY:
//   - renderFrame() builds ONE big string; flushFrame() writes it with a
//     single cout << ... << flush wrapped in sync_start/sync_end
//   - every color goes through Theme::c(), every move through Mv::to()
//   - borders/lines are built inline per frame (cheap); the popup box is
//     cached via Draw::createBox
//   - SIGWINCH is caught by a handler that sets g_resized; the main loop
//     refreshes geometry and redraws

#include "viewer.h"
#include "input.hpp"
#include "widgets.hpp"
#include "utf8.hpp"
#include "banner.hpp"

#include <iostream>
#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <signal.h>
#include <chrono>
#include <poll.h>
#include <set>

static Viewer* g_viewer = nullptr;
static volatile sig_atomic_t g_interrupted = 0;
static volatile sig_atomic_t g_resized = 0;

static void signalHandler(int /*sig*/) {
    g_interrupted = 1;
    if (g_viewer) {
        g_viewer->stop();
    }
}

static void winchHandler(int /*sig*/) {
    g_resized = 1;
}

// Same match semantics as the CONN filter and the parser's
// conn=(\d+) capture: the conn id must appear as the final run of
// digits after a "conn=" marker, and it must equal `connId`.  This
// lets a range scan collect matching lines in one cheap pass without
// running the full (~20 regex) tokenizer.
static bool rawHasExactConn(const std::string& raw, const std::string& connId) {
    const std::string marker = "conn=";
    size_t pos = 0;
    size_t dStart = std::string::npos;
    size_t dLen = 0;
    bool any = false;
    while ((pos = raw.find(marker, pos)) != std::string::npos) {
        pos += marker.size();
        size_t s = pos;
        while (pos < raw.size() && raw[pos] >= '0' && raw[pos] <= '9') {
            pos++;
        }
        if (pos > s) {
            any = true;
            dStart = s;
            dLen = pos - s;
        }
    }
    return any && dLen == connId.size()
        && raw.compare(dStart, dLen, connId) == 0;
}

Viewer::Viewer(const std::string& filename, bool followMode, LogFormat logFormat)
    : filename_(filename), followMode_(followMode), logFormat_(logFormat),
      autoScroll_(followMode) {

    g_viewer = this;

    buffer_.getParser().setLogFormat(logFormat_);
    if (!buffer_.loadFile(filename)) {
        std::cerr << "Error: Failed to load " << filename << std::endl;
        return;
    }

    size_t totalLines = buffer_.getTotalLines();
    visibleIndices_.reserve(totalLines);
    for (size_t i = 0; i < totalLines; i++) {
        visibleIndices_.push_back(i);
    }

    buffer_.prefetchAround(0, 100);

    if (followMode_) {
        followFd_ = inotify_init1(IN_NONBLOCK);
        if (followFd_ >= 0) {
            followWatchFd_ = inotify_add_watch(followFd_, filename.c_str(),
                                               IN_MODIFY | IN_MOVE_SELF |
                                               IN_DELETE_SELF);
        }
        if (!visibleIndices_.empty()) {
            cursorRow_ = (int)visibleIndices_.size() - 1;
            int contentHeight = termHeight_ - 2;
            scrollOffset_ = std::max(0, cursorRow_ - contentHeight + 1);
        }
        lastFollowPoll_ = std::chrono::steady_clock::now();
    }
}

Viewer::~Viewer() {
    if (followFd_ >= 0) {
        if (followWatchFd_ >= 0) {
            inotify_rm_watch(followFd_, followWatchFd_);
        }
        close(followFd_);
    }
    g_viewer = nullptr;
}

const char* Viewer::SCHEMA_NAMES[8] = {
    "Default",
    "Monochrome",
    "Solarized Light",
    "Solarized Dark",
    "Monokai",
    "Nord",
    "Gruvbox",
    "Dracula",
};

void Viewer::initTerminal() {
    Term::init();
    Theme::detectColorSupport();
    Theme::init();
    Theme::setSchema(currentSchema_);

    if (!Theme::ttyMode()) {
        autoColor_ = false;
    } else {
        currentSchema_ = 1;
        autoColor_ = true;
    }

    Term::refresh();
    termHeight_ = Term::height;
    termWidth_ = Term::width;

    if (termHeight_ < 3 || termWidth_ < 10) {
        Term::restore();
        std::cerr << "Error: terminal too small for slaptrack "
                  << "(need at least 3 rows x 10 cols, have "
                  << termHeight_ << "x" << termWidth_ << ")\n";
        running_.store(false);
        return;
    }

    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGQUIT, &sa, nullptr);

    struct sigaction wa;
    wa.sa_handler = winchHandler;
    sigemptyset(&wa.sa_mask);
    wa.sa_flags = 0;
    sigaction(SIGWINCH, &wa, nullptr);

    mouseActive_ = true;
    Term::enterFullscreen();
}

void Viewer::stop() {
    running_.store(false);
}

void Viewer::restoreTerminal() {
    Term::leaveFullscreen();
    Term::restore();
}

int Viewer::getColorForToken(TokenType type) {
    switch (type) {
        case TokenType::TIMESTAMP:   return Theme::TOK_TIMESTAMP;
        case TokenType::THREAD_ID:   return Theme::TOK_THREAD_OP_ID;
        case TokenType::CONN_ID:     return Theme::TOK_CONN_ID;
        case TokenType::OP_ID:       return Theme::TOK_THREAD_OP_ID;
        case TokenType::DN_VALUE:    return Theme::TOK_DN_VALUE;
        case TokenType::FILTER_VALUE:return Theme::TOK_FILTER_VALUE;
        case TokenType::IP_ADDRESS:  return Theme::TOK_IP_KEYWORD;
        case TokenType::ERROR_CODE:  return Theme::TOK_ERROR_CODE;
        case TokenType::KEYWORD:     return Theme::TOK_IP_KEYWORD;
        case TokenType::FD_NUM:      return Theme::TOK_FD_NUM;
        case TokenType::TAG:         return Theme::TOK_TAG;
        case TokenType::ETIME_VAL:   return Theme::TOK_ETIME_VAL;
        case TokenType::NENTRIES:    return Theme::TOK_NENTRIES;
        case TokenType::QTIME_VAL:   return Theme::TOK_QTIME_VAL;
        case TokenType::SCOPE:       return Theme::TOK_SCOPE;
        case TokenType::DEREF:       return Theme::TOK_DEREF;
        case TokenType::ATTR:        return Theme::TOK_ATTR;
        case TokenType::ATTR_LIST:   return Theme::TOK_ATTR_LIST;
        case TokenType::BASE:        return Theme::TOK_BASE;
        default:                     return -1;
    }
}

static bool isBoldToken(TokenType t) {
    switch (t) {
        case TokenType::CONN_ID:
        case TokenType::OP_ID:
        case TokenType::THREAD_ID:
        case TokenType::DN_VALUE:
        case TokenType::ERROR_CODE:
        case TokenType::KEYWORD:
        case TokenType::NENTRIES:
        case TokenType::QTIME_VAL:
        case TokenType::SCOPE:
        case TokenType::DEREF:
        case TokenType::ATTR:
        case TokenType::BASE:
            return true;
        default:
            return false;
    }
}

void Viewer::appendPlain(std::string& out, const std::string& text) {
    if (text.empty()) return;
    out += Theme::mainFg();
    out += text;
}

void Viewer::appendToken(std::string& out, const Token& token,
                         bool isCurrentToken, bool isHovered) {
    int colorIdx = getColorForToken(token.type);
    std::string style;
    if (colorIdx >= 0) style += Theme::c(colorIdx);
    if (isBoldToken(token.type)) style += Fx::b;

    if (isCurrentToken) {
        style += Fx::rev;
        style += Fx::ul;
    } else if (isHovered) {
        style += Fx::rev;
    }

    out += style;
    out += token.value;
    out += Fx::reset;
}

void Viewer::run() {
    initTerminal();
    if (!running_.load()) return;
    fullRedraw();

    while (running_.load() && !g_interrupted) {
        if (followMode_) {
            handleFollowMode();
        }
        handleInput();
    }

    restoreTerminal();
    std::cout << embedded::BANNER_TEXT;
    std::cout.flush();
}

void Viewer::flushFrame(const std::string& out) {
    std::cout << Term::sync_start << out << Term::sync_end << std::flush;
}

void Viewer::fullRedraw() {
    std::string out;
    out.reserve((size_t)termWidth_ * (size_t)termHeight_);
    renderFrame(out);
    flushFrame(out);
}

void Viewer::renderFrame(std::string& out) {
    out += Fx::reset;

    // Content area.
    int contentHeight = termHeight_ - 2;
    int screenRow = 0;
    for (int i = scrollOffset_;
         i < (int)visibleIndices_.size() && screenRow < contentHeight;
         i++, screenRow++) {
        auto line = buffer_.getLine(visibleIndices_[i]);
        if (!line) {
            out += Mv::to(screenRow + 1, 1) + Term::el + Fx::reset;
            continue;
        }
        bool isHighlighted = (i == cursorRow_);
        renderLine(out, screenRow, *line, visibleIndices_[i], isHighlighted);
    }
    // Clear any rows below the last content row (scroll region shortened).
    for (; screenRow < contentHeight; screenRow++) {
        out += Mv::to(screenRow + 1, 1) + Term::el + Fx::reset;
    }

    renderFilterBar(out);
    renderStatusBar(out);

    if (showPopup_) {
        renderPopup(out);
    } else {
        moveCursorToScreen();
    }
}

void Viewer::renderLine(std::string& out, int screenRow, const LogLine& line,
                        size_t lineNum, bool isHighlighted) {
    out += Mv::to(screenRow + 1, 1);
    out += Term::el;

    int col = 0;

    if (showLineNumbers_) {
        size_t totalLines = buffer_.getTotalLines();
        int numWidth = (int)std::to_string(totalLines).length();
        std::string numStr = Tools::rjust(std::to_string(lineNum + 1),
                                          (size_t)numWidth);
        numStr += " ";
        out += Theme::c(Theme::TOK_LINENUM) + numStr + Fx::reset;
        col = numWidth + 1;
    }

    int visibleWidth = termWidth_ - col;
    int adjStart = horizontalOffset_;
    int adjEnd = horizontalOffset_ + visibleWidth;
    size_t lastEnd = (size_t)adjStart;

    for (size_t i = 0; i < line.tokens.size() && col < termWidth_; i++) {
        const auto& token = line.tokens[i];

        if ((int)token.end_pos <= adjStart) continue;
        if ((int)token.start_pos >= adjEnd) break;

        int tStart = std::max((int)token.start_pos, adjStart);
        int tEnd = std::min((int)token.end_pos, adjEnd);

        if ((size_t)tStart > lastEnd) {
            int plainStart = (int)lastEnd;
            int plainEnd = std::min(tStart, adjEnd);
            if (plainEnd > plainStart) {
                int len = plainEnd - plainStart;
                appendPlain(out, line.raw.substr(plainStart, len));
            }
        }

        int len = tEnd - tStart;
        if (len > 0) {
            bool isCurrent = isHighlighted
                && i == currentTokenIndex_;
            bool isHovered = (screenRow == hoverRow_
                && tStart <= hoverCol_ + horizontalOffset_
                && hoverCol_ + horizontalOffset_ < tEnd);
            Token shifted;
            shifted.type = token.type;
            shifted.value = line.raw.substr(tStart, len);
            shifted.start_pos = tStart;
            shifted.end_pos = tStart + len;
            appendToken(out, shifted, isCurrent, isHovered);
        }

        lastEnd = (size_t)tEnd;
    }

    if ((int)lastEnd < adjEnd && (int)line.raw.length() > (int)lastEnd) {
        int len = std::min(adjEnd, (int)line.raw.length()) - (int)lastEnd;
        if (len > 0) {
            appendPlain(out, line.raw.substr(lastEnd, len));
        }
    }

    out += Fx::reset;
}

void Viewer::renderFilterBar(std::string& out) {
    out += Mv::to(termHeight_ - 1, 1);
    out += Term::el;

    std::string filterText = " Filters: ";
    if (filterStack_.empty()) {
        filterText += "(none)";
    } else {
        for (size_t i = 0; i < filterStack_.size(); i++) {
            if (i > 0) filterText += " > ";
            filterText += filterStack_.getFilters()[i].toString();
        }
    }

    filterText += " | Lines: " + std::to_string(visibleIndices_.size())
                  + "/" + std::to_string(buffer_.getTotalLines());

    if (followMode_) {
        filterText += " | [FOLLOW]";
    }

    if (!searchQuery_.empty()) {
        filterText += " | Search: \"" + searchQuery_ + "\"";
        if (!searchResults_.empty()) {
            filterText += " (" + std::to_string(currentSearchResult_ + 1)
                          + "/" + std::to_string(searchResults_.size()) + ")";
        }
    }

    filterText = Tools::ljust(filterText, (size_t)termWidth_, false, false, true);
    out += Fx::rev + filterText + Fx::urev + Fx::reset;
}

void Viewer::renderStatusBar(std::string& out) {
    out += Mv::to(termHeight_, 1);
    out += Term::el;

    std::string status = " " + filename_;
    status += " | " + std::string(SCHEMA_NAMES[currentSchema_]);
    if (autoColor_) status += " [Auto]";

    if (cursorRow_ < (int)visibleIndices_.size()) {
        size_t lineNum = visibleIndices_[cursorRow_];
        status += " | L:" + std::to_string(lineNum + 1)
                  + "/" + std::to_string(buffer_.getTotalLines());

        auto line = buffer_.getLine(lineNum);
        if (line) {
            if (line->conn_id) status += " | conn=" + line->conn_id.value();
            if (line->dn) status += " | dn=" + line->dn.value();
        }
    }

    std::string help = "[F1-8] [Enter]Filter [Esc]Back [/]Search [#]Num [h/l]Scroll [q]Quit ";

    int padding = termWidth_ - (int)status.length() - (int)help.length();
    if (padding < 0) padding = 0;
    status += std::string((size_t)padding, ' ') + help;
    status = Tools::ljust(status, (size_t)termWidth_, false, false, true);

    out += Fx::rev + status + Fx::urev + Fx::reset;
}

void Viewer::moveCursorToScreen() {
    int screenRow = getScreenRowForCursorRow(cursorRow_);
    if (screenRow < 0 || screenRow >= termHeight_ - 2) return;
    // Place the real cursor at the current token position (best effort).
    std::cout << Mv::to(screenRow + 1, cursorCol_ + 1) << std::flush;
}

void Viewer::renderPopup(std::string& out) {
    int popupWidth = 50;
    int popupHeight = 5;
    int startY = (termHeight_ - popupHeight) / 2;
    int startX = (termWidth_ - popupWidth) / 2;

    if (startX < 1 || startY < 1) return;

    out += Draw::createBox(startX, startY, popupWidth, popupHeight,
                           Theme::divLine(), true, " slaptrack ");

    int msgLen = (int)Tools::wide_ulen(popupMessage_);
    int msgX = startX + (popupWidth - msgLen) / 2;
    if (msgX < startX + 1) msgX = startX + 1;

    out += Mv::to(startY + 2, msgX) + Theme::mainFg()
         + Tools::uresize(popupMessage_, (size_t)(popupWidth - 2), true)
         + Fx::reset;

    // Progress bar at row startY+3 (inside 5-row box: row 3 of 5).
    std::string bar = Draw::progressBar(popupWidth - 8, popupProgress_);
    int barX = startX + (popupWidth - (int)Tools::wide_ulen(bar)) / 2;
    if (barX < startX + 1) barX = startX + 1;
    out += Mv::to(startY + 3, barX) + bar + Fx::reset;
}

void Viewer::showPopup(const std::string& message, float progress) {
    popupMessage_ = message;
    popupProgress_ = progress;
    showPopup_ = true;
    fullRedraw();
}

void Viewer::hidePopup() {
    showPopup_ = false;
    fullRedraw();
}

void Viewer::hidePopupNoRedraw() {
    showPopup_ = false;
}

void Viewer::redrawLine(int cursorRow, bool isHighlighted) {
    int screenRow = getScreenRowForCursorRow(cursorRow);
    if (screenRow < 0 || screenRow >= termHeight_ - 2) return;
    if (cursorRow >= (int)visibleIndices_.size()) return;
    size_t lineIdx = visibleIndices_[cursorRow];
    auto line = buffer_.getLine(lineIdx);
    if (!line) return;

    std::string out;
    out.reserve((size_t)termWidth_ * 4);
    renderLine(out, screenRow, *line, lineIdx, isHighlighted);
    flushFrame(out);
    moveCursorToScreen();
}

void Viewer::handleInput() {
    Input::Event ev = Input::pollKey(10);

    if (ev.key == Input::Key::NONE) {
        // Idle tick.
        if (g_resized) {
            g_resized = 0;
            Term::refresh();
            termHeight_ = Term::height;
            termWidth_ = Term::width;
            fullRedraw();
        }
        return;
    }

    if (ev.key == Input::Key::MOUSE) {
        handleMouseEvent(ev);
        return;
    }

    if (g_resized) {
        g_resized = 0;
        Term::refresh();
        termHeight_ = Term::height;
        termWidth_ = Term::width;
        fullRedraw();
    }

    switch (ev.key) {
        case Input::Key::CHAR:
            switch (ev.ch) {
                case 'q': case 'Q': running_.store(false); return;
                case 'k': moveCursorUp(); break;
                case 'j': moveCursorDown(); break;
                case 'h':
                    if (cursorCol_ > 0) {
                        cursorCol_--;
                    } else if (horizontalOffset_ > 0) {
                        horizontalOffset_--;
                        redrawLine(cursorRow_, true);
                    } else if (!filterStack_.empty()) {
                        deactivateFilter();
                    }
                    if (cursorCol_ > 0 || (cursorCol_ == 0 && horizontalOffset_ == 0)) {
                        auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_ + horizontalOffset_);
                        if (tokenIdx.has_value()) currentTokenIndex_ = tokenIdx.value();
                        redrawLine(cursorRow_, true);
                    }
                    break;
                case 'l':
                    if (cursorCol_ < termWidth_ - 1) {
                        cursorCol_++;
                    } else {
                        horizontalOffset_++;
                        redrawLine(cursorRow_, true);
                    }
                    {
                        auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_ + horizontalOffset_);
                        if (tokenIdx.has_value()) currentTokenIndex_ = tokenIdx.value();
                        redrawLine(cursorRow_, true);
                    }
                    break;
                case '\n': case '\r': activateFilter(); break;
                case '/': startSearch(); break;
                case 'n': nextSearchResult(); break;
                case 'N': prevSearchResult(); break;
                case 'g': goToTop(); break;
                case 'G': goToBottom(); break;
                case '#':
                    showLineNumbers_ = !showLineNumbers_;
                    fullRedraw();
                    break;
                case ':': readCommandMode(':'); break;
                case '^': moveCursorToLineStart(); break;
                case '$': moveCursorToLineEnd(); break;
                default: break;
            }
            break;
        case Input::Key::UP: moveCursorUp(); break;
        case Input::Key::DOWN: moveCursorDown(); break;
        case Input::Key::LEFT:
            if (cursorCol_ > 0) {
                cursorCol_--;
            } else if (horizontalOffset_ > 0) {
                horizontalOffset_--;
                redrawLine(cursorRow_, true);
            } else if (!filterStack_.empty()) {
                deactivateFilter();
            }
            if (cursorCol_ > 0 || (cursorCol_ == 0 && horizontalOffset_ == 0)) {
                auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_ + horizontalOffset_);
                if (tokenIdx.has_value()) currentTokenIndex_ = tokenIdx.value();
                redrawLine(cursorRow_, true);
            }
            break;
        case Input::Key::RIGHT:
            if (cursorCol_ < termWidth_ - 1) {
                cursorCol_++;
            } else {
                horizontalOffset_++;
                redrawLine(cursorRow_, true);
            }
            {
                auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_ + horizontalOffset_);
                if (tokenIdx.has_value()) currentTokenIndex_ = tokenIdx.value();
                redrawLine(cursorRow_, true);
            }
            break;
        case Input::Key::PGUP: pageUp(); break;
        case Input::Key::PGDN: pageDown(); break;
        case Input::Key::HOME: goToTop(); break;
        case Input::Key::END: goToBottom(); break;
        case Input::Key::ENTER: activateFilter(); break;
        case Input::Key::BACKSPACE: deactivateFilter(); break;
        case Input::Key::ESC: deactivateFilter(); break;
        case Input::Key::F1: case Input::Key::F2: case Input::Key::F3:
        case Input::Key::F4: case Input::Key::F5: case Input::Key::F6:
        case Input::Key::F7: case Input::Key::F8: {
            int schema = (int)ev.key - (int)Input::Key::F1;
            Theme::setSchema(schema);
            currentSchema_ = schema;
            autoColor_ = false;
            fullRedraw();
            break;
        }
        case Input::Key::CTRL_C:
            running_.store(false);
            return;
        case Input::Key::CTRL_L:
            fullRedraw();
            break;
        default:
            break;
    }
}

void Viewer::handleMouseEvent(const Input::Event& ev) {
    int row = ev.mouseY - 1; // 0-based screen row
    int col = ev.mouseX - 1; // 0-based column

    if (ev.mouseButton == Input::MB_WHEEL_UP) {
        moveCursorUp(); moveCursorUp(); moveCursorUp();
        return;
    }
    if (ev.mouseButton == Input::MB_WHEEL_DOWN) {
        moveCursorDown(); moveCursorDown(); moveCursorDown();
        return;
    }
    if (ev.mouseButton == (Input::MB_MOTION | Input::MB_LEFT) ||
        ev.mouseButton == Input::MB_MOTION) {
        // Hover tracking.
        if (row != hoverRow_ || col != hoverCol_) {
            int oldHoverRow = hoverRow_;
            hoverRow_ = row;
            hoverCol_ = col;
            if (row < termHeight_ - 2 && oldHoverRow >= 0) {
                redrawLine(oldHoverRow, oldHoverRow == cursorRow_);
            }
        }
        return;
    }
    if (!ev.mousePressed) {
        return; // release
    }

    if (ev.mouseButton == Input::MB_LEFT) {
        if (row < termHeight_ - 2) {
            int clickedRow = getCursorRowForScreenRow(row);
            if (clickedRow < (int)visibleIndices_.size()) {
                int oldCursorRow = cursorRow_;
                cursorRow_ = clickedRow;
                cursorCol_ = col;

                auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_);
                if (tokenIdx.has_value()) currentTokenIndex_ = tokenIdx.value();

                if (oldCursorRow != cursorRow_) {
                    redrawLine(oldCursorRow, false);
                    redrawLine(cursorRow_, true);
                } else {
                    redrawLine(cursorRow_, true);
                }
                moveCursorToScreen();
            }
        }
        // Double-click detection.
        auto now = std::chrono::steady_clock::now();
        bool isDouble = (lastClickRow_ == cursorRow_)
            && (now - lastClickTime_ <= std::chrono::milliseconds(400));
        lastClickTime_ = now;
        lastClickRow_ = cursorRow_;
        lastClickCol_ = cursorCol_;
        if (isDouble && cursorRow_ < (int)visibleIndices_.size()) {
            activateFilterAtPosition(cursorRow_, cursorCol_);
        }
    }
}

void Viewer::handleFollowMode() {
    if (followFd_ < 0) return;

    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool inotifyEvent = false;
    bool fileVanished = false;

    while (true) {
        int len = read(followFd_, buf, sizeof(buf));
        if (len <= 0) break;
        inotifyEvent = true;
        int i = 0;
        while (i < len) {
            auto* ev = reinterpret_cast<inotify_event*>(buf + i);
            if (ev->mask & (IN_MOVE_SELF | IN_DELETE_SELF)) {
                fileVanished = true;
            }
            i += sizeof(inotify_event) + ev->len;
        }
    }

    auto now = std::chrono::steady_clock::now();
    auto sincePoll = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - lastFollowPoll_).count();
    if (!inotifyEvent && sincePoll < 500) {
        return;
    }
    lastFollowPoll_ = now;

    if (fileVanished) {
        if (followWatchFd_ >= 0) {
            inotify_rm_watch(followFd_, followWatchFd_);
            followWatchFd_ = -1;
        }
        buffer_.reopenFile(filename_);
        followWatchFd_ = inotify_add_watch(followFd_, filename_.c_str(),
                                           IN_MODIFY | IN_MOVE_SELF |
                                           IN_DELETE_SELF);
        visibleIndices_.clear();
        size_t totalLines = buffer_.getTotalLines();
        for (size_t i = 0; i < totalLines; i++) {
            if (linePassesFilters(i)) visibleIndices_.push_back(i);
        }
    }

    size_t newLines = buffer_.refreshFile();
    if (newLines > 0) {
        size_t totalLines = buffer_.getTotalLines();
        size_t startSearch = totalLines - newLines;
        for (size_t i = startSearch; i < totalLines; i++) {
            if (linePassesFilters(i)) {
                visibleIndices_.push_back(i);
            }
        }

        if (autoScroll_) {
            int contentHeight = termHeight_ - 2;
            cursorRow_ = (int)visibleIndices_.size() - 1;
            scrollOffset_ = std::max(0, cursorRow_ - contentHeight + 1);
        }
        fullRedraw();
    } else if (fileVanished) {
        fullRedraw();
    }
}

void Viewer::readCommandMode(char prompt) {
    // Minimal line editor on the status bar.
    std::string input;
    bool done = false;
    std::cout << Mv::to(termHeight_, 1) << Term::el << Fx::rev
              << prompt << Fx::urev << Fx::reset << std::flush;

    while (!done && running_.load()) {
        Input::Event ev = Input::pollKey(20);
        if (ev.key == Input::Key::NONE) continue;
        switch (ev.key) {
            case Input::Key::ENTER:
                done = true;
                break;
            case Input::Key::ESC:
                input.clear();
                done = true;
                break;
            case Input::Key::BACKSPACE:
                if (!input.empty()) input.pop_back();
                break;
            case Input::Key::CHAR:
                if (ev.ch >= 32 && ev.ch < 127) input += (char)ev.ch;
                break;
            default:
                break;
        }
        std::cout << Mv::to(termHeight_, 1) << Term::el << Fx::rev
                  << prompt << input << Fx::urev << Fx::reset << std::flush;
    }

    if (!input.empty()) {
        if (input == "$") {
            goToBottom();
        } else if (input == "^") {
            goToTop();
        } else {
            try {
                size_t lineNum = std::stoul(input);
                moveToLine(lineNum);
            } catch (...) {
            }
        }
    }

    fullRedraw();
}

void Viewer::moveToLine(size_t lineNum) {
    if (lineNum == 0 || lineNum > visibleIndices_.size()) {
        return;
    }
    cursorRow_ = (int)(lineNum - 1);
    int contentHeight = termHeight_ - 2;
    if (cursorRow_ < scrollOffset_) {
        scrollOffset_ = cursorRow_;
    } else if (cursorRow_ >= scrollOffset_ + contentHeight) {
        scrollOffset_ = cursorRow_ - contentHeight + 1;
    }
    if (cursorRow_ < (int)visibleIndices_.size()) {
        buffer_.prefetchAround(visibleIndices_[cursorRow_], 100);
    }
    fullRedraw();
}

void Viewer::moveCursorToLineStart() {
    cursorCol_ = 0;
    moveCursorToScreen();
}

void Viewer::moveCursorToLineEnd() {
    if (cursorRow_ >= (int)visibleIndices_.size()) return;
    auto line = buffer_.getLine(visibleIndices_[cursorRow_]);
    if (line) {
        cursorCol_ = std::min((int)line->raw.length(), termWidth_ - 1);
        moveCursorToScreen();
    }
}

std::optional<Token> Viewer::getTokenAtCursor() {
    if (cursorRow_ >= (int)visibleIndices_.size()) return std::nullopt;
    auto line = buffer_.getLine(visibleIndices_[cursorRow_]);
    if (!line) return std::nullopt;
    for (const auto& token : line->tokens) {
        if (cursorCol_ >= (int)token.start_pos && cursorCol_ < (int)token.end_pos) {
            return token;
        }
    }
    return std::nullopt;
}

std::optional<size_t> Viewer::getTokenIndexAtPosition(int row, int col) {
    int lineIdx = row;
    if (lineIdx >= (int)visibleIndices_.size()) return std::nullopt;
    auto line = buffer_.getLine(visibleIndices_[lineIdx]);
    if (!line) return std::nullopt;
    for (size_t i = 0; i < line->tokens.size(); i++) {
        const auto& token = line->tokens[i];
        if (col >= (int)token.start_pos && col < (int)token.end_pos) {
            return i;
        }
    }
    return std::nullopt;
}

void Viewer::activateFilter() {
    auto token = getTokenAtCursor();
    if (!token.has_value()) return;

    Filter filter;
    std::vector<size_t> connMatches;
    bool connFastPath = false;

    switch (token->type) {
        case TokenType::CONN_ID: {
            filter.type = FilterType::CONN;
            filter.key = "conn";
            filter.value = token->value.substr(5);

            size_t currentLine = visibleIndices_[cursorRow_];
            size_t connStart = 0, connEnd = 0;
            if (!findConnRange(currentLine, filter.value, connStart, connEnd, &connMatches)) {
                hidePopup();
                return;
            }

            filter.rangeStart = connStart;
            filter.rangeEnd = connEnd;
            filter.hasRange = true;
            connFastPath = filterStack_.empty() && !connMatches.empty();
            break;
        }
        case TokenType::DN_VALUE: {
            const size_t currentLine = visibleIndices_[cursorRow_];
            auto line = buffer_.getLine(currentLine);
            if (!line || !line->conn_id) return;
            filter.type = FilterType::CONN;
            filter.key = "conn";
            filter.value = *line->conn_id;
            size_t connStart = 0, connEnd = 0;
            if (!findConnRange(currentLine, filter.value, connStart, connEnd, &connMatches)) {
                hidePopup();
                return;
            }
            filter.rangeStart = connStart;
            filter.rangeEnd = connEnd;
            filter.hasRange = true;
            connFastPath = filterStack_.empty() && !connMatches.empty();
            break;
        }
        case TokenType::OP_ID:
            filter.type = FilterType::OP;
            filter.key = "op";
            filter.value = token->value.substr(3);
            addConnFilterForCurrentLine();
            break;
        case TokenType::THREAD_ID:
            filter.type = FilterType::THREAD;
            filter.key = "thread";
            filter.value = token->value;
            break;
        case TokenType::ERROR_CODE:
            filter.type = FilterType::ERROR_CODE;
            filter.key = "err";
            filter.value = token->value.substr(4);
            break;
        case TokenType::BASE: {
            const std::string& v = token->value;
            const std::string prefix = "base=\"";
            size_t start = (v.rfind(prefix, 0) == 0) ? prefix.size() : 0;
            size_t end = v.find('"', start);
            if (start == 0 || end == std::string::npos || end <= start) return;
            filter.type = FilterType::BASE;
            filter.key = "base";
            filter.value = v.substr(start, end - start);
            break;
        }
        case TokenType::FILTER_VALUE: {
            const std::string& v = token->value;
            const std::string prefix = "filter=\"";
            size_t start = (v.rfind(prefix, 0) == 0) ? prefix.size() : 0;
            size_t end = v.find('"', start);
            if (start == 0 || end == std::string::npos || end <= start) return;
            filter.type = FilterType::TEXT;
            filter.value = v.substr(start, end - start);
            break;
        }
        case TokenType::ATTR_LIST: {
            filter.type = FilterType::TEXT;
            filter.value = token->value;
            break;
        }
        default:
            return;
    }

    filterStack_.push(filter);

    if (connFastPath) {
        filtering_ = true;
        g_interrupted = 0;
        filterStart_ = std::chrono::steady_clock::now();
        visibleIndices_ = std::move(connMatches);
        finishFilterUpdate();
    } else {
        buildFilteredIndices();
    }

    scrollOffset_ = 0;
    cursorRow_ = 0;
    cursorCol_ = 0;
    searchResults_.clear();
    fullRedraw();
}

void Viewer::addConnFilterForCurrentLine() {
    if (cursorRow_ < 0 || cursorRow_ >= (int)visibleIndices_.size()) return;
    auto line = buffer_.getLine(visibleIndices_[cursorRow_]);
    if (!line || !line->conn_id) return;

    Filter connFilter;
    connFilter.type = FilterType::CONN;
    connFilter.key = "conn";
    connFilter.value = *line->conn_id;
    filterStack_.push(connFilter);
}

void Viewer::activateFilterAtPosition(int row, int col) {
    cursorRow_ = row;
    cursorCol_ = col;
    activateFilter();
}

void Viewer::deactivateFilter() {
    if (filterStack_.empty()) return;
    filterStack_.clear();
    buildFilteredIndices();
    scrollOffset_ = 0;
    cursorRow_ = 0;
    cursorCol_ = 0;
    searchResults_.clear();
    fullRedraw();
}

void Viewer::startSearch() {
    std::string prefill;
    auto token = getTokenAtCursor();
    if (token.has_value()) {
        switch (token->type) {
            case TokenType::CONN_ID:    prefill = token->value.substr(5); break;
            case TokenType::DN_VALUE:   prefill = token->value.substr(4, token->value.length() - 5); break;
            case TokenType::OP_ID:      prefill = token->value.substr(3); break;
            case TokenType::ERROR_CODE: prefill = token->value.substr(4); break;
            case TokenType::THREAD_ID:  prefill = token->value; break;
            default: break;
        }
    }

    std::string query;
    bool done = false;
    std::cout << Mv::to(termHeight_, 1) << Term::el << Fx::rev
              << "/" << prefill << Fx::urev << Fx::reset << std::flush;
    query = prefill;

    while (!done && running_.load()) {
        Input::Event ev = Input::pollKey(20);
        if (ev.key == Input::Key::NONE) continue;
        switch (ev.key) {
            case Input::Key::ENTER:
                done = true;
                break;
            case Input::Key::ESC:
                query.clear();
                done = true;
                break;
            case Input::Key::BACKSPACE:
                if (!query.empty()) query.pop_back();
                break;
            case Input::Key::CHAR:
                if (ev.ch >= 32 && ev.ch < 127) query += (char)ev.ch;
                break;
            default:
                break;
        }
        std::cout << Mv::to(termHeight_, 1) << Term::el << Fx::rev
                  << "/" << query << Fx::urev << Fx::reset << std::flush;
    }

    if (!query.empty()) {
        performSearch(query);
    }
    fullRedraw();
}

void Viewer::performSearch(const std::string& query) {
    searchQuery_ = query;
    searchResults_.clear();
    currentSearchResult_ = 0;

    const size_t total = visibleIndices_.size();
    if (total == 0) return;

    showPopup("Searching... (Esc to cancel)", 0.0f);

    bool cancelled = false;
    auto lastUpdate = std::chrono::steady_clock::now();
    int lastPct = -1;

    for (size_t i = 0; i < total; i++) {
        if ((i & 0x3FF) == 0) {
            Input::Event ev = Input::pollKey(0);
            if (ev.key == Input::Key::ESC || ev.key == Input::Key::BACKSPACE ||
                ev.key == Input::Key::CTRL_C) {
                cancelled = true;
                break;
            }
            if (ev.key == Input::Key::CHAR &&
                (ev.ch == 'q' || ev.ch == 'Q')) {
                cancelled = true;
                break;
            }
            auto now = std::chrono::steady_clock::now();
            if (now - lastUpdate >= std::chrono::milliseconds(100)) {
                int pct = (int)(((double)i * 100.0) / (double)total);
                if (pct != lastPct) {
                    lastPct = pct;
                    popupMessage_ = "Searching... " + std::to_string(pct) + "%";
                    popupProgress_ = pct / 100.0f;
                    fullRedraw();
                }
                lastUpdate = now;
            }
        }
        std::optional<std::string> raw = buffer_.getRawLine(visibleIndices_[i]);
        if (raw && raw->find(query) != std::string::npos) {
            searchResults_.push_back(i);
        }
    }

    if (cancelled) {
        hidePopup();
        return;
    }

    popupMessage_ = "Searching... done (" + std::to_string(searchResults_.size()) + ")";
    popupProgress_ = 1.0f;
    fullRedraw();

    if (!searchResults_.empty()) {
        cursorRow_ = (int)searchResults_[0];
        scrollOffset_ = std::max(0, (int)cursorRow_ - (termHeight_ / 2));
    }

    hidePopup();
}

void Viewer::nextSearchResult() {
    if (searchResults_.empty()) return;
    currentSearchResult_ = (currentSearchResult_ + 1) % searchResults_.size();
    cursorRow_ = (int)searchResults_[currentSearchResult_];
    int contentHeight = termHeight_ - 2;
    if (cursorRow_ < scrollOffset_ || cursorRow_ >= scrollOffset_ + contentHeight) {
        scrollOffset_ = std::max(0, (int)cursorRow_ - contentHeight / 2);
    }
    fullRedraw();
}

void Viewer::prevSearchResult() {
    if (searchResults_.empty()) return;
    currentSearchResult_ = (currentSearchResult_ + searchResults_.size() - 1) % searchResults_.size();
    cursorRow_ = (int)searchResults_[currentSearchResult_];
    int contentHeight = termHeight_ - 2;
    if (cursorRow_ < scrollOffset_ || cursorRow_ >= scrollOffset_ + contentHeight) {
        scrollOffset_ = std::max(0, (int)cursorRow_ - contentHeight / 2);
    }
    fullRedraw();
}

void Viewer::moveCursorUp() {
    if (cursorRow_ > 0) {
        int oldCursorRow = cursorRow_;
        cursorRow_--;
        if (cursorRow_ < scrollOffset_) {
            scrollOffset_ = cursorRow_;
            fullRedraw();
            auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_);
            if (tokenIdx.has_value()) currentTokenIndex_ = tokenIdx.value();
            return;
        }
        buffer_.prefetchAround(visibleIndices_[cursorRow_], 50);
        auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_);
        if (tokenIdx.has_value()) currentTokenIndex_ = tokenIdx.value();
        redrawLine(oldCursorRow, false);
        redrawLine(cursorRow_, true);
        moveCursorToScreen();
    }
}

void Viewer::moveCursorDown() {
    int contentHeight = termHeight_ - 2;
    if (cursorRow_ < (int)visibleIndices_.size() - 1) {
        int oldCursorRow = cursorRow_;
        cursorRow_++;
        if (cursorRow_ >= scrollOffset_ + contentHeight) {
            scrollOffset_ = cursorRow_ - contentHeight + 1;
            fullRedraw();
            auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_);
            if (tokenIdx.has_value()) currentTokenIndex_ = tokenIdx.value();
            return;
        }
        buffer_.prefetchAround(visibleIndices_[cursorRow_], 50);
        auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_);
        if (tokenIdx.has_value()) currentTokenIndex_ = tokenIdx.value();
        redrawLine(oldCursorRow, false);
        redrawLine(cursorRow_, true);
        moveCursorToScreen();
    }
}

void Viewer::pageUp() {
    int contentHeight = termHeight_ - 2;
    cursorRow_ = std::max(0, cursorRow_ - contentHeight);
    scrollOffset_ = std::max(0, scrollOffset_ - contentHeight);
    if (cursorRow_ < (int)visibleIndices_.size()) {
        buffer_.prefetchAround(visibleIndices_[cursorRow_], 100);
    }
    fullRedraw();
}

void Viewer::pageDown() {
    int contentHeight = termHeight_ - 2;
    cursorRow_ = std::min((int)visibleIndices_.size() - 1, cursorRow_ + contentHeight);
    scrollOffset_ = std::min((int)visibleIndices_.size() - 1, scrollOffset_ + contentHeight);
    if (scrollOffset_ < 0) scrollOffset_ = 0;
    if (cursorRow_ < (int)visibleIndices_.size()) {
        buffer_.prefetchAround(visibleIndices_[cursorRow_], 100);
    }
    fullRedraw();
}

void Viewer::goToTop() {
    cursorRow_ = 0;
    scrollOffset_ = 0;
    autoScroll_ = false;
    if (!visibleIndices_.empty()) {
        buffer_.prefetchAround(visibleIndices_[0], 100);
    }
    fullRedraw();
}

void Viewer::goToBottom() {
    int contentHeight = termHeight_ - 2;
    cursorRow_ = (int)visibleIndices_.size() - 1;
    scrollOffset_ = std::max(0, (int)visibleIndices_.size() - contentHeight);
    autoScroll_ = followMode_;
    if (cursorRow_ < (int)visibleIndices_.size()) {
        buffer_.prefetchAround(visibleIndices_[cursorRow_], 100);
    }
    fullRedraw();
}

int Viewer::getScreenRowForCursorRow(int cursorRow) const {
    return cursorRow - scrollOffset_;
}

int Viewer::getCursorRowForScreenRow(int screenRow) const {
    return screenRow + scrollOffset_;
}

bool Viewer::linePassesFilters(size_t lineIndex) {
    if (filterStack_.empty()) return true;
    std::optional<std::string> raw = buffer_.getRawLine(lineIndex);
    if (!raw) return false;
    if (!filterStack_.candidateInRaw(*raw)) return false;
    LogLine line = buffer_.getParser().parseLine(*raw);
    return filterStack_.matches(line, lineIndex);
}

std::vector<size_t> Viewer::scanLines(size_t scanStart, size_t scanEnd, bool& cancelled) {
    std::vector<size_t> result;
    result.reserve(scanEnd >= scanStart ? scanEnd - scanStart + 1 : 0);
    LogParser& parser = buffer_.getParser();
    cancelled = false;

    constexpr size_t kScanBlock = 4096;
    const auto startTime = std::chrono::steady_clock::now();
    auto lastUpdate = startTime;
    int lastPct = -1;

    auto reportProgress = [&](size_t absLine) {
        if (scanEnd <= scanStart) return;
        auto now = std::chrono::steady_clock::now();
        if (now - lastUpdate < std::chrono::milliseconds(100)) return;
        lastUpdate = now;
        int pct = (int)(((double)(absLine - scanStart) * 100.0)
                        / (double)(scanEnd - scanStart));
        if (pct != lastPct) {
            lastPct = pct;
            popupMessage_ = "Filtering... " + std::to_string(pct) + "%";
            popupProgress_ = pct / 100.0f;
            fullRedraw();
        }
    };

    for (size_t blockLo = scanStart; blockLo <= scanEnd; blockLo += kScanBlock) {
        size_t blockHi = std::min(blockLo + kScanBlock, scanEnd + 1);
        std::vector<std::string> lines;
        if (blockHi - blockLo > 0) {
            lines = buffer_.getRawLines(blockLo, blockHi - blockLo);
        }
        for (size_t i = 0; i < lines.size(); i++) {
            size_t idx = blockLo + i;
            if ((idx & 0x3FF) == 0) {
                Input::Event ev = Input::pollKey(0);
                if (ev.key == Input::Key::ESC || ev.key == Input::Key::BACKSPACE ||
                    ev.key == Input::Key::CTRL_C ||
                    (ev.key == Input::Key::CHAR && (ev.ch == 'q' || ev.ch == 'Q'))) {
                    cancelled = true;
                    break;
                }
                reportProgress(idx);
            }
            if (g_interrupted) { cancelled = true; break; }
            const std::string& raw = lines[i];
            if (!filterStack_.candidateInRaw(raw)) continue;
            LogLine line = parser.parseLine(raw);
            if (filterStack_.matches(line, idx)) {
                result.push_back(idx);
            }
        }
        if (cancelled) break;
    }
    return result;
}

bool Viewer::findConnRange(size_t fromLine, const std::string& connId,
                           size_t& connStart, size_t& connEnd,
                           std::vector<size_t>* outMatches) {
    const size_t total = buffer_.getTotalLines();
    if (total == 0) {
        connStart = 0;
        connEnd = 0;
        return true;
    }
    if (fromLine >= total) fromLine = total - 1;

    showPopup("Locating connection... (Esc to cancel)", 0.0f);

    g_interrupted = 0;

    bool cancelled = false;
    auto lastUpdate = std::chrono::steady_clock::now();
    int lastPct = -1;

    auto reportProgress = [&](size_t done, size_t span) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastUpdate < std::chrono::milliseconds(100)) return;
        lastUpdate = now;
        int pct = span > 0 ? (int)(((double)done * 100.0) / (double)span) : 0;
        if (pct != lastPct) {
            lastPct = pct;
            popupMessage_ = "Scanning connection... " + std::to_string(pct) + "%";
            popupProgress_ = pct / 100.0f;
            fullRedraw();
        }
    };

    connStart = fromLine;
    connEnd = fromLine;

    constexpr size_t kScanBlock = 4096;
    const size_t backSpan = fromLine + 1;

    auto collectMatch = [&](size_t idx) {
        if (outMatches) outMatches->push_back(idx);
    };

    size_t blockLo = (fromLine / kScanBlock) * kScanBlock;
    while (!cancelled) {
        size_t blockHi = std::min(blockLo + kScanBlock, fromLine + 1);
        std::vector<std::string> lines = buffer_.getRawLines(blockLo, blockHi - blockLo);
        size_t idx = blockLo + lines.size();
        while (idx > blockLo) {
            --idx;
            if (rawHasExactConn(lines[idx - blockLo], connId)) {
                connStart = idx;
                collectMatch(idx);
            }
            if ((idx & 0x3FF) == 0) {
                Input::Event ev = Input::pollKey(0);
                if (ev.key == Input::Key::ESC || ev.key == Input::Key::BACKSPACE ||
                    ev.key == Input::Key::CTRL_C ||
                    (ev.key == Input::Key::CHAR && (ev.ch == 'q' || ev.ch == 'Q'))) {
                    cancelled = true;
                    break;
                }
                reportProgress(blockHi - (idx + 1), total);
            }
        }
        if (blockLo == 0) break;
        if (!lines.empty()) blockLo -= kScanBlock;
        else break;
    }

    if (!cancelled) {
        size_t blockHi = ((fromLine + 1) / kScanBlock) * kScanBlock;
        while (blockHi < total && !cancelled) {
            size_t lo = std::max(blockHi, fromLine + 1);
            std::vector<std::string> lines = buffer_.getRawLines(lo, std::min(blockHi + kScanBlock, total) - lo);
            for (size_t i = 0; i < lines.size(); ++i) {
                size_t idx = lo + i;
                if (rawHasExactConn(lines[i], connId)) {
                    connEnd = idx;
                    collectMatch(idx);
                }
                if ((idx & 0x3FF) == 0) {
                    Input::Event ev = Input::pollKey(0);
                    if (ev.key == Input::Key::ESC || ev.key == Input::Key::BACKSPACE ||
                        ev.key == Input::Key::CTRL_C ||
                        (ev.key == Input::Key::CHAR && (ev.ch == 'q' || ev.ch == 'Q'))) {
                        cancelled = true;
                        break;
                    }
                    reportProgress(backSpan + (idx - fromLine), total);
                }
            }
            blockHi += kScanBlock;
        }
    }

    if (outMatches) {
        std::sort(outMatches->begin(), outMatches->end());
        outMatches->erase(std::unique(outMatches->begin(), outMatches->end()),
                          outMatches->end());
    }

    if (cancelled) {
        hidePopup();
        return false;
    }

    hidePopup();
    return true;
}

void Viewer::buildFilteredIndices() {
    filtering_ = true;
    g_interrupted = 0;
    filterStart_ = std::chrono::steady_clock::now();

    size_t totalLines = buffer_.getTotalLines();

    if (filterStack_.empty()) {
        visibleIndices_.clear();
        visibleIndices_.reserve(totalLines);
        for (size_t i = 0; i < totalLines; i++) {
            visibleIndices_.push_back(i);
        }
        hidePopup();
        filtering_ = false;
        return;
    }

    size_t scanStart = 0;
    size_t scanEnd = totalLines > 0 ? totalLines - 1 : 0;
    if (filterStack_.hasConnRange() && totalLines > 0) {
        scanStart = filterStack_.getRangeStart();
        size_t rangeEnd = filterStack_.getRangeEnd();
        scanEnd = rangeEnd < totalLines ? rangeEnd : totalLines - 1;
        if (scanStart > scanEnd) scanStart = scanEnd;
    }

    size_t rangeSize = scanEnd >= scanStart ? (scanEnd - scanStart + 1) : 0;

    showPopup("Filtering... (Esc to cancel)", 0.0f);

    std::vector<size_t> newVisibleIndices;
    newVisibleIndices.reserve(rangeSize);

    bool cancelled = false;
    newVisibleIndices = scanLines(scanStart, scanEnd, cancelled);

    if (cancelled) {
        hidePopup();
        filtering_ = false;
        return;
    }

    visibleIndices_ = std::move(newVisibleIndices);
    finishFilterUpdate();
}

void Viewer::finishFilterUpdate() {
    // Hold the popup on screen for at least MIN_POPUP_MS so the user
    // always sees the progress bar, even on a filter that finishes in
    // a few milliseconds.  Cancellable by Esc/Backspace.
    constexpr int MIN_POPUP_MS = 250;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - filterStart_).count();
    if (elapsed < MIN_POPUP_MS) {
        int remaining = MIN_POPUP_MS - (int)elapsed;
        showPopup("Filtering... done", 1.0f);
        while (remaining > 0 && !g_interrupted) {
            int chunk = std::min(remaining, 50);
            std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
            Input::Event ev = Input::pollKey(0);
            remaining -= chunk;
            if (ev.key == Input::Key::ESC || ev.key == Input::Key::BACKSPACE ||
                ev.key == Input::Key::CTRL_C ||
                (ev.key == Input::Key::CHAR && (ev.ch == 'q' || ev.ch == 'Q'))) {
                hidePopup();
                filtering_ = false;
                return;
            }
        }
        if (g_interrupted) {
            hidePopup();
            filtering_ = false;
            return;
        }
    }

    hidePopup();
    filtering_ = false;
}