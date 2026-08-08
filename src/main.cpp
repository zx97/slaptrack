// main.cpp

/* 
    SPDX-License-Identifier: AGPL-3.0-or-later
    GNU Affero General Public License v3.0 (https://www.gnu.org/licenses/agpl-3.0.txt)
    Copyright (c) 2026 Manuel FLURY
    All rights reserved.
    
    This file is part of slaptrack - an OpenLDAP Log Viewer.
    
    Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0-or-later).
    See the LICENSE file distributed with this work for full license text.
    
    THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
    AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
    CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "viewer.h"
#include "follow_tail.h"
#include "compressed_io.h"
#include "embedded.hpp"
#include "version.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

std::string g_tempFile;

void cleanupTempFile() {
    if (!g_tempFile.empty()) {
        unlink(g_tempFile.c_str());
        g_tempFile.clear();
    }
}

// Parse the --log-format argument (see LogFormat in log_parser.h).
// Returns false and prints an error to stderr for unknown values.
bool parseLogFormat(const std::string& s, LogFormat& out) {
    if (s == "auto")                out = LogFormat::AUTO;
    else if (s == "debug")          out = LogFormat::DEBUG;
    else if (s == "syslog-utc")     out = LogFormat::SYSLOG_UTC;
    else if (s == "syslog-local")   out = LogFormat::SYSLOG_LOCALTIME;
    else if (s == "rfc3339")        out = LogFormat::RFC3339_UTC;
    else {
        std::cerr << "Error: unknown log format '" << s << "'\n"
                  << "Valid values: auto, debug, syslog-utc, syslog-local, rfc3339\n";
        return false;
    }
    return true;
}

} // namespace

#ifndef BUILD_NUMBER
#define BUILD_NUMBER 0
#endif

void printUsage(const char* program) {
    std::cerr << program << " - an OpenLDAP Log Viewer v" << SLAPTRACK_VERSION << "\n";
    std::cerr << "\n";
    std::cerr << "Usage: " << program << " [-f] <logfile>\n";
    std::cerr << "       " << program << " [-f] -\n";
    std::cerr << "       tail -f <logfile> | " << program << "\n";
    std::cerr << "\n";
    std::cerr << "Options:\n";
    std::cerr << "  -f                   Follow mode (like tail -f)\n";
    std::cerr << "  --log-format <fmt>   Log format: auto, debug, syslog-utc,\n";
    std::cerr << "                       syslog-local, rfc3339 (default: auto; -L alias)\n";
    std::cerr << "  -                    Read from stdin (pipe mode)\n";
    std::cerr << "  -D, --documentation  Print full documentation\n";
    std::cerr << "  -l, --licence         Print the GNU AGPL-3.0 license\n";
    std::cerr << "  -h, --help           Show this help message\n";
    std::cerr << "  -V, --version        Show version\n";
    std::cerr << "\n";
    std::cerr << "Controls:\n";
    std::cerr << "  Arrow keys / hjkl   Navigate\n";
    std::cerr << "  Page Up/Down         Scroll pages\n";
    std::cerr << "  Home/End or g/G      Go to top/bottom\n";
    std::cerr << "  ^ / $                Cursor to start/end of line\n";
    std::cerr << "  :<num>               Jump to line number\n";
    std::cerr << "  :$ or :^             Jump to end/beginning\n";
    std::cerr << "  Enter                Filter by token under cursor\n";
    std::cerr << "  Esc/Backspace        Remove last filter\n";
    std::cerr << "  /                    Search (pre-filled with token)\n";
    std::cerr << "  n/N                  Next/Previous search result\n";
    std::cerr << "  #                    Toggle line numbers\n";
    std::cerr << "  ←/→ or h/l          Scroll long lines horizontally\n";
    std::cerr << "  F1-F8               Switch color schema (viewer mode only)\n";
    std::cerr << "  Mouse scroll         Scroll up/down\n";
    std::cerr << "  Mouse click          Move cursor\n";
    std::cerr << "  Mouse double-click   Filter by token\n";
    std::cerr << "  q                    Quit\n";
    std::cerr << "\n";
    std::cerr << "Color schemas (F1-F8):\n";
    std::cerr << "  F1 Default  F2 Monochrome  F3 Solarized Light\n";
    std::cerr << "  F4 Solarized Dark  F5 High Contrast  F6 Nord\n";
    std::cerr << "  F7 Gruvbox  F8 Dracula\n";
    std::cerr << "\n";
    std::cerr << "Color legend (default schema):\n";
    std::cerr << "  Cyan     Timestamp\n";
    std::cerr << "  Yellow   Connection ID (conn=)\n";
    std::cerr << "  Purple   Operation ID (op=)\n";
    std::cerr << "  Green    Distinguished Name (dn=)\n";
    std::cerr << "  Yellow   Filter expression\n";
    std::cerr << "  Blue     IP Address\n";
    std::cerr << "  Red      Error code\n";
    std::cerr << "  Blue     Keywords (BIND, SRCH, RESULT, etc.)\n";
    std::cerr << "\n";
    std::cerr << "Copyright (c) 2026 Manuel FLURY\n";
    std::cerr << "Licensed under AGPL-3.0-or-later (https://www.gnu.org/licenses/agpl-3.0.txt)\n";
}

int main(int argc, char* argv[]) {
    bool followMode = false;
    std::string filename;
    LogFormat logFormat = LogFormat::AUTO;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-f") {
            followMode = true;
        } else if (arg == "--log-format" || arg == "-L") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --log-format requires a value.\n";
                printUsage(argv[0]);
                return 1;
            }
            if (!parseLogFormat(argv[++i], logFormat)) {
                return 1;
            }
        } else if (arg.rfind("--log-format=", 0) == 0) {
            if (!parseLogFormat(arg.substr(std::strlen("--log-format=")), logFormat)) {
                return 1;
            }
        } else if (arg == "-D" || arg == "--documentation") {
            std::cout << embedded::DOCUMENTATION_TEXT;
            return 0;
        } else if (arg == "-l" || arg == "--licence") {
            std::cout << embedded::LICENSE_TEXT;
            return 0;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-V" || arg == "--version") {
            std::cout << "slaptrack v" << SLAPTRACK_VERSION << " (built " << SLAPTRACK_BUILD << ") - an OpenLDAP Log Viewer\n";
            return 0;
        } else {
            filename = arg;
        }
    }

    if (filename.empty()) {
        if (followMode) {
            std::cerr << "Error: -f requires a filename argument.\n";
            return 1;
        }
        printUsage(argv[0]);
        return 1;
    }

    if (filename == "-") {
        if (followMode) {
            std::cerr << "Error: -f requires a filename argument, not stdin.\n";
            return 1;
        }
        FollowTailStdin stdinTail(/*schema*/ 0, logFormat);
        stdinTail.run();
        return 0;
    }

    std::string effectivePath = filename;
    CompressionType detected = CompressionType::NONE;
    std::string tempPath = decompressToTempIfCompressed(filename, &detected);
    if (detected != CompressionType::NONE) {
        if (tempPath.empty()) {
            std::cerr << "Error: failed to decompress " << filename
                      << " (" << compressionTypeName(detected) << ")\n";
            return 1;
        }
        std::cerr << "Detected " << compressionTypeName(detected)
                  << " compression; decompressed to " << tempPath << "\n";
        g_tempFile = tempPath;
        std::atexit(cleanupTempFile);
        effectivePath = tempPath;
    }

    if (followMode) {
        FollowTail tail(effectivePath, /*schema*/ 0, logFormat);
        tail.run();
    } else {
        Viewer viewer(effectivePath, followMode, logFormat);
        viewer.run();
    }

    return 0;
}
