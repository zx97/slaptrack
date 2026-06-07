// log_parser.h

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
#include <map>
#include <optional>

enum class TokenType {
    TIMESTAMP,
    HOSTNAME,
    PROCESS,
    CONN_ID,
    OP_ID,
    KEYWORD,
    DN_VALUE,
    FILTER_VALUE,
    IP_ADDRESS,
    ERROR_CODE,
    PLAIN_TEXT,
    FD_NUM,
    TAG,
    ETIME_VAL,
    QTIME_VAL,
    NENTRIES,
    BASE,
    SCOPE,
    DEREF,
    ATTR,
    ATTR_LIST
};

struct Token {
    TokenType type;
    std::string value;
    size_t start_pos;
    size_t end_pos;
};

struct LogLine {
    std::string raw;
    std::vector<Token> tokens;
    std::optional<std::string> conn_id;
    std::optional<std::string> op_id;
    std::optional<std::string> dn;
    std::optional<std::string> filter;
    std::optional<std::string> base;
    std::optional<std::string> error_code;
};

class LogParser {
public:
    LogParser() = default;
    
    LogLine parseLine(const std::string& line);
    
    static std::vector<LogLine> parseFile(const std::string& filename);
    
private:
    void extractTokens(LogLine& logLine);
    Token createToken(TokenType type, const std::string& value, size_t start, size_t end);
};
