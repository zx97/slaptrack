#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// Minimal test harness with no external dependencies.
//
// Tests register themselves via the TEST() macro (static initializer),
// CHECK/CHECK_EQ record failures, and run_all() returns 0 on success.

namespace testfw {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> reg;
    return reg;
}

inline int& failureCount() {
    static int count = 0;
    return count;
}

inline int& runningTests() {
    static int count = 0;
    return count;
}

inline void fail(const char* file, int line, const std::string& expr) {
    failureCount()++;
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, expr.c_str());
}

inline int run_all() {
    int failed = 0;
    for (auto& tc : registry()) {
        int before = failureCount();
        tc.fn();
        if (failureCount() != before) {
            std::fprintf(stderr, "  FAILED: %s\n", tc.name);
            failed++;
        }
        runningTests()++;
    }
    std::fprintf(stderr, "\n%d/%zu tests passed, %d assertion failures\n",
                 runningTests() - failed, registry().size(), failureCount());
    return failed == 0 ? 0 : 1;
}

} // namespace testfw

#define TEST(name)                                                       \
    static void test_##name();                                           \
    static const bool test_##name##_reg = []() {                         \
        testfw::registry().push_back({#name, &test_##name});             \
        return true;                                                     \
    }();                                                                 \
    static void test_##name()

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            testfw::fail(__FILE__, __LINE__, #cond);                     \
        }                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                   \
    do {                                                                 \
        auto va = (a);                                                   \
        auto vb = (b);                                                   \
        if (!(va == vb)) {                                               \
            testfw::fail(__FILE__, __LINE__,                             \
                         std::string(#a " == " #b));                     \
        }                                                                \
    } while (0)
