#include "transmute/a64.hpp"
#include "transmute/jit.hpp"

#include <barrier>
#include <chrono>
#include <exception>
#include <iostream>
#include <thread>
#include <vector>

namespace {

void put(std::vector<std::uint8_t>& bytes, std::size_t offset,
         std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

transmute::PeImage loop_image(bool stores) {
    std::vector<std::uint32_t> words{0xf9400002U}; // ldr x2,[x0]
    for (unsigned i = 0; i < 14; ++i) words.push_back(0x8b020063U); // add x3,x3,x2
    if (stores) words.push_back(0xf9000403U); // str x3,[x0,#8]
    words.push_back(0xd1000421U); // sub x1,x1,#1
    const auto branch_pc = static_cast<std::int32_t>(words.size());
    words.push_back(0xb5000001U | ((static_cast<std::uint32_t>(-branch_pc) &
                                   0x7ffffU) << 5)); // cbnz x1,loop
    words.push_back(0xaa0303e0U); // mov x0,x3
    words.push_back(0xd65f03c0U);
    std::vector<std::uint8_t> bytes(0x400);
    put(bytes, 0, 0x5a4d, 2);
    put(bytes, 0x3c, 0x80, 4);
    put(bytes, 0x80, 0x4550, 4);
    put(bytes, 0x84, 0xaa64, 2);
    put(bytes, 0x86, 1, 2);
    put(bytes, 0x94, 0xf0, 2);
    put(bytes, 0x98, 0x20b, 2);
    put(bytes, 0xa8, 0x1000, 4);
    put(bytes, 0xb0, 0x140000000ULL, 8);
    put(bytes, 0xb8, 0x1000, 4);
    put(bytes, 0xbc, 0x200, 4);
    put(bytes, 0xd0, 0x2000, 4);
    put(bytes, 0xd4, 0x200, 4);
    put(bytes, 0x104, 16, 4);
    bytes[0x188] = '.';
    bytes[0x189] = 't';
    bytes[0x18a] = 'e';
    bytes[0x18b] = 'x';
    bytes[0x18c] = 't';
    put(bytes, 0x190, 0x200, 4);
    put(bytes, 0x194, 0x1000, 4);
    put(bytes, 0x198, 0x200, 4);
    put(bytes, 0x19c, 0x200, 4);
    put(bytes, 0x1ac, 0x60000020, 4);
    for (std::size_t i = 0; i < words.size(); ++i) put(bytes, 0x200 + 4 * i, words[i], 4);
    return transmute::PeImage::parse(bytes);
}

void measure(bool stores, unsigned workers, std::uint64_t iterations) {
    auto image = loop_image(stores);
    transmute::ReferenceEngine parent(image);
    parent.set_jit_enabled(true);
    parent.set_instruction_limit(100'000'000);
    constexpr std::uint64_t data = 0x501100000ULL;
    parent.map_memory(data, workers * 4096);
    std::vector<std::unique_ptr<transmute::ReferenceEngine>> engines;
    for (unsigned i = 0; i < workers; ++i) {
        parent.write64(data + i * 4096, 1);
        engines.push_back(parent.fork_thread(image.entry(), 0));
    }
    for (unsigned i = 0; i < workers; ++i) {
        const std::array<std::uint64_t, 4> args{data + i * 4096, 16, 0, 0};
        if (engines[i]->invoke_callback(image.entry(), args) != 224 ||
            engines[i]->jit_stats().compiled_native_regions == 0) {
            throw std::runtime_error("worker benchmark did not warm its native loop");
        }
    }
    std::barrier start(static_cast<std::ptrdiff_t>(workers + 1));
    std::vector<std::thread> threads;
    std::vector<std::exception_ptr> failures(workers);
    std::vector<std::uint64_t> results(workers);
    for (unsigned i = 0; i < workers; ++i) {
        threads.emplace_back([&, i] {
            start.arrive_and_wait();
            try {
                const std::array<std::uint64_t, 4> args{data + i * 4096, iterations, 0, 0};
                results[i] = engines[i]->invoke_callback(image.entry(), args);
            } catch (...) {
                failures[i] = std::current_exception();
            }
        });
    }
    const auto begin = std::chrono::steady_clock::now();
    start.arrive_and_wait();
    for (auto& thread : threads) thread.join();
    const auto ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin).count();
    for (unsigned i = 0; i < workers; ++i) {
        if (failures[i]) std::rethrow_exception(failures[i]);
        if (results[i] != 14 * iterations) throw std::runtime_error("worker result mismatch");
    }
    std::cout << (stores ? "store" : "read") << ',' << workers << ','
              << iterations << ',' << ms << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto iterations = argc > 1 ? std::stoull(argv[1]) : 2'000'000ULL;
        const auto rounds = argc > 2 ? std::stoul(argv[2]) : 3UL;
        std::cout << "mode,workers,iterations,wall_ms\n";
        for (unsigned round = 0; round < rounds; ++round) {
            for (const bool stores : {false, true}) {
                for (const unsigned workers : {1U, 2U, 4U, 8U}) {
                    measure(stores, workers, iterations);
                }
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
