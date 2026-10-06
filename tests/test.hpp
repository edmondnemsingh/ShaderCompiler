// Tiny self-registering test framework (keeps the project free of external test dependencies).
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace sptest {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace sptest

#define SP_CAT2(a, b) a##b
#define SP_CAT(a, b) SP_CAT2(a, b)
#define TEST_CASE(name)                                                         \
    static void SP_CAT(test_fn_, __LINE__)();                                   \
    static sptest::Registrar SP_CAT(test_reg_, __LINE__)(name, SP_CAT(test_fn_, __LINE__)); \
    static void SP_CAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::fprintf(stderr, "  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++sptest::failures();                                                       \
        }                                                                               \
    } while (0)

#define REQUIRE(cond)                                                                     \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "  %s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #cond); \
            ++sptest::failures();                                                         \
            return;                                                                       \
        }                                                                                 \
    } while (0)
