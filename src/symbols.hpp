// symbols.hpp
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
// Layer 5 of the TUI-ANSI-METHODOLOGY: Unicode symbols (box drawing,
// arrows, meters, graph families).  In tty_mode the box drawing and
// meters degrade to ASCII-safe equivalents (set by Theme::setTtyMode).

#pragma once

#include <string>

namespace Symbols {

// Box drawing
inline const std::string h_line        = "\u2500"; // ─
inline const std::string v_line        = "\u2502"; // │
inline const std::string dotted_v_line = "\u254e"; // ╎
inline const std::string left_up       = "\u250c"; // ┌
inline const std::string right_up      = "\u2510"; // ┐
inline const std::string left_down     = "\u2514"; // └
inline const std::string right_down    = "\u2518"; // ┘
inline const std::string round_left_up    = "\u256d"; // ╭
inline const std::string round_right_up   = "\u256e"; // ╮
inline const std::string round_left_down  = "\u256f"; // ╰
inline const std::string round_right_down = "\u2570"; // ╯
inline const std::string title_left     = "\u2510"; // ┐ (flipped)
inline const std::string title_right    = "\u250c"; // ┌
inline const std::string title_left_down  = "\u2518"; // ┘
inline const std::string title_right_down = "\u2514"; // └
inline const std::string div_right = "\u2524"; // ┤
inline const std::string div_left  = "\u251c"; // ├
inline const std::string div_up    = "\u252c"; // ┬
inline const std::string div_down  = "\u2534"; // ┴

// Arrows / misc
inline const std::string up    = "\u2191"; // ↑
inline const std::string down  = "\u2193"; // ↓
inline const std::string left  = "\u2190"; // ←
inline const std::string right = "\u2192"; // →
inline const std::string enter = "\u21b5"; // ↵
inline const std::string meter = "\u25a0"; // ■

// Superscript digits for box numbering
inline const char* superscript[] = {
    "\u2070", "\u00b9", "\u00b2", "\u00b3", "\u2074",
    "\u2075", "\u2076", "\u2077", "\u2078", "\u2079",
};

// ASCII-safe fallbacks, switched in by Theme::setTtyMode(true).
inline std::string h_line_a = "-";
inline std::string v_line_a = "|";
inline std::string left_up_a = "+";
inline std::string right_up_a = "+";
inline std::string left_down_a = "+";
inline std::string right_down_a = "+";
inline std::string meter_a = "#";

// Return the active (possibly tty-degraded) glyphs.
inline const std::string& H() { return h_line_a.empty() ? h_line : h_line_a; }
inline const std::string& V() { return v_line_a.empty() ? v_line : v_line_a; }
inline const std::string& LU() { return left_up_a.empty() ? left_up : left_up_a; }
inline const std::string& RU() { return right_up_a.empty() ? right_up : right_up_a; }
inline const std::string& LD() { return left_down_a.empty() ? left_down : left_down_a; }
inline const std::string& RD() { return right_down_a.empty() ? right_down : right_down_a; }
inline const std::string& M() { return meter_a.empty() ? meter : meter_a; }

} // namespace Symbols