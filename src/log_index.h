// log_index.h

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

#include <string>
#include <vector>
#include <cstdint>
#include <fstream>

class LogIndex {
public:
    LogIndex();
    ~LogIndex();
    
    bool buildIndex(const std::string& filename);
    size_t refreshIndex();

    // Close the current file (if any) and re-build the index from
    // scratch against a new file path.  Used for log rotation
    // recovery in follow mode.
    bool reopen(const std::string& filename);
    
    size_t getTotalLines() const { return lineOffsets_.size(); }
    uint64_t getLineOffset(size_t lineIndex) const;
    size_t getLineLength(size_t lineIndex) const;
    
    std::string readLine(size_t lineIndex) const;
    
    const std::string& getFilename() const { return filename_; }
    
private:
    std::string filename_;
    std::vector<uint64_t> lineOffsets_;
    mutable std::ifstream file_;
    uint64_t fileSize_;
};
