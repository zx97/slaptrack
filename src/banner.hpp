// banner.hpp
//
// SPDX-License-Identifier: AGPL-3.0-or-later
// GNU Affero General Public License v3.0 (https://www.gnu.org/licenses/agpl-3.0.txt)
// Copyright (c) 2026 Manuel FLURY
// All rights reserved.
//
// This file is part of slaptrack - an OpenLDAP Log Viewer (ANSI edition).
//
// ANSI banner (256-color gradient), embedded so it can be printed in the
// help screen and on exit.  Kept in sync with ansi_banner.utf8.

#pragma once

namespace embedded {

inline const char* BANNER_TEXT =
    "\033[38;5;39m╔═══════════════════════════════════════════════════════════════════════════╗\n"
    "\033[38;5;51m║ ███████╗██╗      █████╗ ██████╗ ████████╗██████╗  █████╗  ██████╗██╗  ██╗ \033[38;5;39m║\n"
    "\033[38;5;45m║ ██╔════╝██║     ██╔══██╗██╔══██╗╚══██╔══╝██╔══██╗██╔══██╗██╔════╝██║ ██╔╝ \033[38;5;39m║\n"
    "\033[38;5;45m║ ███████╗██║     ███████║██████╔╝   ██║   ██████╔╝███████║██║     █████╔╝  \033[38;5;39m║\n"
    "\033[38;5;39m║ ╚════██║██║     ██╔══██║██╔═══╝    ██║   ██╔══██╗██╔══██║██║     ██╔═██╗  \033[38;5;39m║\n"
    "\033[38;5;33m║ ███████║███████╗██║  ██║██║        ██║   ██║  ██║██║  ██║╚██████╗██║  ██╗ \033[38;5;39m║\n"
    "\033[38;5;27m║ ╚══════╝╚══════╝╚═╝  ╚═╝╚═╝        ╚═╝   ╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝╚═╝  ╚═╝ \033[38;5;39m║\n"
    "\033[38;5;33m║                  OpenLDAP Log Viewer - Pure ANSI edition                  \033[38;5;39m║\n"
    "\033[38;5;39m╚═══════════════════════════════════════════════════════════════════════════╝\n"
    "\033[0m\n"
    ;

} // namespace embedded

