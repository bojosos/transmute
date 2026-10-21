// Real-binary tests: parse every RUNTIME_FUNCTION of the ARM64 Blender build
// and of the fx corpus programs, and check virtual unwinding against real
// prolog/epilog code executed by the reference interpreter.

#include "fx/common/interpreter.hpp"
#include "fx/seh/seh.hpp"
#include "fx/seh/tests/seh_test_util.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::seh;
namespace fs = std::filesystem;

namespace {

const wchar_t* kBlenderDir = L"C:\\dev\\transmute\\artifacts\\arm64-stage\\Blender Foundation\\Blender 4.5";
const wchar_t* kCorpusDir = L"C:\\dev\\transmute\\build-fx-corpus\\fx\\arm64";

// An image mapped at its own buffer address (guest VA == host VA).
struct MappedImage {
    std::uint8_t* base = nullptr;
    std::uint32_t size = 0;
    std::uint16_t machine = 0;
    std::uint32_t exception_rva = 0;
    std::uint32_t exception_size = 0;
    ~MappedImage() {
        if (base) VirtualFree(base, 0, MEM_RELEASE);
    }
    std::uint64_t address() const { return reinterpret_cast<std::uint64_t>(base); }
};

template <typename T>
T get(const std::vector<std::uint8_t>& file, std::size_t offset) {
    T value{};
    if (offset + sizeof(T) <= file.size()) std::memcpy(&value, file.data() + offset, sizeof(T));
    return value;
}

bool map_image(const fs::path& path, MappedImage& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.size() < 0x200 || get<std::uint16_t>(file, 0) != 0x5a4d) return false;
    const std::uint32_t pe = get<std::uint32_t>(file, 0x3c);
    if (get<std::uint32_t>(file, pe) != 0x00004550) return false;
    out.machine = get<std::uint16_t>(file, pe + 4);
    const std::uint16_t sections = get<std::uint16_t>(file, pe + 6);
    const std::uint16_t optional_size = get<std::uint16_t>(file, pe + 20);
    const std::size_t optional = pe + 24;
    if (get<std::uint16_t>(file, optional) != 0x20b) return false;   // PE32+
    out.size = get<std::uint32_t>(file, optional + 56);
    const std::uint32_t header_size = get<std::uint32_t>(file, optional + 60);
    const std::uint32_t directories = get<std::uint32_t>(file, optional + 108);
    if (directories > 3) {
        out.exception_rva = get<std::uint32_t>(file, optional + 112 + 3 * 8);
        out.exception_size = get<std::uint32_t>(file, optional + 112 + 3 * 8 + 4);
    }
    out.base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, out.size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!out.base) return false;
    std::memcpy(out.base, file.data(), std::min<std::size_t>(header_size, file.size()));
    const std::size_t table = optional + optional_size;
    for (unsigned i = 0; i < sections; ++i) {
        const std::size_t s = table + i * 40;
        const std::uint32_t virtual_size = get<std::uint32_t>(file, s + 8);
        const std::uint32_t rva = get<std::uint32_t>(file, s + 12);
        const std::uint32_t raw_size = get<std::uint32_t>(file, s + 16);
        const std::uint32_t raw_offset = get<std::uint32_t>(file, s + 20);
        std::uint32_t copy = std::min(raw_size, virtual_size ? virtual_size : raw_size);
        if (rva >= out.size) continue;
        copy = std::min<std::uint32_t>(copy, out.size - rva);
        if (raw_offset + std::size_t{copy} > file.size()) copy = static_cast<std::uint32_t>(file.size() - raw_offset);
        std::memcpy(out.base + rva, file.data() + raw_offset, copy);
    }
    return true;
}

std::vector<fs::path> arm64_binaries(const wchar_t* root) {
    std::vector<fs::path> result;
    std::error_code ec;
    if (!fs::exists(root, ec)) return result;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::wstring ext = it->path().extension().wstring();
        for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
        if (ext == L".exe" || ext == L".dll" || ext == L".pyd") result.push_back(it->path());
    }
    return result;
}

struct ParseStats {
    std::uint64_t files = 0, skipped_files = 0, entries = 0;
    std::uint64_t packed1 = 0, packed2 = 0, xdata = 0, handlers = 0, epilog_scopes = 0;
    std::map<std::string, std::uint64_t> failures;
};

void parse_file(const fs::path& path, ParseStats& stats, std::vector<std::string>& examples) {
    MappedImage image;
    if (!map_image(path, image) || image.machine != 0xAA64) {
        ++stats.skipped_files;
        return;
    }
    ++stats.files;
    const std::uint32_t count = image.exception_size / 8;
    FunctionUnwindInfo info;
    for (std::uint32_t i = 0; i < count; ++i) {
        Arm64RuntimeFunction entry;
        std::memcpy(&entry, image.base + image.exception_rva + 8ull * i, 8);
        ++stats.entries;
        UnwindStatus status = describe_function(image.address(), entry, info);
        if (status == UnwindStatus::ok) status = validate_function(info);
        if (status != UnwindStatus::ok) {
            ++stats.failures[unwind_status_name(status)];
            if (examples.size() < 10) {
                char line[256];
                std::snprintf(line, sizeof(line), "%ls rva=0x%x unwind=0x%x: %s", path.filename().c_str(),
                              entry.BeginAddress, entry.UnwindData, unwind_status_name(status));
                examples.push_back(line);
            }
            continue;
        }
        if (info.packed) (info.packed_flag == 1 ? stats.packed1 : stats.packed2)++;
        else {
            ++stats.xdata;
            stats.epilog_scopes += info.epilog_count;
        }
        if (info.has_handler) ++stats.handlers;
    }
}

// ---- real prolog/epilog simulation ----

bool is_prolog_instruction(std::uint32_t w) {
    if (w == 0xd503237fu || w == 0xd503201fu) return true;                 // pacibsp, nop
    if ((w & 0xffc003ffu) == 0x910003fdu) return true;                    // add x29, sp, #imm
    if ((w & 0xff8003ffu) == 0xd10003ffu) return true;                    // sub sp, sp, #imm{, lsl #12}
    const unsigned rn = (w >> 5) & 31;
    if (rn != 31) return false;
    // stp/str (x or d), signed offset or pre-index, base sp.
    if ((w & 0x7fc00000u) == 0x29000000u || (w & 0x7fc00000u) == 0x29800000u) return (w >> 31) == 1;  // stp x
    if ((w & 0xffc00000u) == 0x6d000000u || (w & 0xffc00000u) == 0x6d800000u) return true;            // stp d
    if ((w & 0xffc00000u) == 0xf9000000u) return true;                                                 // str x, uimm
    if ((w & 0xffe00c00u) == 0xf8000c00u) return true;                                                 // str x, pre
    if ((w & 0xffc00000u) == 0xfd000000u) return true;                                                 // str d, uimm
    if ((w & 0xffe00c00u) == 0xfc000c00u) return true;                                                 // str d, pre
    return false;
}

bool is_epilog_instruction(std::uint32_t w) {
    if (w == 0xd50323ffu || w == 0xd503201fu) return true;                // autibsp, nop
    if ((w & 0xffc003ffu) == 0x910003ffu) return true;                    // add sp, sp, #imm
    if (w == 0x910003bfu) return true;                                    // mov sp, x29
    if ((w & 0xffc003e0u) == 0x910003a0u && (w & 31) == 31) return true;   // add sp, x29, #imm
    const unsigned rn = (w >> 5) & 31;
    if (rn != 31) return false;
    if ((w & 0xffc00000u) == 0xa9400000u || (w & 0xffc00000u) == 0xa8c00000u) return true;   // ldp x
    if ((w & 0xffc00000u) == 0x6d400000u || (w & 0xffc00000u) == 0x6cc00000u) return true;   // ldp d
    if ((w & 0xffc00000u) == 0xf9400000u) return true;                                         // ldr x, uimm
    if ((w & 0xffe00c00u) == 0xf8400400u) return true;                                         // ldr x, post
    if ((w & 0xffc00000u) == 0xfd400000u) return true;                                         // ldr d, uimm
    if ((w & 0xffe00c00u) == 0xfc400400u) return true;                                         // ldr d, post
    return false;
}

bool is_return(std::uint32_t w) {
    return w == 0xd65f03c0u || (w & 0xfc000000u) == 0x14000000u ||        // ret, b
           (w & 0xfffffc1fu) == 0xd61f0000u;                              // br xN
}

thread_local std::string g_current_file;

struct SimStats {
    std::uint64_t functions = 0, prolog_points = 0, epilog_points = 0, skipped = 0, mismatches = 0;
    std::uint64_t unbalanced = 0;   // helpers that return with SP deliberately changed
    std::uint64_t fragments = 0;    // end_c fragments (parent frame needed)
};

struct Stack {
    std::uint8_t* base = nullptr;
    std::size_t size = 8u << 20;
    Stack() { base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)); }
    ~Stack() { VirtualFree(base, 0, MEM_RELEASE); }
    std::uint64_t low() const { return reinterpret_cast<std::uint64_t>(base); }
    std::uint64_t top() const { return low() + size - 4096; }
};

struct EntryState {
    std::uint64_t sp, lr, x[11];   // x19..x29
    std::uint64_t d[8];
};

std::uint64_t mix(std::uint64_t& seed) {
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    return seed;
}

bool check_unwind(std::uint64_t image_base, const FunctionUnwindInfo& info, const GuestContext& ctx,
                  std::uint64_t pc, const EntryState& entry, std::string& why) {
    Arm64Context c;
    guest_to_arm64_context(ctx, c, kCtxFull);
    c.Pc = pc;
    VirtualUnwindResult r = virtual_unwind_info(kHandlerTypeNone, image_base, pc, info, c, nullptr);
    char buf[160];
    if (r.status != UnwindStatus::ok) { why = unwind_status_name(r.status); return false; }
    if (c.Sp != entry.sp) { std::snprintf(buf, sizeof buf, "sp %llx != %llx", c.Sp, entry.sp); why = buf; return false; }
    if (c.Pc != entry.lr) { std::snprintf(buf, sizeof buf, "pc %llx != lr %llx", c.Pc, entry.lr); why = buf; return false; }
    for (unsigned i = 0; i < 11; ++i)
        if (c.X[19 + i] != entry.x[i]) { std::snprintf(buf, sizeof buf, "x%u mismatch", 19 + i); why = buf; return false; }
    for (unsigned i = 0; i < 8; ++i)
        if (c.V[8 + i].Low != entry.d[i]) { std::snprintf(buf, sizeof buf, "d%u mismatch", 8 + i); why = buf; return false; }
    return true;
}

bool step(GuestContext& ctx, std::uint32_t word) {
    try {
        interpret_instruction(ctx, word);
        return true;
    } catch (...) {
        return false;
    }
}

void simulate_function(std::uint64_t image_base, const Arm64RuntimeFunction& rf, Stack& stack, SimStats& stats,
                       std::vector<std::string>& examples, std::uint64_t& seed) {
    static thread_local FunctionUnwindInfo info;
    if (describe_function(image_base, rf, info) != UnwindStatus::ok) return;
    if (info.packed && info.packed_flag == 2) return;
    // Fragments whose codes continue into a parent region's phantom prolog
    // (end_c) need the parent's frame, which this harness does not build.
    for (std::uint32_t pos = 0; !info.packed && pos < info.code_size;) {
        const std::uint8_t op = info.codes[pos];
        if (op == 0xe4) break;
        if (op == 0xe5) { ++stats.fragments; return; }
        pos += unwind_code_size(op);
    }
    const std::uint32_t prolog = prolog_instruction_count(info);
    const std::uint64_t func = image_base + rf.BeginAddress;
    auto word_at = [&](std::uint32_t index) {
        std::uint32_t w = 0;
        std::memcpy(&w, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(func + 4ull * index)), 4);
        return w;
    };
    if (prolog > info.length) { ++stats.skipped; return; }
    for (std::uint32_t i = 0; i < prolog; ++i)
        if (!is_prolog_instruction(word_at(i))) { ++stats.skipped; return; }

    EntryState entry{};
    entry.sp = stack.top() & ~15ull;
    entry.lr = 0x7700'0000'0000ull | (mix(seed) & 0xfffffffcull);
    GuestContext ctx{};
    for (unsigned i = 0; i < 31; ++i) ctx.x[i] = mix(seed);
    for (unsigned i = 0; i < 32; ++i) { ctx.v[i][0] = mix(seed); ctx.v[i][1] = mix(seed); }
    ctx.x[30] = entry.lr;
    ctx.x[31] = entry.sp;
    for (unsigned i = 0; i < 11; ++i) entry.x[i] = ctx.x[19 + i];
    for (unsigned i = 0; i < 8; ++i) entry.d[i] = ctx.v[8 + i][0];

    auto record = [&](const char* where, std::uint32_t offset, const std::string& why) {
        ++stats.mismatches;
        if (examples.size() < 12) {
            char line[256];
            std::snprintf(line, sizeof line, "%s rva=0x%x %s offset %u (%s, unwind=0x%08x): %s", g_current_file.c_str(), rf.BeginAddress,
                          where, offset, info.packed ? "packed" : "xdata", rf.UnwindData, why.c_str());
            examples.push_back(line);
        }
    };

    ++stats.functions;
    std::string why;
    for (std::uint32_t i = 0; i <= prolog; ++i) {
        const std::uint64_t pc = func + 4ull * i;
        ++stats.prolog_points;
        if (!check_unwind(image_base, info, ctx, pc, entry, why)) { record("prolog", i, why); return; }
        if (i == prolog) break;
        ctx.pc = pc;
        if (!step(ctx, word_at(i)) || ctx.x[31] < stack.low() + 4096) { ++stats.skipped; return; }
    }

    // Compilers may place further stack setup right after the described prolog
    // (e.g. `sub sp, sp, #n` when the body unwind is frame-pointer based).
    // Execute it too, checking body unwinding at each point, so the state that
    // enters the epilogs matches what the epilogs undo.
    const GuestContext after_described_prolog = ctx;
    for (std::uint32_t i = prolog; i < prolog + 8 && i + 1 < info.length; ++i) {
        const std::uint32_t w = word_at(i);
        if (!is_prolog_instruction(w)) break;
        ctx.pc = func + 4ull * i;
        if (!step(ctx, w) || ctx.x[31] < stack.low() + 4096) { ++stats.skipped; return; }
        ++stats.prolog_points;
        if (!check_unwind(image_base, info, ctx, func + 4ull * (i + 1), entry, why)) { record("post-prolog", i + 1, why); return; }
    }

    // Epilogs: jump from the end of the prolog straight to each epilog.
    // The epilog may be entered with the extra setup undone by body code
    // just before it (MSVC) or undone by the epilog itself (clang). The
    // unwinder only interprets the codes, so the epilog is consistent if
    // either entry state unwinds correctly at every offset.
    EpilogScope scopes[16];
    const std::uint32_t epilogs = epilog_scopes(info, scopes, 16);
    const GuestContext bases[2] = {after_described_prolog, ctx};
    for (std::uint32_t e = 0; e < epilogs; ++e) {
        const std::uint32_t start = scopes[e].start;
        const std::uint32_t length = sequence_instruction_count(info, scopes[e].index);
        if (start + length >= info.length + 1 || start < prolog) continue;
        bool shaped = true;
        for (std::uint32_t i = 0; i < length && shaped; ++i) shaped = is_epilog_instruction(word_at(start + i));
        if (!shaped || !is_return(word_at(start + length))) continue;
        // If executing the epilog itself never brings SP back to the entry
        // value (stack-cookie push/pop helpers), the function is unbalanced
        // by design and the entry-state comparison does not apply.
        bool balanced = false;
        for (const GuestContext& base_state : bases) {
            GuestContext run = base_state;
            bool ran = true;
            for (std::uint32_t i = 0; i < length && ran; ++i) {
                run.pc = func + 4ull * (start + i);
                ran = step(run, word_at(start + i));
            }
            if (ran && run.x[31] == entry.sp) balanced = true;
        }
        if (!balanced) { ++stats.unbalanced; continue; }
        std::string first_why;
        bool consistent = false;
        std::uint64_t points = 0;
        for (const GuestContext& base_state : bases) {
            GuestContext ectx = base_state;
            bool ok = true;
            points = 0;
            for (std::uint32_t i = 0; i <= length; ++i) {
                const std::uint64_t pc = func + 4ull * (start + i);
                ++points;
                if (!check_unwind(image_base, info, ectx, pc, entry, why)) {
                    if (first_why.empty()) first_why = "offset " + std::to_string(i) + ": " + why;
                    ok = false;
                    break;
                }
                if (i == length) break;
                ectx.pc = pc;
                if (!step(ectx, word_at(start + i))) { ok = false; break; }
            }
            if (ok) { consistent = true; break; }
        }
        stats.epilog_points += points;
        if (!consistent) { record("epilog", start, first_why); return; }
    }
}

void print_parse(const char* label, const ParseStats& s, const std::vector<std::string>& examples) {
    std::uint64_t failed = 0;
    for (const auto& [name, n] : s.failures) failed += n;
    std::printf("  %s: %llu ARM64 images (%llu other skipped), %llu RUNTIME_FUNCTIONs: %llu packed(flag1), "
                "%llu packed(flag2), %llu xdata (%llu epilog scopes), %llu with handler, %llu failed\n",
                label, s.files, s.skipped_files, s.entries, s.packed1, s.packed2, s.xdata, s.epilog_scopes,
                s.handlers, failed);
    for (const auto& [name, n] : s.failures) std::printf("    %s: %llu\n", name.c_str(), n);
    for (const auto& line : examples) std::printf("    e.g. %s\n", line.c_str());
}

void run_parse(const wchar_t* root, const char* label) {
    const auto files = arm64_binaries(root);
    if (files.empty()) {
        std::printf("  %s: directory not present, skipped\n", label);
        return;
    }
    ParseStats stats;
    std::vector<std::string> examples;
    for (const auto& path : files) parse_file(path, stats, examples);
    print_parse(label, stats, examples);
    FX_CHECK(stats.files > 0);
    FX_CHECK(stats.failures.empty());
}

void run_simulation(const wchar_t* root, const char* label, std::uint32_t stride) {
    const auto files = arm64_binaries(root);
    if (files.empty()) {
        std::printf("  %s: directory not present, skipped\n", label);
        return;
    }
    Stack stack;
    SimStats stats;
    std::vector<std::string> examples;
    std::uint64_t seed = 0x9e3779b97f4a7c15ull;
    for (const auto& path : files) {
        MappedImage image;
        if (!map_image(path, image) || image.machine != 0xAA64) continue;
        g_current_file = path.filename().string();
        const std::uint32_t count = image.exception_size / 8;
        for (std::uint32_t i = 0; i < count; i += stride) {
            Arm64RuntimeFunction rf;
            std::memcpy(&rf, image.base + image.exception_rva + 8ull * i, 8);
            simulate_function(image.address(), rf, stack, stats, examples, seed);
        }
    }
    std::printf("  %s: simulated %llu functions (stride %u): %llu prolog offsets, %llu epilog offsets checked; "
                "skipped %llu non-canonical, %llu end_c fragments, %llu unbalanced helpers; %llu mismatches\n",
                label, stats.functions, stride, stats.prolog_points, stats.epilog_points, stats.skipped,
                stats.fragments, stats.unbalanced, stats.mismatches);
    for (const auto& line : examples) std::printf("    e.g. %s\n", line.c_str());
    FX_CHECK(stats.functions > 0);
    FX_CHECK_EQ(stats.mismatches, 0u);
}

} // namespace

FX_TEST(corpus_parse_blender) { run_parse(kBlenderDir, "blender"); }
FX_TEST(corpus_parse_fx_corpus) { run_parse(kCorpusDir, "fx corpus"); }
FX_TEST(corpus_simulate_corpus_prologs_epilogs) { run_simulation(kCorpusDir, "fx corpus", 1); }
FX_TEST(corpus_simulate_blender_prologs_epilogs) { run_simulation(kBlenderDir, "blender", 1); }
