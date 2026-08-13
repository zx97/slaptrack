// ansi.hpp
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
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
// AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// Layer 1-4 of the TUI-ANSI-METHODOLOGY:
//   Layer 1: SGR style codes          (namespace Fx)
//   Layer 2: Cursor movement          (namespace Mv)
//   Layer 3: Terminal manipulation    (namespace Term)
//   Layer 4: Colors and themes        (namespace Theme, implemented in theme.hpp)

#pragma once

#include <string>
#include <termios.h>
#include <unistd.h>
#include <cstdio>
#include <iostream>
#include <sys/ioctl.h>

namespace Fx {

// CSI sequence start
inline const std::string e = "\x1b[";

// Style codes (SGR) — close every style you open.
inline const std::string b   = "\x1b[1m";   // Bold
inline const std::string ub  = "\x1b[22m";  // Bold off
inline const std::string d   = "\x1b[2m";   // Dark / faint
inline const std::string ud  = "\x1b[22m";  // Dark off
inline const std::string i   = "\x1b[3m";   // Italic
inline const std::string ui  = "\x1b[23m";  // Italic off
inline const std::string ul  = "\x1b[4m";   // Underline
inline const std::string uul = "\x1b[24m";  // Underline off
inline const std::string bl  = "\x1b[5m";   // Blink
inline const std::string ubl = "\x1b[25m";  // Blink off
inline const std::string s   = "\x1b[9m";   // Strikethrough
inline const std::string us  = "\x1b[29m";  // Strikethrough off
inline const std::string rev = "\x1b[7m";   // Reverse video
inline const std::string urev = "\x1b[27m"; // Reverse off

// Full reset
inline const std::string reset_base = "\x1b[0m";
// Reset + restore theme colors (filled by Term::init / Theme).  Kept as a
// mutable string so tty/lowcolor modes can adjust the default fg/bg.
inline std::string reset = "\x1b[0m";

} // namespace Fx

namespace Mv {

// Absolute move: line (1-based), column (1-based)
inline std::string to(int line, int col) {
    return "\x1b[" + std::to_string(line) + ";" + std::to_string(col) + "f";
}

// Relative moves
inline std::string r(int x) { return "\x1b[" + std::to_string(x) + "C"; }
inline std::string l(int x) { return "\x1b[" + std::to_string(x) + "D"; }
inline std::string u(int x) { return "\x1b[" + std::to_string(x) + "A"; }
inline std::string d(int x) { return "\x1b[" + std::to_string(x) + "B"; }

// Save / restore position
inline const std::string save    = "\x1b[s";
inline const std::string restore = "\x1b[u";

} // namespace Mv

namespace Term {

// Alternate / normal screen
inline const std::string alt_screen    = "\x1b[?1049h";
inline const std::string normal_screen = "\x1b[?1049l";

// Cursor visibility
inline const std::string hide_cursor = "\x1b[?25l";
inline const std::string show_cursor = "\x1b[?25h";

// Mouse reporting: SGR extended mode (1006) + button events (1000).
//   * 1000 : button press/release
//   * 1002 : button drag (motion while pressed)
//   * 1003 : any motion (used for hover)
//   * 1006 : SGR extended coordinates (1-based, better than legacy X10)
inline const std::string mouse_on  = "\x1b[?1000h\x1b[?1002h\x1b[?1006h";
inline const std::string mouse_off = "\x1b[?1000l\x1b[?1002l\x1b[?1006l";
// Full motion reporting (hover) — enabled lazily, disabled at exit.
inline const std::string mouse_direct_on  = "\x1b[?1003h";
inline const std::string mouse_direct_off = "\x1b[?1003l";

// Synchronized output (Kitty protocol, reduces flicker).  Wrapped around
// every frame flush.  Harmless on terminals that ignore it.
inline const std::string sync_start = "\x1b[?2026h";
inline const std::string sync_end   = "\x1b[?2026l";

// Clear screen + home cursor
inline const std::string clear = "\x1b[2J\x1b[H";
// Clear to end of line / start of line / whole line
inline const std::string el = "\x1b[K";   // erase to end of line
inline const std::string elb = "\x1b[1K"; // erase to start of line
inline const std::string ell = "\x1b[2K"; // erase entire line

// Window title (nice touch)
inline std::string title(const std::string& t) {
    return "\x1b]0;" + t + "\x1b\\";
}

// Terminal geometry, refreshed by refresh().  0,0 when unavailable.
inline int width = 80;
inline int height = 24;

// Raw-mode termios saved on init, restored on restore().
inline struct termios initial_termios;
inline bool have_initial_termios = false;

// Refresh geometry via ioctl(TIOCGWINSZ).
inline void refresh() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        width = ws.ws_col;
        height = ws.ws_row;
    }
}

// Init raw mode (no echo, no line buffering) + save termios.
inline void init() {
    refresh();
    if (tcgetattr(STDIN_FILENO, &initial_termios) == 0) {
        have_initial_termios = true;
    }
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) == 0) {
        t.c_lflag &= ~(ICANON | ECHO | ISIG);
        t.c_iflag &= ~(IXON | ICRNL);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
    std::cout.sync_with_stdio(false);
    std::cout.tie(nullptr);
}

// Restore termios + normal screen.  Call at exit, including crash handlers.
inline void restore() {
    if (have_initial_termios) {
        tcsetattr(STDIN_FILENO, TCSANOW, &initial_termios);
        have_initial_termios = false;
    }
    std::cout << normal_screen << show_cursor << mouse_off
              << Fx::reset_base << "\x1b[?2026l" << std::flush;
}

// Enter the alternate screen for a full-screen TUI.
inline void enterFullscreen() {
    std::cout << alt_screen << hide_cursor << mouse_on << Fx::reset_base << std::flush;
}

// Exit the alternate screen (pair of enterFullscreen).
inline void leaveFullscreen() {
    std::cout << Fx::reset_base << mouse_off << show_cursor << normal_screen << std::flush;
}

} // namespace Term

// String multiplication: `str * n` repeats the string n times.
inline std::string operator*(const std::string& s, int n) {
    if (n <= 0) return {};
    std::string out;
    out.reserve(s.size() * (size_t)n);
    for (int i = 0; i < n; i++) out += s;
    return out;
}