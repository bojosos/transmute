// Re-runs the differential suites with Programs emitted through
// CoreLikeContext (the core RegionEmitter's register constraints) under
// several pin layouts, with and without the EmitContext register hooks
// (owner: atomics/system).
#include "fx/translator/emit_system_test_util.hpp"

#include "fx/testing/test.hpp"

#include <cstring>
#include <exception>
#include <string>

using namespace transmute::fx;
using namespace transmute::fx::systest;
using x64::Gp;

namespace {

ProgramMode make_mode(const char* name, bool hooks, std::initializer_list<std::pair<unsigned, Gp>> pins) {
    ProgramMode mode;
    mode.core_like = true;
    mode.name = name;
    mode.core.hooks = hooks;
    for (const auto& [guest, host] : pins) mode.core.pins[guest] = static_cast<std::int8_t>(x64::id(host));
    return mode;
}

bool selected(const char* name, bool hooks, bool with_stress) {
    const std::string n = name;
    if (n.rfind("core_like", 0) == 0 || n.rfind("bench_", 0) == 0 || n == "debug_dump_emitted") return false;
    if (n.rfind("stress_", 0) == 0) return with_stress;
    // CASP has 5 operands: without the hooks it cannot fit 7 scratch registers
    // when nothing is pinned (documented limitation, see NOTES_SYSTEM.md).
    if (!hooks && n == "atomic_casp_pairs") return false;
    if (n == "system_counter_is_monotonic_and_runs_at_cntfrq") return false;   // timing only
    return n.rfind("atomic_", 0) == 0 || n.rfind("fusion_", 0) == 0 || n.rfind("system_", 0) == 0 ||
           n.rfind("crypto_", 0) == 0;
}

void run_suites(const ProgramMode& mode, bool with_stress) {
    ProgramModeScope scope(mode);
    for (const auto& test : testing::registry()) {
        if (!selected(test.name, mode.core.hooks, with_stress)) continue;
        const int before = testing::current_failures();
        try {
            test.body();
        } catch (const std::exception& error) {
            testing::report_failure(__FILE__, __LINE__, std::string(mode.name) + " / " + test.name + ": " + error.what());
        }
        if (testing::current_failures() != before) {
            std::printf("    %s / %s failed\n", mode.name, test.name);
        }
    }
}

} // namespace

FX_TEST(core_like_no_pins_with_hooks) {
    run_suites(make_mode("no pins, hooks", true, {}), false);
}

FX_TEST(core_like_no_pins_without_hooks) {
    run_suites(make_mode("no pins, no hooks", false, {}), false);
}

FX_TEST(core_like_low_registers_pinned) {
    const auto pins = {std::pair{0u, Gp::rbx}, std::pair{1u, Gp::rbp}, std::pair{2u, Gp::rsi}, std::pair{8u, Gp::rdi},
                       std::pair{9u, Gp::r12}, std::pair{31u, Gp::r13}, std::pair{30u, Gp::r14}};
    run_suites(make_mode("pins A, hooks", true, pins), true);
    run_suites(make_mode("pins A, no hooks", false, pins), false);
}

FX_TEST(core_like_other_registers_pinned) {
    run_suites(make_mode("pins B, hooks", true,
                         {{4u, Gp::rbx}, {6u, Gp::rbp}, {10u, Gp::rsi}, {12u, Gp::rdi}, {3u, Gp::r12},
                          {5u, Gp::r13}, {7u, Gp::r14}}),
               false);
    run_suites(make_mode("pins C, hooks", true,
                         {{6u, Gp::rbx}, {2u, Gp::rbp}, {30u, Gp::rsi}, {0u, Gp::rdi}, {8u, Gp::r12},
                          {12u, Gp::r13}, {1u, Gp::r14}}),
               false);
    run_suites(make_mode("pins C, no hooks", false,
                         {{6u, Gp::rbx}, {2u, Gp::rbp}, {30u, Gp::rsi}, {0u, Gp::rdi}, {8u, Gp::r12},
                          {12u, Gp::r13}, {1u, Gp::r14}}),
               false);
}
