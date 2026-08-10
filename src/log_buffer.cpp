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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    cache_.clear();
    rawLines_.clear();
    if (!index_.buildIndex(filename)) {
        return false;
    }
    // Load the first window so the initial screen renders without a
    // disk round-trip; the rest of the file is read only on demand.
    loadRawWindow(0);
    return true;
}

void LogBuffer::loadRawWindow(size_t startLine) const {
    const size_t total = index_.getTotalLines();
    if (startLine >= total) {
        rawPageStart_ = startLine;
        rawLines_.clear();
        return;
    }

    rawPageStart_ = startLine;
    const size_t count = std::min(kRawWindowLines, total - startLine);
    std::ifstream f(index_.getFilename(), std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "Error: Could not open file " << index_.getFilename() << std::endl;
        rawLines_.clear();
        return;
    }
    f.seekg(index_.getLineOffset(startLine));

    rawLines_.clear();
    rawLines_.reserve(count);
    std::string line;
    while (rawLines_.size() < count && std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        rawLines_.push_back(std::move(line));
    }
}

size_t LogBuffer::refreshFile() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    size_t newLines = index_.refreshIndex();
    if (newLines == 0) {
        return 0;
    }
    // The file grew: shift the raw window so it covers the end of the
    // file where follow mode keeps reading.  Lines behind the window
    // are re-read from disk only if the user actually scrolls to them.
    const size_t total = index_.getTotalLines();
    const size_t pageStart = total > kRawWindowLines ? total - kRawWindowLines : 0;
    if (pageStart != rawPageStart_) {
        loadRawWindow(pageStart);
    } else {
        rawLines_.resize(total - rawPageStart_);
        std::ifstream f(index_.getFilename(), std::ios::binary);
        if (f.is_open()) {
            f.seekg(index_.getLineOffset(rawPageStart_ + rawLines_.size()));
            std::string line;
            while (rawLines_.size() < total - rawPageStart_ && std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                rawLines_.push_back(std::move(line));
            }
        }
    }
    return newLines;
}

bool LogBuffer::reopenFile(const std::string& filename) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    cache_.clear();
    rawLines_.clear();
    if (!index_.reopen(filename)) {
        return false;
    }
    loadRawWindow(0);
    return true;
}

size_t LogBuffer::getTotalLines() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return index_.getTotalLines();
}

std::optional<std::string> LogBuffer::getRawLine(size_t index) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const size_t total = index_.getTotalLines();
    if (index >= total) {
        return std::nullopt;
    }
    if (index < rawPageStart_ || index >= rawPageStart_ + rawLines_.size()) {
        loadRawWindow(index);
    }
    const size_t offset = index - rawPageStart_;
    if (offset >= rawLines_.size()) {
        return std::nullopt;
    }
    return rawLines_[offset];
}

std::vector<std::string> LogBuffer::getRawLines(size_t startLine, size_t count) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const size_t total = index_.getTotalLines();
    if (startLine >= total) {
        return {};
    }
    const size_t n = std::min(count, total - startLine);
    std::vector<std::string> out;
    out.reserve(n);
    std::ifstream f(index_.getFilename(), std::ios::binary);
    if (!f.is_open()) {
        return out;
    }
    f.seekg(index_.getLineOffset(startLine));
    std::string line;
    while (out.size() < n && std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(std::move(line));
    }
    return out;
}

std::optional<LogLine> LogBuffer::getLine(size_t index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (index >= index_.getTotalLines()) {
        return std::nullopt;
    }
    
    auto it = cache_.find(index);
    if (it != cache_.end()) {
        return it->second;
    }
    
    std::optional<std::string> rawLine = getRawLine(index);
    if (!rawLine) {
        return std::nullopt;
    }
    
    LogLine parsed = parser_.parseLine(*rawLine);
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    size_t start = (centerIndex > (size_t)radius) ? centerIndex - radius : 0;
    size_t end = std::min(centerIndex + radius, index_.getTotalLines());
    
    for (size_t i = start; i < end; i++) {
        if (cache_.find(i) == cache_.end()) {
            getLine(i);
        }
    }
}

void LogBuffer::clearCache() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    cache_.clear();
}
