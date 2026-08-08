// filter.h

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

#include "log_parser.h"
#include <string>
#include <vector>
#include <cstdint>

enum class FilterType {
    CONN,
    DN,
    OP,
    BASE,
    ERROR_CODE,
    TEXT
};

struct Filter {
    FilterType type;
    std::string key;
    std::string value;

    size_t rangeStart = 0;
    size_t rangeEnd = SIZE_MAX;
    bool hasRange = false;

    std::string toString() const;
    bool matches(const LogLine& line) const;
    bool matches(const LogLine& line, size_t lineIndex) const;

    // Cheap candidate test: returns true if `key=value` or
    // `key="value"` is present in the raw line at all.  If false, we
    // can skip the regex-based parse entirely.  Returns true for TEXT
    // filters (matches() uses raw find on the line).
    bool candidateInRaw(const std::string& rawLine) const {
        if (type == FilterType::TEXT) return true;
        // Try the unquoted shape first (op=0, err=0, conn=700635…)
        // then the quoted shape (base="ou=…", dn="…", filter="…").
        // We can't know which shape the calling site stripped, so
        // accept both as candidates.
        if (rawLine.find(key + "=" + value) != std::string::npos) return true;
        if (rawLine.find(key + "=\"" + value + "\"") != std::string::npos) return true;
        return false;
    }
};

class FilterStack {
public:
    void push(const Filter& filter);
    void pop();
    void clear();
    
    bool empty() const { return filters_.empty(); }
    size_t size() const { return filters_.size(); }
    
    const std::vector<Filter>& getFilters() const { return filters_; }
    
    bool matches(const LogLine& line) const;
    bool matches(const LogLine& line, size_t lineIndex) const;
    bool candidateInRaw(const std::string& rawLine) const;
    
    bool hasConnRange() const;
    size_t getRangeStart() const;
    size_t getRangeEnd() const;
    
private:
    std::vector<Filter> filters_;
};
