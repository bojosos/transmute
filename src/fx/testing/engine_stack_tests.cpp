// Configurable guest stacks in the reference engine (main, resized, workers).
#include "fx/testing/test.hpp"
#include "transmute/a64.hpp"
#include "transmute/pe_image.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

using transmute::ExecutionStart;
using transmute::PeImage;
using transmute::ReferenceEngine;

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kCode = 0x0000000200000000ULL;

// mov x1,#0xc00000; sub x2,sp,x1; mov x3,#0x55; str x3,[x2]; ldr x0,[x2]; ret
constexpr std::uint32_t kDeepStackProgram[] = {
    0xd2a01801u, 0xcb2163e2u, 0xd2800aa3u, 0xf9000043u, 0xf9400040u, 0xd65f03c0u,
};

void map_program(ReferenceEngine& engine) {
    engine.map_memory(kCode, 4096);
    for (std::size_t i = 0; i < std::size(kDeepStackProgram); ++i)
        engine.write32(kCode + i * 4, kDeepStackProgram[i]);
    engine.set_memory_access(kCode, true, false, true);
}

std::uint64_t stack_bytes(const ReferenceEngine& engine) {
    const auto [low, high] = engine.stack_bounds();
    return high - low;
}

} // namespace

FX_TEST(engine_default_stack_is_one_mib) {
    ReferenceEngine engine(ExecutionStart{kCode});
    FX_CHECK_EQ(stack_bytes(engine), kMiB);
    map_program(engine);
    bool faulted = false;
    try {
        static_cast<void>(engine.run(100));
    } catch (const std::exception&) {
        faulted = true;   // 12 MiB below SP is outside a 1 MiB stack
    }
    FX_CHECK(faulted);
}

FX_TEST(engine_sixteen_mib_main_stack_runs_deep_access) {
    ReferenceEngine engine(ExecutionStart{kCode, 16 * kMiB});
    FX_CHECK_EQ(stack_bytes(engine), 16 * kMiB);
    map_program(engine);
    const auto result = engine.run(100);
    FX_CHECK_EQ(result.exit_code, 0x55u);
}

FX_TEST(engine_resize_main_stack_before_workers) {
    ReferenceEngine engine(ExecutionStart{kCode});
    engine.resize_stack(16 * kMiB);
    FX_CHECK_EQ(stack_bytes(engine), 16 * kMiB);
    map_program(engine);
    FX_CHECK_EQ(engine.run(100).exit_code, 0x55u);
    // Sizes round up to 64 KiB and are capped at 1 GiB.
    ReferenceEngine rounded(ExecutionStart{kCode, 100000});
    FX_CHECK_EQ(stack_bytes(rounded), 128 * 1024ULL);
    bool rejected = false;
    try {
        ReferenceEngine huge(ExecutionStart{kCode, 2048 * kMiB});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    FX_CHECK(rejected);
}

FX_TEST(engine_worker_stacks_are_sized_and_disjoint) {
    ReferenceEngine engine(ExecutionStart{kCode, 16 * kMiB});
    auto a = engine.fork_thread(kCode, 0, 16 * kMiB);
    auto b = engine.fork_thread(kCode, 0);            // process default (1 MiB)
    engine.set_default_worker_stack_size(4 * kMiB);
    auto c = engine.fork_thread(kCode, 0);
    FX_CHECK_EQ(stack_bytes(*a), 16 * kMiB);
    FX_CHECK_EQ(stack_bytes(*b), kMiB);
    FX_CHECK_EQ(stack_bytes(*c), 4 * kMiB);
    const auto [main_low, main_high] = engine.stack_bounds();
    const auto [a_low, a_high] = a->stack_bounds();
    const auto [b_low, b_high] = b->stack_bounds();
    const auto [c_low, c_high] = c->stack_bounds();
    FX_CHECK(a_high < main_low);   // gaps below every stack
    FX_CHECK(b_high < a_low);
    FX_CHECK(c_high < b_low);
    (void)main_high;
    bool rejected = false;
    try {
        engine.resize_stack(32 * kMiB);
    } catch (const std::logic_error&) {
        rejected = true;   // main stack is fixed once workers exist
    }
    FX_CHECK(rejected);
    // A 16 MiB worker can touch 12 MiB below its stack pointer.
    map_program(engine);
    auto deep = engine.fork_thread(kCode, 0, 16 * kMiB);
    FX_CHECK_EQ(deep->run(100).exit_code, 0x55u);
}

FX_TEST(pe_image_reports_stack_reserve) {
    const std::filesystem::path blender =
        "C:/dev/transmute/artifacts/arm64-stage/Blender Foundation/Blender 4.5/blender.exe";
    const std::filesystem::path small = "C:/dev/transmute/build/return42_arm64.exe";
    for (const auto& [path, expected] : {std::pair{blender, 2 * kMiB}, std::pair{small, kMiB}}) {
        if (!std::filesystem::exists(path)) continue;
        std::ifstream file(path, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
        const auto image = PeImage::parse(bytes);
        FX_CHECK_EQ(image.stack_reserve(), expected);
    }
}
