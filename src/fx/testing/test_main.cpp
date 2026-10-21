#include "fx/testing/test.hpp"

#include <chrono>
#include <cstring>
#include <exception>

namespace transmute::fx::testing {
namespace {
int failures_in_test = 0;
const char* running = "";
} // namespace

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void report_failure(const char* file, int line, const std::string& message) {
    ++failures_in_test;
    std::fprintf(stderr, "  FAIL %s (%s:%d): %s\n", running, file, line, message.c_str());
}

int current_failures() { return failures_in_test; }

} // namespace transmute::fx::testing

int main(int argc, char** argv) {
    using namespace transmute::fx::testing;
    const char* filter = nullptr;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) filter = argv[++i];
        else if (std::strcmp(argv[i], "--list") == 0) list = true;
    }
    int failed = 0, ran = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const auto& test : registry()) {
        if (filter && !std::strstr(test.name, filter)) continue;
        if (list) { std::printf("%s\n", test.name); continue; }
        running = test.name;
        failures_in_test = 0;
        std::printf("[ RUN  ] %s\n", test.name);
        std::fflush(stdout);
        try {
            test.body();
        } catch (const std::exception& error) {
            report_failure(__FILE__, __LINE__, std::string("exception: ") + error.what());
        } catch (...) {
            report_failure(__FILE__, __LINE__, "unknown exception");
        }
        ++ran;
        if (failures_in_test != 0) { ++failed; std::printf("[ FAIL ] %s\n", test.name); }
        else std::printf("[  OK  ] %s\n", test.name);
        std::fflush(stdout);
    }
    if (!list) {
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("%d/%d tests passed in %.3f s\n", ran - failed, ran, seconds);
    }
    return failed > 255 ? 255 : failed;
}
