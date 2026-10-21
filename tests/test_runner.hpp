#pragma once

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace transmute::test_detail {

// Thrown when the machine cannot exercise what a test checks (no GPU context,
// workers that never ran at the same time): the runner reports the test as
// skipped, with the reason, instead of passed or failed. Not a runtime_error,
// so expect_failure-style handlers do not mistake it for a guest failure.
class TestSkipped : public std::exception {
public:
    explicit TestSkipped(std::string reason) : reason_(std::move(reason)) {}
    const char* what() const noexcept override { return reason_.c_str(); }

private:
    std::string reason_;
};

[[noreturn]] inline void skip_test(std::string reason) { throw TestSkipped(std::move(reason)); }

// Whether worker threads happened to overlap depends on the processor count,
// the load and the scheduler, not on the code under test. A test records each
// case's overlap and calls finish() after all of its correctness checks: a miss
// skips the test with the reason. TRANSMUTE_TEST_REQUIRE_OVERLAP=1 makes a miss
// a failure, for machines that must show overlap.
class OverlapCheck {
public:
    explicit OverlapCheck(std::string what) : what_(std::move(what)) {}

    void observe(bool overlapped, std::string detail = {}) {
        ++cases_;
        if (overlapped) return;
        if (missed_++ == 0) first_ = std::move(detail);
    }

    void finish() const {
        if (missed_ == 0) return;
        const auto reason = what_ + " did not overlap in " + std::to_string(missed_) + " of " +
                            std::to_string(cases_) + " cases" +
                            (first_.empty() ? std::string() : " (first: " + first_ + ")") + " with " +
                            std::to_string(std::thread::hardware_concurrency()) +
                            " logical processors; the correctness checks passed";
        if (const char* strict = std::getenv("TRANSMUTE_TEST_REQUIRE_OVERLAP"); strict && *strict == '1')
            throw std::runtime_error(reason + " (TRANSMUTE_TEST_REQUIRE_OVERLAP=1)");
        skip_test(reason + ", so the overlap check is skipped");
    }

private:
    std::string what_;
    std::string first_;
    unsigned cases_ = 0;
    unsigned missed_ = 0;
};

// Each shard is a separate process: tests may change process-wide Windows/CRT
// state, so running their bodies concurrently inside one process is unsafe.
class TestRunner {
public:
    void configure(int& argc, char**& argv) {
        unsigned consumed = 0;
        if (argc >= 2 && std::string_view(argv[1]) == "--list-tests") {
            list_ = true;
            consumed = 1;
        } else if (argc >= 2 && std::string_view(argv[1]) == "--test") {
            if (argc < 3) throw std::runtime_error("--test requires a test name");
            selected_ = argv[2];
            consumed = 2;
        } else if (argc >= 2 && std::string_view(argv[1]) == "--shard") {
            if (argc < 4) throw std::runtime_error("--shard requires INDEX COUNT");
            shard_ = number(argv[2]);
            shards_ = number(argv[3]);
            if (shards_ == 0 || shards_ > 256 || shard_ >= shards_)
                throw std::runtime_error("invalid test shard: require INDEX < COUNT <= 256");
            consumed = 3;
        }
        argc -= static_cast<int>(consumed);
        argv += consumed;
    }

    template<class Test>
    void run(std::string_view name, Test&& test) {
        const auto index = available_++;
        if (list_) {
            std::cout << name << '\n';
            return;
        }
        if ((!selected_.empty() && name != selected_) || index % shards_ != shard_)
            return;
        ++executed_;
        std::cout << "[ RUN      ] " << name << std::endl;
        const auto start = std::chrono::steady_clock::now();
        try {
            test();
        } catch (const TestSkipped& skipped) {
            ++skipped_;
            std::cout << "[  SKIPPED ] " << name << ": " << skipped.what() << std::endl;
            return;
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string(name) + ": " + error.what());
        }
        const auto seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "[       OK ] " << name << " (" << seconds << "s)\n";
    }

    void finish() const {
        if (!selected_.empty() && executed_ == 0)
            throw std::runtime_error("unknown or unavailable test: " + std::string(selected_));
        if (!list_)
            std::cout << "Passed " << executed_ - skipped_ << " tests, skipped " << skipped_
                      << ", shard " << shard_ << '/' << shards_ << '\n';
    }

private:
    static unsigned number(std::string_view value) {
        unsigned result{};
        const auto [end, error] = std::from_chars(value.data(),
                                                value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size())
            throw std::runtime_error("test shard arguments must be unsigned integers");
        return result;
    }

    std::string_view selected_;
    unsigned shard_ = 0;
    unsigned shards_ = 1;
    unsigned available_ = 0;
    unsigned executed_ = 0;
    unsigned skipped_ = 0;
    bool list_ = false;
};

} // namespace transmute::test_detail
