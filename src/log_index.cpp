// log_index.cpp

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

#include "log_index.h"
#include <iostream>

LogIndex::LogIndex() : fileSize_(0) {}

LogIndex::~LogIndex() {
    if (file_.is_open()) {
        file_.close();
    }
}

bool LogIndex::buildIndex(const std::string& filename) {
    filename_ = filename;
    lineOffsets_.clear();
    
    file_.open(filename, std::ios::binary);
    if (!file_.is_open()) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return false;
    }
    
    file_.seekg(0, std::ios::end);
    fileSize_ = file_.tellg();
    file_.seekg(0, std::ios::beg);
    
    lineOffsets_.reserve(fileSize_ / 80);
    
    lineOffsets_.push_back(0);
    
    const size_t BUFFER_SIZE = 1024 * 1024;
    std::vector<char> buffer(BUFFER_SIZE);
    uint64_t filePos = 0;
    
    while (file_.read(buffer.data(), BUFFER_SIZE) || file_.gcount() > 0) {
        size_t bytesRead = file_.gcount();
        
        for (size_t i = 0; i < bytesRead; i++) {
            if (buffer[i] == '\n') {
                lineOffsets_.push_back(filePos + i + 1);
            }
        }
        
        filePos += bytesRead;
    }
    
    if (!lineOffsets_.empty() && lineOffsets_.back() >= fileSize_) {
        lineOffsets_.pop_back();
    }
    
    file_.clear();
    file_.seekg(0, std::ios::beg);
    
    return true;
}

bool LogIndex::reopen(const std::string& filename) {
    if (file_.is_open()) {
        file_.close();
    }
    lineOffsets_.clear();
    fileSize_ = 0;
    return buildIndex(filename);
}

size_t LogIndex::refreshIndex() {
    if (!file_.is_open()) return 0;
    
    file_.clear();
    file_.seekg(0, std::ios::end);
    uint64_t newSize = file_.tellg();
    
    if (newSize <= fileSize_) {
        return 0;
    }
    
    size_t newLines = 0;
    uint64_t startPos = fileSize_;
    
    if (lineOffsets_.empty()) {
        lineOffsets_.push_back(0);
        startPos = 0;
    }
    
    file_.seekg(startPos);
    
    const size_t BUFFER_SIZE = 64 * 1024;
    std::vector<char> buffer(BUFFER_SIZE);
    uint64_t filePos = startPos;
    
    while (filePos < newSize) {
        size_t toRead = std::min((uint64_t)BUFFER_SIZE, newSize - filePos);
        file_.read(buffer.data(), toRead);
        size_t bytesRead = file_.gcount();
        
        for (size_t i = 0; i < bytesRead; i++) {
            if (buffer[i] == '\n') {
                uint64_t newLineStart = filePos + i + 1;
                if (newLineStart < newSize) {
                    lineOffsets_.push_back(newLineStart);
                    newLines++;
                }
            }
        }
        
        filePos += bytesRead;
    }
    
    fileSize_ = newSize;
    
    return newLines;
}

uint64_t LogIndex::getLineOffset(size_t lineIndex) const {
    if (lineIndex >= lineOffsets_.size()) {
        return fileSize_;
    }
    return lineOffsets_[lineIndex];
}

size_t LogIndex::getLineLength(size_t lineIndex) const {
    if (lineIndex >= lineOffsets_.size()) {
        return 0;
    }
    uint64_t start = lineOffsets_[lineIndex];
    uint64_t end = (lineIndex + 1 < lineOffsets_.size()) ? lineOffsets_[lineIndex + 1] : fileSize_;
    return end - start;
}

std::string LogIndex::readLine(size_t lineIndex) const {
    if (lineIndex >= lineOffsets_.size()) {
        return "";
    }
    
    uint64_t offset = lineOffsets_[lineIndex];
    size_t length = getLineLength(lineIndex);
    
    if (length == 0) return "";
    
    if (length > 0 && lineOffsets_[lineIndex + (lineIndex + 1 < lineOffsets_.size() ? 1 : 0)] > offset) {
        if (lineIndex + 1 < lineOffsets_.size()) {
            length--;
        }
    }
    
    std::string result(length, '\0');
    file_.seekg(offset);
    file_.read(&result[0], length);
    
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    
    return result;
}
