// theme.cpp
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
// Implements the 8 color schemas (F1-F8) of the ncurses version as ANSI
// escape codes, plus a truecolor/256-color gradient engine.

#include "theme.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include "ansi.hpp"

namespace Theme {

namespace {

// A token color: hex fg ("#rrggbb" or "#gg", "" = default) + hex bg
// ("" = default).  Each schema preserves the ncurses version's color
// roles: timestamp/times, conn, thread, dn/attr, filter, error, ip,
// keywords, dims, popup.
struct HexPair { const char* fg; const char* bg; };

const HexPair SCHEMAS[8][TOK_COUNT] = {
    // F1: Default
    {{"#4678ce",""},{"#fbed76",""},{"#794070",""},{"#74c5a7",""},{"#fbed76",""},
     {"#4678ce",""},{"#f94845",""},{"#c8",""},{"#ea",""},
     {"#5254e2",""},{"#74c5a7",""},{"#794070",""},{"#5c",""},{"#5c",""},
     {"#3c",""},{"#c8",""},{"#ffffff","#c00000"},
     {"#74c5a7",""},{"#c8",""},{"#4678ce",""}},
    // F2: Monochrome
    {{"#88",""},{"#c4",""},{"#88",""},{"#b3",""},{"#c4",""},
     {"#88",""},{"#f7",""},{"#b3",""},{"#c4",""},
     {"#88",""},{"#b3",""},{"#88",""},{"#48",""},{"#48",""},
     {"#28",""},{"#b3",""},{"#ffffff","#c00000"},
     {"#b3",""},{"#b3",""},{"#88",""}},
    // F3: Solarized Light (bg = main_bg #fcfcea)
    {{"#3063ce","#fcfcea"},{"#aeb209","#fcfcea"},{"#cc4361","#fcfcea"},
     {"#79c30a","#fcfcea"},{"#ccd10b","#fcfcea"},
     {"#3063ce","#fcfcea"},{"#d8653d","#fcfcea"},{"#5c6977","#fcfcea"},
     {"#032039","#fcfcea"},
     {"#8575c1","#fcfcea"},{"#82d10b","#fcfcea"},{"#ef7893","#fcfcea"},
     {"#97a0a3","#fcfcea"},{"#97a0a3","#fcfcea"},
     {"#97a0a3","#fcfcea"},{"#5c6977","#fcfcea"},{"#ffffff","#c00000"},
     {"#79c30a","#fcfcea"},{"#5c6977","#fcfcea"},{"#3063ce","#fcfcea"}},
    // F4: Solarized Dark
    {{"#20b2d2",""},{"#af5c00",""},{"#d72ca7",""},{"#c1ad00",""},{"#d06d00",""},
     {"#20b2d2",""},{"#e52452",""},{"#eddfd0",""},{"#feeddc",""},
     {"#6581c5",""},{"#d0bb00",""},{"#f962cf",""},{"#557272",""},{"#557272",""},
     {"#557272",""},{"#eddfd0",""},{"#ffffff","#c00000"},
     {"#c1ad00",""},{"#eddfd0",""},{"#20b2d2",""}},
    // F5: Monokai
    {{"#63a6e8",""},{"#c5e070",""},{"#c02c46",""},{"#66d92d",""},{"#c5e070",""},
     {"#63a6e8",""},{"#f0252b",""},{"#f1f4ec",""},{"#f1f4ec",""},
     {"#d3c0ad",""},{"#caf1b6",""},{"#f58c9f",""},{"#6b6f5a",""},{"#6b6f5a",""},
     {"#505343",""},{"#f1f4ec",""},{"#ffffff","#c00000"},
     {"#66d92d",""},{"#f1f4ec",""},{"#63a6e8",""}},
    // F6: Nord
    {{"#81aec5",""},{"#90bfb6",""},{"#88d0d4",""},{"#eef2f6",""},{"#5e91b0",""},
     {"#81aec5",""},{"#eef2f6",""},{"#dae3eb",""},{"#90bfb6",""},
     {"#88d0d4",""},{"#dae3eb",""},{"#88d0d4",""},{"#4c5d6e",""},{"#4c5d6e",""},
     {"#4c5d6e",""},{"#dae3eb",""},{"#ffffff","#c00000"},
     {"#eef2f6",""},{"#dae3eb",""},{"#81aec5",""}},
    // F7: Gruvbox
    {{"#457284",""},{"#f5e830",""},{"#d08588",""},{"#91b429",""},{"#cfbd25",""},
     {"#82a29e",""},{"#f67635",""},{"#a59f83",""},{"#e9e6b0",""},
     {"#7a6bc1",""},{"#77921c",""},{"#ae6172",""},{"#565656",""},{"#565656",""},
     {"#565656",""},{"#a59f83",""},{"#ffffff","#c00000"},
     {"#91b429",""},{"#a59f83",""},{"#82a29e",""}},
    // F8: Dracula
    {{"#81ffef",""},{"#ff8664",""},{"#9689fb",""},{"#4dfe44",""},{"#ff592b",""},
     {"#1cffe3",""},{"#ff4d82",""},{"#f5f3ed",""},{"#f5f3ed",""},
     {"#ff71ed",""},{"#4dfe44",""},{"#ff2be4",""},{"#5c82a2",""},{"#5c82a2",""},
     {"#404a56",""},{"#f5f3ed",""},{"#ffffff","#c00000"},
     {"#4dfe44",""},{"#f5f3ed",""},{"#81ffef",""}},
};

// Cache of escape codes for the current schema's token colors.
std::array<std::string, TOK_COUNT> g_cache;
int g_schema = 0;
bool g_tty = false;
bool g_truecolor = false;

// Background for popups (red on white in every schema, matching the
// ncurses version's pair 17).
std::string g_popup_bg = "\x1b[37;41m";
// Background for search matches (hlsearch): yellow in colour mode,
// reverse video when the terminal is monochrome.
std::string g_search_bg = "\x1b[30;43m";

// Truecolor gradient: green → yellow → red over [0,1].
std::array<std::string, 101> g_gradient;
// 16-color plateau gradient fallback (bright green → yellow → red).
std::array<std::string, 101> g_gradient_16;

void buildGradients() {
    // Truecolor: linear interpolate green(0,255,0) → yellow(255,255,0)
    // → red(255,0,0).  Store 101 steps.
    for (int i = 0; i <= 100; i++) {
        int r, g, b;
        if (i <= 50) {
            double t = i / 50.0; // 0..1 green→yellow
            r = (int)(0 + t * 255);
            g = 255;
            b = 0;
        } else {
            double t = (i - 50) / 50.0; // 0..1 yellow→red
            r = 255;
            g = (int)(255 - t * 255);
            b = 0;
        }
        g_gradient[i] = "\x1b[38;2;" + std::to_string(r) + ";"
                        + std::to_string(g) + ";" + std::to_string(b) + "m";
    }
    // 16-color plateau: <=40% bright green, <=70% bright yellow, else
    // bright red.
    for (int i = 0; i <= 100; i++) {
        if (i <= 40)      g_gradient_16[i] = "\x1b[92m";
        else if (i <= 70) g_gradient_16[i] = "\x1b[93m";
        else              g_gradient_16[i] = "\x1b[91m";
    }
}

void rebuildCache() {
    for (int i = 0; i < TOK_COUNT; i++) {
        const HexPair& p = SCHEMAS[g_schema][i];
        std::string code;
        if (p.fg && *p.fg) code += hexToColor(p.fg, "fg");
        if (p.bg && *p.bg) code += hexToColor(p.bg, "bg");
        g_cache[i] = code;
    }
    g_popup_bg = "\x1b[37;41m";
    g_search_bg = g_tty ? "\x1b[7m" : "\x1b[30;43m";
}

} // namespace

const char* SCHEMA_NAMES[8] = {
    "Default", "Monochrome", "Solarized Light", "Solarized Dark",
    "Monokai", "Nord", "Gruvbox", "Dracula",
};

void setSchema(int schema) {
    if (schema < 0) schema = 0;
    if (schema > 7) schema = 7;
    g_schema = schema;
    rebuildCache();
}

void setTtyMode(bool tty) {
    g_tty = tty;
    if (tty) setSchema(1);
}

bool ttyMode() { return g_tty; }

void detectColorSupport() {
    g_truecolor = false;
    const char* term = std::getenv("TERM");
    if (!term || std::strcmp(term, "dumb") == 0) {
        g_tty = true;
        return;
    }
    const char* colorterm = std::getenv("COLORTERM");
    if (colorterm &&
        (std::strstr(colorterm, "truecolor") || std::strstr(colorterm, "24bit"))) {
        g_truecolor = true;
    }
    // If tput colors < 8 → tty mode.
    FILE* tputf = popen("tput colors 2>/dev/null", "r");
    if (tputf) {
        int colors = 0;
        if (std::fscanf(tputf, "%d", &colors) == 1 && colors < 8) {
            g_tty = true;
        }
        pclose(tputf);
    }
    if (g_tty) setSchema(1);
}

bool truecolor() { return g_truecolor; }

const std::string& c(int tokenColorIndex) {
    if (tokenColorIndex < 0 || tokenColorIndex >= TOK_COUNT) {
        static const std::string empty;
        return empty;
    }
    return g_cache[tokenColorIndex];
}

const std::string& mainFg()      { return c(TOK_BASE_TEXT); }
const std::string& selectedFg()  { return c(TOK_LINENUM); }
const std::string& selectedBg()  { return c(TOK_POPUP); }
const std::string& title()       { return c(TOK_BASE_TEXT); }
const std::string& divLine()     { return c(TOK_TIMESTAMP); }
const std::string& popupBg()     { return g_popup_bg; }
const std::string& searchBg()    { return g_search_bg; }

const std::string& gradient(float pct) {
    int idx = (int)(pct * 100.0f);
    idx = std::max(0, std::min(100, idx));
    if (g_truecolor && !g_tty) return g_gradient[idx];
    return g_gradient_16[idx];
}

const std::string& reset() { return Fx::reset; }

std::string hexToColor(const std::string& hexa, const std::string& depth) {
    // Accept "#rrggbb" or "#gg" (grey).
    std::string h = hexa;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    int r = 0, g = 0, b = 0;
    if (h.size() == 6) {
        r = (int)std::strtol(h.substr(0, 2).c_str(), nullptr, 16);
        g = (int)std::strtol(h.substr(2, 2).c_str(), nullptr, 16);
        b = (int)std::strtol(h.substr(4, 2).c_str(), nullptr, 16);
    } else if (h.size() == 2) {
        r = g = b = (int)std::strtol(h.c_str(), nullptr, 16);
    } else {
        return "";
    }
    if (g_truecolor) {
        if (depth == "bg") {
            return "\x1b[48;2;" + std::to_string(r) + ";" + std::to_string(g)
                   + ";" + std::to_string(b) + "m";
        }
        return "\x1b[38;2;" + std::to_string(r) + ";" + std::to_string(g)
               + ";" + std::to_string(b) + "m";
    }
    // 256-color fallback (cube).
    int ri = (r * 5 + 127) / 255, gi = (g * 5 + 127) / 255, bi = (b * 5 + 127) / 255;
    int code = 16 + 36 * ri + 6 * gi + bi;
    if (depth == "bg") return "\x1b[48;5;" + std::to_string(code) + "m";
    return "\x1b[38;5;" + std::to_string(code) + "m";
}

// Initialization: called once at startup.
void init() {
    buildGradients();
    rebuildCache();
}

} // namespace Theme