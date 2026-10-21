#pragma once

// Test support for fx_runtime_tests (owner: runtime): configurable stand-ins
// for the translator, seh, loader and abi, and a builder for hand-assembled
// x64 regions that follow the translated-code conventions.

#include "fx/cache/cache.hpp"
#include "fx/common/x64_assembler.hpp"
#include "fx/runtime/runtime.hpp"
#include "fx/runtime/runtime_internal.hpp"
#include "fx/translator/translator.hpp"
#include "transmute/fx/process.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <vector>

namespace rtt {

using namespace transmute::fx;
using x64::Gp;
using x64::Label;
using x64::ptr;

// ---- translate_region stand-in ---------------------------------------------------------
// Default: an "interpret-only" region (one entry that exits with interpret).
// mini_translator: also lowers `movz x0, #imm; ret` (used by the SMC test).
using RegionFactory = std::function<TranslatedRegion(std::uint64_t pc)>;
void set_region_factory(std::uint64_t pc, RegionFactory factory);
void set_mini_translator(bool enabled);
// Mini-translator regions get `bytes` of int3 after their code (fills arena chunks).
void set_mini_padding(std::size_t bytes);
TranslatedRegion interpret_only_region(std::uint64_t pc);
extern std::atomic<int> g_translate_calls;

// ---- dispatch_guest_exception stand-in ----------------------------------------------------
// The handler decides how to resume. Without one, the stand-in resumes at
// kReturnToHostPc with x0 = kUnhandledMarker | code (call_guest returns it).
inline constexpr std::uint64_t kUnhandledMarker = 0xEEEE'0000'0000'0000ULL;
using ExceptionHandler = std::function<void(GuestContext&, const GuestExceptionRecord&)>;
void set_exception_handler(ExceptionHandler handler);
std::vector<GuestExceptionRecord> exception_log();
void clear_exception_log();

// ---- loader stand-ins ------------------------------------------------------------------------
extern std::atomic<int> g_attach_calls;
extern std::atomic<int> g_detach_calls;
void set_fake_module(const GuestModule* module);
void set_activation_context(void* context);   // returned by guest_activation_context()

// ---- cache / abi stand-ins ----------------------------------------------------------------------
extern std::atomic<int> g_cache_stores;
extern std::atomic<int> g_cache_configures;
extern CacheConfig g_cache_config;
// cache_lookup stand-in: regions "stored" for a pc (nullptr factory removes).
void set_cached_region(std::uint64_t pc, RegionFactory factory);
extern std::atomic<int> g_cache_lookups;
extern std::atomic<bool> g_last_lazy;   // TranslateOptions::lazy of the last translate_region call
// cache_region_starts stand-in: the cached region starts of a fake module.
void set_cached_starts(std::uint64_t base, std::vector<std::uint64_t> starts);
// Calls the handler the runtime registered with cache_set_module_ready_handler
// (false if none).
bool notify_module_ready(std::uint64_t base, std::uint64_t sequence);
void add_abi_frame(const AbiCodeFrame& frame);   // reported by abi_code_frame()
// install_system_helpers stand-in fills helpers[48..63] with marker + id.
inline constexpr std::uint64_t kSystemHelperMarker = 0x5157'0000'0000ULL;

// ---- helpers -----------------------------------------------------------------------------------
inline constexpr std::int32_t d_pc = ctx_disp(offsetof(GuestContext, pc));
inline constexpr std::int32_t d_exit_reason = ctx_disp(offsetof(GuestContext, exit_reason));
inline constexpr std::int32_t d_stub_indirect = ctx_disp(offsetof(GuestContext, stub_indirect));
inline constexpr std::int32_t d_stub_link = ctx_disp(offsetof(GuestContext, stub_link));
inline constexpr std::int32_t d_stub_exit = ctx_disp(offsetof(GuestContext, stub_dispatch_exit));
inline constexpr std::int32_t d_rsb = ctx_disp(offsetof(GuestContext, rsb));
inline constexpr std::int32_t d_rsb_top = ctx_disp(offsetof(GuestContext, rsb_top));

// Guest code in a fresh RW allocation registered as guest code memory.
class GuestCode {
public:
    explicit GuestCode(std::initializer_list<std::uint32_t> words, std::size_t size = 0x4000);
    ~GuestCode();
    GuestCode(const GuestCode&) = delete;
    GuestCode& operator=(const GuestCode&) = delete;
    std::uint64_t at(std::size_t index) const { return base_ + index * 4; }
    std::uint64_t base() const { return base_; }
    std::uint32_t* words() const { return reinterpret_cast<std::uint32_t*>(base_); }

private:
    std::uint64_t base_{};
    std::size_t size_{};
};

// Builds a TranslatedRegion from hand-written x64 that follows the
// translated-code conventions (flushed entry state, exits through the stubs).
class RegionBuilder {
public:
    explicit RegionBuilder(std::uint64_t guest_start);
    x64::Assembler& as() { return a_; }
    void entry(std::uint64_t pc);
    // ctx->pc = pc (imm64 with a fixup); clobbers rax.
    void set_pc(std::uint64_t pc);
    // Unlinked direct exit; returns the region offset of the rel32 field.
    std::uint32_t direct_exit(std::uint64_t target);
    // ctx->pc = x30; jmp stub_indirect.
    void return_via_x30();
    // ctx->pc = target; jmp stub_indirect.
    void indirect_exit(std::uint64_t target);
    // BL: push {return_pc, host continuation}, x30 = return_pc, direct exit to target.
    void call(std::uint64_t return_pc, Label continuation, std::uint64_t target);
    // RET through the RSB; a hit adds 1 to guest x[hit_counter].
    void ret(unsigned hit_counter = 9);
    void cover(std::uint64_t begin, std::uint64_t end) { region_.guest_ranges.emplace_back(begin, end); }
    void fault_site(std::size_t host_begin, std::size_t host_end, std::uint64_t guest_pc,
                    FlagsLocation flags = FlagsLocation::context, std::uint8_t flags_register = 0);
    void pin(unsigned guest, Gp host) { region_.pins.push_back({static_cast<std::uint8_t>(guest), static_cast<std::uint8_t>(x64::id(host)), false}); }
    // Attaches a fault map (guest registers held in host registers there) to the last fault site.
    void fault_map(std::initializer_list<PinnedRegister> regs) {
        region_.fault_maps.push_back({static_cast<std::uint32_t>(region_.fault_map_regs.size()),
                                      static_cast<std::uint32_t>(regs.size())});
        region_.fault_map_regs.insert(region_.fault_map_regs.end(), regs.begin(), regs.end());
        region_.faults.back().map = static_cast<std::uint16_t>(region_.fault_maps.size());
    }
    TranslatedRegion finish();

private:
    struct Tail {
        Label label;
        std::uint64_t target;
        std::uint32_t field;
    };
    x64::Assembler a_;
    Label start_;
    TranslatedRegion region_;
    std::vector<Tail> tails_;
};

// Hand-written host entry in the arena: x0 += value; return via x30.
const void* make_add_host_entry(std::uint64_t value);

double now_ns();

} // namespace rtt
