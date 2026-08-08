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

// gmtime_r()/mktime() are POSIX; -std=c++17 (strict ISO) hides them
// unless a feature-test macro is defined before any system header.
#define _POSIX_C_SOURCE 200809L

#include "log_parser.h"
#include <regex>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cerrno>

Token LogParser::createToken(TokenType type, const std::string& value, size_t start, size_t end) {
    return Token{type, value, start, end};
}

LogLine LogParser::parseLine(const std::string& line) {
    LogLine logLine;
    logLine.raw = convertTimestamps(line);
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

    // Thread-id: OpenLDAP debug/access logs print the emitting thread as
    // `0x…` right after the timestamp (`<rfc3339> <thread> conn=...`).
    // Only match that head-of-line position so pointer addresses that
    // appear *inside* the message body (e.g. `_csn: queueing 0x...`) are
    // not treated as a thread.  The pattern also only fires on the
    // decoded rfc3339 form, so syslog peer lines (hostname follows the
    // stamp) never match.
    static const std::regex thread_regex(
        R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+Z\s+(0x[0-9a-fA-F]{8,16}))");
    std::string::const_iterator searchStart = line.cbegin();
    while (std::regex_search(searchStart, line.cend(), match, thread_regex)) {
        size_t threadStart = std::distance(line.cbegin(), searchStart) + match.position(1);
        logLine.tokens.push_back(createToken(TokenType::THREAD_ID, match[1], threadStart, threadStart + match[1].length()));
        logLine.thread_id = match[1].str();
        searchStart = match.suffix().first;
    }

    searchStart = line.cbegin();
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

namespace {

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
long long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

std::string formatRfc3339Utc(time_t t, const std::string& frac) {
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tmv);
    return std::string(buf) + "." + frac + "Z";
}

} // namespace

// Convert the timestamp prefix according to the configured input format.
// The returned string is the line that should be tokenized, so offsets
// computed by extractTokens() stay coherent with the rendered text.
std::string LogParser::convertTimestamps(const std::string& line) const {
    switch (format_) {
        case LogFormat::RFC3339_UTC:
            return line;

        case LogFormat::DEBUG:
            return convertDebugHex(line);

        case LogFormat::SYSLOG_UTC:
            return convertSyslog(line, /*local*/ false);

        case LogFormat::SYSLOG_LOCALTIME:
            return convertSyslog(line, /*local*/ true);

        case LogFormat::AUTO:
        default:
            // Auto-detection: convert whichever unambiguous prefix the
            // line has.  rfc3339 is left untouched; syslog UTC vs
            // localtime cannot be told apart without a flag, so AUTO
            // treats syslog as UTC.
            if (looksLikeDebugHex(line)) return convertDebugHex(line);
            if (looksLikeSyslog(line)) return convertSyslog(line, /*local*/ false);
            return line;
    }
}

bool LogParser::looksLikeDebugHex(const std::string& line) const {
    static const std::regex debug_prefix(R"(^[0-9a-fA-F]+\.[0-9a-fA-F]{5,8}[ \t]+)");
    std::smatch m;
    return std::regex_search(line, m, debug_prefix) && m.position(0) == 0;
}

bool LogParser::looksLikeSyslog(const std::string& line) const {
    static const std::regex syslog_prefix(
        R"(^[A-Za-z]{3}[ ]+[0-9]{1,2}[ ]+[0-9]{2}:[0-9]{2}:[0-9]{2}[ ]+)");
    std::smatch m;
    return std::regex_search(line, m, syslog_prefix) && m.position(0) == 0;
}

std::string LogParser::convertDebugHex(const std::string& line) const {
    static const std::regex debug_prefix(
        R"(^([0-9a-fA-F]+)\.([0-9a-fA-F]{5,8})[ \t]+)");
    std::smatch m;
    if (!std::regex_search(line, m, debug_prefix) || m.position(0) != 0) return line;

    const std::string secStr = m[1].str();
    const std::string fracStr = m[2].str();

    errno = 0;
    char* endp = nullptr;
    const unsigned long sec = strtoul(secStr.c_str(), &endp, 16);
    if (errno != 0 || endp != secStr.c_str() + secStr.size()) return line;

    errno = 0;
    const unsigned long long frac = strtoull(fracStr.c_str(), &endp, 16);
    if (errno != 0 || endp != fracStr.c_str() + fracStr.size()) return line;

    char fracOut[16];
    if (fracStr.size() <= 5) {
        // Microseconds (gettimeofday path, `%05x`).
        snprintf(fracOut, sizeof fracOut, "%06llu", frac);
    } else {
        // Nanoseconds (clock_gettime path, `%08x`).
        snprintf(fracOut, sizeof fracOut, "%09llu", frac);
    }

    const size_t tsLen = secStr.size() + 1 + fracStr.size();

    size_t pos = tsLen;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    const size_t threadStart = pos;
    while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') ++pos;
    const std::string thread = line.substr(threadStart, pos - threadStart);
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    const std::string rest = line.substr(pos);

    std::string out = formatRfc3339Utc((time_t)sec, fracOut);
    if (!thread.empty()) out += " " + thread;
    if (!rest.empty()) out += " " + rest;
    return out;
}

std::string LogParser::convertSyslog(const std::string& line, bool local) const {
    static const std::regex syslog_prefix(
        R"(^([A-Za-z]{3})[ ]+([0-9]{1,2})[ ]+([0-9]{2}):([0-9]{2}):([0-9]{2})[ ]+)");
    std::smatch m;
    if (!std::regex_search(line, m, syslog_prefix) || m.position(0) != 0) return line;

    static const char* months[] = {
        "jan", "feb", "mar", "apr", "may", "jun",
        "jul", "aug", "sep", "oct", "nov", "dec"
    };
    const std::string monStr = m[1].str();
    int mon = -1;
    for (int i = 0; i < 12; ++i) {
        if (std::equal(months[i], months[i] + 3, monStr.begin(),
                       [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); })) {
            mon = i;
            break;
        }
    }
    if (mon < 0) return line;

    const int day = atoi(m[2].str().c_str());
    const int hh = atoi(m[3].str().c_str());
    const int mm = atoi(m[4].str().c_str());
    const int ss = atoi(m[5].str().c_str());

    time_t now = time(nullptr);
    struct tm nowTm;
    gmtime_r(&now, &nowTm);
    int year = nowTm.tm_year + 1900;
    // Syslog has no year; if this month is ahead of now, the log entry is
    // from the previous year.
    if (mon > nowTm.tm_mon) --year;
    if (year < 1970) year = 1970;

    time_t epoch;
    if (local) {
        struct tm tmv = {};
        tmv.tm_year = year - 1900;
        tmv.tm_mon = mon;
        tmv.tm_mday = day;
        tmv.tm_hour = hh;
        tmv.tm_min = mm;
        tmv.tm_sec = ss;
        tmv.tm_isdst = -1;
        epoch = mktime(&tmv);
    } else {
        epoch = daysFromCivil(year, mon + 1, day) * 86400 + hh * 3600 + mm * 60 + ss;
    }

    const size_t tsLen = m.length(0);
    return formatRfc3339Utc(epoch, "000000000") + " " + line.substr(tsLen);
}
