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

class LogBuffer {
public:
    LogBuffer(size_t windowSize = 2000);
    
    bool loadFile(const std::string& filename);
    size_t refreshFile();

    // Close + re-open the underlying index file (log rotation).
    bool reopenFile(const std::string& filename);
    
    size_t getTotalLines() const;
    std::optional<LogLine> getLine(size_t index);
    
    // O(1) access to the raw line text.  Used by the filter loop to
    // avoid the per-line fseek that getLine() triggers on a cache miss.
    // Returns an empty string if the index is out of range.
    const std::string& getRawLine(size_t index) const;
    
    // Expose the parser so the filter loop can parse raw lines
    // without going through the (LRU-bounded) parsed-line cache.
    LogParser& getParser() { return parser_; }
    
    void prefetchAround(size_t centerIndex, int radius);
    
    void clearCache();
    
    const LogIndex& getIndex() const { return index_; }
    
private:
    // Bulk-read every line of the file into memory at load time.
    // This is what makes filter scans O(n) without 300K fseeks.
    bool loadRawLines(const std::string& filename);
    
    LogIndex index_;
    LogParser parser_;
    std::vector<std::string> rawLines_;
    std::unordered_map<size_t, LogLine> cache_;
    size_t windowSize_;
};
