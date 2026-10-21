// Test-only definitions of functions owned by other components, plus the
// test support library (see test_support.hpp).

#include "fx/runtime/tests/test_support.hpp"

#include "fx/cache/cache.hpp"
#include "fx/loader/loader.hpp"
#include "fx/translator/emit_system.hpp"

#include <windows.h>

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace rtt {

std::atomic<int> g_translate_calls{0};
std::atomic<int> g_attach_calls{0};
std::atomic<int> g_detach_calls{0};
std::atomic<int> g_cache_stores{0};
std::atomic<int> g_cache_configures{0};
std::atomic<int> g_cache_lookups{0};
std::atomic<bool> g_last_lazy{false};
CacheConfig g_cache_config;

namespace {

std::mutex g_mutex;
std::unordered_map<std::uint64_t, RegionFactory> g_factories;
std::unordered_map<std::uint64_t, RegionFactory> g_cached;
std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> g_cached_starts;
std::atomic<CacheModuleReadyHandler> g_ready_handler{nullptr};
std::atomic<bool> g_mini{false};
std::atomic<std::size_t> g_mini_padding{0};
ExceptionHandler g_exception_handler;
std::vector<GuestExceptionRecord> g_exceptions;
std::atomic<const GuestModule*> g_fake_module{nullptr};
std::atomic<void*> g_activation{nullptr};
std::vector<AbiCodeFrame> g_abi_frames;

constexpr std::uint32_t kRet = 0xd65f03c0u;

bool is_movz_x0(std::uint32_t word) { return (word & 0xffe0001fu) == 0xd2800000u; }

TranslatedRegion mini_region(std::uint64_t pc, std::uint32_t movz) {
    RegionBuilder b(pc);
    b.entry(pc);
    b.as().mov(64, ptr(Gp::r15, x_disp(0)), static_cast<std::int32_t>((movz >> 5) & 0xffff));
    b.return_via_x30();
    for (std::size_t i = g_mini_padding.load(); i > 0; --i) b.as().db(0xcc);   // never executed
    b.cover(pc, pc + 8);
    return b.finish();
}

} // namespace

void set_region_factory(std::uint64_t pc, RegionFactory factory) {
    std::lock_guard lock(g_mutex);
    if (factory) g_factories[pc] = std::move(factory);
    else g_factories.erase(pc);
}

void set_mini_translator(bool enabled) { g_mini.store(enabled); }
void set_mini_padding(std::size_t bytes) { g_mini_padding.store(bytes); }

void set_cached_region(std::uint64_t pc, RegionFactory factory) {
    std::lock_guard lock(g_mutex);
    if (factory) g_cached[pc] = std::move(factory);
    else g_cached.erase(pc);
}

void set_cached_starts(std::uint64_t base, std::vector<std::uint64_t> starts) {
    std::lock_guard lock(g_mutex);
    if (starts.empty()) g_cached_starts.erase(base);
    else g_cached_starts[base] = std::move(starts);
}

bool notify_module_ready(std::uint64_t base, std::uint64_t sequence) {
    const CacheModuleReadyHandler handler = g_ready_handler.load();
    if (!handler) return false;
    handler(base, sequence);
    return true;
}

TranslatedRegion interpret_only_region(std::uint64_t pc) {
    x64::Assembler a;
    const auto imm = a.mov_imm64(Gp::rax, pc);
    a.mov(64, ptr(Gp::r15, d_pc), Gp::rax);
    a.mov(32, ptr(Gp::r15, d_exit_reason), static_cast<std::int32_t>(ExitReason::interpret));
    a.jmp(ptr(Gp::r15, d_stub_exit));
    a.finalize();
    TranslatedRegion region;
    region.guest_start = pc;
    region.guest_ranges.emplace_back(pc, pc + 4);
    region.code = a.bytes();
    region.entries.push_back({pc, 0});
    region.fixups.push_back({static_cast<std::uint32_t>(imm), FixupKind::guest_address_imm64, pc});
    region.stats.guest_instructions = 1;
    region.stats.interpreted_instructions = 1;
    return region;
}

void set_exception_handler(ExceptionHandler handler) {
    std::lock_guard lock(g_mutex);
    g_exception_handler = std::move(handler);
}

std::vector<GuestExceptionRecord> exception_log() {
    std::lock_guard lock(g_mutex);
    return g_exceptions;
}

void clear_exception_log() {
    std::lock_guard lock(g_mutex);
    g_exceptions.clear();
}

void set_fake_module(const GuestModule* module) { g_fake_module.store(module); }
void add_abi_frame(const AbiCodeFrame& frame) {
    std::lock_guard lock(g_mutex);
    g_abi_frames.push_back(frame);
}
void set_activation_context(void* context) { g_activation.store(context); }

// ---- GuestCode ------------------------------------------------------------------------------
GuestCode::GuestCode(std::initializer_list<std::uint32_t> words, std::size_t size) : size_(size) {
    auto* memory = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!memory) throw std::runtime_error("GuestCode: VirtualAlloc failed");
    std::size_t i = 0;
    for (auto w : words) memory[i++] = w;
    for (; i < size / 4; ++i) memory[i] = 0xd503201fu;   // NOP
    base_ = reinterpret_cast<std::uint64_t>(memory);
    register_guest_code_memory(base_, size_);
}

GuestCode::~GuestCode() {
    unregister_guest_code_memory(base_, size_);
    VirtualFree(reinterpret_cast<void*>(base_), 0, MEM_RELEASE);
}

// ---- RegionBuilder --------------------------------------------------------------------------
RegionBuilder::RegionBuilder(std::uint64_t guest_start) : a_(512) {
    start_ = a_.bound_label();
    region_.guest_start = guest_start;
}

void RegionBuilder::entry(std::uint64_t pc) {
    region_.entries.push_back({pc, static_cast<std::uint32_t>(a_.size())});
}

void RegionBuilder::set_pc(std::uint64_t pc) {
    const auto at = a_.mov_imm64(Gp::rax, pc);
    region_.fixups.push_back({static_cast<std::uint32_t>(at), FixupKind::guest_address_imm64, pc});
    a_.mov(64, ptr(Gp::r15, d_pc), Gp::rax);
}

std::uint32_t RegionBuilder::direct_exit(std::uint64_t target) {
    a_.align_rel32_after(1);
    const Label tail = a_.new_label();
    const auto field = static_cast<std::uint32_t>(a_.jmp(tail));
    tails_.push_back({tail, target, field});
    region_.exits.push_back({field, target});
    return field;
}

void RegionBuilder::return_via_x30() {
    a_.mov(64, Gp::rax, ptr(Gp::r15, x_disp(30)));
    a_.mov(64, ptr(Gp::r15, d_pc), Gp::rax);
    a_.jmp(ptr(Gp::r15, d_stub_indirect));
}

void RegionBuilder::indirect_exit(std::uint64_t target) {
    set_pc(target);
    a_.jmp(ptr(Gp::r15, d_stub_indirect));
}

void RegionBuilder::call(std::uint64_t return_pc, Label continuation, std::uint64_t target) {
    a_.mov(32, Gp::rax, ptr(Gp::r15, d_rsb_top));
    a_.and_(32, Gp::rax, static_cast<std::int32_t>(kRsbEntries - 1));
    a_.shl(32, Gp::rax, 4);
    const auto at = a_.mov_imm64(Gp::rdx, return_pc);
    region_.fixups.push_back({static_cast<std::uint32_t>(at), FixupKind::guest_address_imm64, return_pc});
    a_.mov(64, ptr(Gp::r15, Gp::rax, 1, d_rsb), Gp::rdx);
    a_.mov(64, ptr(Gp::r15, x_disp(30)), Gp::rdx);
    a_.lea(64, Gp::rdx, x64::rip(continuation));
    a_.mov(64, ptr(Gp::r15, Gp::rax, 1, d_rsb + 8), Gp::rdx);
    a_.alu(x64::Alu::add, 32, ptr(Gp::r15, d_rsb_top), 1);
    direct_exit(target);
}

void RegionBuilder::ret(unsigned hit_counter) {
    const Label miss = a_.new_label();
    a_.mov(64, Gp::rcx, ptr(Gp::r15, x_disp(30)));
    a_.mov(32, Gp::rax, ptr(Gp::r15, d_rsb_top));
    a_.sub(32, Gp::rax, 1);
    a_.and_(32, Gp::rax, static_cast<std::int32_t>(kRsbEntries - 1));
    a_.shl(32, Gp::rax, 4);
    a_.cmp(64, Gp::rcx, ptr(Gp::r15, Gp::rax, 1, d_rsb));
    a_.jcc(x64::Cond::ne, miss);
    a_.alu(x64::Alu::sub, 32, ptr(Gp::r15, d_rsb_top), 1);
    a_.alu(x64::Alu::add, 64, ptr(Gp::r15, x_disp(hit_counter)), 1);
    a_.jmp(ptr(Gp::r15, Gp::rax, 1, d_rsb + 8));
    a_.bind(miss);
    a_.mov(64, ptr(Gp::r15, d_pc), Gp::rcx);
    a_.jmp(ptr(Gp::r15, d_stub_indirect));
}

void RegionBuilder::fault_site(std::size_t host_begin, std::size_t host_end, std::uint64_t guest_pc,
                               FlagsLocation flags, std::uint8_t flags_register) {
    region_.faults.push_back({static_cast<std::uint32_t>(host_begin), static_cast<std::uint32_t>(host_end),
                              guest_pc, flags, flags_register});
}

TranslatedRegion RegionBuilder::finish() {
    for (const auto& tail : tails_) {
        a_.bind(tail.label);
        const auto at = a_.mov_imm64(Gp::rax, tail.target);
        region_.fixups.push_back({static_cast<std::uint32_t>(at), FixupKind::guest_address_imm64, tail.target});
        a_.mov(64, ptr(Gp::r15, d_pc), Gp::rax);
        a_.lea(64, Gp::rax, x64::rip(start_, static_cast<std::int32_t>(tail.field)));
        a_.jmp(ptr(Gp::r15, d_stub_link));
    }
    a_.finalize();
    region_.code = a_.bytes();
    if (region_.guest_ranges.empty()) region_.guest_ranges.emplace_back(region_.guest_start, region_.guest_start + 4);
    return region_;
}

const void* make_add_host_entry(std::uint64_t value) {
    x64::Assembler a;
    a.mov_imm(Gp::rax, value);
    a.alu(x64::Alu::add, 64, ptr(Gp::r15, x_disp(0)), Gp::rax);
    a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(30)));
    a.mov(64, ptr(Gp::r15, d_pc), Gp::rax);
    a.jmp(ptr(Gp::r15, d_stub_indirect));
    a.finalize();
    void* memory = allocate_code(a.size());
    std::memcpy(memory, a.data(), a.size());
    return memory;
}

double now_ns() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::nano>(clock::now().time_since_epoch()).count();
}

} // namespace rtt

// ============================================================================
// Stand-ins for functions owned by other components (tests only).
// ============================================================================
namespace transmute::fx {

// The stand-in regions below use the flushed state at every boundary: no
// cross-region convention registers (translator.hpp).
const RegisterConvention& translator_register_convention() {
    static const RegisterConvention none{};
    return none;
}

TranslatedRegion translate_region(std::uint64_t guest_pc, const CodeSource& code, const TranslateOptions& options) {
    ++rtt::g_translate_calls;
    rtt::g_last_lazy.store(options.lazy);
    std::uint32_t word;
    if (!code.read(guest_pc, word)) throw std::runtime_error("translate_region: unreadable guest pc");
    rtt::RegionFactory factory;
    {
        std::lock_guard lock(rtt::g_mutex);
        if (auto it = rtt::g_factories.find(guest_pc); it != rtt::g_factories.end()) factory = it->second;
    }
    if (factory) return factory(guest_pc);
    std::uint32_t next;
    if (rtt::g_mini.load() && rtt::is_movz_x0(word) && code.read(guest_pc + 4, next) && next == rtt::kRet)
        return rtt::mini_region(guest_pc, word);
    return rtt::interpret_only_region(guest_pc);
}

void translate_region(std::uint64_t guest_pc, const CodeSource& code, const TranslateOptions& options,
                      TranslatedRegion& out) {
    out = translate_region(guest_pc, code, options);
}

bool cache_lookup(std::uint64_t pc, TranslatedRegion& region) {
    ++rtt::g_cache_lookups;
    rtt::RegionFactory factory;
    {
        std::lock_guard lock(rtt::g_mutex);
        if (auto it = rtt::g_cached.find(pc); it != rtt::g_cached.end()) factory = it->second;
    }
    if (!factory) return false;
    region = factory(pc);
    return true;
}
void cache_store(const TranslatedRegion&) { ++rtt::g_cache_stores; }
bool cache_exit_quiescent() noexcept { return true; }
void cache_prepare_exit() noexcept {}
void cache_set_module_ready_handler(CacheModuleReadyHandler handler) { rtt::g_ready_handler.store(handler); }
// Cursors of the stand-in are indexes into the configured start list.
std::size_t cache_region_starts(std::uint64_t base, std::uint64_t& cursor, std::uint64_t end, std::uint64_t* out,
                                std::size_t max) {
    std::lock_guard lock(rtt::g_mutex);
    auto it = rtt::g_cached_starts.find(base);
    if (it == rtt::g_cached_starts.end()) return 0;
    std::size_t n = 0;
    while (n < max && cursor < it->second.size() && cursor < end) out[n++] = it->second[static_cast<std::size_t>(cursor++)];
    return n;
}
std::uint64_t cache_region_cursor(std::uint64_t pc) {
    std::lock_guard lock(rtt::g_mutex);
    for (const auto& [base, starts] : rtt::g_cached_starts)
        for (std::size_t i = 0; i < starts.size(); ++i)
            if (starts[i] == pc) return i;
    return kCacheCursorEnd;
}
void cache_configure(const CacheConfig& config) {
    rtt::g_cache_config = config;
    ++rtt::g_cache_configures;
}

bool abi_code_frame(std::uint64_t address, AbiCodeFrame& frame) noexcept {
    std::lock_guard lock(rtt::g_mutex);
    for (const auto& f : rtt::g_abi_frames) {
        if (address >= f.begin && address < f.end) {
            frame = f;
            return true;
        }
    }
    return false;
}

void loader_thread_attach(GuestContext&) { ++rtt::g_attach_calls; }
void loader_thread_detach(GuestContext&) { ++rtt::g_detach_calls; }

const GuestModule* find_guest_module(std::uint64_t address) noexcept {
    const auto* module = rtt::g_fake_module.load();
    return module && address >= module->base && address - module->base < module->size ? module : nullptr;
}

std::vector<const GuestModule*> guest_modules() { return {}; }
void* guest_activation_context() noexcept { return rtt::g_activation.load(); }

void install_system_helpers(GuestContext& ctx) {
    for (unsigned id = helper_system_first; id < helper_system_first + 16; ++id)
        ctx.helpers[id] = reinterpret_cast<const void*>(rtt::kSystemHelperMarker + id);
}
std::uint64_t guest_image_stack_reserve() noexcept { return 0; }
std::uint64_t guest_image_stack_commit() noexcept { return 0; }

// Positional thunk: rcx, rdx, r8, r9 = x0..x3; x0 = rax; return to x30.
std::uint64_t bind_unknown_host_target(const void* host_address) {
    using namespace rtt;
    x64::Assembler a;
    a.mov(64, Gp::rcx, ptr(Gp::r15, x_disp(0)));
    a.mov(64, Gp::rdx, ptr(Gp::r15, x_disp(1)));
    a.mov(64, Gp::r8, ptr(Gp::r15, x_disp(2)));
    a.mov(64, Gp::r9, ptr(Gp::r15, x_disp(3)));
    a.mov_imm64(Gp::rax, reinterpret_cast<std::uint64_t>(host_address));
    a.call(Gp::rax);
    a.mov(64, ptr(Gp::r15, x_disp(0)), Gp::rax);
    a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(30)));
    a.mov(64, ptr(Gp::r15, d_pc), Gp::rax);
    a.jmp(ptr(Gp::r15, d_stub_indirect));
    a.finalize();
    void* memory = allocate_code(a.size());
    std::memcpy(memory, a.data(), a.size());
    return reinterpret_cast<std::uint64_t>(memory);
}

void dispatch_guest_exception(GuestContext& ctx, const GuestExceptionRecord& record) {
    rtt::ExceptionHandler handler;
    {
        std::lock_guard lock(rtt::g_mutex);
        rtt::g_exceptions.push_back(record);
        handler = rtt::g_exception_handler;
    }
    if (handler) {
        handler(ctx, record);
        return;
    }
    ctx.x[0] = rtt::kUnhandledMarker | record.code;
    ctx.pc = kReturnToHostPc;
}

} // namespace transmute::fx
