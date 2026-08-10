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
    stopWrapWorker();
    if (wrapWorker_.joinable()) {
        wrapWorker_.join();
    }
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

    // Windows are created after the first setupColors() call, so apply
    // the current theme's background now that mainWindow_ exists.
    setupSchema(currentSchema_);
    
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

    // Pair index layout (index -> COLOR_PAIR(index+1)):
    //   0 TIMESTAMP | 1 CONN_ID | 2 THREAD/OP_ID | 3 DN_VALUE | 4 FILTER_VALUE
    //   5 IP/KEYWORD | 6 ERROR_CODE | 7 FD_NUM | 8 TAG | 9 ETIME_VAL
    //   10 NENTRIES | 11 QTIME_VAL | 12 SCOPE | 13 DEREF
    //   14 line numbers | 15 base text | 16 popup | 17 ATTR | 18 ATTR_LIST | 19 BASE
    //
    // The per-token colours of F1 (Default) are the ones documented in the
    // "Token colour mapping" table; the follow/pipe mode reuses exactly those
    // colours, so F1 and follow mode always match.

    static const SchemaColor SCHEMAS[8][20] = {
        // F1: Default (bright) — documented token colours
        {
            {COLOR_CYAN,    -1}, {COLOR_YELLOW,  -1}, {COLOR_MAGENTA, -1},
            {COLOR_GREEN,   -1}, {COLOR_YELLOW,  -1}, {COLOR_BLUE,    -1},
            {COLOR_RED,     -1}, {COLOR_WHITE,   -1}, {COLOR_YELLOW,  -1},
            {COLOR_CYAN,    -1}, {COLOR_GREEN,   -1}, {COLOR_MAGENTA, -1},
            {COLOR_YELLOW,  -1}, {COLOR_CYAN,    -1}, {COLOR_WHITE,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   COLOR_RED},
            {COLOR_GREEN,   -1}, {COLOR_WHITE,   -1}, {COLOR_BLUE,    -1},
        },
        // F2: Monochrome (white on black, bold for emphasis)
        {
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   COLOR_RED},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1}, {COLOR_WHITE,   -1},
        },
        // F3: Solarized Light — light background, warm accent colours
        {
            {COLOR_BLUE,    COLOR_WHITE}, {COLOR_MAGENTA, COLOR_WHITE},
            {COLOR_MAGENTA, COLOR_WHITE}, {COLOR_GREEN,   COLOR_WHITE},
            {COLOR_BLACK,   COLOR_WHITE}, {COLOR_BLUE,    COLOR_WHITE},
            {COLOR_RED,     COLOR_WHITE}, {COLOR_BLACK,   COLOR_WHITE},
            {COLOR_MAGENTA, COLOR_WHITE}, {COLOR_BLUE,    COLOR_WHITE},
            {COLOR_GREEN,   COLOR_WHITE}, {COLOR_MAGENTA, COLOR_WHITE},
            {COLOR_RED,     COLOR_WHITE}, {COLOR_BLUE,    COLOR_WHITE},
            {COLOR_BLACK,   COLOR_WHITE}, {COLOR_BLACK,   COLOR_WHITE},
            {COLOR_WHITE,   COLOR_RED},
            {COLOR_GREEN,   COLOR_WHITE}, {COLOR_BLACK,   COLOR_WHITE},
            {COLOR_BLUE,    COLOR_WHITE},
        },
        // F4: Solarized Dark — dark background, muted accent colours
        {
            {COLOR_CYAN,    -1}, {COLOR_YELLOW,  -1}, {COLOR_MAGENTA, -1},
            {COLOR_GREEN,   -1}, {COLOR_YELLOW,  -1}, {COLOR_BLUE,    -1},
            {COLOR_RED,     -1}, {COLOR_WHITE,   -1}, {COLOR_YELLOW,  -1},
            {COLOR_CYAN,    -1}, {COLOR_GREEN,   -1}, {COLOR_MAGENTA, -1},
            {COLOR_YELLOW,  -1}, {COLOR_CYAN,    -1}, {COLOR_WHITE,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   COLOR_RED},
            {COLOR_GREEN,   -1}, {COLOR_WHITE,   -1}, {COLOR_BLUE,    -1},
        },
        // F5: High Contrast — black text on a white background
        {
            {COLOR_BLACK,   COLOR_WHITE}, {COLOR_BLACK,   COLOR_WHITE},
            {COLOR_BLACK,   COLOR_WHITE}, {COLOR_BLACK,   COLOR_WHITE},
            {COLOR_BLACK,   COLOR_WHITE}, {COLOR_BLUE,    COLOR_WHITE},
            {COLOR_RED,     COLOR_WHITE}, {COLOR_BLACK,   COLOR_WHITE},
            {COLOR_BLACK,   COLOR_WHITE}, {COLOR_BLACK,   COLOR_WHITE},
            {COLOR_BLACK,   COLOR_WHITE}, {COLOR_BLACK,   COLOR_WHITE},
            {COLOR_BLACK,   COLOR_WHITE}, {COLOR_BLACK,   COLOR_WHITE},
            {COLOR_BLACK,   COLOR_WHITE}, {COLOR_BLACK,   COLOR_WHITE},
            {COLOR_WHITE,   COLOR_RED},
            {COLOR_BLACK,   COLOR_WHITE}, {COLOR_BLACK,   COLOR_WHITE},
            {COLOR_BLUE,    COLOR_WHITE},
        },
        // F6: Nord — cool grey-blue palette
        {
            {COLOR_CYAN,    -1}, {COLOR_BLUE,    -1}, {COLOR_CYAN,    -1},
            {COLOR_GREEN,   -1}, {COLOR_CYAN,    -1}, {COLOR_BLUE,    -1},
            {COLOR_RED,     -1}, {COLOR_WHITE,   -1}, {COLOR_BLUE,    -1},
            {COLOR_CYAN,    -1}, {COLOR_GREEN,   -1}, {COLOR_CYAN,    -1},
            {COLOR_BLUE,    -1}, {COLOR_CYAN,    -1}, {COLOR_WHITE,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   COLOR_RED},
            {COLOR_GREEN,   -1}, {COLOR_WHITE,   -1}, {COLOR_BLUE,    -1},
        },
        // F7: Gruvbox — retro warm palette
        {
            {COLOR_YELLOW,  -1}, {COLOR_YELLOW,  -1}, {COLOR_MAGENTA, -1},
            {COLOR_GREEN,   -1}, {COLOR_YELLOW,  -1}, {COLOR_BLUE,    -1},
            {COLOR_RED,     -1}, {COLOR_WHITE,   -1}, {COLOR_YELLOW,  -1},
            {COLOR_YELLOW,  -1}, {COLOR_GREEN,   -1}, {COLOR_MAGENTA, -1},
            {COLOR_YELLOW,  -1}, {COLOR_YELLOW,  -1}, {COLOR_WHITE,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   COLOR_RED},
            {COLOR_GREEN,   -1}, {COLOR_WHITE,   -1}, {COLOR_BLUE,    -1},
        },
        // F8: Dracula — purple-pink accent palette
        {
            {COLOR_MAGENTA, -1}, {COLOR_MAGENTA, -1}, {COLOR_MAGENTA, -1},
            {COLOR_GREEN,   -1}, {COLOR_YELLOW,  -1}, {COLOR_CYAN,    -1},
            {COLOR_RED,     -1}, {COLOR_WHITE,   -1}, {COLOR_MAGENTA, -1},
            {COLOR_CYAN,    -1}, {COLOR_GREEN,   -1}, {COLOR_MAGENTA, -1},
            {COLOR_YELLOW,  -1}, {COLOR_CYAN,    -1}, {COLOR_WHITE,   -1},
            {COLOR_WHITE,   -1}, {COLOR_WHITE,   COLOR_RED},
            {COLOR_GREEN,   -1}, {COLOR_WHITE,   -1}, {COLOR_CYAN,    -1},
        },
    };

    const auto& s = SCHEMAS[schema];
    for (int i = 0; i < 20; i++) {
        init_pair(i + 1, s[i].fg, s[i].bg);
    }
    if (mainWindow_) {
        wbkgd(mainWindow_, COLOR_PAIR(16));
    }
}

int Viewer::getColorForToken(TokenType type) {
    switch (type) {
        case TokenType::TIMESTAMP: return 1;
        case TokenType::THREAD_ID: return 3;
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
    stopWrapWorker();
}

void Viewer::fullRedraw(bool recomputeRows) {
    getmaxyx(stdscr, termHeight_, termWidth_);
    
    wresize(mainWindow_, termHeight_ - 2, termWidth_);
    wresize(filterWindow_, 1, termWidth_);
    wresize(statusWindow_, 1, termWidth_);
    mvwin(filterWindow_, termHeight_ - 2, 0);
    mvwin(statusWindow_, termHeight_ - 1, 0);
    
    if (recomputeRows) {
        recalculateScreenRows();
    }
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
}

void Viewer::toggleWrap() {
}

void Viewer::ensureScreenRowsCachedUpTo(size_t) {
}

int Viewer::getContentWidthForWrap() const {
    int contentWidth = termWidth_;
    if (showLineNumbers_) {
        size_t totalLines = buffer_.getTotalLines();
        int numWidth = (int)std::to_string(totalLines).length() + 2;
        contentWidth -= numWidth;
    }
    if (contentWidth < 1) contentWidth = 1;
    return contentWidth;
}

void Viewer::computeWrapRowsSync(size_t start, size_t end, int contentWidth,
                                 std::vector<int>& out) {
    out.clear();
    out.reserve(end - start);
    for (size_t i = start; i < end; i++) {
        std::optional<std::string> raw = buffer_.getRawLine(visibleIndices_[i]);
        if (!raw) {
            out.push_back(1);
            continue;
        }
        int len = (int)buffer_.getParser().renderedLength(*raw);
        int rows = (len + contentWidth - 1) / contentWidth;
        if (rows < 1) rows = 1;
        out.push_back(rows);
    }
}

int Viewer::wrapRowAt(size_t i) const {
    std::lock_guard<std::mutex> lk(wrapMutex_);
    if (i >= lineScreenRows_.size()) return 1;
    return lineScreenRows_[i];
}

void Viewer::startWrapCompute(int contentWidth) {
#if 0
    if (wrapWorker_.joinable()) {
        wrapStopRequested_.store(true);
        wrapWorker_.detach();
    }
    wrapStopRequested_.store(false);

    {
        std::lock_guard<std::mutex> lk(wrapMutex_);
        if (wrapWidth_ == contentWidth
            && wrapDone_
            && wrapSnapshot_ == visibleIndices_) {
            return;
        }
        wrapWidth_ = contentWidth;
        wrapSnapshot_ = visibleIndices_;
        wrapDone_ = false;
        lineScreenRows_.clear();
        lineScreenRows_.reserve(wrapSnapshot_.size());
    }
    wrapPopupPct_ = -1;
    wrapWorker_ = std::thread(&Viewer::wrapRunOneJob, this);
#endif
    (void)contentWidth;
}

void Viewer::stopWrapWorker() {
#if 0
    wrapStopRequested_.store(true);
#endif
}

void Viewer::wrapRunOneJob() {
#if 0
    static constexpr size_t kBatch = 256;
    static constexpr auto kYield = std::chrono::milliseconds(1);

    // Top-level try/catch: an uncaught exception in a std::thread body
    // calls std::terminate → SIGABRT.  We swallow everything so the
    // main thread can recover (it polls wrapDone_/lineScreenRows_).
    try {
        // Snapshot the job parameters under the lock; the main thread
        // can change them on the next startWrapCompute() but our local
        // copy keeps us consistent for this run.
        std::vector<size_t> snapshot;
        int width;
        size_t total;
        {
            std::lock_guard<std::mutex> lk(wrapMutex_);
            snapshot = wrapSnapshot_;
            width = wrapWidth_;
            total = snapshot.size();
        }

        for (size_t cur = 0; cur < total; ) {
            if (wrapStopRequested_.load()) {
                return;
            }
            const size_t end = std::min(cur + kBatch, total);
            std::vector<int> batch;
            batch.reserve(end - cur);
            for (size_t i = cur; i < end; i++) {
                std::optional<std::string> raw = buffer_.getRawLine(snapshot[i]);
                if (!raw) {
                    batch.push_back(1);
                    continue;
                }
                int len = (int)buffer_.getParser().renderedLength(*raw);
                int rows = (len + width - 1) / width;
                if (rows < 1) rows = 1;
                batch.push_back(rows);
            }

            {
                std::lock_guard<std::mutex> lk(wrapMutex_);
                if (wrapStopRequested_.load()
                    || wrapWidth_ != width
                    || wrapSnapshot_ != snapshot) {
                    return;
                }
                lineScreenRows_.insert(lineScreenRows_.end(),
                                       batch.begin(), batch.end());
            }
            cur = end;
            if (cur < total) {
                std::this_thread::sleep_for(kYield);
            }
        }

        {
            std::lock_guard<std::mutex> lk(wrapMutex_);
            if (wrapWidth_ == width && wrapSnapshot_ == snapshot) {
                wrapDone_ = true;
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr,
                     "slaptrack: wrap worker terminated: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr,
                     "slaptrack: wrap worker terminated: unknown\n");
    }
#endif
}

void Viewer::pumpWrapProgress() {
#if 0
    if (!wrapLines_) return;

    size_t have = 0;
    bool done = false;
    bool workerAlive = wrapWorker_.joinable();
    {
        std::lock_guard<std::mutex> lk(wrapMutex_);
        have = lineScreenRows_.size();
        done = wrapDone_;
    }
    const size_t total = visibleIndices_.size();
    if (total == 0) {
        if (showPopup_) hidePopupNoRedraw();
        return;
    }
    const size_t needed = std::min<size_t>(
        (size_t)scrollOffset_ + (size_t)(termHeight_ - 2), total);

    if (!done && !workerAlive && have < needed) {
        const auto now = std::chrono::steady_clock::now();
        if (lastPumpRecovery_.time_since_epoch().count() == 0
            || now - lastPumpRecovery_ >= std::chrono::seconds(1)) {
            lastPumpRecovery_ = now;
            recalculateScreenRows();
        }
        return;
    }

    if (done || have >= needed) {
        if (showPopup_) hidePopupNoRedraw();
        wrapPopupPct_ = -1;
        return;
    }

    int pct = (int)(((double)have * 100.0) / (double)total);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    if (pct == wrapPopupPct_) {
        return;
    }
    wrapPopupPct_ = pct;
    const std::string msg = "Computing wrap... " + std::to_string(pct) + "%";
    if (showPopup_) {
        popupMessage_ = msg;
        popupProgress_ = pct / 100.0f;
        drawPopup();
    } else {
        showPopup(msg, pct / 100.0f);
    }
#endif
}

int Viewer::calculateLineScreenRows(const LogLine& line) {
    (void)line;
    return 1;
}

int Viewer::getScreenRowForCursorRow(int cursorRow) {
    return cursorRow - scrollOffset_;
}

int Viewer::getCursorRowForScreenRow(int screenRow) {
    return screenRow + scrollOffset_;
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
        currentScreenRow++;
    }
}

void Viewer::drawLine(int startRow, const LogLine& line, size_t lineNum, bool isHighlighted) {
    printTruncatedLine(line, lineNum, startRow, isHighlighted);
}

void Viewer::printTruncatedLine(const LogLine& line, size_t lineNum, int row, bool isHighlighted) {
    wmove(mainWindow_, row, 0);
    wclrtoeol(mainWindow_);

    wattrset(mainWindow_, COLOR_PAIR(16));

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
                int offsetInLine = plainStart;
                int len = plainEnd - plainStart;
                int screenCol = col + (offsetInLine - adjStart);
                if (screenCol >= termWidth_) break;
                if (screenCol + len > termWidth_) {
                    len = termWidth_ - screenCol;
                }
                wattrset(mainWindow_, COLOR_PAIR(16));
                mvwprintw(mainWindow_, row, screenCol, "%s",
                          line.raw.substr(offsetInLine, len).c_str());
            }
        }

        int screenCol = col + (tStart - adjStart);
        if (screenCol < termWidth_) {
            int len = tEnd - tStart;
            if (screenCol + len > termWidth_) {
                len = termWidth_ - screenCol;
            }
            int tokIdx = (int)i;
            bool isCurrent = isHighlighted
                && (size_t)tokIdx == currentTokenIndex_
                && (size_t)(adjStart + (screenCol - col)) >= token.start_pos;
            bool isHovered = (row == hoverRow_
                && screenCol <= hoverCol_
                && hoverCol_ < screenCol + (int)token.value.length());
            Token shifted;
            shifted.type = token.type;
            shifted.value = line.raw.substr(tStart, len);
            shifted.start_pos = tStart;
            shifted.end_pos = tStart + len;
            printToken(shifted, isCurrent, isHovered);
            mvwprintw(mainWindow_, row, screenCol, "%s", shifted.value.c_str());
        }

        lastEnd = (size_t)tEnd;
    }

    if ((int)lastEnd < adjEnd && (int)line.raw.length() > (int)lastEnd) {
        int screenCol = col + ((int)lastEnd - adjStart);
        if (screenCol < termWidth_) {
            int len = std::min(adjEnd, (int)line.raw.length()) - (int)lastEnd;
            if (screenCol + len > termWidth_) {
                len = termWidth_ - screenCol;
            }
            if (len > 0) {
                wattrset(mainWindow_, COLOR_PAIR(16));
                mvwprintw(mainWindow_, row, screenCol, "%s",
                          line.raw.substr(lastEnd, len).c_str());
            }
        }
    }

    // Nothing to fill here: the cursor line is no longer drawn in
    // full reverse video — only the token under the cursor is lit.
    // Leaving this empty on purpose so the rest of the cursor line
    // keeps its normal colouring.
}

void Viewer::printWrappedLine(const LogLine& line, size_t lineNum, int startRow, bool isHighlighted) {
    int contentWidth = termWidth_;
    int numWidth = 0;
    
    if (showLineNumbers_) {
        size_t totalLines = buffer_.getTotalLines();
        numWidth = std::to_string(totalLines).length() + 1;
        contentWidth -= numWidth;
    }
    
    int col = 0;
    int row = startRow;
    wattrset(mainWindow_, COLOR_PAIR(16));
    
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
    
    size_t lastEnd = 0;
    for (size_t i = 0; i < line.tokens.size() && row < termHeight_ - 2; i++) {
        const auto& token = line.tokens[i];

        if (token.start_pos > lastEnd) {
            std::string plainText = line.raw.substr(lastEnd, token.start_pos - lastEnd);
            wattrset(mainWindow_, COLOR_PAIR(16));
            size_t pos = 0;
            while (pos < plainText.length() && row < termHeight_ - 2) {
                int spaceLeft = contentWidth - (col - numWidth);
                if (spaceLeft <= 0) {
                    row++;
                    col = numWidth;
                    wattrset(mainWindow_, COLOR_PAIR(16));
                }
                size_t chunkLen = std::min((size_t)spaceLeft, plainText.length() - pos);
                mvwprintw(mainWindow_, row, col, "%s", plainText.substr(pos, chunkLen).c_str());
                col += chunkLen;
                pos += chunkLen;
            }
        }

        if (row >= termHeight_ - 2) break;

        bool isCurrentToken = isHighlighted && (i == currentTokenIndex_);
        bool isHovered = (row == hoverRow_ && col <= hoverCol_ && hoverCol_ < col + (int)token.value.length());
        printToken(token, isCurrentToken, isHovered);

        size_t pos = 0;
        const std::string& val = token.value;
        while (pos < val.length() && row < termHeight_ - 2) {
            int spaceLeft = contentWidth - (col - numWidth);
            if (spaceLeft <= 0) {
                row++;
                col = numWidth;
                wattrset(mainWindow_, COLOR_PAIR(16));
            }
            size_t chunkLen = std::min((size_t)spaceLeft, val.length() - pos);
            mvwprintw(mainWindow_, row, col, "%s", val.substr(pos, chunkLen).c_str());
            col += chunkLen;
            pos += chunkLen;
        }

        lastEnd = token.end_pos;
    }

    if (lastEnd < line.raw.length() && row < termHeight_ - 2) {
        std::string remaining = line.raw.substr(lastEnd);
        wattrset(mainWindow_, COLOR_PAIR(16));
        size_t pos = 0;
        while (pos < remaining.length() && row < termHeight_ - 2) {
            int spaceLeft = contentWidth - (col - numWidth);
            if (spaceLeft <= 0) {
                row++;
                col = numWidth;
                wattrset(mainWindow_, COLOR_PAIR(16));
            }
            size_t chunkLen = std::min((size_t)spaceLeft, remaining.length() - pos);
            mvwprintw(mainWindow_, row, col, "%s", remaining.substr(pos, chunkLen).c_str());
            col += chunkLen;
            pos += chunkLen;
        }
    }

    // Reset all attributes we may have set so the next line drawn
    // (possibly by another code path) doesn't inherit e.g. a leftover
    // A_BOLD or COLOR_PAIR from the last token — that's the colour
    // "bleed" across lines.
    wattrset(mainWindow_, COLOR_PAIR(16));
    wattroff(mainWindow_, A_BOLD);
    wattroff(mainWindow_, A_UNDERLINE);
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
        // Lit the whole token (A_REVERSE + its colour pair), not the
        // whole cursor line: the block the cursor moves over is the
        // selection the user tracks, so it is the only thing in
        // reverse video.
        wattron(mainWindow_, A_REVERSE);
        wattron(mainWindow_, A_UNDERLINE);
    } else if (isHovered) {
        wattron(mainWindow_, A_REVERSE);
    }

    if (token.type == TokenType::CONN_ID ||
        token.type == TokenType::OP_ID ||
        token.type == TokenType::THREAD_ID ||
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
    if (wrapLines_) {
        ensureScreenRowsCachedUpTo(cursorRow);
    }
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
    
    std::string help = "[F1-8] [Enter]Filter [Esc]Back [/]Search [#]Num [h/l]Scroll [q]Quit ";
    
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
    wrefresh(popupWindow_);
}

void Viewer::hidePopup() {
    showPopup_ = false;
    if (popupWindow_) {
        delwin(popupWindow_);
        popupWindow_ = nullptr;
    }
    fullRedraw();
}

void Viewer::hidePopupNoRedraw() {
    showPopup_ = false;
    if (popupWindow_) {
        delwin(popupWindow_);
        popupWindow_ = nullptr;
    }
}

void Viewer::handleInput() {
    int ch = wgetch(mainWindow_);
    
    if (ch == ERR) {
        // Idle tick (10 ms wgetch timeout): drive the wrap-compute
        // progress popup and let the worker make further progress.
        pumpWrapProgress();
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
            } else if (horizontalOffset_ > 0) {
                horizontalOffset_--;
                redrawLine(cursorRow_, true);
                wmove(mainWindow_, getScreenRowForCursorRow(cursorRow_), cursorCol_);
                wnoutrefresh(mainWindow_);
                doupdate();
            } else if (!filterStack_.empty()) {
                deactivateFilter();
            }
            if (cursorCol_ > 0 || (cursorCol_ == 0 && horizontalOffset_ == 0)) {
                auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_ + horizontalOffset_);
                if (tokenIdx.has_value()) {
                    currentTokenIndex_ = tokenIdx.value();
                }
                redrawLine(cursorRow_, true);
                wmove(mainWindow_, getScreenRowForCursorRow(cursorRow_), cursorCol_);
                wnoutrefresh(mainWindow_);
                doupdate();
            }
            break;
        case KEY_RIGHT:
        case 'l':
            if (cursorCol_ < termWidth_ - 1) {
                cursorCol_++;
            } else {
                horizontalOffset_++;
                redrawLine(cursorRow_, true);
                wmove(mainWindow_, getScreenRowForCursorRow(cursorRow_), cursorCol_);
                wnoutrefresh(mainWindow_);
                doupdate();
            }
            {
                auto tokenIdx = getTokenIndexAtPosition(cursorRow_, cursorCol_ + horizontalOffset_);
                if (tokenIdx.has_value()) {
                    currentTokenIndex_ = tokenIdx.value();
                }
                redrawLine(cursorRow_, true);
                wmove(mainWindow_, getScreenRowForCursorRow(cursorRow_), cursorCol_);
                wnoutrefresh(mainWindow_);
                doupdate();
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
            fullRedraw(false);
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
            // The single CONN filter is the most common click target.
            // findConnRange already collected every matching line index
            // while it scanned, so the view can be built from those
            // directly, skipping the second full pass over the range.
            connFastPath = filterStack_.empty() && !connMatches.empty();
            break;
        }
        case TokenType::DN_VALUE: {
            // A DN is usually shared by every line of one connection
            // (bind + the operations it runs), so showing "everything
            // with this dn" is the whole log again.  Show the
            // connection of the clicked line instead — one row is
            // enough to know which connection the user meant.
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
            // An op is meaningless without its conn — the same op
            // number can appear in unrelated connections.
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

    if (connFastPath) {
        // findConnRange already collected every matching index while
        // it scanned, so there is no second pass.  Mirror the state
        // buildFilteredIndices sets up so finishFilterUpdate's popup
        // acknowledgement is identical in both paths.
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
    if (cursorRow_ < 0
        || cursorRow_ >= (int)visibleIndices_.size()) {
        return;
    }
    auto line = buffer_.getLine(visibleIndices_[cursorRow_]);
    if (!line || !line->conn_id) return;

    // Push a plain conn= filter: no range.  Computing a range would
    // trigger a sequential scan of the file (which may be huge for a
    // long-lived connection) before the rebuild; the plain filter
    // reuses the cheap conn= substring candidate test in scanLines,
    // so the rebuild is fast and the "Filtering..." popup shows.
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
    
    // One Esc/Backspace clears the whole filter chain: the user is
    // usually not trying to unwind one auto-added conn= at a time,
    // they want the filtered view gone.
    filterStack_.clear();
    
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
            case TokenType::THREAD_ID:
                prefill = token->value;
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

    const size_t total = visibleIndices_.size();
    if (total == 0) return;

    showPopup("Searching... (Esc to cancel)", 0.0f);
    touchwin(mainWindow_);
    wnoutrefresh(mainWindow_);
    touchwin(filterWindow_);
    wnoutrefresh(filterWindow_);
    touchwin(statusWindow_);
    wnoutrefresh(statusWindow_);
    doupdate();

    nodelay(stdscr, TRUE);

    bool cancelled = false;
    auto lastUpdate = std::chrono::steady_clock::now();
    int lastPct = -1;

    for (size_t i = 0; i < total; i++) {
        if ((i & 0x3FF) == 0) {
            int ch = getch();
            if (ch == 27 || ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                cancelled = true;
                break;
            }
            if (ch == 'q' || ch == 'Q' || ch == 3 || ch == 28) {
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
                    drawPopup();
                }
                lastUpdate = now;
            }
        }
        std::optional<std::string> raw = buffer_.getRawLine(visibleIndices_[i]);
        if (raw && raw->find(query) != std::string::npos) {
            searchResults_.push_back(i);
        }
    }

    nodelay(stdscr, FALSE);
    timeout(10);

    if (cancelled) {
        hidePopup();
        return;
    }

    popupMessage_ = "Searching... done (" + std::to_string(searchResults_.size()) + ")";
    popupProgress_ = 1.0f;
    drawPopup();

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

    // Read in 4096-line blocks via one grouped disk read each.  Walking
    // a raw line one by one through getRawLine() reloads a fresh
    // 4096-line page from disk at every window boundary — quadratic
    // disk I/O on long ranges, freezing the UI for seconds.  Block
    // reads also let us poll for Esc between blocks so a long filter
    // stays cancellable with live progress.
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
            drawPopup();
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
                int ch = getch();
                if (ch == 27 || ch == KEY_BACKSPACE || ch == 127 || ch == 8 ||
                    ch == 'q' || ch == 'Q' || ch == 3 || ch == 28) {
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

    // The only correct bound for an interleaved connection is the
    // exact first/last line carrying the conn id.  Growing-window
    // guesses truncate the range when the connection has gaps wider
    // than the window, silently dropping lines from the filter.  So
    // scan sequentially; show progress and allow Esc to cancel.
    showPopup("Locating connection... (Esc to cancel)", 0.0f);
    touchwin(mainWindow_);
    wnoutrefresh(mainWindow_);
    touchwin(filterWindow_);
    wnoutrefresh(filterWindow_);
    touchwin(statusWindow_);
    wnoutrefresh(statusWindow_);
    doupdate();

    nodelay(stdscr, TRUE);
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
            drawPopup();
        }
    };

    connStart = fromLine;
    connEnd = fromLine;

    // Both directions are scanned in 4096-line blocks fetched with one
    // grouped disk read each (getRawLines).  Walking a raw line one by
    // one through getRawLine would reload a 4096-line page from disk
    // for every line once the page boundary is crossed — quadratic
    // disk I/O on multi-million-line logs, freezing the UI for
    // minutes.
    constexpr size_t kScanBlock = 4096;
    const size_t backSpan = fromLine + 1;

    auto collectMatch = [&](size_t idx) {
        if (outMatches) {
            outMatches->push_back(idx);
        }
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
                int ch = getch();
                if (ch == 27 || ch == KEY_BACKSPACE || ch == 127 || ch == 8 ||
                    ch == 'q' || ch == 'Q' || ch == 3 || ch == 28) {
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
                    int ch = getch();
                    if (ch == 27 || ch == KEY_BACKSPACE || ch == 127 || ch == 8 ||
                        ch == 'q' || ch == 'Q' || ch == 3 || ch == 28) {
                        cancelled = true;
                        break;
                    }
                    reportProgress(backSpan + (idx - fromLine), total);
                }
            }
            blockHi += kScanBlock;
        }
    }

    // The backward pass walks from `fromLine` to 0, so its matches are
    // in descending order; the forward pass is ascending.  Sort so the
    // caller gets a strictly increasing index list for the filter view.
    if (outMatches) {
        std::sort(outMatches->begin(), outMatches->end());
        outMatches->erase(std::unique(outMatches->begin(), outMatches->end()),
                          outMatches->end());
    }

    nodelay(stdscr, FALSE);
    timeout(10);

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
    // keys without slowing the scan with 10ms timeouts.
    nodelay(stdscr, TRUE);

    bool cancelled = false;
    newVisibleIndices = scanLines(scanStart, scanEnd, cancelled);

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
    finishFilterUpdate();
}

void Viewer::finishFilterUpdate() {
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
