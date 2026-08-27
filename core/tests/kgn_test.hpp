// A minimal zero-dependency test harness.
//
// Deliberately not Catch2 or GoogleTest: both want either a network fetch at
// configure time or a vendored copy, and neither is worth it for a test suite
// that asserts on a pure state machine. The whole framework is below, and
// swapping it for a real one later is mechanical -- the assertions read the
// same.
//
// Usage:
//     KGN_TEST(name_of_the_thing_being_asserted) {
//         KGN_CHECK(condition);
//         KGN_CHECK_EQ(actual, expected);
//     }
//     int main() { return kgn::test::runAll(); }

#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace kgn::test {

struct Failure {
    std::string test;
    std::string detail;
};

inline std::vector<Failure>& failures() {
    static std::vector<Failure> f;
    return f;
}

inline std::string& currentTest() {
    static std::string name;
    return name;
}

struct Case {
    std::string name;
    std::function<void()> body;
};

inline std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        cases().push_back({name, std::move(body)});
    }
};

inline void fail(const std::string& detail) {
    failures().push_back({currentTest(), detail});
}

template <typename A, typename B>
void checkEq(const A& actual, const B& expected, const char* actualExpr,
             const char* expectedExpr, const char* file, int line) {
    if (actual == expected) return;
    std::ostringstream out;
    out << file << ":" << line << ": " << actualExpr << " == " << expectedExpr
        << "\n    actual:   " << actual << "\n    expected: " << expected;
    fail(out.str());
}

inline void check(bool ok, const char* expr, const char* file, int line) {
    if (ok) return;
    std::ostringstream out;
    out << file << ":" << line << ": " << expr;
    fail(out.str());
}

inline int runAll() {
    std::size_t failed = 0;
    for (auto& c : cases()) {
        currentTest() = c.name;
        const std::size_t before = failures().size();
        c.body();
        if (failures().size() > before) {
            ++failed;
            std::printf("FAIL  %s\n", c.name.c_str());
        } else {
            std::printf("ok    %s\n", c.name.c_str());
        }
    }
    for (const auto& f : failures()) {
        std::printf("\n%s\n  %s\n", f.test.c_str(), f.detail.c_str());
    }
    std::printf("\n%zu tests, %zu failed\n", cases().size(), failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace kgn::test

#define KGN_TEST(name)                                                     \
    static void kgn_test_##name();                                         \
    static ::kgn::test::Registrar kgn_reg_##name(#name, kgn_test_##name);  \
    static void kgn_test_##name()

#define KGN_CHECK(expr) ::kgn::test::check((expr), #expr, __FILE__, __LINE__)

#define KGN_CHECK_EQ(a, b) \
    ::kgn::test::checkEq((a), (b), #a, #b, __FILE__, __LINE__)
