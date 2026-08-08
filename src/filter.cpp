// filter.cpp

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

#include "filter.h"

std::string Filter::toString() const {
    switch (type) {
        case FilterType::CONN: return "conn=" + value;
        case FilterType::DN: return "dn=" + value;
        case FilterType::OP: return "op=" + value;
        case FilterType::BASE: return "base=" + value;
        case FilterType::ERROR_CODE: return "err=" + value;
        case FilterType::TEXT: return "text:" + value;
    }
    return value;
}

bool Filter::matches(const LogLine& line) const {
    switch (type) {
        case FilterType::CONN:
            return line.conn_id.has_value() && line.conn_id.value() == value;
        case FilterType::DN:
            return line.dn.has_value() && line.dn.value() == value;
        case FilterType::OP:
            return line.op_id.has_value() && line.op_id.value() == value;
        case FilterType::BASE:
            return line.base.has_value() && line.base.value() == value;
        case FilterType::ERROR_CODE:
            return line.error_code.has_value() && line.error_code.value() == value;
        case FilterType::TEXT:
            return line.raw.find(value) != std::string::npos;
    }
    return false;
}

bool Filter::matches(const LogLine& line, size_t lineIndex) const {
    if (hasRange && (lineIndex < rangeStart || lineIndex > rangeEnd)) {
        return false;
    }
    return matches(line);
}

void FilterStack::push(const Filter& filter) {
    filters_.push_back(filter);
}

void FilterStack::pop() {
    if (!filters_.empty()) {
        filters_.pop_back();
    }
}

void FilterStack::clear() {
    filters_.clear();
}

bool FilterStack::matches(const LogLine& line) const {
    if (filters_.empty()) return true;
    for (const auto& filter : filters_) {
        if (!filter.matches(line)) return false;
    }
    return true;
}

bool FilterStack::matches(const LogLine& line, size_t lineIndex) const {
    if (filters_.empty()) return true;
    for (const auto& filter : filters_) {
        if (!filter.matches(line, lineIndex)) return false;
    }
    return true;
}

// All filters must have a hit in the raw line for a parse to be worth
// doing.  We AND the per-filter candidate tests so we skip parses for
// any line that cannot possibly match the full filter stack.
bool FilterStack::candidateInRaw(const std::string& rawLine) const {
    if (filters_.empty()) return true;
    for (const auto& filter : filters_) {
        if (!filter.candidateInRaw(rawLine)) return false;
    }
    return true;
}

bool FilterStack::hasConnRange() const {
    for (const auto& filter : filters_) {
        if (filter.hasRange) return true;
    }
    return false;
}

size_t FilterStack::getRangeStart() const {
    size_t start = 0;
    for (const auto& filter : filters_) {
        if (filter.hasRange && filter.rangeStart > start) {
            start = filter.rangeStart;
        }
    }
    return start;
}

size_t FilterStack::getRangeEnd() const {
    size_t end = SIZE_MAX;
    for (const auto& filter : filters_) {
        if (filter.hasRange && filter.rangeEnd < end) {
            end = filter.rangeEnd;
        }
    }
    return end;
}
