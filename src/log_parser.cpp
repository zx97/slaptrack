// log_parser.cpp

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

#include "log_parser.h"
#include <regex>
#include <algorithm>

Token LogParser::createToken(TokenType type, const std::string& value, size_t start, size_t end) {
    return Token{type, value, start, end};
}

LogLine LogParser::parseLine(const std::string& line) {
    LogLine logLine;
    logLine.raw = line;
    extractTokens(logLine);
    return logLine;
}

void LogParser::extractTokens(LogLine& logLine) {
    const std::string& line = logLine.raw;

    // Static regex objects: constructing a std::regex compiles the
    // pattern (slow).  Re-using the same object across all lines
    // turns 300K * 16 = 4.8M compiles into 16.
    static const std::regex timestamp_regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+Z)");
    static const std::regex conn_regex(R"(conn=(\d+))");
    static const std::regex op_regex(R"(op=(\d+))");
    static const std::regex dn_regex(R"xx(dn="([^"]*)")xx");
    static const std::regex filter_regex(R"xx(filter="([^"]*)")xx");
    static const std::regex base_regex(R"xx(base="([^"]*)")xx");
    static const std::regex ip_regex(R"(IP=(\d+\.\d+\.\d+\.\d+:\d+))");
    static const std::regex err_regex(R"(err=(\d+))");
    static const std::regex fd_regex(R"(fd=(\d+))");
    static const std::regex tag_regex(R"(tag=(\d+))");
    static const std::regex etime_regex(R"(etime=([0-9.]+))");
    static const std::regex qtime_regex(R"(qtime=([0-9.]+))");
    static const std::regex nentries_regex(R"(nentries=(\d+))");
    static const std::regex scope_regex(R"(scope=(\d+))");
    static const std::regex deref_regex(R"(deref=(\d+))");
    static const std::regex attr_regex(R"(attr=(\S+(?:\s\S+)*))");

    std::smatch match;

    if (std::regex_search(line, match, timestamp_regex)) {
        logLine.tokens.push_back(createToken(TokenType::TIMESTAMP, match[0], match.position(0), match.position(0) + match.length(0)));
    }

    std::string::const_iterator searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, conn_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        logLine.tokens.push_back(createToken(TokenType::CONN_ID, match[0], start, start + match.length(0)));
        logLine.conn_id = match[1].str();
        searchStart = match.suffix().first;
    }

    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, op_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        logLine.tokens.push_back(createToken(TokenType::OP_ID, match[0], start, start + match.length(0)));
        logLine.op_id = match[1].str();
        searchStart = match.suffix().first;
    }

    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, dn_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        logLine.tokens.push_back(createToken(TokenType::DN_VALUE, match[0], start, start + match.length(0)));
        logLine.dn = match[1].str();
        searchStart = match.suffix().first;
    }

    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, filter_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        logLine.tokens.push_back(createToken(TokenType::FILTER_VALUE, match[0], start, start + match.length(0)));
        logLine.filter = match[1].str();
        searchStart = match.suffix().first;
    }

    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, base_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        logLine.tokens.push_back(createToken(TokenType::BASE, match[0], start, start + match.length(0)));
        logLine.base = match[1].str();
        searchStart = match.suffix().first;
    }

    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, ip_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        logLine.tokens.push_back(createToken(TokenType::IP_ADDRESS, match[0], start, start + match.length(0)));
        searchStart = match.suffix().first;
    }

    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, err_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        logLine.tokens.push_back(createToken(TokenType::ERROR_CODE, match[0], start, start + match.length(0)));
        logLine.error_code = match[1].str();
        searchStart = match.suffix().first;
    }

    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, fd_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        logLine.tokens.push_back(createToken(TokenType::FD_NUM, match[0], start, start + match.length(0)));
        searchStart = match.suffix().first;
    }

    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, tag_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        logLine.tokens.push_back(createToken(TokenType::TAG, match[0], start, start + match.length(0)));
        searchStart = match.suffix().first;
    }

    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, etime_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        logLine.tokens.push_back(createToken(TokenType::ETIME_VAL, match[0], start, start + match.length(0)));
        searchStart = match.suffix().first;
    }

    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, qtime_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        logLine.tokens.push_back(createToken(TokenType::QTIME_VAL, match[0], start, start + match.length(0)));
        searchStart = match.suffix().first;
    }

    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, scope_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        logLine.tokens.push_back(createToken(TokenType::SCOPE, match[0], start, start + match.length(0)));
        searchStart = match.suffix().first;
    }

    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, deref_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        logLine.tokens.push_back(createToken(TokenType::DEREF, match[0], start, start + match.length(0)));
        searchStart = match.suffix().first;
    }

    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, attr_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        size_t end = start + match.length(0);
        // Split into the "attr=" keyword and the attribute list value so
        // they can be colored independently.  "attr=" is 5 characters.
        logLine.tokens.push_back(createToken(TokenType::ATTR, "attr=", start, start + 5));
        logLine.tokens.push_back(createToken(TokenType::ATTR_LIST, match[1].str(), start + 5, end));
        searchStart = match.suffix().first;
    }

    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, nentries_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        logLine.tokens.push_back(createToken(TokenType::NENTRIES, match[0], start, start + match.length(0)));
        searchStart = match.suffix().first;
    }

    // Multi-word keyword: "SEARCH RESULT"
    static const std::regex search_result_regex(R"(SEARCH RESULT)");
    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, search_result_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        logLine.tokens.push_back(createToken(TokenType::KEYWORD, match[0], start, start + match.length(0)));
        searchStart = match.suffix().first;
    }

    // Single-word keywords with \b boundaries.  Use a single combined
    // alternation regex so we only do one search per line.
    static const std::regex keyword_regex(
        R"(\b(?:ACCEPT|TLS|BIND|SRCH|RESULT|UNBIND|closed|method=|mech=|text=)\b)");
    searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, keyword_regex)) {
        size_t start = std::distance(line.cbegin(), searchStart) + match.position(0);
        size_t end = start + match.length(0);

        bool overlaps = false;
        for (const auto& existing : logLine.tokens) {
            if (!(end <= existing.start_pos || start >= existing.end_pos)) {
                overlaps = true;
                break;
            }
        }

        if (!overlaps) {
            logLine.tokens.push_back(createToken(TokenType::KEYWORD, match[0], start, end));
        }
        searchStart = match.suffix().first;
    }

    std::sort(logLine.tokens.begin(), logLine.tokens.end(), [](const Token& a, const Token& b) {
        return a.start_pos < b.start_pos;
    });
}
