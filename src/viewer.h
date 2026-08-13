// viewer.h
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
// The ncurses version is replaced by the TUI-ANSI-METHODOLOGY:
//   - one frame = one big string, written with a single flush
//   - static borders cached, only values change every tick
//   - every color through Theme::c(), every move through Mv::to()
//   - SIGWINCH triggers a geometry refresh + full redraw
//   - raw input parsing replaces wgetch (see input.hpp)

#pragma once

#include "ansi.hpp"
#include "theme.hpp"
#include "input.hpp"
#include "log_buffer.h"
#include "filter.h"
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>

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

    void fullRedraw();
    void renderFrame(std::string& out);
    void renderLine(std::string& out, int screenRow, const LogLine& line,
                    size_t lineNum, bool isHighlighted);
    void renderFilterBar(std::string& out);
    void renderStatusBar(std::string& out);
    void renderPopup(std::string& out);
    void flushFrame(const std::string& out);

    // Incremental: redraw a single visible row (cursor moves).
    void redrawLine(int cursorRow, bool isHighlighted);
    void moveCursorToScreen();

    void handleInput();
    void handleMouseEvent(const Input::Event& ev);
    void handleFollowMode();

    std::optional<Token> getTokenAtCursor();
    std::optional<size_t> getTokenIndexAtPosition(int row, int col);

    void activateFilter();
    void activateFilterAtPosition(int row, int col);
    void deactivateFilter();
    void addConnFilterForCurrentLine();

    void startSearch();
    void readCommandMode(char prompt);
    void performSearch(const std::string& query);
    void nextSearchResult();
    void prevSearchResult();

    void buildFilteredIndices();
    void finishFilterUpdate();
    bool linePassesFilters(size_t lineIndex);
    std::vector<size_t> scanLines(size_t scanStart, size_t scanEnd, bool& cancelled);
    bool findConnRange(size_t fromLine, const std::string& connId,
                       size_t& connStart, size_t& connEnd,
                       std::vector<size_t>* outMatches = nullptr);

    void showPopup(const std::string& message, float progress);
    void hidePopup();
    void hidePopupNoRedraw();

    int getColorForToken(TokenType type);
    void appendToken(std::string& out, const Token& token,
                     bool isCurrentToken, bool isHovered);
    void appendPlain(std::string& out, const std::string& text);

    void moveCursorUp();
    void moveCursorDown();
    void pageUp();
    void pageDown();
    void goToTop();
    void goToBottom();
    void moveToLine(size_t lineNum);
    void moveCursorToLineStart();
    void moveCursorToLineEnd();
    int getScreenRowForCursorRow(int cursorRow) const;
    int getCursorRowForScreenRow(int screenRow) const;

    std::string filename_;
    bool followMode_;
    LogFormat logFormat_ = LogFormat::AUTO;
    LogBuffer buffer_;
    std::vector<size_t> visibleIndices_;
    FilterStack filterStack_;

    int scrollOffset_ = 0;
    int cursorRow_ = 0;
    int cursorCol_ = 0;
    int termWidth_ = 80;
    int termHeight_ = 24;
    std::atomic<bool> running_{true};

    std::string searchQuery_;
    std::vector<size_t> searchResults_;
    size_t currentSearchResult_ = 0;

    std::chrono::steady_clock::time_point lastClickTime_{};
    int lastClickRow_ = -1;
    int lastClickCol_ = -1;

    bool showLineNumbers_ = false;
    size_t currentTokenIndex_ = 0;

    int followFd_ = -1;
    int followWatchFd_ = -1;
    bool autoScroll_ = false;
    std::chrono::steady_clock::time_point lastFollowPoll_{};
    std::chrono::steady_clock::time_point filterStart_{};

    int horizontalOffset_ = 0;

    int hoverRow_ = -1;
    int hoverCol_ = -1;
    bool mouseActive_ = false;

    bool showPopup_ = false;
    std::string popupMessage_;
    float popupProgress_ = 0.0f;
    bool filtering_ = false;

    int currentSchema_ = 0;
    bool autoColor_ = false;
    static const char* SCHEMA_NAMES[8];
};