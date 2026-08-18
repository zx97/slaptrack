// follow_tail.h

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
    THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
    IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
    CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include "log_parser.h"
#include <string>

// A simple, "tail -f" with colour implementation of slaptrack's
// follow mode.  Unlike the full Viewer class this:
//   - does not initialise ncurses (works in any terminal / pipe)
//   - does not show filters, navigation, mouse, status bar, etc.
//   - prints the last N lines of the file on startup, then streams
//     new lines as they are written, colourised to stdout
//   - exits cleanly on Ctrl+C, 'q', 'Q', or Esc
//   - recovers from log rotation (IN_MOVE_SELF / IN_DELETE_SELF)
class FollowTail {
public:
    explicit FollowTail(const std::string& filename, int schema = 0,
                        LogFormat logFormat = LogFormat::AUTO);
    ~FollowTail();

    void run();

private:
    void printColoredLine(const std::string& rawLine);
    void printLastLines(int n);
    int setupInotify();
    bool drainInotify();
    void readAndPrintNewLines();
    bool reopenAfterRotation();

    std::string filename_;
    int fd_;
    int inotifyFd_;
    int watchFd_;
    long lastSize_;
    bool fileMissing_ = false;
    bool terminalRaw_;
    int schema_;
    LogFormat logFormat_ = LogFormat::AUTO;
};

// Reads from stdin and colourises each line as it arrives.  Used by
// `tail -f | slaptrack -` or `slaptrack -` (stdin pipe mode).
class FollowTailStdin {
public:
    FollowTailStdin(int schema = 0, LogFormat logFormat = LogFormat::AUTO);
    void run();

private:
    void detectColorSupport();
    bool setupTerminal();
    void restoreTerminal() const;
    void setupSignals();
    void drainStdin();

    bool terminalRaw_ = false;
    int schema_;
    LogFormat logFormat_ = LogFormat::AUTO;
};