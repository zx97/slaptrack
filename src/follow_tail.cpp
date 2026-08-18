// follow_tail.cpp

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
    THE AUTHORS BE LIABLE FOR A CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
    IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
    CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "follow_tail.h"
#include "log_parser.h"
#include "banner.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace {

// ANSI colour codes for the same token types the viewer uses.
// Same scheme as the viewer's setupColors(): basic 16 colours only
// so the output renders identically on every terminal, including
// RHEL 8's default xterm.

const char* ansiForToken(TokenType t, int schema) {
    if (schema == 1) return "";

    struct { TokenType type; const char* code; } static const TABLE[] = {
        {TokenType::TIMESTAMP,    "\x1b[36m"},
        {TokenType::THREAD_ID,    "\x1b[35m"},
        {TokenType::CONN_ID,      "\x1b[33m"},
        {TokenType::OP_ID,        "\x1b[35m"},
        {TokenType::DN_VALUE,     "\x1b[32m"},
        {TokenType::FILTER_VALUE, "\x1b[33m"},
        {TokenType::IP_ADDRESS,   "\x1b[34m"},
        {TokenType::ERROR_CODE,   "\x1b[31m"},
        {TokenType::FD_NUM,       "\x1b[37m"},
        {TokenType::TAG,          "\x1b[33m"},
        {TokenType::ETIME_VAL,    "\x1b[36m"},
        {TokenType::NENTRIES,     "\x1b[32m"},
        {TokenType::QTIME_VAL,    "\x1b[35m"},
        {TokenType::SCOPE,        "\x1b[33m"},
        {TokenType::DEREF,        "\x1b[36m"},
        {TokenType::ATTR,         "\x1b[32m"},
        {TokenType::ATTR_LIST,    "\x1b[37m"},
        {TokenType::BASE,         "\x1b[34m"},
        {TokenType::KEYWORD,      "\x1b[34m"},
    };
    for (const auto& e : TABLE) {
        if (e.type == t) return e.code;
    }
    return "";
}

// In monochrome mode, only "important" tokens get bold (conn=, op=,
// dn=, err=, keywords).  Everything else stays plain so the log
// remains readable without color cues.
bool isBoldToken(TokenType t, int schema) {
    if (schema == 1) {
        switch (t) {
            case TokenType::CONN_ID:
            case TokenType::OP_ID:
            case TokenType::THREAD_ID:
            case TokenType::DN_VALUE:
            case TokenType::ERROR_CODE:
            case TokenType::KEYWORD:
            case TokenType::NENTRIES:
            case TokenType::BASE:
                return true;
            default:
                return false;
        }
    }
    switch (t) {
        case TokenType::CONN_ID:
        case TokenType::OP_ID:
        case TokenType::THREAD_ID:
        case TokenType::DN_VALUE:
        case TokenType::ERROR_CODE:
        case TokenType::KEYWORD:
        case TokenType::NENTRIES:
        case TokenType::QTIME_VAL:
        case TokenType::SCOPE:
        case TokenType::DEREF:
        case TokenType::ATTR:
        case TokenType::BASE:
            return true;
        default:
            return false;
    }
}

volatile std::sig_atomic_t g_followRunning = 1;

void followSignalHandler(int) {
    g_followRunning = 0;
}

} // namespace

FollowTail::FollowTail(const std::string& filename, int schema, LogFormat logFormat)
    : filename_(filename),
      fd_(-1),
      inotifyFd_(-1),
      watchFd_(-1),
      lastSize_(0),
      terminalRaw_(false),
      schema_(schema),
      logFormat_(logFormat) {}

FollowTail::~FollowTail() {
    if (watchFd_ >= 0 && inotifyFd_ >= 0) {
        inotify_rm_watch(inotifyFd_, watchFd_);
    }
    if (inotifyFd_ >= 0) close(inotifyFd_);
    if (fd_ >= 0) close(fd_);
}

void FollowTail::printColoredLine(const std::string& rawLine) {
    static thread_local LogParser parser;
    parser.setLogFormat(logFormat_);
    LogLine line = parser.parseLine(rawLine);
    const std::string& text = line.raw;

    size_t lastEnd = 0;
    for (const auto& tok : line.tokens) {
        if (tok.start_pos > lastEnd && tok.start_pos <= text.size()) {
            std::cout << text.substr(lastEnd, tok.start_pos - lastEnd);
        }
        const char* c = ansiForToken(tok.type, schema_);
        if (*c) std::cout << c;
        if (isBoldToken(tok.type, schema_)) std::cout << "\x1b[1m";
        if (tok.end_pos <= text.size()) {
            std::cout << text.substr(tok.start_pos, tok.end_pos - tok.start_pos);
        } else {
            std::cout << tok.value;
        }
        std::cout << "\x1b[0m";
        lastEnd = tok.end_pos;
    }
    if (lastEnd < text.size()) {
        std::cout << text.substr(lastEnd);
    }
    std::cout << "\n";
    std::cout.flush();
}

void FollowTail::printLastLines(int n) {
    if (fd_ < 0) return;

    struct stat st;
    if (fstat(fd_, &st) != 0) return;
    if (S_ISDIR(st.st_mode) || st.st_size == 0) {
        lastSize_ = 0;
        return;
    }

    const long CHUNK = 4096;
    long pos = st.st_size;
    int found = 0;

    // Walk backwards from EOF in 4 KB chunks to locate the start of the
    // last N lines.
    while (pos > 0 && found <= n) {
        long readSize = std::min<long>(CHUNK, pos);
        pos -= readSize;
        std::string buf(readSize, '\0');
        if (lseek(fd_, pos, SEEK_SET) == (off_t)-1) break;
        ssize_t r = read(fd_, &buf[0], readSize);
        if (r <= 0) break;
        buf.resize(r);
        for (ssize_t i = r - 1; i >= 0; i--) {
            if (buf[i] == '\n') {
                found++;
                if (found > n) { pos += (i + 1); break; }
            }
        }
        if (found > n) break;
    }

    if (pos < 0) pos = 0;
    lseek(fd_, pos, SEEK_SET);

    // Read forward from the located offset to EOF and print each line.
    std::string leftover;
    char readBuf[CHUNK];
    while (true) {
        ssize_t r = read(fd_, readBuf, sizeof(readBuf));
        if (r <= 0) break;
        leftover.append(readBuf, r);
    }
    lastSize_ = st.st_size;

    // Split into lines (drop trailing partial).
    size_t start = 0;
    while (start < leftover.size()) {
        size_t nl = leftover.find('\n', start);
        if (nl == std::string::npos) break;
        std::string line = leftover.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        printColoredLine(line);
        start = nl + 1;
    }
}

int FollowTail::setupInotify() {
    inotifyFd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotifyFd_ < 0) {
        std::cerr << "inotify_init1 failed: " << strerror(errno) << "\n";
        return -1;
    }
    watchFd_ = inotify_add_watch(inotifyFd_, filename_.c_str(),
                                 IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF);
    if (watchFd_ < 0) {
        std::cerr << "inotify_add_watch(" << filename_ << ") failed: "
                  << strerror(errno) << "\n";
        close(inotifyFd_);
        inotifyFd_ = -1;
        return -1;
    }
    return 0;
}

bool FollowTail::drainInotify() {
    if (inotifyFd_ < 0) return false;

    char buf[4096] __attribute__((aligned(__alignof__(inotify_event))));
    bool rotated = false;
    while (true) {
        ssize_t len = read(inotifyFd_, buf, sizeof(buf));
        if (len <= 0) break;
        int i = 0;
        while (i < len) {
            auto* ev = reinterpret_cast<inotify_event*>(buf + i);
            if (ev->mask & (IN_MOVE_SELF | IN_DELETE_SELF)) {
                rotated = true;
            }
            i += sizeof(inotify_event) + ev->len;
        }
    }
    return rotated;
}

void FollowTail::readAndPrintNewLines() {
    if (fd_ < 0) return;

    struct stat st;
    if (fstat(fd_, &st) != 0) return;

    if (st.st_size < lastSize_) {
        // File was truncated or replaced.  Re-open and reset.
        reopenAfterRotation();
        return;
    }
    if (st.st_size == lastSize_) return;

    lseek(fd_, lastSize_, SEEK_SET);
    std::string leftover;
    char buf[4096];
    while (true) {
        ssize_t r = read(fd_, buf, sizeof(buf));
        if (r <= 0) break;
        leftover.append(buf, r);
    }
    lastSize_ = st.st_size;

    // Handle the case where the previous tail ends mid-line: keep
    // the unprinted prefix to prepend to the next line.
    static thread_local std::string carry;
    leftover = carry + leftover;
    carry.clear();

    size_t start = 0;
    while (start < leftover.size()) {
        size_t nl = leftover.find('\n', start);
        if (nl == std::string::npos) {
            carry = leftover.substr(start);
            break;
        }
        std::string line = leftover.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        printColoredLine(line);
        start = nl + 1;
    }
}

bool FollowTail::reopenAfterRotation() {
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    if (watchFd_ >= 0 && inotifyFd_ >= 0) {
        inotify_rm_watch(inotifyFd_, watchFd_);
        watchFd_ = -1;
    }
    // Reopen by name in case the inode changed; the new file may not
    // exist yet during the logrotate window, so retry until it appears.
    fd_ = open(filename_.c_str(), O_RDONLY);
    if (fd_ < 0) {
        if (!fileMissing_) {
            std::cerr << "\r\n[file disappeared: " << strerror(errno)
                      << ", waiting for it to reappear...]\r\n";
            fileMissing_ = true;
        }
        return false;
    }
    fileMissing_ = false;
    if (inotifyFd_ >= 0) {
        watchFd_ = inotify_add_watch(inotifyFd_, filename_.c_str(),
                                     IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF);
    }
    lastSize_ = 0;
    // Print the last 10 lines of the new file to preserve context.
    std::cout << "\x1b[2m\x1b[37m--- log rotated, showing last 10 lines of new file ---\x1b[0m\n";
    printLastLines(10);
    return true;
}

void FollowTail::run() {
    const char* term = std::getenv("TERM");
    bool supportsColor = true;
    if (!term || std::strcmp(term, "dumb") == 0) {
        supportsColor = false;
    } else {
        FILE* tputf = popen("tput colors 2>/dev/null", "r");
        if (tputf) {
            int colors = 0;
            if (fscanf(tputf, "%d", &colors) == 1 && colors < 8) {
                supportsColor = false;
            }
            pclose(tputf);
        }
    }
    if (!supportsColor) {
        schema_ = 1;
    }

    fd_ = open(filename_.c_str(), O_RDONLY);
    if (fd_ < 0) {
        std::cerr << "Error: cannot open " << filename_ << ": "
                  << strerror(errno) << "\n";
        return;
    }

    printLastLines(10);

    if (setupInotify() != 0) {
        // Even without inotify we can still poll the file size.
    }

    // Put the terminal in raw mode so q / Esc arrive instantly and
    // don't echo.  Save the old termios for restoration.
    struct termios oldTio, newTio;
    bool haveTio = (tcgetattr(STDIN_FILENO, &oldTio) == 0);
    if (haveTio) {
        newTio = oldTio;
        newTio.c_lflag &= ~(ICANON | ECHO);
        newTio.c_cc[VMIN] = 0;
        newTio.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &newTio) == 0) {
            terminalRaw_ = true;
        }
    }

    // Restore the terminal on Ctrl+C, even if the user smashes it
    // before the loop can react.
    struct sigaction sa;
    sa.sa_handler = followSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Header
    std::cout << "\x1b[2m\x1b[37m--- following " << filename_
              << " (Ctrl+C, q or Esc to quit) ---\x1b[0m\n";

    while (g_followRunning) {
        struct pollfd fds[2];
        int nfds = 0;
        if (inotifyFd_ >= 0) {
            fds[nfds].fd = inotifyFd_;
            fds[nfds].events = POLLIN;
            nfds++;
        }
        fds[nfds].fd = STDIN_FILENO;
        fds[nfds].events = POLLIN;
        nfds++;

        int ret = poll(fds, nfds, 500);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) {
            // Periodic size poll as a fallback for filesystems / kernels
            // that batch inotify events.  Retry reopen here so a rotated-away
            // file is picked up again once it reappears.
            if (fd_ < 0) {
                reopenAfterRotation();
            } else {
                readAndPrintNewLines();
            }
            continue;
        }

        bool gotInotify = (nfds > 1 && (fds[0].revents & POLLIN));
        bool gotStdin   = (fds[nfds - 1].revents & POLLIN);

        if (gotStdin) {
            char ch;
            ssize_t r = read(STDIN_FILENO, &ch, 1);
            if (r == 1) {
                if (ch == 'q' || ch == 'Q' || ch == 27 /* Esc */ ||
                    ch == 3  /* Ctrl+C */ || ch == 4 /* Ctrl+D */) {
                    break;
                }
            }
        }

        if (gotInotify) {
            if (drainInotify()) {
                reopenAfterRotation();
            } else {
                readAndPrintNewLines();
            }
        } else if (ret == 0) {
            // Timeout path: poll size to catch missed inotify
            // events (some kernels / filesystems batch them).
            readAndPrintNewLines();
        }
    }

    if (terminalRaw_ && haveTio) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldTio);
    }
    std::cout << "\x1b[0m\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// FollowTailStdin: reads from stdin and colourises each line as it arrives.
// Used by `tail -f | slaptrack -` or `slaptrack -` (stdin pipe mode).
// No inotify since stdin is not a real file.
// ─────────────────────────────────────────────────────────────────────────────
namespace {
void printAnsiLine(const std::string& rawLine, int schema, LogFormat logFormat) {
    static thread_local LogParser parser;
    parser.setLogFormat(logFormat);
    LogLine line = parser.parseLine(rawLine);
    const std::string& text = line.raw;

    size_t lastEnd = 0;
    for (const auto& tok : line.tokens) {
        if (tok.start_pos > lastEnd && tok.start_pos <= text.size()) {
            std::cout << text.substr(lastEnd, tok.start_pos - lastEnd);
        }
        const char* c = ansiForToken(tok.type, schema);
        if (*c) std::cout << c;
        if (isBoldToken(tok.type, schema)) std::cout << "\x1b[1m";
        if (tok.end_pos <= text.size()) {
            std::cout << text.substr(tok.start_pos, tok.end_pos - tok.start_pos);
        } else {
            std::cout << tok.value;
        }
        std::cout << "\x1b[0m";
        lastEnd = tok.end_pos;
    }
    if (lastEnd < text.size()) {
        std::cout << text.substr(lastEnd);
    }
    std::cout << "\n";
    std::cout.flush();
}
} // namespace

FollowTailStdin::FollowTailStdin(int schema, LogFormat logFormat)
    : schema_(schema), logFormat_(logFormat) {}

void FollowTailStdin::detectColorSupport() {
    const char* term = std::getenv("TERM");
    if (!term || std::strcmp(term, "dumb") == 0) {
        schema_ = 1;
        return;
    }
    FILE* tputf = popen("tput colors 2>/dev/null", "r");
    if (!tputf) return;
    int colors = 0;
    if (fscanf(tputf, "%d", &colors) == 1 && colors < 8) {
        schema_ = 1;
    }
    pclose(tputf);
}

bool FollowTailStdin::setupTerminal() {
    struct termios newTio;
    if (tcgetattr(STDIN_FILENO, &newTio) != 0) return false;
    newTio.c_lflag &= ~(ICANON | ECHO);
    newTio.c_cc[VMIN] = 0;
    newTio.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &newTio) == 0) {
        terminalRaw_ = true;
        return true;
    }
    return false;
}

void FollowTailStdin::restoreTerminal() const {
    if (!terminalRaw_) return;
    struct termios oldTio;
    tcgetattr(STDIN_FILENO, &oldTio);
    oldTio.c_lflag |= ICANON | ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &oldTio);
}

void FollowTailStdin::setupSignals() {
    struct sigaction sa;
    sa.sa_handler = followSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

void FollowTailStdin::drainStdin() {
    std::string carry;
    while (g_followRunning) {
        struct pollfd fds[1];
        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;
        int ret = poll(fds, 1, 100);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue;

        if (fds[0].revents & (POLLHUP | POLLNVAL)) break;

        if (fds[0].revents & POLLIN) {
            char buf[4096];
            ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
            if (r <= 0) break;
            carry.append(buf, r);
        }

        size_t start = 0;
        while (start < carry.size()) {
            size_t nl = carry.find('\n', start);
            if (nl == std::string::npos) break;
            std::string line = carry.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            printAnsiLine(line, schema_, logFormat_);
            start = nl + 1;
        }
        carry.erase(0, start);
    }
}

void FollowTailStdin::run() {
    detectColorSupport();
    setupTerminal();
    setupSignals();

    std::cout << "\x1b[2m\x1b[37m--- reading stdin (tail -f | slaptrack -) Ctrl+C or q to quit ---\x1b[0m\n";
    std::cout.flush();

    drainStdin();

    restoreTerminal();
    std::cout << "\x1b[0m\n";
    std::cout << embedded::BANNER_TEXT;
    std::cout.flush();
}
