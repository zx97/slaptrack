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