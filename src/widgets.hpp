// widgets.hpp
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
// Layer 6 of the TUI-ANSI-METHODOLOGY: reusable widgets with caches.
// Every widget computes once, then replays the string.

#pragma once

#include <string>
#include <array>
#include <vector>

#include "ansi.hpp"
#include "symbols.hpp"
#include "theme.hpp"

namespace Draw {

// createBox: builds one string containing the border (lines + corners +
// optional title) and positions the cursor just inside the box.
//   x, y       : 1-based absolute position of the top-left corner
//   width      : total box width in columns (>= 3)
//   height     : total box height in rows (>= 3)
//   line_color : escape code for the border (empty = theme divLine)
//   fill       : true to fill the interior with spaces (for overlay popups)
//   title      : optional title text drawn in the top border
// Returns an empty string when the box does not fit on the terminal.
std::string createBox(int x, int y, int width, int height,
                      const std::string& line_color = "",
                      bool fill = false,
                      const std::string& title = "");

// Meter: percentage gauge (0-100) with a per-value cache — one string per
// possible value, replayed on subsequent calls.
class Meter {
public:
    Meter(int width = 10, const std::string& gradientName = "")
        : width_(width) { (void)gradientName; }
    void setWidth(int width);
    int width() const { return width_; }
    // Renders the gauge at `value` (clamped 0..100) using the theme
    // gradient; cached per value.
    std::string operator()(int value);
private:
    int width_;
    std::array<std::string, 101> cache_;
};

// ProgressBar: used for popup progress.  Renders "[=====>    ] 45%".
// No cache needed (values change continuously), but it keeps the popup
// rendering in one place.
std::string progressBar(int barWidth, float progress);

} // namespace Draw