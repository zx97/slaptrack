// viewer.cpp

/* 
    SPDX-License-Identifier: AGPL-3.0-or-later
    GNU Affero General Public License v3.0 (https://www.gnu.org/licenses/agpl-3.0.txt)
    Copyright (c) 2026 Manuel FLURY
    All rights reserved.
    
    This file is part of slaptrack - an OpenLDAP Log Viewer.
    
    Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0-or-later).
    See the LICENSE file distributed with this work for full license text.
    
    THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
    AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
    CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "viewer.h"
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

static void signalHandler(int /*sig*/) {
    g_interrupted = 1;
    if (g_viewer) {
        g_viewer->stop();
    }
}

Viewer::Viewer(const std::string& filename, bool followMode) 
    : filename_(filename), followMode_(followMode),
      mainWindow_(nullptr), filterWindow_(nullptr), statusWindow_(nullptr), popupWindow_(nullptr),
      scrollOffset_(0), cursorRow_(0), cursorCol_(0),
      termWidth_(80), termHeight_(24), running_(true), 
      currentSearchResult_(0),
      lastClickRow_(-1), lastClickCol_(-1),
      showLineNumbers_(false), currentTokenIndex_(0),
      followFd_(-1), followWatchFd_(-1), autoScroll_(followMode),
      wrapLines_(false),
      hoverRow_(-1), hoverCol_(-1), mouseActive_(false),
      showPopup_(false), popupProgress_(0.0f), filtering_(false),
      currentSchema_(0), autoColor_(false) {
    
    g_viewer = this;
    
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
            // Watch for content modification AND for self-move /
            // self-delete so we can re-open the file on log rotation
            // (slapd renames slapd.log to slapd.log.1 and creates a
            // new slapd.log).
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

void Viewer::initTerminal() {
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);
    start_color();
    use_default_colors();

    bool colorSupport = has_colors() && COLORS >= 8;
    const char* term = std::getenv("TERM");
    if (!term || std::strcmp(term, "dumb") == 0) {
        colorSupport = false;
    }
    if (!colorSupport) {
        currentSchema_ = 1;
        autoColor_ = true;
    }

    setupColors();
    
    getmaxyx(stdscr, termHeight_, termWidth_);

    if (termHeight_ < 3 || termWidth_ < 10) {
        // The UI needs at least 3 rows (content + filter + status) and a
        // usable width; newwin() with non-positive dimensions returns NULL
        // and every subsequent draw would crash.
        endwin();
        std::cerr << "Error: terminal too small for slaptrack "
                  << "(need at least 3 rows x 10 cols, have "
                  << termHeight_ << "x" << termWidth_ << ")\n";
        running_.store(false);
        return;
    }

    mainWindow_ = newwin(termHeight_ - 2, termWidth_, 0, 0);
    filterWindow_ = newwin(1, termWidth_, termHeight_ - 2, 0);
    statusWindow_ = newwin(1, termWidth_, termHeight_ - 1, 0);
    
    keypad(mainWindow_, TRUE);
    timeout(10);
    
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    mouseActive_ = true;
    
    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGQUIT, &sa, nullptr);
    
    recalculateScreenRows();
}

void Viewer::stop() {
    running_.store(false);
}

void Viewer::restoreTerminal() {
    if (popupWindow_) { delwin(popupWindow_); popupWindow_ = nullptr; }
    if (mainWindow_) delwin(mainWindow_);
    if (filterWindow_) delwin(filterWindow_);
    if (statusWindow_) delwin(statusWindow_);
    endwin();
}

const char* Viewer::SCHEMA_NAMES[8] = {
    "Default",
    "Monochrome",
    "Solarized Light",
    "Solarized Dark",
    "High Contrast",
    "Nord",
    "Gruvbox",
    "Dracula",
};

void Viewer::setupColors() {
    setupSchema(0);
}

void Viewer::setupSchema(int schema) {
    if (schema < 0 || schema > 7) return;
    currentSchema_ = schema;

    struct SchemaColor { short fg; short bg; };

    static const SchemaColor SCHEMAS[8][20] = {
        // F1: Default (bright)
        {
            {COLOR_CYAN,    -1}, {COLOR_YELLOW,  -1}, {COLOR_MAGENTA, -1},
            {COLOR_GREEN,   -1}, {COLOR_YELLOW,  -1}, {COLOR_BLUE,    -1},
            {COLOR_RED,     -1}, {COLOR_WHITE,   -1}, {COLOR_YELLOW,  -1},
            {COLOR_CYAN,    -1}, {COLOR_GREEN,   -1}, {COLOR_MAGENTA, -1},
            {COLOR_YELLOW,  -1}, {COLOR_CYAN,    -1}, {COLOR_BLACK,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   COLOR_RED},
            {COLOR_GREEN,   -1}, {COLOR_WHITE,   -1}, {COLOR_BLUE,    -1},
        },
        // F2: Monochrome (white on black, bold for emphasis)
        {
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1}, {COLOR_BLACK,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   COLOR_RED},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1},
        },
        // F3: Solarized Light (base03=bg 235, base0=content 244, base1=emph 147, yellow 136, orange 166, red 124, magenta 125, violet 61, blue 33, cyan 37, green 64)
        {
            {COLOR_BLUE,    -1}, {COLOR_YELLOW,  -1}, {COLOR_MAGENTA, -1},
            {COLOR_GREEN,   -1}, {COLOR_YELLOW,  -1}, {COLOR_BLUE,    -1},
            {COLOR_RED,     -1}, {COLOR_BLUE,    -1}, {COLOR_YELLOW,  -1},
            {COLOR_CYAN,    -1}, {COLOR_GREEN,   -1}, {COLOR_MAGENTA, -1},
            {COLOR_YELLOW,  -1}, {COLOR_CYAN,    -1}, {COLOR_BLACK,   -1},
            {COLOR_BLUE,    -1}, {COLOR_WHITE,   COLOR_RED},
            {COLOR_GREEN,   -1}, {COLOR_BLUE,    -1}, {COLOR_BLUE,    -1},
        },
        // F4: Solarized Dark (base03=bg 235, base0=content 244, base1=emph 147, yellow 136, orange 166, red 124, magenta 125, violet 61, blue 33, cyan 37, green 64)
        {
            {COLOR_CYAN,    -1}, {COLOR_YELLOW,  -1}, {COLOR_MAGENTA, -1},
            {COLOR_GREEN,   -1}, {COLOR_YELLOW,  -1}, {COLOR_BLUE,    -1},
            {COLOR_RED,     -1}, {COLOR_BLUE,    -1}, {COLOR_YELLOW,  -1},
            {COLOR_CYAN,    -1}, {COLOR_GREEN,   -1}, {COLOR_MAGENTA, -1},
            {COLOR_YELLOW,  -1}, {COLOR_CYAN,    -1}, {COLOR_BLACK,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   COLOR_RED},
            {COLOR_GREEN,   -1}, {COLOR_BLUE,    -1}, {COLOR_BLUE,    -1},
        },
        // F5: High Contrast (black text on white)
        {
            {COLOR_BLACK,   -1}, {COLOR_BLACK,   -1}, {COLOR_BLACK,   -1},
            {COLOR_BLACK,   -1}, {COLOR_BLACK,   -1}, {COLOR_BLUE,    -1},
            {COLOR_RED,     -1}, {COLOR_BLACK,   -1}, {COLOR_BLACK,   -1},
            {COLOR_BLACK,   -1}, {COLOR_BLACK,   -1}, {COLOR_BLACK,   -1},
            {COLOR_BLACK,   -1}, {COLOR_BLACK,   -1}, {COLOR_BLACK,   COLOR_WHITE},
            {COLOR_BLACK,   -1}, {COLOR_BLACK,   COLOR_RED},
            {COLOR_BLACK,   -1}, {COLOR_BLACK,   -1}, {COLOR_BLUE,    -1},
        },
        // F6: Nord (polar night 15,4,8,9 / snow storm 7,6,5 / frost 10,14,15 / aurora 12,11,13)
        {
            {COLOR_CYAN,    -1}, {COLOR_YELLOW,  -1}, {COLOR_MAGENTA, -1},
            {COLOR_GREEN,   -1}, {COLOR_YELLOW,  -1}, {COLOR_CYAN,    -1},
            {COLOR_RED,     -1}, {COLOR_WHITE,   -1}, {COLOR_YELLOW,  -1},
            {COLOR_CYAN,    -1}, {COLOR_GREEN,   -1}, {COLOR_MAGENTA, -1},
            {COLOR_YELLOW,  -1}, {COLOR_CYAN,    -1}, {COLOR_BLACK,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   COLOR_RED},
            {COLOR_GREEN,   -1}, {COLOR_WHITE,   -1}, {COLOR_CYAN,    -1},
        },
        // F7: Gruvbox (dark bg 235, light bg 229, dark1 241, dark3 246, light1 223, light3 250 / colors: red 124, green 142, yellow 136, blue 109, purple 175, aqua 108, orange 208, pink 219)
        {
            {COLOR_YELLOW,  -1}, {COLOR_YELLOW,  -1}, {COLOR_MAGENTA, -1},
            {COLOR_GREEN,   -1}, {COLOR_YELLOW,  -1}, {COLOR_BLUE,    -1},
            {COLOR_RED,     -1}, {COLOR_WHITE,   -1}, {COLOR_YELLOW,  -1},
            {COLOR_YELLOW,  -1}, {COLOR_GREEN,   -1}, {COLOR_MAGENTA, -1},
            {COLOR_YELLOW,  -1}, {COLOR_YELLOW,  -1}, {COLOR_BLACK,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   COLOR_RED},
            {COLOR_GREEN,   -1}, {COLOR_WHITE,   -1}, {COLOR_BLUE,    -1},
        },
        // F8: Dracula (bg 46,34,39 / fg 248,248,242 / comment 68,64,80 / red 237,80,74 / orange 255,140,0 / yellow 241,250,140 / green 80,250,123 / purple 189,147,249 / cyan 116,231,219 / pink 255,121,198)
        {
            {COLOR_CYAN,    -1}, {COLOR_YELLOW,  -1}, {COLOR_MAGENTA, -1},
            {COLOR_GREEN,   -1}, {COLOR_YELLOW,  -1}, {COLOR_CYAN,    -1},
            {COLOR_RED,     -1}, {COLOR_WHITE,   -1}, {COLOR_YELLOW,  -1},
            {COLOR_CYAN,    -1}, {COLOR_GREEN,   -1}, {COLOR_MAGENTA, -1},
            {COLOR_YELLOW,  -1}, {COLOR_CYAN,    -1}, {COLOR_BLACK,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   COLOR_RED},
            {COLOR_GREEN,   -1}, {COLOR_WHITE,   -1}, {COLOR_CYAN,    -1},
        },
    };

    const auto& s = SCHEMAS[schema];
    for (int i = 0; i < 20; i++) {
        init_pair(i + 1, s[i].fg, s[i].bg);
    }
}

int Viewer::getColorForToken(TokenType type) {
    switch (type) {
        case TokenType::TIMESTAMP: return 1;
        case TokenType::CONN_ID: return 2;
        case TokenType::OP_ID: return 3;
        case TokenType::DN_VALUE: return 4;
        case TokenType::FILTER_VALUE: return 5;
        case TokenType::IP_ADDRESS: return 6;
        case TokenType::ERROR_CODE: return 7;
        case TokenType::KEYWORD: return 6;
        case TokenType::FD_NUM: return 8;
        case TokenType::TAG: return 9;
        case TokenType::ETIME_VAL: return 10;
        case TokenType::NENTRIES: return 11;
        case TokenType::QTIME_VAL: return 12;
        case TokenType::SCOPE: return 13;
        case TokenType::DEREF: return 14;
        case TokenType::ATTR: return 18;
        case TokenType::ATTR_LIST: return 19;
        case TokenType::BASE: return 20;
        default: return 0;
    }
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
        // drawPopup() (called from background work) flushes its own
        // changes via doupdate(), so the main loop only needs to
        // redraw the rest of the screen here.
        if (showPopup_) {
            wnoutrefresh(popupWindow_);
        }
        doupdate();
    }

    restoreTerminal();
}

void Viewer::fullRedraw() {
    getmaxyx(stdscr, termHeight_, termWidth_);
    
    wresize(mainWindow_, termHeight_ - 2, termWidth_);
    wresize(filterWindow_, 1, termWidth_);
    wresize(statusWindow_, 1, termWidth_);
    mvwin(filterWindow_, termHeight_ - 2, 0);
    mvwin(statusWindow_, termHeight_ - 1, 0);
    
    recalculateScreenRows();
    drawContent();
    drawFilterBar();
    drawStatusBar();
    
    if (showPopup_) {
        drawPopup();
    } else {
        int screenRow = getScreenRowForCursorRow(cursorRow_);
        wmove(mainWindow_, screenRow, cursorCol_);
    }
    
    wnoutrefresh(mainWindow_);
    wnoutrefresh(filterWindow_);
    wnoutrefresh(statusWindow_);
    doupdate();
}

void Viewer::recalculateScreenRows() {
    lineScreenRows_.clear();
    if (!wrapLines_) {
        lineScreenRows_.resize(visibleIndices_.size(), 1);
        return;
    }
    
    int contentWidth = termWidth_;
    if (showLineNumbers_) {
        size_t totalLines = buffer_.getTotalLines();
        int numWidth = std::to_string(totalLines).length() + 2;
        contentWidth -= numWidth;
    }
    
    for (size_t i = 0; i < visibleIndices_.size(); i++) {
        auto line = buffer_.getLine(visibleIndices_[i]);
        if (!line) {
            lineScreenRows_.push_back(1);
            continue;
        }
        
        int len = line->raw.length();
        int rows = (len + contentWidth - 1) / contentWidth;
        if (rows < 1) rows = 1;
        lineScreenRows_.push_back(rows);
    }
}

int Viewer::calculateLineScreenRows(const LogLine& line) {
    if (!wrapLines_) return 1;
    
    int contentWidth = termWidth_;
    if (showLineNumbers_) {
        size_t totalLines = buffer_.getTotalLines();
        int numWidth = std::to_string(totalLines).length() + 2;
        contentWidth -= numWidth;
    }
    
    int len = line.raw.length();
    int rows = (len + contentWidth - 1) / contentWidth;
    return rows < 1 ? 1 : rows;
}

int Viewer::getScreenRowForCursorRow(int cursorRow) {
    if (!wrapLines_) return cursorRow - scrollOffset_;
    
    int screenRow = 0;
    for (int i = scrollOffset_; i < cursorRow && i < (int)lineScreenRows_.size(); i++) {
        screenRow += lineScreenRows_[i];
    }
    return screenRow;
}

int Viewer::getCursorRowForScreenRow(int screenRow) {
    if (!wrapLines_) return screenRow + scrollOffset_;
    
    int currentScreenRow = 0;
    for (size_t i = scrollOffset_; i < lineScreenRows_.size(); i++) {
        currentScreenRow += lineScreenRows_[i];
        if (currentScreenRow > screenRow) {
            return i;
        }
    }
    return (int)visibleIndices_.size() - 1;
}

void Viewer::drawContent() {
    werase(mainWindow_);
    
    int contentHeight = termHeight_ - 2;
    int currentScreenRow = 0;
    
    for (int i = scrollOffset_; i < (int)visibleIndices_.size() && currentScreenRow < contentHeight; i++) {
        auto line = buffer_.getLine(visibleIndices_[i]);
        if (!line) {
            currentScreenRow++;
            continue;
        }
        
        bool isHighlighted = (i == cursorRow_);
        drawLine(currentScreenRow, *line, visibleIndices_[i], isHighlighted);
        
        if (wrapLines_) {
            int rows = lineScreenRows_[i];
            currentScreenRow += rows;
        } else {
            currentScreenRow++;
        }
    }
}

void Viewer::drawLine(int startRow, const LogLine& line, size_t lineNum, bool isHighlighted) {
    if (wrapLines_) {
        printWrappedLine(line, lineNum, startRow, isHighlighted);
    } else {
        printTruncatedLine(line, lineNum, startRow, isHighlighted);
    }
}

void Viewer::printTruncatedLine(const LogLine& line, size_t lineNum, int row, bool isHighlighted) {
    wmove(mainWindow_, row, 0);
    wclrtoeol(mainWindow_);
    
    if (isHighlighted) {
        wattron(mainWindow_, A_REVERSE);
    }
    
    int col = 0;
    
    if (showLineNumbers_) {
        size_t totalLines = buffer_.getTotalLines();
        int numWidth = std::to_string(totalLines).length();
        std::string numStr = std::to_string(lineNum + 1);
        while ((int)numStr.length() < numWidth) {
            numStr = " " + numStr;
        }
        numStr += " ";
        
        wattron(mainWindow_, COLOR_PAIR(15));
        mvwprintw(mainWindow_, row, 0, "%s", numStr.c_str());
        wattroff(mainWindow_, COLOR_PAIR(15));
        col = numWidth + 1;
    }
    
    size_t lastEnd = 0;
    for (size_t i = 0; i < line.tokens.size() && col < termWidth_; i++) {
        const auto& token = line.tokens[i];
        
        if (token.start_pos > lastEnd && col < termWidth_) {
            std::string plainText = line.raw.substr(lastEnd, token.start_pos - lastEnd);
            if (col + (int)plainText.length() > termWidth_) {
                plainText = plainText.substr(0, termWidth_ - col);
            }
            mvwprintw(mainWindow_, row, col, "%s", plainText.c_str());
            col += plainText.length();
        }
        
        if (col < termWidth_) {
            bool isCurrentToken = isHighlighted && (i == currentTokenIndex_);
            bool isHovered = (row == hoverRow_ && col <= hoverCol_ && hoverCol_ < col + (int)token.value.length());
            printToken(token, isCurrentToken, isHovered);
            std::string val = token.value;
            if (col + (int)val.length() > termWidth_) {
                val = val.substr(0, termWidth_ - col);
            }
            mvwprintw(mainWindow_, row, col, "%s", val.c_str());
            col += token.value.length();
        }
        
        lastEnd = token.end_pos;
    }
    
    if (lastEnd < line.raw.length() && col < termWidth_) {
        std::string remaining = line.raw.substr(lastEnd, termWidth_ - col);
        mvwprintw(mainWindow_, row, col, "%s", remaining.c_str());
        col += remaining.length();
    }
    
    if (isHighlighted) {
        while (col < termWidth_) {
            mvwaddch(mainWindow_, row, col, ' ');
            col++;
        }
        wattroff(mainWindow_, A_REVERSE);
    }
}

void Viewer::printWrappedLine(const LogLine& line, size_t lineNum, int startRow, bool isHighlighted) {
    int contentWidth = termWidth_;
    int numWidth = 0;
    
    if (showLineNumbers_) {
        size_t totalLines = buffer_.getTotalLines();
        numWidth = std::to_string(totalLines).length() + 1;
        contentWidth -= numWidth;
    }
    
    if (isHighlighted) {
        int rows = calculateLineScreenRows(line);
        for (int r = 0; r < rows && startRow + r < termHeight_ - 2; r++) {
            wmove(mainWindow_, startRow + r, 0);
            wattron(mainWindow_, A_REVERSE);
            for (int c = 0; c < termWidth_; c++) {
                waddch(mainWindow_, ' ');
            }
            wattroff(mainWindow_, A_REVERSE);
        }
    }
    
    int col = 0;
    int row = startRow;
    
    if (showLineNumbers_ && row < termHeight_ - 2) {
        size_t totalLines = buffer_.getTotalLines();
        int numWidthFull = std::to_string(totalLines).length();
        std::string numStr = std::to_string(lineNum + 1);
        while ((int)numStr.length() < numWidthFull) {
            numStr = " " + numStr;
        }
        numStr += " ";
        
        wattron(mainWindow_, COLOR_PAIR(15));
        mvwprintw(mainWindow_, row, 0, "%s", numStr.c_str());
        wattroff(mainWindow_, COLOR_PAIR(15));
        col = numWidth;
    }
    
    std::string fullText = line.raw;
    size_t pos = 0;
    
    while (pos < fullText.length() && row < termHeight_ - 2) {
        int spaceLeft = contentWidth - (col - numWidth);
        if (spaceLeft <= 0) {
            row++;
            col = numWidth;
            spaceLeft = contentWidth;
        }
        
        size_t chunkLen = std::min((size_t)spaceLeft, fullText.length() - pos);
        std::string chunk = fullText.substr(pos, chunkLen);
        
        mvwprintw(mainWindow_, row, col, "%s", chunk.c_str());
        col += chunkLen;
        pos += chunkLen;
        
        if (col >= termWidth_ && pos < fullText.length()) {
            row++;
            col = numWidth;
        }
    }
}

void Viewer::printToken(const Token& token, bool isCurrentToken, bool isHovered) {
    int colorPair = getColorForToken(token.type);

    // Reset the per-token attributes we manage so the previous token's
    // color and bold do NOT leak into the next one.  The line-wide
    // A_REVERSE (set by printTruncatedLine for the cursor line) is
    // NOT touched here, so the cursor-line tint is preserved.
    wattroff(mainWindow_, A_COLOR);
    wattroff(mainWindow_, A_BOLD);
    wattroff(mainWindow_, A_UNDERLINE);

    if (colorPair > 0) {
        wattron(mainWindow_, COLOR_PAIR(colorPair));
    }

    if (isCurrentToken) {
        // Underline the token under the cursor so the user can see
        // exactly which block of text the cursor "englobes" and will
        // be filtered by Enter.
        wattron(mainWindow_, A_UNDERLINE);
    } else if (isHovered) {
        wattron(mainWindow_, A_REVERSE);
    }

    if (token.type == TokenType::CONN_ID ||
        token.type == TokenType::OP_ID ||
        token.type == TokenType::DN_VALUE ||
        token.type == TokenType::ERROR_CODE ||
        token.type == TokenType::KEYWORD ||
        token.type == TokenType::NENTRIES ||
        token.type == TokenType::QTIME_VAL ||
        token.type == TokenType::SCOPE ||
        token.type == TokenType::DEREF ||
        token.type == TokenType::ATTR ||
        token.type == TokenType::BASE) {
        wattron(mainWindow_, A_BOLD);
    }
    // TIMESTAMP is intentionally NOT bold: the prefix (timestamp +
    // hostname + pid) should be the same color/weight so the visual
    // grouping reads as a single unit, not as "this part is brighter
    // than the rest".  Removing BOLD here is what makes the
    // timestamp, server name, and pid all look the same.
}

void Viewer::redrawLine(int cursorRow, bool isHighlighted) {
    int screenRow = getScreenRowForCursorRow(cursorRow);
    if (screenRow < 0 || screenRow >= termHeight_ - 2) return;
    if (cursorRow >= (int)visibleIndices_.size()) return;
    size_t lineIdx = visibleIndices_[cursorRow];
    auto line = buffer_.getLine(lineIdx);
    if (!line) return;
    drawLine(screenRow, *line, lineIdx, isHighlighted);
}

void Viewer::drawFilterBar() {
    werase(filterWindow_);
    wattron(filterWindow_, A_REVERSE);
    
    std::string filterText = " Filters: ";
    if (filterStack_.empty()) {
        filterText += "(none)";
    } else {
        for (size_t i = 0; i < filterStack_.size(); i++) {
            if (i > 0) filterText += " > ";
            filterText += filterStack_.getFilters()[i].toString();
        }
    }
    
    filterText += " | Lines: " + std::to_string(visibleIndices_.size()) + "/" + std::to_string(buffer_.getTotalLines());
    
    if (followMode_) {
        filterText += " | [FOLLOW]";
    }
    
    if (!searchQuery_.empty()) {
        filterText += " | Search: \"" + searchQuery_ + "\"";
        if (!searchResults_.empty()) {
            filterText += " (" + std::to_string(currentSearchResult_ + 1) + "/" + std::to_string(searchResults_.size()) + ")";
        }
    }
    
    while ((int)filterText.length() < termWidth_) {
        filterText += " ";
    }
    filterText = filterText.substr(0, termWidth_);
    
    mvwprintw(filterWindow_, 0, 0, "%s", filterText.c_str());
    wattroff(filterWindow_, A_REVERSE);
    wnoutrefresh(filterWindow_);
}

void Viewer::drawStatusBar() {
    werase(statusWindow_);
    wattron(statusWindow_, A_REVERSE);
    
    std::string status = " " + filename_;
    status += " | " + std::string(SCHEMA_NAMES[currentSchema_]);
    if (autoColor_) status += " [Auto]";

    if (cursorRow_ < (int)visibleIndices_.size()) {
        size_t lineNum = visibleIndices_[cursorRow_];
        status += " | L:" + std::to_string(lineNum + 1) + "/" + std::to_string(buffer_.getTotalLines());
        
        auto line = buffer_.getLine(lineNum);
        if (line) {
            if (line->conn_id) status += " | conn=" + line->conn_id.value();
            if (line->dn) status += " | dn=" + line->dn.value();
        }
    }
    
    std::string help = "[F1-8] [Enter]Filter [Esc]Back [/]Search [#]Num [W]rap [q]Quit ";
    
    int padding = termWidth_ - status.length() - help.length();
    if (padding < 0) padding = 0;
    
    std::string fullStatus = status + std::string(padding, ' ') + help;
    fullStatus = fullStatus.substr(0, termWidth_);
    
    mvwprintw(statusWindow_, 0, 0, "%s", fullStatus.c_str());
    wattroff(statusWindow_, A_REVERSE);
    wnoutrefresh(statusWindow_);
}

void Viewer::showPopup(const std::string& message, float progress) {
    popupMessage_ = message;
    popupProgress_ = progress;
    showPopup_ = true;
    drawPopup();
}

void Viewer::drawPopup() {
    int popupWidth = 50;
    int popupHeight = 5;
    int startY = (termHeight_ - popupHeight) / 2;
    int startX = (termWidth_ - popupWidth) / 2;

    if (popupWindow_) {
        delwin(popupWindow_);
    }
    popupWindow_ = newwin(popupHeight, popupWidth, startY, startX);
    keypad(popupWindow_, FALSE);

    wbkgd(popupWindow_, COLOR_PAIR(17));
    werase(popupWindow_);
    box(popupWindow_, 0, 0);

    int msgLen = (int)popupMessage_.length();
    int msgX = (popupWidth - msgLen) / 2;
    if (msgX < 1) msgX = 1;
    mvwprintw(popupWindow_, 1, msgX, "%s", popupMessage_.c_str());

    int barWidth = popupWidth - 6;
    int pos = (int)(barWidth * popupProgress_);

    wmove(popupWindow_, 3, 2);
    waddch(popupWindow_, '[');
    for (int i = 0; i < barWidth; i++) {
        if (i < pos) {
            wattron(popupWindow_, A_REVERSE);
            waddch(popupWindow_, '=');
            wattroff(popupWindow_, A_REVERSE);
        } else if (i == pos) {
            wattron(popupWindow_, A_REVERSE);
            waddch(popupWindow_, '>');
            wattroff(popupWindow_, A_REVERSE);
        } else {
            waddch(popupWindow_, ' ');
        }
    }
    waddch(popupWindow_, ']');

    std::string pct = std::to_string((int)(popupProgress_ * 100)) + "%";
    mvwprintw(popupWindow_, 3, popupWidth - (int)pct.length() - 2, "%s", pct.c_str());

    touchwin(popupWindow_);
    wnoutrefresh(popupWindow_);
}

void Viewer::hidePopup() {
    showPopup_ = false;
    if (popupWindow_) {
        delwin(popupWindow_);
        popupWindow_ = nullptr;
    }
    fullRedraw();
}

void Viewer::handleInput() {
    int ch = wgetch(mainWindow_);
    
    if (ch == ERR) {
        return;
    }
    
    if (ch == KEY_MOUSE) {
        handleMouseEvent();
        return;
    }
    
    if (ch == KEY_RESIZE) {
        fullRedraw();
        return;
    }
    
    if (ch == 'q' || ch == 'Q') {
        running_.store(false);
        return;
    }
    
    if (ch == 27) {
        deactivateFilter();
        return;
    }
    
    switch (ch) {
        case KEY_UP:
        case 'k':
            moveCursorUp();
            break;
        case KEY_DOWN:
        case 'j':
            moveCursorDown();
            break;
        case KEY_LEFT:
        case 'h':
            if (cursorCol_ > 0) {
                cursorCol_--;
                auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_);
                if (tokenIdx.has_value()) {
                    currentTokenIndex_ = tokenIdx.value();
                }
                // Redraw the current line so the token underline
                // follows the cursor within the line.
                redrawLine(cursorRow_, true);
                int screenRow = getScreenRowForCursorRow(cursorRow_);
                wmove(mainWindow_, screenRow, cursorCol_);
                wnoutrefresh(mainWindow_);
                doupdate();
            } else if (!filterStack_.empty()) {
                deactivateFilter();
            }
            break;
        case KEY_RIGHT:
        case 'l':
            if (cursorCol_ < termWidth_ - 1) {
                cursorCol_++;
                auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_);
                if (tokenIdx.has_value()) {
                    currentTokenIndex_ = tokenIdx.value();
                }
                redrawLine(cursorRow_, true);
                int screenRow = getScreenRowForCursorRow(cursorRow_);
                wmove(mainWindow_, screenRow, cursorCol_);
                wnoutrefresh(mainWindow_);
                doupdate();
            } else {
                // Right at end of line jumps to start of next line.
                moveCursorDown();
                cursorCol_ = 0;
                currentTokenIndex_ = 0;
                redrawLine(cursorRow_, true);
            }
            break;
        case KEY_PPAGE:
            pageUp();
            break;
        case KEY_NPAGE:
            pageDown();
            break;
        case KEY_HOME:
            goToTop();
            break;
        case KEY_END:
            goToBottom();
            break;
        case '\n':
        case '\r':
            activateFilter();
            break;
        case KEY_BACKSPACE:
        case 127:
            deactivateFilter();
            break;
        case '/':
            startSearch();
            break;
        case 'n':
            nextSearchResult();
            break;
        case 'N':
            prevSearchResult();
            break;
        case 'g':
            goToTop();
            break;
        case 'G':
            goToBottom();
            break;
        case '#':
            showLineNumbers_ = !showLineNumbers_;
            recalculateScreenRows();
            fullRedraw();
            break;
        case ':':
            handleCommandMode();
            break;
        case '^':
            moveCursorToLineStart();
            break;
        case '$':
            moveCursorToLineEnd();
            break;
        case 'W':
        case 'w':
            wrapLines_ = !wrapLines_;
            recalculateScreenRows();
            fullRedraw();
            break;
        case KEY_F(1): case KEY_F(2): case KEY_F(3): case KEY_F(4):
        case KEY_F(5): case KEY_F(6): case KEY_F(7): case KEY_F(8): {
            int schema = (ch - KEY_F(1));
            setupSchema(schema);
            fullRedraw();
            break;
        }
    }
}

void Viewer::handleMouseEvent() {
    MEVENT event;
    if (getmouse(&event) == OK) {
        if (event.bstate & BUTTON1_CLICKED) {
            if (event.y < termHeight_ - 2) {
                int clickedRow = getCursorRowForScreenRow(event.y);
                if (clickedRow < (int)visibleIndices_.size()) {
                    int oldCursorRow = cursorRow_;
                    cursorRow_ = clickedRow;
                    cursorCol_ = event.x;

                    auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_);
                    if (tokenIdx.has_value()) {
                        currentTokenIndex_ = tokenIdx.value();
                    }

                    // Re-render both lines so the cursor's line
                    // highlight visibly follows the click.  Without
                    // this, only the terminal mouse moves and the
                    // line highlight stays stuck on the old row,
                    // which the user perceives as "two independent
                    // cursors".
                    if (oldCursorRow != cursorRow_) {
                        redrawLine(oldCursorRow, false);
                        redrawLine(cursorRow_, true);
                    } else {
                        redrawLine(cursorRow_, true);
                    }

                    int screenRow = getScreenRowForCursorRow(cursorRow_);
                    wmove(mainWindow_, screenRow, cursorCol_);
                    wnoutrefresh(mainWindow_);
                    doupdate();
                }
            }
        } else if (event.bstate & BUTTON1_DOUBLE_CLICKED) {
            if (event.y < termHeight_ - 2) {
                int clickedRow = getCursorRowForScreenRow(event.y);
                if (clickedRow < (int)visibleIndices_.size()) {
                    activateFilterAtPosition(clickedRow, event.x);
                }
            }
        } else if (event.bstate & BUTTON4_PRESSED) {
            moveCursorUp();
            moveCursorUp();
            moveCursorUp();
        } else if (event.bstate & BUTTON5_PRESSED) {
            moveCursorDown();
            moveCursorDown();
            moveCursorDown();
        } else if (event.bstate & REPORT_MOUSE_POSITION) {
            if (event.y != hoverRow_ || event.x != hoverCol_) {
                hoverRow_ = event.y;
                hoverCol_ = event.x;
                
                if (hoverRow_ < termHeight_ - 2) {
                    wmove(mainWindow_, hoverRow_, hoverCol_);
                    wnoutrefresh(mainWindow_);
                    doupdate();
                }
            }
        }
    }
}

void Viewer::handleFollowMode() {
    if (followFd_ < 0) return;

    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool inotifyEvent = false;
    bool fileVanished = false;

    // Drain inotify events.  Each inotify_event is variable-length,
    // so we walk the buffer in 4-byte-aligned steps.
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

    // Some kernels / filesystems can be slow to deliver IN_MODIFY for
    // small appends, or the writer may be O_APPEND with write sizes
    // below the inotify batching threshold.  Poll the file size every
    // 500 ms as a fallback so we never miss updates silently.
    auto now = std::chrono::steady_clock::now();
    auto sincePoll = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - lastFollowPoll_).count();
    if (!inotifyEvent && sincePoll < 500) {
        return;
    }
    lastFollowPoll_ = now;

    // If the file was rotated away, try to re-open it.  We always
    // re-stat the file by name to detect a replacement (e.g. mv
    // slapd.log.1 slapd.log.2; touch slapd.log).
    if (fileVanished) {
        // Drop the old watch first to avoid EBADF races; the inotify
        // fd itself stays valid.
        if (followWatchFd_ >= 0) {
            inotify_rm_watch(followFd_, followWatchFd_);
            followWatchFd_ = -1;
        }
        // Reopen the underlying index file by closing then asking
        // refreshFile to pull in the new inode.
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

        recalculateScreenRows();

        if (autoScroll_) {
            int contentHeight = termHeight_ - 2;
            cursorRow_ = (int)visibleIndices_.size() - 1;
            scrollOffset_ = std::max(0, cursorRow_ - contentHeight + 1);
        }
        fullRedraw();
    } else if (fileVanished) {
        // File was replaced but contained no new lines yet; still
        // redraw so the new content shows up.
        recalculateScreenRows();
        fullRedraw();
    }
}

void Viewer::handleCommandMode() {
    echo();
    curs_set(1);
    
    mvwprintw(statusWindow_, 0, 0, ":");
    wrefresh(statusWindow_);
    
    char input[256];
    wgetnstr(statusWindow_, input, sizeof(input) - 1);
    
    noecho();
    curs_set(0);
    
    std::string command(input);
    
    if (!command.empty()) {
        if (command == "$") {
            goToBottom();
        } else if (command == "^") {
            goToTop();
        } else {
            try {
                size_t lineNum = std::stoul(command);
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
    
    cursorRow_ = lineNum - 1;
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
    int screenRow = getScreenRowForCursorRow(cursorRow_);
    wmove(mainWindow_, screenRow, cursorCol_);
    wnoutrefresh(mainWindow_);
    doupdate();
}

void Viewer::moveCursorToLineEnd() {
if (cursorRow_ >= (int)visibleIndices_.size()) {
        return;
    }

    auto line = buffer_.getLine(visibleIndices_[cursorRow_]);
    if (line) {
        cursorCol_ = std::min((int)line->raw.length(), termWidth_ - 1);
        int screenRow = getScreenRowForCursorRow(cursorRow_);
        wmove(mainWindow_, screenRow, cursorCol_);
        wnoutrefresh(mainWindow_);
        doupdate();
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

    switch (token->type) {
        case TokenType::CONN_ID: {
            filter.type = FilterType::CONN;
            filter.key = "conn";
            filter.value = token->value.substr(5);

            size_t currentLine = visibleIndices_[cursorRow_];
            size_t connStart = findConnectionStart(currentLine, filter.value);
            size_t connEnd = findConnectionEnd(currentLine, filter.value);

            filter.rangeStart = connStart;
            filter.rangeEnd = connEnd;
            filter.hasRange = true;
            break;
        }
        case TokenType::DN_VALUE:
            filter.type = FilterType::DN;
            filter.key = "dn";
            filter.value = token->value.substr(4, token->value.length() - 5);
            break;
        case TokenType::OP_ID:
            filter.type = FilterType::OP;
            filter.key = "op";
            filter.value = token->value.substr(3);
            break;
        case TokenType::ERROR_CODE:
            filter.type = FilterType::ERROR_CODE;
            filter.key = "err";
            filter.value = token->value.substr(4);
            break;
        case TokenType::BASE: {
            // Token value is the full `base="..."` shape; the parser
            // stores the unquoted DN in LogLine::base, so we must
            // strip the wrapper before storing the filter value.
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
            // Strip the leading "filter=" and surrounding quotes so
            // the search needle is the LDAP filter expression only.
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
            // Activating a filter on an attribute name like
            // "(objectClass=inetOrgPerson)" searches every line that
            // mentions it.  Very useful for tracing attribute usage.
            filter.type = FilterType::TEXT;
            filter.value = token->value;
            break;
        }
        default:
            return;
    }
    
    filterStack_.push(filter);
    
    buildFilteredIndices();
    
    scrollOffset_ = 0;
    cursorRow_ = 0;
    cursorCol_ = 0;
    searchResults_.clear();
    fullRedraw();
}

void Viewer::activateFilterAtPosition(int row, int col) {
    cursorRow_ = row;
    cursorCol_ = col;
    activateFilter();
}

void Viewer::deactivateFilter() {
    if (filterStack_.empty()) return;
    
    filterStack_.pop();
    
    buildFilteredIndices();
    
    scrollOffset_ = 0;
    cursorRow_ = 0;
    cursorCol_ = 0;
    searchResults_.clear();
    fullRedraw();
}

void Viewer::startSearch() {
    echo();
    curs_set(1);
    
    std::string prefill;
    auto token = getTokenAtCursor();
    if (token.has_value()) {
        switch (token->type) {
            case TokenType::CONN_ID:
                prefill = token->value.substr(5);
                break;
            case TokenType::DN_VALUE:
                prefill = token->value.substr(4, token->value.length() - 5);
                break;
            case TokenType::OP_ID:
                prefill = token->value.substr(3);
                break;
            case TokenType::ERROR_CODE:
                prefill = token->value.substr(4);
                break;
            default:
                break;
        }
    }
    
    mvwprintw(statusWindow_, 0, 0, "/%s", prefill.c_str());
    wrefresh(statusWindow_);
    
    char input[256];
    if (!prefill.empty()) {
        mvwprintw(statusWindow_, 0, 1, "%s", prefill.c_str());
        wrefresh(statusWindow_);
    }
    
    wgetnstr(statusWindow_, input, sizeof(input) - 1);
    
    noecho();
    curs_set(0);
    
    std::string query(input);
    
    if (!query.empty()) {
        performSearch(query);
    }
    
    fullRedraw();
}

void Viewer::performSearch(const std::string& query) {
    searchQuery_ = query;
    searchResults_.clear();
    currentSearchResult_ = 0;
    
    for (size_t i = 0; i < visibleIndices_.size(); i++) {
        auto line = buffer_.getLine(visibleIndices_[i]);
        if (line && line->raw.find(query) != std::string::npos) {
            searchResults_.push_back(i);
        }
    }
    
    if (!searchResults_.empty()) {
        cursorRow_ = searchResults_[0];
        scrollOffset_ = std::max(0, (int)cursorRow_ - (termHeight_ / 2));
    }
}

void Viewer::nextSearchResult() {
    if (searchResults_.empty()) return;
    currentSearchResult_ = (currentSearchResult_ + 1) % searchResults_.size();
    cursorRow_ = searchResults_[currentSearchResult_];
    
    int contentHeight = termHeight_ - 2;
    if (cursorRow_ < scrollOffset_ || cursorRow_ >= scrollOffset_ + contentHeight) {
        scrollOffset_ = std::max(0, (int)cursorRow_ - contentHeight / 2);
    }
    
    fullRedraw();
}

void Viewer::prevSearchResult() {
    if (searchResults_.empty()) return;
    currentSearchResult_ = (currentSearchResult_ + searchResults_.size() - 1) % searchResults_.size();
    cursorRow_ = searchResults_[currentSearchResult_];
    
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
            // Whole view shifted; safest path is a full redraw.
            recalculateScreenRows();
            fullRedraw();
            auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_);
            if (tokenIdx.has_value()) currentTokenIndex_ = tokenIdx.value();
            return;
        }

        buffer_.prefetchAround(visibleIndices_[cursorRow_], 50);

        // Update which token the cursor is on so the highlight follows.
        auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_);
        if (tokenIdx.has_value()) currentTokenIndex_ = tokenIdx.value();

        // Swap the line highlight: un-highlight the old line, then
        // highlight the new one.  printToken needs currentTokenIndex_
        // to be correct to draw the underline.
        redrawLine(oldCursorRow, false);
        redrawLine(cursorRow_, true);

        int screenRow = getScreenRowForCursorRow(cursorRow_);
        wmove(mainWindow_, screenRow, cursorCol_);
        wnoutrefresh(mainWindow_);
        doupdate();
    }
}

void Viewer::moveCursorDown() {
    int contentHeight = termHeight_ - 2;
    if (cursorRow_ < (int)visibleIndices_.size() - 1) {
        int oldCursorRow = cursorRow_;
        cursorRow_++;
        if (cursorRow_ >= scrollOffset_ + contentHeight) {
            scrollOffset_ = cursorRow_ - contentHeight + 1;
            recalculateScreenRows();
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

        int screenRow = getScreenRowForCursorRow(cursorRow_);
        wmove(mainWindow_, screenRow, cursorCol_);
        wnoutrefresh(mainWindow_);
        doupdate();
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

// Returns true when the line at `lineIndex` should be visible given the
// current filter stack.  Used by the follow-mode appends (rotation and
// refresh) so newly arriving lines obey active filters instead of being
// shown unconditionally.
bool Viewer::linePassesFilters(size_t lineIndex) {
    if (filterStack_.empty()) return true;

    const std::string& raw = buffer_.getRawLine(lineIndex);
    if (raw.empty()) return false;
    if (!filterStack_.candidateInRaw(raw)) return false;

    LogLine line = buffer_.getParser().parseLine(raw);
    return filterStack_.matches(line, lineIndex);
}

size_t Viewer::findConnectionStart(size_t fromLine, const std::string& connId) {
    for (size_t i = fromLine; i > 0; i--) {
        size_t lineIdx = i - 1;
        auto line = buffer_.getLine(lineIdx);
        if (!line) continue;
        
        if (line->conn_id && line->conn_id.value() == connId) {
            if (line->raw.find("ACCEPT") != std::string::npos) {
                return lineIdx;
            }
            if (line->raw.find("closed") != std::string::npos) {
                return i;
            }
        }
    }
    
    auto line = buffer_.getLine(0);
    if (line && line->conn_id && line->conn_id.value() == connId) {
        if (line->raw.find("ACCEPT") != std::string::npos) {
            return 0;
        }
    }
    
    return fromLine;
}

size_t Viewer::findConnectionEnd(size_t fromLine, const std::string& connId) {
    size_t totalLines = buffer_.getTotalLines();
    
    for (size_t i = fromLine; i < totalLines; i++) {
        auto line = buffer_.getLine(i);
        if (!line) continue;
        
        if (line->conn_id && line->conn_id.value() == connId) {
            if (line->raw.find("closed") != std::string::npos) {
                return i;
            }
        }
    }
    
    return totalLines - 1;
}

void Viewer::buildFilteredIndices() {
    filtering_ = true;
    g_interrupted = 0;
    filterStart_ = std::chrono::steady_clock::now();

    size_t totalLines = buffer_.getTotalLines();

    // Fast path: empty filter stack means "show everything".  Skipping
    // the scan here is critical because the user hits Esc to clear
    // filters very often and an O(n) walk over a 300K-line log would
    // spin the CPU for seconds with no visible feedback.
    if (filterStack_.empty()) {
        visibleIndices_.clear();
        visibleIndices_.reserve(totalLines);
        for (size_t i = 0; i < totalLines; i++) {
            visibleIndices_.push_back(i);
        }
        recalculateScreenRows();
        hidePopup();
        filtering_ = false;
        return;
    }

    // Determine scan range from any conn filter on the stack.
    // Without a conn filter, scan the whole log (but the conn filter's
    // range check inside Filter::matches() will reject non-matching
    // lines cheaply for any subsequent filter).
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
    touchwin(mainWindow_);
    wnoutrefresh(mainWindow_);
    touchwin(filterWindow_);
    wnoutrefresh(filterWindow_);
    touchwin(statusWindow_);
    wnoutrefresh(statusWindow_);
    doupdate();

    std::vector<size_t> newVisibleIndices;
    newVisibleIndices.reserve(rangeSize);

    // Switch to non-blocking input so we can poll for cancellation
    // keys without slowing the filter loop with 10ms timeouts.
    nodelay(stdscr, TRUE);

    size_t lastProgressLine = scanStart;
    bool cancelled = false;

    LogParser& parser = buffer_.getParser();
    for (size_t i = scanStart; i <= scanEnd; i++) {
        if (g_interrupted) { cancelled = true; break; }

        // Fast path: O(1) access to the raw line text.  Use the
        // cheap "candidate" check (substring search) to skip the
        // regex parse for any line whose raw text cannot possibly
        // contain the filter value.  On a 300K-line log with a base=
        // filter this skips 99% of parses.
        const std::string& raw = buffer_.getRawLine(i);
        if (raw.empty()) continue;
        if (!filterStack_.candidateInRaw(raw)) continue;
        LogLine line = parser.parseLine(raw);
        if (filterStack_.matches(line, i)) {
            newVisibleIndices.push_back(i);
        }

        // Periodically poll for a cancellation key and refresh the
        // progress bar.  Every 100 lines keeps the popup responsive
        // without making the inner loop measurably slower.
        if (i - lastProgressLine >= 100) {
            lastProgressLine = i;

            if (g_interrupted) { cancelled = true; break; }

            int ch = getch();
            if (ch != ERR) {
                if (ch == 27 /* Esc */ ||
                    ch == KEY_BACKSPACE || ch == 127 || ch == 8 ||
                    ch == 'q' || ch == 'Q' ||
                    ch == 3 /* Ctrl+C */ || ch == 28 /* Ctrl+\ */) {
                    cancelled = true;
                    g_interrupted = 1;
                } else {
                    ungetch(ch);
                }
            }

            if (cancelled) break;

            if (rangeSize > 0) {
                float progress = (float)(i - scanStart + 1) / (float)rangeSize;
                if (progress > 1.0f) progress = 1.0f;
                showPopup("Filtering... (Esc to cancel)", progress);
            }
        }
    }

    // Restore blocking input mode for the main loop.
    nodelay(stdscr, FALSE);
    timeout(10);

    if (cancelled) {
        // Keep filter stack and visible indices unchanged; just hide
        // the popup and return.  The caller (deactivateFilter) will
        // fullRedraw() so the popup overlay is removed cleanly.
        hidePopup();
        filtering_ = false;
        return;
    }

    visibleIndices_ = std::move(newVisibleIndices);
    recalculateScreenRows();

    // Hold the popup on screen for at least MIN_POPUP_MS so the user
    // always sees the progress bar, even on a filter that finishes in
    // a few milliseconds.  Cancellable by Esc/Backspace so the user
    // is not locked out.
    constexpr int MIN_POPUP_MS = 250;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - filterStart_).count();
    if (elapsed < MIN_POPUP_MS) {
        int remaining = MIN_POPUP_MS - (int)elapsed;
        showPopup("Filtering... done", 1.0f);
        doupdate();
        while (remaining > 0 && !g_interrupted) {
            int chunk = std::min(remaining, 50);
            napms(chunk);
            int ch = getch();
            remaining -= chunk;
            if (ch != ERR && ch != -1) {
                if (ch == 27 || ch == KEY_BACKSPACE || ch == 127 || ch == 8 ||
                    ch == 'q' || ch == 'Q' || ch == 3 || ch == 28) {
                    hidePopup();
                    filtering_ = false;
                    return;
                }
                ungetch(ch);
                break;
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
