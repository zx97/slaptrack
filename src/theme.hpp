// theme.hpp
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
// Layer 4 of the TUI-ANSI-METHODOLOGY: colors and themes.
// Every color goes through Theme::c() / Theme::g() — never raw ANSI in
// business code.  The token palette mirrors the ncurses version's 20
// COLOR_PAIR indices so the "Token colour mapping" stays identical.

#pragma once

#include <string>
#include <array>

namespace Theme {

// Token color indices (same layout as the ncurses version).
enum TokenColor : int {
    TOK_TIMESTAMP = 0,
    TOK_CONN_ID = 1,
    TOK_THREAD_OP_ID = 2,
    TOK_DN_VALUE = 3,
    TOK_FILTER_VALUE = 4,
    TOK_IP_KEYWORD = 5,
    TOK_ERROR_CODE = 6,
    TOK_FD_NUM = 7,
    TOK_TAG = 8,
    TOK_ETIME_VAL = 9,
    TOK_NENTRIES = 10,
    TOK_QTIME_VAL = 11,
    TOK_SCOPE = 12,
    TOK_DEREF = 13,
    TOK_LINENUM = 14,
    TOK_BASE_TEXT = 15,
    TOK_POPUP = 16,
    TOK_ATTR = 17,
    TOK_ATTR_LIST = 18,
    TOK_BASE = 19,
    TOK_COUNT = 20,
};

// Schema names (F1-F8), same as the ncurses version.
extern const char* SCHEMA_NAMES[8];

// Select schema 0..7 (clamped).
void setSchema(int schema);

// True when the terminal reports < 8 colors or TERM=dumb.
void detectColorSupport();

// tty_mode: force monochrome schema + ASCII-safe symbols.
void setTtyMode(bool tty);
bool ttyMode();

// Escape code for a token color index (current schema).
// Returns an empty string for the base/default color.
const std::string& c(int tokenColorIndex);

// Named convenience helpers for the current schema's core colors.
const std::string& mainFg();
const std::string& selectedFg();
const std::string& selectedBg();
const std::string& title();
const std::string& divLine();
const std::string& popupBg();
const std::string& searchBg();

// Gradient helpers for progress bars (0.0..1.0).  Returns an SGR escape
// code interpolating through a green→yellow→red ramp in truecolor when
// supported, otherwise a 3-step plateau of bright colors.
const std::string& gradient(float pct);

// Reset to default foreground/background (restores theme baseline).
const std::string& reset();

// Truecolor support flag (COLORTERM=truecolor / 24bit).
bool truecolor();

// One-time init (gradients + cache).  Call after detectColorSupport().
void init();

// Convert "#rrggbb" or "#gg" (grey) to an ANSI escape code.
// depth "fg" → 38;2;r;g;b, "bg" → 48;2;r;g;b.  Falls back to 256-color
// when t_to_256=true and truecolor is unavailable.
std::string hexToColor(const std::string& hexa, const std::string& depth = "fg");

} // namespace Theme