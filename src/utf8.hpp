// utf8.hpp
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
// Layer 8 of the TUI-ANSI-METHODOLOGY: measure everything in "terminal
// columns", not bytes.  `str.size()` counts bytes; `wide_ulen()` counts
// columns via wcwidth, which is what the terminal actually uses to
// position the cursor.

#pragma once

#include <string>

namespace Tools {

// Number of UTF-8 characters (grapheme clusters, best effort).
size_t ulen(const std::string& str, bool wide = false);

// Number of terminal columns (via wcwidth).  Wide characters (CJK,
// braille) count 2, combining marks 0.
size_t wide_ulen(const std::string& str);

// Truncate right to `len` columns (wide-aware).
std::string uresize(const std::string& str, size_t len, bool wide = false);

// Truncate left to `len` columns (wide-aware).
std::string luresize(const std::string& str, size_t len, bool wide = false);

// Align helpers.  `utf` measures by columns instead of bytes;
// `limit` truncates instead of overflowing.
std::string ljust(const std::string& str, size_t x,
                  bool utf = false, bool wide = false, bool limit = false);
std::string rjust(const std::string& str, size_t x,
                  bool utf = false, bool wide = false, bool limit = false);
std::string cjust(const std::string& str, size_t x,
                  bool utf = false, bool wide = false, bool limit = false);

// Replace all occurrences of `from` with `to`.
std::string s_replace(const std::string& str, const std::string& from,
                      const std::string& to);

} // namespace Tools