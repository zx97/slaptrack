// log_buffer.h

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

#pragma once

#include "log_index.h"
#include "log_parser.h"
#include <vector>
#include <optional>
#include <unordered_map>
#include <mutex>

class LogBuffer {
public:
    LogBuffer(size_t windowSize = 2000);
    
    bool loadFile(const std::string& filename);
    size_t refreshFile();

    // Close/re-open the index file (log rotation).
    bool reopenFile(const std::string& filename);
    
    size_t getTotalLines() const;
    std::optional<LogLine> getLine(size_t index);
    
    // Access to the raw line text.  Only a bounded window of lines is
    // kept in memory (rawWindowSize_); requesting a line outside that
    // window reloads a fresh page from disk in one grouped read.  Used
    // by the filter loop and the wrap-layout scan.  Returns std::nullopt
    // if the index is out of range.
    std::optional<std::string> getRawLine(size_t index) const;
    
    // Expose the parser so the filter loop can parse raw lines
    // without going through the (LRU-bounded) parsed-line cache.
    LogParser& getParser() { return parser_; }
    
    void prefetchAround(size_t centerIndex, int radius);
    
    void clearCache();
    
    const LogIndex& getIndex() const { return index_; }
    
private:
    // Number of raw lines kept in memory at once.  This is the strict
    // minimum needed: one screen/wrap window, not the whole file.
    static constexpr size_t kRawWindowLines = 4096;
    
    void loadRawWindow(size_t startLine) const;
    
    LogIndex index_;
    LogParser parser_;
    // Parsed-line cache (LRU-bounded by windowSize_).
    std::unordered_map<size_t, LogLine> cache_;
    size_t windowSize_;
    // Raw text window: we never hold the whole file in RAM.
    mutable size_t rawPageStart_ = 0;
    mutable std::vector<std::string> rawLines_;

    // All public methods that touch rawLines_/rawPageStart_/cache_/index_/
    // parser_ take this lock first.  Recursive because getLine() calls
    // getRawLine() and prefetchAround() calls getLine() — the public
    // boundary is the only safe place to take the lock without an extra
    // unlocked helper.
    mutable std::recursive_mutex mutex_;
};
