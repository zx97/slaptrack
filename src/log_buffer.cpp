// log_buffer.cpp

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

#include "log_buffer.h"
#include <algorithm>
#include <fstream>
#include <iostream>

LogBuffer::LogBuffer(size_t windowSize) : windowSize_(windowSize) {}

bool LogBuffer::loadFile(const std::string& filename) {
    cache_.clear();
    rawLines_.clear();
    if (!index_.buildIndex(filename)) {
        return false;
    }
    // Bulk-read all lines into memory so filter scans do not fseek
    // for every line they visit.
    return loadRawLines(filename);
}

bool LogBuffer::loadRawLines(const std::string& filename) {
    std::ifstream f(filename, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return false;
    }

    const size_t total = index_.getTotalLines();
    rawLines_.clear();
    rawLines_.reserve(total);

    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        rawLines_.push_back(std::move(line));
    }

    return true;
}

size_t LogBuffer::refreshFile() {
    size_t newLines = index_.refreshIndex();
    if (newLines > 0) {
        // Append the new lines to rawLines_ so the filter sees them.
        const size_t prev = rawLines_.size();
        rawLines_.resize(index_.getTotalLines());
        // We need the actual text.  Reopen and seek to where we left off.
        std::ifstream f(index_.getFilename(), std::ios::binary);
        if (f.is_open()) {
            f.seekg(prev > 0 ? index_.getLineOffset(prev) : 0);
            for (size_t i = prev; i < rawLines_.size(); i++) {
                std::string line;
                if (!std::getline(f, line)) break;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                rawLines_[i] = std::move(line);
            }
        }
    }
    return newLines;
}

bool LogBuffer::reopenFile(const std::string& filename) {
    cache_.clear();
    rawLines_.clear();
    if (!index_.reopen(filename)) {
        return false;
    }
    return loadRawLines(filename);
}

size_t LogBuffer::getTotalLines() const {
    return index_.getTotalLines();
}

const std::string& LogBuffer::getRawLine(size_t index) const {
    static const std::string empty;
    if (index >= rawLines_.size()) {
        return empty;
    }
    return rawLines_[index];
}

std::optional<LogLine> LogBuffer::getLine(size_t index) {
    if (index >= rawLines_.size()) {
        return std::nullopt;
    }
    
    auto it = cache_.find(index);
    if (it != cache_.end()) {
        return it->second;
    }
    
    const std::string& rawLine = rawLines_[index];
    if (rawLine.empty()) {
        return std::nullopt;
    }
    
    LogLine parsed = parser_.parseLine(rawLine);
    cache_[index] = parsed;
    
    if (cache_.size() > windowSize_ * 2) {
        size_t evictCount = cache_.size() - windowSize_;
        auto evictIt = cache_.begin();
        for (size_t i = 0; i < evictCount && evictIt != cache_.end(); i++) {
            evictIt = cache_.erase(evictIt);
        }
    }
    
    return parsed;
}

void LogBuffer::prefetchAround(size_t centerIndex, int radius) {
    size_t start = (centerIndex > (size_t)radius) ? centerIndex - radius : 0;
    size_t end = std::min(centerIndex + radius, index_.getTotalLines());
    
    for (size_t i = start; i < end; i++) {
        if (cache_.find(i) == cache_.end()) {
            getLine(i);
        }
    }
}

void LogBuffer::clearCache() {
    cache_.clear();
}
