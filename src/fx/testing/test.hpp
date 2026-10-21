#pragma once

// Minimal test framework for fx component tests (owner: integration).
//
//   FX_TEST(name) { FX_CHECK(cond); FX_CHECK_EQ(a, b); }
//
// Each test executable links fx_testing (which provides main). Options:
//   --filter <substring>   run tests whose name contains the substring
//   --list                 list test names
// Exit code is the number of failed tests (capped at 255).

#include <cstdint>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace transmute::fx::testing {

struct TestCase {
    const char* name;
    void (*body)();
};

std::vector<TestCase>& registry();
struct Registrar {
    Registrar(const char* name, void (*body)()) { registry().push_back({name, body}); }
};

// Records a failure for the running test; the test continues.
void report_failure(const char* file, int line, const std::string& message);
// Number of failures recorded so far in the running test.
int current_failures();

template <typename T>
std::string show(const T& value) {
    std::ostringstream out;
    if constexpr (std::is_integral_v<T> && sizeof(T) > 1) {
        out << value << " (0x" << std::hex << static_cast<std::uint64_t>(value) << ")";
    } else {
        out << value;
    }
    return out.str();
}

} // namespace transmute::fx::testing

#define FX_TEST_CONCAT2(a, b) a##b
#define FX_TEST_CONCAT(a, b) FX_TEST_CONCAT2(a, b)
#define FX_TEST(name)                                                              \
    static void FX_TEST_CONCAT(fx_test_body_, name)();                             \
    static ::transmute::fx::testing::Registrar FX_TEST_CONCAT(fx_test_reg_, name)( \
        #name, &FX_TEST_CONCAT(fx_test_body_, name));                              \
    static void FX_TEST_CONCAT(fx_test_body_, name)()

#define FX_CHECK(cond)                                                                         \
    do {                                                                                       \
        if (!(cond)) ::transmute::fx::testing::report_failure(__FILE__, __LINE__, #cond);      \
    } while (0)

#define FX_CHECK_EQ(a, b)                                                                      \
    do {                                                                                       \
        const auto& fx_a_ = (a);                                                               \
        const auto& fx_b_ = (b);                                                               \
        if (!(fx_a_ == fx_b_))                                                                 \
            ::transmute::fx::testing::report_failure(__FILE__, __LINE__,                       \
                std::string(#a " == " #b ": ") + ::transmute::fx::testing::show(fx_a_) +       \
                " vs " + ::transmute::fx::testing::show(fx_b_));                               \
    } while (0)

#define FX_REQUIRE(cond)                                                                       \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            ::transmute::fx::testing::report_failure(__FILE__, __LINE__, #cond);               \
            return;                                                                            \
        }                                                                                      \
    } while (0)
