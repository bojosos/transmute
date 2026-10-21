#pragma once

#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace transmute::test_detail {

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
            std::cout << "Passed " << executed_ << " tests, shard " << shard_
                      << '/' << shards_ << '\n';
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
    bool list_ = false;
};

} // namespace transmute::test_detail
