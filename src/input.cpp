// input.cpp
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

#include "input.hpp"

#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <vector>

namespace Input {

namespace {

// Read up to one byte with an optional timeout.  Returns -1 on timeout,
// the byte otherwise (0 is a valid NUL).
int readByte(int timeoutMs) {
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, timeoutMs);
    if (ret <= 0) return -1;
    unsigned char c = 0;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n != 1) return -1;
    return (int)c;
}

// Timeout used between bytes of an escape sequence.  Some terminals
// send the bytes in one write, but a slow one may split them; give it
// a small window so we don't misread ESC as a lone escape.
constexpr int kEscapeFollowMs = 10;

// Parse "ESC [ ... <final>" — CSI sequence.  Returns true and fills the
// event when recognized.
bool parseCsi(Event& ev) {
    // We have consumed ESC '['.  Collect params until the final byte.
    std::vector<int> params;
    int num = -1;
    int final = 0;
    while (true) {
        int c = readByte(kEscapeFollowMs);
        if (c < 0) return false;             // truncated
        if (c >= '0' && c <= '9') {
            if (num < 0) num = 0;
            num = num * 10 + (c - '0');
        } else if (c == ';') {
            params.push_back(num < 0 ? 0 : num);
            num = -1;
        } else if (c == '<') {
            // SGR mouse marker; params accumulate below with the '<'
            // consumed first.  We treat '<' as "start of SGR params".
            continue;
        } else {
            params.push_back(num < 0 ? 0 : num);
            num = -1;
            final = c;
            break;
        }
    }

    // SGR mouse: ESC [ < b ; x ; y M|m
    if (final == 'M' || final == 'm') {
        // We need at least the button + x + y.
        if (params.size() < 3) return false;
        ev.key = Key::MOUSE;
        int b = params[0];
        int x = params[1];
        int y = params[2];
        bool motion = (b & MB_MOTION) != 0;
        int btn = b & 3;
        if ((b & 65) == 65)      btn = MB_WHEEL_DOWN;
        else if ((b & 64) != 0)  btn = MB_WHEEL_UP;
        if (motion && btn <= 2)  btn |= MB_MOTION;
        ev.mouseButton = btn;
        ev.mouseX = x;
        ev.mouseY = y;
        ev.mousePressed = (final == 'M');
        return true;
    }

    // Keyboard CSI sequences.
    int p0 = params.empty() ? 0 : params[0];
    switch (final) {
        case 'A': ev.key = Key::UP; return true;
        case 'B': ev.key = Key::DOWN; return true;
        case 'C': ev.key = Key::RIGHT; return true;
        case 'D': ev.key = Key::LEFT; return true;
        case 'H': ev.key = Key::HOME; return true;
        case 'F': ev.key = Key::END; return true;
        case '~':
            switch (p0) {
                case 1: ev.key = Key::HOME; return true;
                case 4: ev.key = Key::END; return true;
                case 5: ev.key = Key::PGUP; return true;
                case 6: ev.key = Key::PGDN; return true;
                case 11: ev.key = Key::F1; return true;
                case 12: ev.key = Key::F2; return true;
                case 13: ev.key = Key::F3; return true;
                case 14: ev.key = Key::F4; return true;
                case 15: ev.key = Key::F5; return true;
                case 17: ev.key = Key::F6; return true;
                case 18: ev.key = Key::F7; return true;
                case 19: ev.key = Key::F8; return true;
                default: ev.key = Key::UNKNOWN; return true;
            }
        default:
            ev.key = Key::UNKNOWN;
            return true;
    }
}

// Parse "ESC O <x>" — SS3 sequences (F1-F4, Home/End on some terminals).
bool parseSs3(Event& ev) {
    int c = readByte(kEscapeFollowMs);
    if (c < 0) return false;
    switch (c) {
        case 'P': ev.key = Key::F1; return true;
        case 'Q': ev.key = Key::F2; return true;
        case 'R': ev.key = Key::F3; return true;
        case 'S': ev.key = Key::F4; return true;
        case 'H': ev.key = Key::HOME; return true;
        case 'F': ev.key = Key::END; return true;
        default: ev.key = Key::UNKNOWN; return true;
    }
}

} // namespace

void flushPending() {
    // Drain everything currently available without waiting.
    while (readByte(0) >= 0) {}
}

Event pollKey(int timeoutMs) {
    Event ev;
    int c = readByte(timeoutMs);
    if (c < 0) {
        ev.key = Key::NONE;
        return ev;
    }

    if (c == 0x1b) {
        // Escape — could be ESC alone, or the start of a sequence.
        int c2 = readByte(kEscapeFollowMs);
        if (c2 < 0) {
            ev.key = Key::ESC;
            return ev;
        }
        if (c2 == '[') {
            parseCsi(ev);
            if (ev.key == Key::UNKNOWN) ev.key = Key::ESC;
            return ev;
        }
        if (c2 == 'O') {
            parseSs3(ev);
            if (ev.key == Key::UNKNOWN) ev.key = Key::ESC;
            return ev;
        }
        // ESC followed by a printable char (e.g. Alt-x): treat the
        // printable char as the key; the ESC modifier is dropped.
        ev.key = Key::CHAR;
        ev.ch = (unsigned char)c2;
        return ev;
    }

    ev.key = Key::CHAR;
    ev.ch = (unsigned char)c;
    switch (c) {
        case '\n': case '\r': ev.key = Key::ENTER; ev.ch = '\n'; break;
        case 0x7f: case 0x08: ev.key = Key::BACKSPACE; ev.ch = 0x7f; break;
        case '\t': ev.key = Key::TAB; break;
        case 0x03: ev.key = Key::CTRL_C; break;
        case 0x04: ev.key = Key::CTRL_D; break;
        case 0x0c: ev.key = Key::CTRL_L; break;
        default: break;
    }
    return ev;
}

} // namespace Input