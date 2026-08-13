// input.hpp
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
// Replaces ncurses wgetch(): reads raw bytes from stdin and decodes
// keyboard escape sequences (arrows, pgup/pgdn, home/end, F1-F8) plus
// SGR mouse reports (ESC [ < b ; x ; y M/m).

#pragma once

#include <string>
#include <cstdint>

namespace Input {

// Key codes returned by pollKey().  CHAR means a printable byte was read
// (stored in ch), MOUSE means a mouse report (stored in mouse*).
enum class Key {
    NONE,       // timeout / no data
    CHAR,       // printable character in ch
    UP, DOWN, LEFT, RIGHT,
    PGUP, PGDN,
    HOME, END,
    F1, F2, F3, F4, F5, F6, F7, F8,
    ENTER, ESC, BACKSPACE, TAB,
    CTRL_C, CTRL_D, CTRL_L,
    MOUSE,      // mouse event in mouseButton/mouseX/mouseY
    UNKNOWN,    // unrecognized escape sequence (swallowed)
};

// Mouse button bit flags (SGR decoding).
enum MouseButton : int {
    MB_LEFT   = 0,
    MB_MIDDLE = 1,
    MB_RIGHT  = 2,
    MB_RELEASE = 3,
    MB_WHEEL_UP   = 64,
    MB_WHEEL_DOWN = 65,
    MB_MOTION     = 32,
};

struct Event {
    Key key = Key::NONE;
    unsigned char ch = 0;
    // Mouse:
    int mouseButton = 0;   // decoded button (0-2, 64, 65, +MB_MOTION)
    int mouseX = 0;        // 1-based column
    int mouseY = 0;        // 1-based row
    bool mousePressed = false; // true = press 'M', false = release 'm'
};

// Read one key event.  `timeoutMs` is the poll timeout (0 = non-blocking,
// negative = block until data).  Returns NONE on timeout.
Event pollKey(int timeoutMs = 10);

// Flush any pending escape-sequence bytes (used before reading a line).
void flushPending();

} // namespace Input