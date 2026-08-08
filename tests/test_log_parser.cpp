#include "test_framework.h"
#include "log_parser.h"

#include <algorithm>

static LogLine parse(const std::string& line) {
    LogParser parser;
    return parser.parseLine(line);
}

TEST(parse_empty_line) {
    LogLine l = parse("");
    CHECK(l.raw.empty());
    CHECK(l.tokens.empty());
    CHECK(!l.conn_id.has_value());
    CHECK(!l.dn.has_value());
}

TEST(parse_timestamp) {
    LogLine l = parse("2024-01-15T10:30:00.123Z conn=1000 fd=15 ACCEPT");
    CHECK(!l.tokens.empty());
    CHECK(l.tokens[0].type == TokenType::TIMESTAMP);
    CHECK_EQ(l.tokens[0].value, "2024-01-15T10:30:00.123Z");
    CHECK_EQ(l.tokens[0].start_pos, 0u);
    CHECK_EQ(l.tokens[0].end_pos, 24u);
}

TEST(parse_conn_id) {
    LogLine l = parse("conn=1234 op=5");
    CHECK(l.conn_id.has_value());
    CHECK_EQ(l.conn_id.value(), "1234");
}

TEST(parse_conn_id_multiple) {
    // A summary line can mention conn= twice ("conn=1000 nentries=... conn=1000")
    LogLine l = parse("conn=7 junk conn=8 done");
    // Last occurrence wins for the stored conn_id
    CHECK(l.conn_id.has_value());
    CHECK_EQ(l.conn_id.value(), "8");
}

TEST(parse_op_id) {
    LogLine l = parse("conn=1 op=42 RESULT");
    CHECK(l.op_id.has_value());
    CHECK_EQ(l.op_id.value(), "42");
}

TEST(parse_dn) {
    LogLine l = parse("conn=1 op=0 BIND dn=\"uid=jdoe,ou=People,dc=example,dc=org\" method=128");
    CHECK(l.dn.has_value());
    CHECK_EQ(l.dn.value(), "uid=jdoe,ou=People,dc=example,dc=org");
}

TEST(parse_dn_no_quote_suffix) {
    LogLine l = parse("dn=\"cn=admin,dc=example\"");
    CHECK(l.dn.has_value());
    CHECK_EQ(l.dn.value(), "cn=admin,dc=example");
}

TEST(parse_filter) {
    LogLine l = parse("SRCH base=\"dc=example\" filter=\"(uid=alice)\" scope=2");
    CHECK(l.filter.has_value());
    CHECK_EQ(l.filter.value(), "(uid=alice)");
}

TEST(parse_base) {
    LogLine l = parse("SRCH base=\"dc=example,dc=com\" filter=\"(uid=*)\"");
    CHECK(l.base.has_value());
    CHECK_EQ(l.base.value(), "dc=example,dc=com");
}

TEST(parse_error_code) {
    LogLine l = parse("RESULT tag=97 err=49 nentries=0");
    CHECK(l.error_code.has_value());
    CHECK_EQ(l.error_code.value(), "49");
}

TEST(parse_ip_address) {
    LogLine l = parse("conn=1000 fd=15 ACCEPT from IP=192.168.1.1:38900");
    bool found = false;
    for (const auto& t : l.tokens) {
        if (t.type == TokenType::IP_ADDRESS) {
            CHECK_EQ(t.value, "IP=192.168.1.1:38900");
            found = true;
        }
    }
    CHECK(found);
}

TEST(parse_tokens_sorted) {
    // Tokens must be in increasing start_pos order regardless of the
    // order the regex passes ran in.
    LogLine l = parse("conn=9 op=1 BIND dn=\"cn=a\" err=3 tag=97");
    for (size_t i = 1; i < l.tokens.size(); ++i) {
        CHECK(l.tokens[i - 1].start_pos <= l.tokens[i].start_pos);
    }
}

TEST(parse_token_bounds_within_raw) {
    LogLine l = parse("conn=9 op=1 BIND dn=\"cn=a\" err=3");
    for (const auto& t : l.tokens) {
        CHECK(t.start_pos < t.end_pos);
        CHECK(t.end_pos <= l.raw.size());
    }
}

TEST(parse_multiword_keyword) {
    LogLine l = parse("SRCH base=\"\" filter=\"(objectClass=*)\"");
    bool found = false;
    for (const auto& t : l.tokens) {
        if (t.type == TokenType::KEYWORD && t.value == "SRCH") found = true;
    }
    CHECK(found);
}

TEST(parse_attr_split) {
    LogLine l = parse("conn=1 op=3 attr=cn uid");
    bool attrKw = false, attrList = false;
    for (const auto& t : l.tokens) {
        if (t.type == TokenType::ATTR) attrKw = true;
        if (t.type == TokenType::ATTR_LIST) {
            CHECK_EQ(t.value, "cn uid");
            attrList = true;
        }
    }
    CHECK(attrKw);
    CHECK(attrList);
}

TEST(parse_raw_preserved) {
    const std::string raw = "2024-01-15T10:30:00.123Z conn=10 op=1 STRMAC";
    LogLine l = parse(raw);
    CHECK_EQ(l.raw, raw);
}

TEST(parse_nentries) {
    LogLine l = parse("RESULT tag=97 err=0 nentries=12");
    bool found = false;
    for (const auto& t : l.tokens) {
        if (t.type == TokenType::NENTRIES) {
            CHECK_EQ(t.value, "nentries=12");
            found = true;
        }
    }
    CHECK(found);
}

// ---------------------------------------------------------------------------
// Log format conversion (--log-format)
// ---------------------------------------------------------------------------

TEST(format_default_keeps_rfc3339) {
    const std::string line = "2024-01-15T10:30:00.123Z conn=7 op=1 BIND";
    LogLine l = parse(line);
    CHECK_EQ(l.raw, line);
}

TEST(format_auto_detects_debug_hex) {
    // sec=1, frac 0x12345 = 74565 usec (5-digit frac → usec path)
    LogParser p;
    p.setLogFormat(LogFormat::AUTO);
    LogLine l = p.parseLine("1.12345 conn=2 fd=12 ACCEPT");
    CHECK_EQ(l.raw, "1970-01-01T00:00:01.074565Z conn=2 fd=12 ACCEPT");
    CHECK_EQ(l.tokens[0].type, TokenType::TIMESTAMP);
    CHECK_EQ(l.tokens[0].value, "1970-01-01T00:00:01.074565Z");
}

TEST(format_debug_hex_nanoseconds) {
    // sec=1, frac 0x1 = 1 ns (7-digit frac → ns path)
    LogParser p;
    p.setLogFormat(LogFormat::DEBUG);
    LogLine l = p.parseLine("1.0000001 conn=3 fd=5");
    CHECK_EQ(l.raw, "1970-01-01T00:00:01.000000001Z conn=3 fd=5");
}

TEST(format_debug_keeps_thread_id) {
    LogParser p;
    p.setLogFormat(LogFormat::DEBUG);
    LogLine l = p.parseLine("1.12345 0x7f9c1e33a700 conn=3");
    CHECK_EQ(l.raw, "1970-01-01T00:00:01.074565Z 0x7f9c1e33a700 conn=3");
}

TEST(format_syslog_utc) {
    // Syslog has no year; the decoder uses the current year (decrementing
    // if the month is in the future).  find() is used because the rfc3339
    // prefix starts with the year, so "T12:34:56" is not at index 0.
    LogParser p;
    p.setLogFormat(LogFormat::SYSLOG_UTC);
    LogLine l = p.parseLine("Nov  9 12:34:56 host slapd[1]: conn=4");
    CHECK(l.raw.find("T12:34:56.000000000Z") != std::string::npos);
    CHECK_EQ(l.raw.substr(l.raw.find(' ') + 1), "host slapd[1]: conn=4");
    CHECK_EQ(l.tokens[0].type, TokenType::TIMESTAMP);
    CHECK_EQ(l.tokens[0].value, l.raw.substr(0, l.raw.find(' ')));
}

TEST(format_syslog_localtime_keeps_suffix) {
    // SYSLOG_LOCALTIME is conceptually "treat the timestamp as local
    // time", so the rendered epoch depends on the system TZ.  Only the
    // invariant parts are asserted: a valid rfc3339 prefix and that the
    // text after the timestamp is preserved verbatim.
    LogParser p;
    p.setLogFormat(LogFormat::SYSLOG_LOCALTIME);
    LogLine l = p.parseLine("Nov  9 12:00:56 host=1");
    CHECK(l.raw.rfind("T", 0) == std::string::npos);  // rfc3339 has a year prefix, not plain "T"
    CHECK(l.raw.size() > 20);
    CHECK_EQ(l.raw.substr(l.raw.find(' ') + 1), "host=1");
}

TEST(format_does_not_touch_plain_lines) {
    // A line that is neither debug-hex nor syslog passes through unchanged
    // even in explicit DEBUG mode (no false positive on "conn=" logs).
    LogParser p;
    p.setLogFormat(LogFormat::DEBUG);
    LogLine l = p.parseLine("conn=1 op=0 RESULT");
    CHECK_EQ(l.raw, "conn=1 op=0 RESULT");
}