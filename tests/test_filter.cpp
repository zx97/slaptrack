#include "test_framework.h"
#include "filter.h"

#include <limits>

static LogLine makeLine(FilterType type, const std::string& value) {
    LogLine l;
    l.raw = "raw";
    switch (type) {
        case FilterType::CONN: l.conn_id = value; break;
        case FilterType::DN: l.dn = value; break;
        case FilterType::OP: l.op_id = value; break;
        case FilterType::BASE: l.base = value; break;
        case FilterType::ERROR_CODE: l.error_code = value; break;
        case FilterType::THREAD: l.thread_id = value; break;
        case FilterType::TEXT: l.raw = value; break;
    }
    return l;
}

TEST(filter_conn_matches) {
    Filter f{FilterType::CONN, "conn", "1234"};
    CHECK(f.matches(makeLine(FilterType::CONN, "1234")));
    CHECK(!f.matches(makeLine(FilterType::CONN, "9999")));
    CHECK(!f.matches(LogLine{})); // no conn_id present
}

TEST(filter_dn_matches) {
    Filter f{FilterType::DN, "dn", "uid=jdoe,dc=example"};
    CHECK(f.matches(makeLine(FilterType::DN, "uid=jdoe,dc=example")));
    CHECK(!f.matches(makeLine(FilterType::DN, "uid=other")));
}

TEST(filter_text_matches) {
    Filter f{FilterType::TEXT, "text", "needle"};
    CHECK(f.matches(makeLine(FilterType::TEXT, "haystack needle haystack")));
    CHECK(!f.matches(makeLine(FilterType::TEXT, "haystack")));
}

TEST(filter_thread_matches) {
    Filter f{FilterType::THREAD, "thread", "0x7f9c1e33a700"};
    CHECK(f.matches(makeLine(FilterType::THREAD, "0x7f9c1e33a700")));
    CHECK(!f.matches(makeLine(FilterType::THREAD, "0x7f9c1e33b800")));
    CHECK(!f.matches(LogLine{})); // no thread_id present
}

TEST(candidate_in_raw_thread) {
    Filter f{FilterType::THREAD, "thread", "0x7f9c1e33a700"};
    CHECK(f.candidateInRaw("... 0x7f9c1e33a700 conn=3 ..."));
    CHECK(!f.candidateInRaw("... 0x7f9c1e33b800 conn=3 ..."));
}

TEST(filter_range) {
    Filter f{FilterType::CONN, "conn", "5"};
    f.hasRange = true;
    f.rangeStart = 10;
    f.rangeEnd = 20;
    LogLine l = makeLine(FilterType::CONN, "5");
    CHECK(f.matches(l, 10));
    CHECK(f.matches(l, 20));
    CHECK(!f.matches(l, 9));
    CHECK(!f.matches(l, 21));
}

TEST(filter_without_range_ignores_index) {
    Filter f{FilterType::CONN, "conn", "5"};
    LogLine l = makeLine(FilterType::CONN, "5");
    CHECK(f.matches(l, 0));
    CHECK(f.matches(l, 999999));
}

TEST(candidate_in_raw_conn) {
    Filter f{FilterType::CONN, "conn", "1234"};
    CHECK(f.candidateInRaw("... conn=1234 fd=3 ..."));
    CHECK(f.candidateInRaw("... conn=\"1234\" ..."));
    CHECK(!f.candidateInRaw("... conn=9999 ..."));
}

TEST(candidate_in_raw_text_always_true) {
    Filter f{FilterType::TEXT, "text", "anything"};
    CHECK(f.candidateInRaw("no match here"));
}

TEST(stack_empty_matches_everything) {
    FilterStack s;
    CHECK(s.empty());
    CHECK(s.matches(LogLine{}));
    CHECK(s.matches(LogLine{}, 0));
    CHECK(s.candidateInRaw(""));
}

TEST(stack_push_pop) {
    FilterStack s;
    Filter f{FilterType::CONN, "conn", "1"};
    s.push(f);
    CHECK_EQ(s.size(), 1u);
    s.push(f);
    CHECK_EQ(s.size(), 2u);
    s.pop();
    CHECK_EQ(s.size(), 1u);
    s.clear();
    CHECK(s.empty());
    s.pop(); // pop on empty is a no-op
    CHECK(s.empty());
}

TEST(stack_and_chain) {
    FilterStack s;
    s.push(Filter{FilterType::CONN, "conn", "1"});
    s.push(Filter{FilterType::TEXT, "text", "BIND"});

    LogLine match;
    match.conn_id = "1";
    match.raw = "... BIND ...";
    CHECK(s.matches(match));

    LogLine wrongConn = match;
    wrongConn.conn_id = "2";
    CHECK(!s.matches(wrongConn));

    LogLine wrongText = match;
    wrongText.raw = "... SEARCH ...";
    CHECK(!s.matches(wrongText));
}

TEST(stack_candidate_and_chain) {
    FilterStack s;
    s.push(Filter{FilterType::CONN, "conn", "1"});
    s.push(Filter{FilterType::DN, "dn", "uid=jdoe"});

    CHECK(s.candidateInRaw("conn=1 dn=\"uid=jdoe\" op=0"));
    CHECK(!s.candidateInRaw("conn=1 dn=\"uid=other\""));
    CHECK(!s.candidateInRaw("conn=2 dn=\"uid=jdoe\""));
}

TEST(stack_range_start_max) {
    FilterStack s;
    Filter a{FilterType::CONN, "conn", "1"};
    a.hasRange = true; a.rangeStart = 5; a.rangeEnd = 100;
    Filter b{FilterType::DN, "dn", "x"};
    b.hasRange = true; b.rangeStart = 20; b.rangeEnd = 50;
    s.push(a);
    s.push(b);

    CHECK(s.hasConnRange());
    CHECK_EQ(s.getRangeStart(), 20u); // max of starts
    CHECK_EQ(s.getRangeEnd(), 50u);   // min of ends
}

TEST(stack_no_range) {
    FilterStack s;
    s.push(Filter{FilterType::TEXT, "text", "x"});
    CHECK(!s.hasConnRange());
    CHECK_EQ(s.getRangeStart(), 0u);
    CHECK_EQ(s.getRangeEnd(), std::numeric_limits<size_t>::max());
}

TEST(filter_to_string) {
    std::string a = Filter{FilterType::CONN, "conn", "1"}.toString();
    CHECK_EQ(a, "conn=1");
    std::string b = Filter{FilterType::DN, "dn", "x"}.toString();
    CHECK_EQ(b, "dn=x");
    std::string c = Filter{FilterType::TEXT, "text", "hey"}.toString();
    CHECK_EQ(c, "text:hey");
}