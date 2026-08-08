// viewer.h

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

#pragma once

#include "log_buffer.h"
#include "filter.h"
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>
#include <ncurses.h>

class Viewer {
public:
    Viewer(const std::string& filename, bool followMode = false,
           LogFormat logFormat = LogFormat::AUTO);
    ~Viewer();
    
    void run();
    void stop();
    
private:
    void initTerminal();
    void restoreTerminal();
    void setupColors();
    void setupSchema(int schema);
    
    void fullRedraw(bool recomputeRows = true);
    void drawContent();
    void drawFilterBar();
    void drawStatusBar();
    void drawLine(int screenRow, const LogLine& line, size_t lineNum, bool isHighlighted);
    void redrawLine(int cursorRow, bool isHighlighted);
    
    void handleInput();
    void handleMouseEvent();
    void handleFollowMode();
    void handleCommandMode();
    
    void toggleWrap();
    void ensureScreenRowsCachedUpTo(size_t);
    // Background wrap-row computation (see viewer.cpp for design).
    void startWrapCompute(int contentWidth);
    void stopWrapWorker();
    void wrapRunOneJob();
    void pumpWrapProgress();
    int getContentWidthForWrap() const;
    void computeWrapRowsSync(size_t start, size_t end, int contentWidth,
                             std::vector<int>& out);
    // Thread-safe accessor; returns 1 if the row hasn't been computed yet
    // (matches the legacy fallback so the UI keeps drawing before the
    // background worker fills the prefix).
    int wrapRowAt(size_t i) const;
    
    void moveCursorUp();
    void moveCursorDown();
    void pageUp();
    void pageDown();
    void goToTop();
    void goToBottom();
    
    void moveToLine(size_t lineNum);
    void moveCursorToLineStart();
    void moveCursorToLineEnd();
    
    std::optional<Token> getTokenAtCursor();
    std::optional<size_t> getTokenIndexAtPosition(int row, int col);
    
    void activateFilter();
    void activateFilterAtPosition(int row, int col);
    void deactivateFilter();
    
    void startSearch();
    void performSearch(const std::string& query);
    void nextSearchResult();
    void prevSearchResult();
    
    void buildFilteredIndices();
    bool linePassesFilters(size_t lineIndex);
    std::vector<size_t> scanLines(size_t scanStart, size_t scanEnd, bool& cancelled);
    size_t findConnectionStart(size_t fromLine, const std::string& connId);
    size_t findConnectionEnd(size_t fromLine, const std::string& connId);
    
    void showPopup(const std::string& message, float progress);
    void hidePopup();
    void hidePopupNoRedraw();
    
    int getColorForToken(TokenType type);
    void printToken(const Token& token, bool isCurrentToken, bool isHovered);
    void printWrappedLine(const LogLine& line, size_t lineNum, int startRow, bool isHighlighted);
    void printTruncatedLine(const LogLine& line, size_t lineNum, int row, bool isHighlighted);
    
    int calculateLineScreenRows(const LogLine& line);
    void recalculateScreenRows();
    int getScreenRowForCursorRow(int cursorRow);
    int getCursorRowForScreenRow(int screenRow);
    
    std::string filename_;
    bool followMode_;
    LogFormat logFormat_ = LogFormat::AUTO;
    LogBuffer buffer_;
    std::vector<size_t> visibleIndices_;
    FilterStack filterStack_;
    
    WINDOW* mainWindow_;
    WINDOW* filterWindow_;
    WINDOW* statusWindow_;
    WINDOW* popupWindow_;
    
    int scrollOffset_;
    int cursorRow_;
    int cursorCol_;
    int termWidth_;
    int termHeight_;
    std::atomic<bool> running_;
    
    std::string searchQuery_;
    std::vector<size_t> searchResults_;
    size_t currentSearchResult_;
    
    std::chrono::steady_clock::time_point lastClickTime_;
    int lastClickRow_;
    int lastClickCol_;
    
    bool showLineNumbers_;
    size_t currentTokenIndex_;
    
    int followFd_;
    int followWatchFd_;
    bool autoScroll_;
    std::chrono::steady_clock::time_point lastFollowPoll_;
    std::chrono::steady_clock::time_point filterStart_;
    
    bool wrapLines_;
    std::vector<int> lineScreenRows_;

    // ---- Background wrap-row computation ----
    // DISABLED: wrap mode was replaced by horizontal scroll (toggleHorizontalScroll).
    // The members and worker thread machinery below are kept around in case we
    // want to bring wrap back; startWrapCompute() is now a no-op so no thread
    // is ever spawned.  See viewer.cpp for the disabled-but-preserved code.
    //
    // Worker is spawned by startWrapCompute() and runs to completion.
    // The main thread never joins it at runtime (would block the UI on
    // disk reads); stopWrapWorker() only sets wrapStopRequested_ and the
    // worker exits at the next batch boundary.  ~Viewer() joins.
    std::thread wrapWorker_;
    std::atomic<bool> wrapStopRequested_{false};
    mutable std::mutex wrapMutex_;
    int wrapWidth_ = 0;
    std::vector<size_t> wrapSnapshot_;
    bool wrapDone_ = true;
    int wrapPopupPct_ = -1;

    // ---- Horizontal scroll (replaces wrap mode) ----
    // Always-on: the cursor keys (Left/Right and h/l) move the cursor
    // within the line; if the cursor would go off-screen, the line
    // content shifts so the cursor stays visible.  horizontalOffset_
    // is how many columns the line content is scrolled to the right.
    int horizontalOffset_ = 0;
    
    int hoverRow_;
    int hoverCol_;
    bool mouseActive_;
    std::chrono::steady_clock::time_point lastPumpRecovery_ = {};
    
    bool showPopup_;
    std::string popupMessage_;
    float popupProgress_;
    bool filtering_;
    
    void drawPopup();

    int currentSchema_;
    bool autoColor_;
    static const char* SCHEMA_NAMES[8];
};
