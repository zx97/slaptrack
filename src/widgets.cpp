// widgets.cpp
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

#include "widgets.hpp"

#include <algorithm>
#include <cmath>
#include "utf8.hpp"

namespace Draw {

std::string createBox(int x, int y, int width, int height,
                      const std::string& line_color,
                      bool fill,
                      const std::string& title) {
    if (width < 3 || height < 3) return {};
    if (x < 1 || y < 1) return {};
    if (x + width - 1 > Term::width || y + height - 1 > Term::height) return {};

    const std::string& lc = line_color.empty() ? Theme::divLine() : line_color;
    const bool tty = Theme::ttyMode();

    const std::string& h_line = tty ? Symbols::H() : Symbols::h_line;
    const std::string& v_line = tty ? Symbols::V() : Symbols::v_line;
    const std::string& lu = tty ? Symbols::LU() : Symbols::round_left_up;
    const std::string& ru = tty ? Symbols::RU() : Symbols::round_right_up;
    const std::string& ld = tty ? Symbols::LD() : Symbols::round_left_down;
    const std::string& rd = tty ? Symbols::RD() : Symbols::round_right_down;

    std::string out;
    out.reserve((size_t)width * (size_t)height + 32);
    out += Fx::reset + lc;

    // Top border with optional title.
    out += Mv::to(y, x) + lu + h_line * (width - 2) + ru;

    // Title in the top border: "╭─ title ─────╮".
    if (!title.empty()) {
        int titleLen = (int)Tools::wide_ulen(title);
        int avail = width - 4;
        if (titleLen > avail) {
            std::string t = Tools::uresize(title, (size_t)avail, true);
            titleLen = (int)Tools::wide_ulen(t);
        }
        int tx = x + 2;
        out += Mv::to(y, tx) + Fx::b + Theme::title() + title
             + Fx::ub + lc + h_line * (width - 2 - 2 - titleLen);
    }

    // Vertical borders + optional fill.
    for (int r = 1; r < height - 1; r++) {
        out += Mv::to(y + r, x) + v_line;
        if (fill) {
            out += std::string((size_t)(width - 2), ' ');
        }
        out += v_line;
    }

    // Bottom border.
    out += Mv::to(y + height - 1, x) + ld + h_line * (width - 2) + rd;

    // Position just inside the box (below the top border).
    out += Mv::to(y + 1, x + 1) + Fx::reset;
    return out;
}

void Meter::setWidth(int width) {
    if (width == width_) return;
    width_ = width;
    for (auto& s : cache_) s.clear();
}

std::string Meter::operator()(int value) {
    if (width_ < 1) return {};
    value = std::max(0, std::min(100, value));
    if (!cache_[value].empty()) return cache_[value];

    std::string out;
    out.reserve((size_t)width_ * 4 + 8);
    for (int i = 1; i <= width_; i++) {
        int y = (int)std::lround((double)i * 100.0 / (double)width_);
        if (value >= y) {
            out += Theme::gradient((float)y / 100.0f) + Symbols::M();
        } else {
            out += Theme::mainFg() + Symbols::M() * (width_ + 1 - i);
            break;
        }
    }
    out += Fx::reset;
    cache_[value] = out;
    return out;
}

std::string progressBar(int barWidth, float progress) {
    if (barWidth < 1) return {};
    progress = std::max(0.0f, std::min(1.0f, progress));
    int pos = (int)((float)barWidth * progress);
    std::string out = "[";
    for (int i = 0; i < barWidth; i++) {
        if (i < pos)      out += Theme::gradient((float)i / (float)barWidth) + "=" + Fx::reset;
        else if (i == pos) out += ">";
        else               out += " ";
    }
    out += "] ";
    out += std::to_string((int)(progress * 100.0f)) + "%";
    return out;
}

} // namespace Draw