// utf8.cpp
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

#include "utf8.hpp"

#include <cwctype>
#include <locale>
#include <cstdint>

namespace Tools {

namespace {

// Decode the next UTF-8 codepoint at str[pos]; advances pos past it.
// Returns 0 (and does not advance) on invalid input.
uint32_t decodeNext(const std::string& str, size_t& pos) {
    const unsigned char c = (unsigned char)str[pos];
    if (c < 0x80) {
        pos++;
        return c;
    }
    size_t n = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0)      { n = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
    else return 0;
    if (pos + n > str.size()) return 0;
    for (size_t i = 1; i < n; i++) {
        const unsigned char cc = (unsigned char)str[pos + i];
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    pos += n;
    return cp;
}

// Return a fallback display width for a codepoint using wcwidth when
// available; wide CJK ranges are handled by wcwidth on glibc.
int cpWidth(uint32_t cp) {
    if (cp == 0) return 0;
    return std::max(0, wcwidth((wchar_t)cp));
}

} // namespace

size_t ulen(const std::string& str, bool wide) {
    size_t count = 0;
    size_t pos = 0;
    while (pos < str.size()) {
        uint32_t cp = decodeNext(str, pos);
        if (cp == 0) { pos++; count++; continue; }
        if (wide) count += (size_t)cpWidth(cp);
        else count++;
    }
    return count;
}

size_t wide_ulen(const std::string& str) {
    return ulen(str, true);
}

std::string uresize(const std::string& str, size_t len, bool wide) {
    if (len == 0) return {};
    size_t width = 0;
    size_t pos = 0;
    while (pos < str.size()) {
        size_t start = pos;
        uint32_t cp = decodeNext(str, pos);
        if (cp == 0) { pos++; width++; }
        else if (wide) width += (size_t)cpWidth(cp);
        else width++;
        if (width > len) return str.substr(0, start);
    }
    return str;
}

std::string luresize(const std::string& str, size_t len, bool wide) {
    if (len == 0) return {};
    // Measure from the right until we've consumed `len` columns.
    size_t width = 0;
    size_t cut = str.size();
    size_t pos = str.size();
    while (pos > 0 && width < len) {
        // Walk backwards over one codepoint.
        size_t i = pos - 1;
        while (i > 0 && ((unsigned char)str[i] & 0xC0) == 0x80) i--;
        uint32_t cp = decodeNext(str, i); // decodes from i, advances to old pos
        (void)cp;
        // decodeNext advanced i to pos (contiguous valid seq); recompute width:
        size_t w;
        if (cp == 0) w = 1;
        else w = wide ? (size_t)cpWidth(cp) : 1;
        if (width + w > len) break;
        width += w;
        cut = i;
        pos = i;
    }
    return str.substr(cut);
}

namespace {

size_t padWidth(const std::string& str, bool utf, bool wide) {
    return utf ? (wide ? wide_ulen(str) : ulen(str)) : str.size();
}

} // namespace

std::string ljust(const std::string& str, size_t x,
                  bool utf, bool wide, bool limit) {
    size_t w = padWidth(str, utf, wide);
    if (limit && w > x) return utf ? uresize(str, x, wide) : str.substr(0, x);
    if (w >= x) return str;
    return str + std::string(x - w, ' ');
}

std::string rjust(const std::string& str, size_t x,
                  bool utf, bool wide, bool limit) {
    size_t w = padWidth(str, utf, wide);
    if (limit && w > x) return utf ? luresize(str, x, wide) : str.substr(str.size() - x);
    if (w >= x) return str;
    return std::string(x - w, ' ') + str;
}

std::string cjust(const std::string& str, size_t x,
                  bool utf, bool wide, bool limit) {
    size_t w = padWidth(str, utf, wide);
    if (limit && w > x) return utf ? uresize(str, x, wide) : str.substr(0, x);
    if (w >= x) return str;
    size_t left = (x - w) / 2;
    size_t right = x - w - left;
    return std::string(left, ' ') + str + std::string(right, ' ');
}

std::string s_replace(const std::string& str, const std::string& from,
                      const std::string& to) {
    if (from.empty()) return str;
    std::string out;
    size_t pos = 0;
    while (true) {
        size_t found = str.find(from, pos);
        if (found == std::string::npos) {
            out.append(str, pos, std::string::npos);
            break;
        }
        out.append(str, pos, found - pos);
        out += to;
        pos = found + from.size();
    }
    return out;
}

} // namespace Tools