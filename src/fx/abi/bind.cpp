// Thunk binding, caches and the public ABI entry points (owner: abi).

#include "fx/abi/abi_internal.hpp"
#include "transmute/fx/process.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace transmute::fx {
namespace abi {
namespace {

// ---- code memory ----------------------------------------------------------------

struct CodeHeap {
    std::mutex mutex;
    std::uint8_t* cursor{};
    std::uint8_t* end{};
};

CodeHeap& code_heap() {
    static auto* heap = new CodeHeap();
    return *heap;
}

constexpr std::size_t kCodeChunk = 64 * 1024;

// ---- registries --------------------------------------------------------------------

struct Registry {
    std::mutex create;          // serialises thunk creation (never held by lookups)
    std::shared_mutex mutex;    // guards the maps below
    std::unordered_map<std::uint64_t, std::unique_ptr<ThunkRecord>> thunks;     // by thunk address
    std::unordered_map<std::string, std::uint64_t> bound;                       // bind_host_function cache
    std::unordered_map<std::uint64_t, std::uint64_t> unknown;                   // host address -> thunk
    std::map<std::pair<std::uint64_t, const Signature*>, std::uint64_t> typed;  // (host, sig) -> thunk
    std::vector<std::unique_ptr<OverrideBinding>> bindings;
};

Registry& registry() {
    static auto* r = new Registry();
    return *r;
}

struct CallbackKeyHash {
    std::size_t operator()(const std::pair<std::uint64_t, const Signature*>& k) const noexcept {
        return std::hash<std::uint64_t>()(k.first * 0x9e3779b97f4a7c15ULL ^
                                          reinterpret_cast<std::uintptr_t>(k.second));
    }
};

struct Callbacks {
    std::shared_mutex mutex;
    std::unordered_map<std::pair<std::uint64_t, const Signature*>, std::unique_ptr<CallbackInfo>,
                       CallbackKeyHash> by_key;
    std::unordered_map<std::uint64_t, const CallbackInfo*> by_trampoline;
};

Callbacks& callbacks() {
    static auto* c = new Callbacks();
    return *c;
}

// Frames of every piece of abi code, by start address (abi_code_frame).
struct Frames {
    std::shared_mutex mutex;
    std::map<std::uint64_t, AbiCodeFrame> by_begin;
};

Frames& frames() {
    static auto* f = new Frames();
    return *f;
}

void record_frame(const std::uint8_t* code, std::size_t size, std::uint32_t prolog, std::size_t epilog,
                  std::uint32_t frame_bytes, bool entered_by_call) {
    AbiCodeFrame frame;
    frame.begin = reinterpret_cast<std::uint64_t>(code);
    frame.end = frame.begin + size;
    frame.prolog_bytes = prolog;
    frame.epilog_offset = static_cast<std::uint32_t>(std::min(epilog, size));
    frame.frame_bytes = frame_bytes;
    frame.entered_by_call = entered_by_call;
    auto& f = frames();
    std::unique_lock lock(f.mutex);
    f.by_begin[frame.begin] = frame;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string dll_key(std::string_view dll) {
    const auto slash = dll.find_last_of("\\/");
    if (slash != std::string_view::npos) dll = dll.substr(slash + 1);
    return lower(dll);
}

// Creates and registers a thunk. Caller holds r.create (not r.mutex): the
// runtime's register_host_entry may query is_abi_thunk.
std::uint64_t install_thunk(Registry& r, const std::vector<std::uint8_t>& bytes, const void* native,
                            const Signature* sig, bool is_override, std::string name,
                            std::uint32_t prolog = 0, std::size_t epilog = SIZE_MAX, std::uint32_t frame_bytes = 0) {
    auto* code = emit_code(bytes);
    const auto address = reinterpret_cast<std::uint64_t>(code);
    record_frame(code, bytes.size(), prolog, epilog, frame_bytes, false);
    auto record = std::make_unique<ThunkRecord>();
    record->address = address;
    record->native = native;
    record->sig = sig;
    record->is_override = is_override;
    record->name = std::move(name);
    {
        std::unique_lock lock(r.mutex);
        r.thunks.emplace(address, std::move(record));
    }
    register_host_entry(address, code);
    return address;
}

// Name of the host module containing `address` (lower case), for traces.
std::string host_module_name(const void* address) {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module) ||
        GetModuleFileNameW(module, path, MAX_PATH) == 0) {
        return "?";
    }
    std::wstring w(path);
    const auto slash = w.find_last_of(L"\\/");
    if (slash != std::wstring::npos) w = w.substr(slash + 1);
    std::string out;
    for (const wchar_t ch : w) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch < 128 ? ch : '?'))));
    return out;
}

// Trace record for a new thunk, or nullptr when import tracing is off.
// Records live for the process lifetime (thunks are never freed).
TraceInfo* make_trace(std::string dll, std::string name, const void* native) {
    if (import_trace_mode() == TraceMode::off) return nullptr;
    auto* info = new TraceInfo();
    info->dll = dll.empty() ? (native ? host_module_name(native) : std::string("?")) : std::move(dll);
    if (name.empty()) {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(native)));
        name = buffer;
    }
    info->name = std::move(name);
    return info;
}

std::uint64_t make_plain_thunk(Registry& r, const Signature* sig, const void* native, std::string name,
                               std::string dll = {}) {
    const auto code = generate_host_thunk(*sig, native, make_trace(std::move(dll), name, native));
    return install_thunk(r, code.bytes, native, sig, false, std::move(name), code.prolog_bytes,
                         code.epilog_offset, code.frame_bytes);
}

bool looks_like_host_code(std::uint64_t address) noexcept {
    if (address < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & exec) != 0;
}

const void* vcruntime_native(const char* name) {
    static const HMODULE vcruntime = [] {
        HMODULE m = GetModuleHandleW(L"vcruntime140.dll");
        if (!m) m = LoadLibraryW(L"vcruntime140.dll");
        return m;
    }();
    static const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (vcruntime) {
        if (auto* p = GetProcAddress(vcruntime, name)) return reinterpret_cast<const void*>(p);
    }
    if (ntdll) {
        if (auto* p = GetProcAddress(ntdll, name)) return reinterpret_cast<const void*>(p);
    }
    return nullptr;
}

} // namespace

// ---- code memory ----------------------------------------------------------------------

std::uint8_t* emit_code(const std::vector<std::uint8_t>& bytes) {
    auto& heap = code_heap();
    const std::size_t size = (bytes.size() + 15) & ~std::size_t{15};
    std::uint8_t* at = nullptr;
    {
        std::lock_guard lock(heap.mutex);
        if (heap.cursor == nullptr || static_cast<std::size_t>(heap.end - heap.cursor) < size) {
            const std::size_t chunk = std::max(kCodeChunk, size);
            heap.cursor = static_cast<std::uint8_t*>(allocate_code(chunk, 16));
            heap.end = heap.cursor + chunk;
        }
        at = heap.cursor;
        heap.cursor += size;
    }
    std::memcpy(at, bytes.data(), bytes.size());
    std::memset(at + bytes.size(), 0xcc, size - bytes.size());
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return at;
}

const ThunkRecord* find_thunk(std::uint64_t address) noexcept {
    auto& r = registry();
    try {
        std::shared_lock lock(r.mutex);
        const auto it = r.thunks.find(address);
        return it == r.thunks.end() ? nullptr : it->second.get();
    } catch (...) {
        return nullptr;
    }
}

std::uint64_t trampoline_guest_function(std::uint64_t address) noexcept {
    auto& c = callbacks();
    try {
        std::shared_lock lock(c.mutex);
        const auto it = c.by_trampoline.find(address);
        return it == c.by_trampoline.end() ? 0 : it->second->guest_function;
    } catch (...) {
        return 0;
    }
}

std::uint64_t bind_host_pointer(const void* host_address, const Signature* sig) {
    if (host_address == nullptr) return 0;
    if (sig == nullptr) sig = unknown_signature();
    const auto key = std::make_pair(reinterpret_cast<std::uint64_t>(host_address), sig);
    auto& r = registry();
    {
        std::shared_lock lock(r.mutex);
        if (const auto it = r.typed.find(key); it != r.typed.end()) return it->second;
    }
    std::lock_guard create(r.create);
    {
        std::shared_lock lock(r.mutex);
        if (const auto it = r.typed.find(key); it != r.typed.end()) return it->second;
    }
    const auto thunk = make_plain_thunk(r, sig, host_address, std::string());
    std::unique_lock lock(r.mutex);
    r.typed.emplace(key, thunk);
    return thunk;
}

std::string host_export_name(const void* host_address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(host_address), &module) || module == nullptr) {
        return {};
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return {};
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return {};
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    const auto rva = static_cast<DWORD>(reinterpret_cast<const std::uint8_t*>(host_address) - base);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const WORD ordinal = ordinals[i];
        if (ordinal < exports->NumberOfFunctions && functions[ordinal] == rva)
            return reinterpret_cast<const char*>(base + names[i]);
    }
    return {};
}

// ---- callbacks ---------------------------------------------------------------------

std::uint64_t wrap_callback_argument(std::uint64_t value, const Signature* sig) {
    if (value == 0) return 0;
    if (const auto* thunk = find_thunk(value)) {
        if (!thunk->is_override && thunk->native) return reinterpret_cast<std::uint64_t>(thunk->native);
        return reinterpret_cast<std::uint64_t>(host_callable(value, sig ? sig->text.c_str() : nullptr));
    }
    if (!is_guest_code(value)) return value;    // host code or a sentinel (SIG_IGN, ...)
    return reinterpret_cast<std::uint64_t>(host_callable(value, sig ? sig->text.c_str() : nullptr));
}

std::uint64_t unwrap_callback_result(std::uint64_t value, const Signature* sig) {
    if (value == 0) return 0;
    if (const auto guest = trampoline_guest_function(value)) return guest;
    if (find_thunk(value) || is_guest_code(value)) return value;
    if (!looks_like_host_code(value)) return value;
    return bind_host_pointer(reinterpret_cast<const void*>(value), sig);
}

std::uint64_t callback_dispatch(const CallbackInfo* info, std::uint64_t* slots, std::uint64_t* xmm) {
    GuestContext* ctx = current_context();
    if (import_trace_mode() != TraceMode::off) trace_callback(info->guest_function, info->sig->text.c_str(), ctx != nullptr);
    if (ctx == nullptr) ctx = &ensure_thread_context();
    const Signature& sig = *info->sig;
    const std::uint64_t saved_sp = ctx->x[31];
    try {
        if (sig.unknown) {
            for (unsigned n = 0; n < 8; ++n) ctx->x[n] = slots[n];
            for (unsigned n = 0; n < 4; ++n) { ctx->v[n][0] = xmm[n]; ctx->v[n][1] = 0; }
            call_guest_prepared(*ctx, info->guest_function);
            ctx->x[31] = saved_sp;
            xmm[0] = ctx->v[0][0];
            return ctx->x[0];
        }
        const CallPlan& plan = info->plan;
        const unsigned variadic_extra = plan.variadic ? 8u : 0u;
        std::uint64_t sp = saved_sp;
        const std::uint64_t stack_bytes = plan.arm_stack_bytes + 8ull * variadic_extra;
        if (stack_bytes != 0) sp = (saved_sp - stack_bytes) & ~std::uint64_t{15};
        auto* stack = reinterpret_cast<std::uint8_t*>(sp);
        const auto put_piece = [&](const ArmPiece& p, std::uint64_t bits) {
            switch (p.kind) {
            case ArmPiece::gpr: ctx->x[p.index] = bits; break;
            case ArmPiece::vreg:
                ctx->v[p.index][0] = p.bytes == 4 ? (bits & 0xffffffffULL) : bits;
                ctx->v[p.index][1] = 0;
                break;
            case ArmPiece::stack: std::memcpy(stack + p.offset, &bits, 8); break;
            }
        };
        std::uint64_t hidden = 0;
        if (plan.win_hidden_return) {
            hidden = slots[0];
            if (plan.ret_kind == ReturnKind::record_memory) ctx->x[8] = hidden;
        }
        for (const auto& arg : plan.args) {
            const std::uint64_t value = (arg.win_fp && arg.win_pos < 4) ? xmm[arg.win_pos] : slots[arg.win_pos];
            const auto& t = arg.type;
            if (t.is_aggregate()) {
                if (arg.arm_indirect) { put_piece(arg.arm[0], value); continue; }
                std::uint8_t bytes[32] = {};
                if (arg.win_by_reference) std::memcpy(bytes, reinterpret_cast<const void*>(value), std::min<std::uint32_t>(t.size, 32));
                else std::memcpy(bytes, &value, std::min<std::uint32_t>(t.size, 8));
                for (std::size_t k = 0; k < arg.arm.size(); ++k) {
                    const auto& p = arg.arm[k];
                    std::uint64_t bits = 0;
                    const std::size_t off = p.kind == ArmPiece::vreg ? k * t.hfa_element_size() : k * 8;
                    std::memcpy(&bits, bytes + off, p.kind == ArmPiece::vreg ? p.bytes : 8);
                    put_piece(p, bits);
                }
                continue;
            }
            std::uint64_t bits = value;
            switch (t.kind) {
            case TypeKind::i8: bits = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(value))); break;
            case TypeKind::u8: bits = value & 0xff; break;
            case TypeKind::i16: bits = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(value))); break;
            case TypeKind::u16: bits = value & 0xffff; break;
            case TypeKind::i32: bits = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value))); break;
            case TypeKind::u32: bits = value & 0xffffffffULL; break;
            case TypeKind::f32: bits = value & 0xffffffffULL; break;
            default: break;
            }
            put_piece(arg.arm[0], bits);
        }
        if (plan.variadic) {
            // Forward a fixed number of extra slots (host variadic callers
            // into guest code are rare; the count is not knowable).
            unsigned slot = plan.arm_named_slots;
            for (unsigned k = 0; k < variadic_extra; ++k, ++slot) {
                const std::uint64_t value = slots[plan.win_slots + k];
                if (slot < 8) ctx->x[slot] = value;
                else std::memcpy(stack + 8ull * (slot - 8), &value, 8);
            }
        }
        ctx->x[31] = sp;
        call_guest_prepared(*ctx, info->guest_function);
        ctx->x[31] = saved_sp;

        const auto& ret = plan.ret;
        switch (plan.ret_kind) {
        case ReturnKind::none:
            return 0;
        case ReturnKind::integer: {
            const std::uint64_t x0 = ctx->x[0];
            switch (ret.kind) {
            case TypeKind::i8: return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(x0)));
            case TypeKind::u8: return x0 & 0xff;
            case TypeKind::i16: return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(x0)));
            case TypeKind::u16: return x0 & 0xffff;
            case TypeKind::i32: return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(x0)));
            case TypeKind::u32: return x0 & 0xffffffffULL;
            default: return x0;
            }
        }
        case ReturnKind::fp:
            xmm[0] = ret.kind == TypeKind::f32 ? (ctx->v[0][0] & 0xffffffffULL) : ctx->v[0][0];
            return 0;
        case ReturnKind::record_regs: {
            if (!plan.win_hidden_return) {
                const std::uint64_t mask = ret.size >= 8 ? ~0ULL : ((1ULL << (8 * ret.size)) - 1);
                return ctx->x[0] & mask;
            }
            std::uint8_t bytes[16];
            std::memcpy(bytes, &ctx->x[0], 8);
            std::memcpy(bytes + 8, &ctx->x[1], 8);
            std::memcpy(reinterpret_cast<void*>(hidden), bytes, ret.size);
            return hidden;
        }
        case ReturnKind::record_memory:
            return hidden;
        case ReturnKind::hfa_regs: {
            const unsigned esz = ret.hfa_element_size();
            std::uint8_t bytes[32] = {};
            for (unsigned k = 0; k < ret.hfa_count; ++k) std::memcpy(bytes + k * esz, &ctx->v[k][0], esz);
            if (!plan.win_hidden_return) {
                std::uint64_t value = 0;
                std::memcpy(&value, bytes, ret.size);
                return value;
            }
            std::memcpy(reinterpret_cast<void*>(hidden), bytes, ret.size);
            return hidden;
        }
        }
        return ctx->x[0];
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fx abi: exception in guest callback %llx: %s\n",
                     static_cast<unsigned long long>(info->guest_function), error.what());
    } catch (...) {
        std::fprintf(stderr, "fx abi: exception in guest callback %llx\n",
                     static_cast<unsigned long long>(info->guest_function));
    }
    // A C++ exception cannot cross the host frames that called us.
    std::fflush(stderr);
    std::abort();
}

} // namespace abi

// ============================================================================
// Public API (process.hpp)
// ============================================================================

using namespace abi;

std::uint64_t bind_host_function(std::string_view dll, std::string_view function, const void* host_address) {
    const std::string module = dll_key(dll);
    std::string key = module;
    key += '!';
    key += function;
    key += '@';
    key += std::to_string(reinterpret_cast<std::uint64_t>(host_address));
    auto& r = registry();
    {
        std::shared_lock lock(r.mutex);
        if (const auto it = r.bound.find(key); it != r.bound.end()) return it->second;
    }

    // Typed lookups are done outside the registry lock.
    std::string name(function);
    if ((name.empty() || name[0] == '#') && host_address != nullptr) {
        const auto exported = host_export_name(host_address);
        if (!exported.empty()) name = exported;
    }
    const Signature* sig = lookup_signature(name);
    if (sig->unknown && host_address != nullptr) {
        const auto exported = host_export_name(host_address);
        if (!exported.empty() && exported != name) sig = lookup_signature(exported);
    }
    const OverrideHandler handler = find_override(module, name);
    if (handler == nullptr && host_address == nullptr) return 0;

    std::lock_guard create(r.create);
    {
        std::shared_lock lock(r.mutex);
        if (const auto it = r.bound.find(key); it != r.bound.end()) return it->second;
    }
    std::uint64_t thunk = 0;
    if (handler != nullptr) {
        auto binding = std::make_unique<OverrideBinding>();
        binding->dll = module;
        binding->name = name;
        binding->native = host_address;
        binding->sig = sig;
        const auto bytes = generate_override_thunk(handler, binding.get(), make_trace(module, name, host_address));
        thunk = install_thunk(r, bytes, host_address, sig, true, name);
        std::unique_lock lock(r.mutex);
        r.bindings.push_back(std::move(binding));
    } else {
        thunk = make_plain_thunk(r, sig, host_address, name, module);
    }
    std::unique_lock lock(r.mutex);
    r.bound.emplace(std::move(key), thunk);
    return thunk;
}

std::uint64_t bind_unknown_host_target(const void* host_address) {
    if (host_address == nullptr) return 0;
    const auto address = reinterpret_cast<std::uint64_t>(host_address);
    if (find_thunk(address)) return address;     // already guest-callable
    // One of our callback trampolines: the runtime treats the result as host
    // code unless it is a registered entry, so bind a thunk with the
    // trampoline's own signature rather than returning the guest function.
    {
        const Signature* tramp_sig = nullptr;
        auto& c = callbacks();
        {
            std::shared_lock lock(c.mutex);
            if (const auto it = c.by_trampoline.find(address); it != c.by_trampoline.end()) tramp_sig = it->second->sig;
        }
        if (tramp_sig) return bind_host_pointer(host_address, tramp_sig);
    }
    auto& r = registry();
    {
        std::shared_lock lock(r.mutex);
        if (const auto it = r.unknown.find(address); it != r.unknown.end()) return it->second;
    }
    // A named export of a host module gets its database signature.
    const Signature* sig = unknown_signature();
    const auto exported = host_export_name(host_address);
    if (!exported.empty()) sig = lookup_signature(exported);
    std::lock_guard create(r.create);
    {
        std::shared_lock lock(r.mutex);
        if (const auto it = r.unknown.find(address); it != r.unknown.end()) return it->second;
    }
    const auto thunk = make_plain_thunk(r, sig, host_address, exported);
    std::unique_lock lock(r.mutex);
    r.unknown.emplace(address, thunk);
    return thunk;
}

const void* host_callable(std::uint64_t guest_function, const char* signature) {
    if (guest_function == 0) return nullptr;
    const Signature* sig = signature ? intern_signature(signature) : unknown_signature();
    if (sig == nullptr) sig = unknown_signature();
    const auto key = std::make_pair(guest_function, sig);
    auto& c = callbacks();
    {
        std::shared_lock lock(c.mutex);
        if (const auto it = c.by_key.find(key); it != c.by_key.end()) return it->second->trampoline;
    }
    std::unique_lock lock(c.mutex);
    if (const auto it = c.by_key.find(key); it != c.by_key.end()) return it->second->trampoline;
    auto info = std::make_unique<CallbackInfo>();
    info->guest_function = guest_function;
    info->sig = sig;
    if (!sig->unknown) info->plan = plan_call(*sig);
    std::uint32_t prolog = 0, frame_bytes = 0;
    const auto bytes = generate_callback_trampoline(info.get(), &prolog, &frame_bytes);
    auto* code = emit_code(bytes);
    // The trampoline ends with "add rsp, frame; ret": only the ret runs
    // with rsp back at the entry value.
    record_frame(code, bytes.size(), prolog, bytes.size() - 1, frame_bytes, true);
    info->trampoline = code;
    c.by_trampoline.emplace(reinterpret_cast<std::uint64_t>(code), info.get());
    const void* result = info->trampoline;
    c.by_key.emplace(key, std::move(info));
    return result;
}

namespace {

// Exports of the ARM64 UCRT that the x64 UCRT only declares inline.
float host_fabsf(float x) { return std::fabs(x); }

bool is_crt_dll(const std::string& module) {
    return module.rfind("api-ms-win-crt-", 0) == 0 || module.rfind("ucrtbase", 0) == 0 ||
           module.rfind("msvcr", 0) == 0;
}

} // namespace

// `dll` is the name as written in the importing image; compared
// case-insensitively. Used both before binding a guest export (vcruntime
// mem*/str* -> native) and when a host DLL lacks an export (fabsf).
std::uint64_t abi_guest_import_replacement(std::string_view dll, std::string_view function) {
    const std::string module = dll_key(dll);
    if (is_crt_dll(module) && function == "fabsf")
        return bind_host_function("ucrtbase.dll", "fabsf", reinterpret_cast<const void*>(&host_fabsf));
    if (module.rfind("vcruntime140", 0) != 0) return 0;
    static constexpr const char* kReplaced[] = {
        "memchr", "memcmp", "memcpy", "memmove", "memset",
        "strchr", "strrchr", "strstr", "wcschr", "wcsrchr", "wcsstr",
    };
    for (const char* name : kReplaced) {
        if (function != name) continue;
        const void* native = vcruntime_native(name);
        if (native == nullptr) return 0;
        return bind_host_function("vcruntime140.dll", name, native);
    }
    return 0;
}

bool is_abi_thunk(std::uint64_t address) noexcept {
    return find_thunk(address) != nullptr;
}

bool abi_code_frame(std::uint64_t address, AbiCodeFrame& frame) noexcept {
    auto& f = frames();
    try {
        std::shared_lock lock(f.mutex);
        auto it = f.by_begin.upper_bound(address);
        if (it == f.by_begin.begin()) return false;
        --it;
        if (address >= it->second.end) return false;
        frame = it->second;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace transmute::fx
