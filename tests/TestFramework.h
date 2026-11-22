// Tiny dependency-free test harness.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace bstest {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace bstest

#define BS_TEST(name)                                              \
    static void bs_test_##name();                                  \
    static bstest::Registrar bs_reg_##name(#name, bs_test_##name); \
    static void bs_test_##name()

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            ++bstest::failures();                                                   \
        }                                                                           \
    } while (0)

#define CHECK_MSG(cond, ...)                                                        \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("    FAILED %s:%d: %s -- ", __FILE__, __LINE__, #cond);     \
            std::printf(__VA_ARGS__);                                               \
            std::printf("\n");                                                      \
            ++bstest::failures();                                                   \
        }                                                                           \
    } while (0)

#define REQUIRE(cond)                                                               \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("    FAILED %s:%d: %s (aborting test)\n", __FILE__, __LINE__, #cond); \
            ++bstest::failures();                                                   \
            return;                                                                 \
        }                                                                           \
    } while (0)
