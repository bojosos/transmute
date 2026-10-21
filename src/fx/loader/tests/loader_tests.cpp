// fx loader tests: mapping, relocation, protections, imports, exports,
// forwarders, ordinals, cycles, delay load, module APIs, command line.

#include "test_support.hpp"

#include "fx/loader/command_line.hpp"
#include "fx/loader/loader_internal.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

using namespace transmute::fx;
using namespace transmute::fx::loader;
using namespace fxl_test;

namespace {

#define FXL_SKIP_UNLESS(path)                                                   \
    do {                                                                        \
        if (!exists(path)) {                                                    \
            std::printf("  skipped: %ls not built\n", std::wstring(path).c_str()); \
            return;                                                             \
        }                                                                       \
    } while (0)

std::vector<std::uint8_t> read_file(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

DWORD protection_at(std::uint64_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi));
    return mbi.Protect;
}

// Writable image pages read back as PAGE_WRITECOPY until first written
// (image-section views, as for native images) or PAGE_READWRITE (copies).
bool writable_data(DWORD protect) { return protect == PAGE_READWRITE || protect == PAGE_WRITECOPY; }

DWORD state_at(std::uint64_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi));
    return mbi.State;
}

const PeSection* find_section(const PeHeaders& h, const char* name) {
    for (const auto& s : h.sections) {
        if (std::strcmp(s.name, name) == 0) return &s;
    }
    return nullptr;
}

PeHeaders headers_at(std::uint64_t base) {
    PeHeaders h;
    std::string error;
    parse_pe_headers({reinterpret_cast<const std::uint8_t*>(base), 4096}, h, error);
    return h;
}

std::int64_t rva_to_offset(const PeHeaders& h, std::uint32_t rva) {
    for (const auto& s : h.sections) {
        if (rva >= s.rva && rva < s.rva + s.raw_size) return static_cast<std::int64_t>(s.raw_offset) + (rva - s.rva);
    }
    return -1;
}

// Every DIR64 relocation target holds the file value plus the load delta.
std::size_t check_relocations(const MappedImage& image, const std::vector<std::uint8_t>& file) {
    const auto& h = image.headers;
    const PeDirectory dir = h.dir(dir_basereloc);
    const std::int64_t start = rva_to_offset(h, dir.rva);
    std::size_t checked = 0;
    if (start < 0) return 0;
    for (std::int64_t at = start; at + 8 <= start + dir.size;) {
        std::uint32_t page, size;
        std::memcpy(&page, file.data() + at, 4);
        std::memcpy(&size, file.data() + at + 4, 4);
        if (size < 8) break;
        for (std::uint32_t i = 8; i + 2 <= size; i += 2) {
            std::uint16_t e;
            std::memcpy(&e, file.data() + at + i, 2);
            if ((e >> 12) != 10) continue;
            const std::uint32_t rva = page + (e & 0xfff);
            const std::int64_t off = rva_to_offset(h, rva);
            if (off < 0) continue;
            std::uint64_t original, mapped;
            std::memcpy(&original, file.data() + off, 8);
            std::memcpy(&mapped, image.base + rva, 8);
            FX_CHECK_EQ(mapped, original + static_cast<std::uint64_t>(image.delta));
            ++checked;
        }
        at += size;
    }
    return checked;
}

std::uint64_t exe_base(const wchar_t* path_name, bool force_relocation = false, bool legacy = false) {
    const std::wstring path = legacy ? legacy_fixture(path_name) : fixture(path_name);
    LoaderConfig config;
    config.application_path = path;
    config.force_relocation = force_relocation;
    loader_configure(config);
    std::string error;
    bool init_failed = false;
    const std::uint64_t base = load_exe(path, error, init_failed);
    if (!base) std::printf("  load_exe(%ls): %s\n", path.c_str(), error.c_str());
    return base;
}

const GuestModule* module_named(const wchar_t* name) {
    for (const auto* m : guest_modules()) {
        if (_wcsicmp(m->name.c_str(), name) == 0) return m;
    }
    return nullptr;
}

bool has_diagnostic(LoaderDiagnostic::Kind kind, std::string_view dll, std::string_view function = {}) {
    for (const auto& d : loader_diagnostics()) {
        if (d.kind == kind && _stricmp(d.dll.c_str(), std::string(dll).c_str()) == 0 &&
            (function.empty() || d.function == function)) {
            return true;
        }
    }
    return false;
}

std::uint64_t proc(std::uint64_t module, const char* name) { return guest_get_proc_address(module, name); }

} // namespace

// ---- PE parsing and mapping -----------------------------------------------------

FX_TEST(pe_headers_of_arm64_fixtures) {
    const auto path = legacy_fixture(L"return42_arm64.exe");
    FXL_SKIP_UNLESS(path);
    PeHeaders h;
    std::string error;
    FX_REQUIRE(read_image_headers(path, h, error));
    FX_CHECK_EQ(h.machine, kMachineArm64);
    FX_CHECK(!h.is_dll());
    FX_CHECK(h.entry_rva != 0);
    FX_CHECK(find_section(h, ".text") != nullptr);
    FX_CHECK_EQ(probe_pe_file(path), kMachineArm64);
    bool is_dll = false;
    FX_CHECK_EQ(probe_pe_file(legacy_fixture(L"guest_leaf.dll"), &is_dll), kMachineArm64);
    FX_CHECK(is_dll);
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    FX_CHECK_EQ(probe_pe_file(self), kMachineAmd64);
    FX_CHECK_EQ(probe_pe_file(L"C:\\definitely\\missing.dll"), 0);
}

FX_TEST(map_return42_at_preferred_base_with_protections) {
    const auto path = legacy_fixture(L"return42_arm64.exe");
    FXL_SKIP_UNLESS(path);
  for (const bool view : {false, true}) {
    MappedImage image;
    std::string error;
    MapOptions options;
    options.image_view = view;
    FX_REQUIRE(map_image(path, options, image, error));
    FX_CHECK_EQ(image.view, view);
    if (view) {
        // At the base the kernel relocated the image for (its ASLR base).
        FX_CHECK_EQ(headers_at(reinterpret_cast<std::uint64_t>(image.base)).image_base,
                    reinterpret_cast<std::uint64_t>(image.base));
        FX_CHECK_EQ(static_cast<std::uint64_t>(image.delta),
                    reinterpret_cast<std::uint64_t>(image.base) - image.headers.image_base);
        FX_CHECK_EQ(image.relocations_applied, 0);
    } else {
        FX_CHECK_EQ(reinterpret_cast<std::uint64_t>(image.base), image.headers.image_base);
        FX_CHECK_EQ(image.delta, 0);
    }
    FX_REQUIRE(protect_image(image, error));
    const auto base = reinterpret_cast<std::uint64_t>(image.base);
    FX_CHECK_EQ(protection_at(base), static_cast<DWORD>(PAGE_READONLY));   // headers
    const auto* text = find_section(image.headers, ".text");
    FX_REQUIRE(text);
    FX_CHECK_EQ(protection_at(base + text->rva), static_cast<DWORD>(PAGE_READONLY));
    FX_CHECK_EQ(image.text_begin, base + text->rva);
    FX_CHECK(image.text_end > image.text_begin);
    // Section bytes match the file.
    const auto file = read_file(path);
    const std::uint32_t n = std::min(text->raw_size, text->virtual_size);
    FX_CHECK(std::memcmp(image.base + text->rva, file.data() + text->raw_offset, n) == 0);
    FX_CHECK(image.image_hash != 0);
    unmap_image(image);
    FX_CHECK_EQ(state_at(base), static_cast<DWORD>(MEM_FREE));
  }
}

FX_TEST(map_relocates_at_forced_non_preferred_base) {
    for (const bool view : {false, true})
    for (const wchar_t* name : {L"relocated_pointer_arm64.exe", L"guest_library.dll", L"win32_import_arm64.exe"}) {
        const auto path = legacy_fixture(name);
        FXL_SKIP_UNLESS(path);
        MapOptions options;
        options.force_relocation = true;
        options.image_view = view;
        MappedImage image;
        std::string error;
        FX_REQUIRE(map_image(path, options, image, error));
        const auto base = reinterpret_cast<std::uint64_t>(image.base);
        FX_CHECK(base != image.headers.image_base);
        FX_CHECK_EQ(static_cast<std::uint64_t>(image.delta), base - image.headers.image_base);
        const std::size_t checked = check_relocations(image, read_file(path));
        if (std::wstring_view(name) != L"win32_import_arm64.exe") FX_CHECK(checked > 0);   // holds pointers
        // Views placed at the kernel's base were relocated by the kernel.
        if (!image.view) FX_CHECK_EQ(static_cast<std::int64_t>(checked), image.relocations_applied);
        // The in-memory ImageBase reflects the actual base.
        FX_CHECK_EQ(headers_at(base).image_base, base);
        unmap_image(image);
    }
}

FX_TEST(image_hash_identity) {
    const auto a = legacy_fixture(L"return42_arm64.exe");
    const auto b = legacy_fixture(L"relocated_pointer_arm64.exe");
    FXL_SKIP_UNLESS(a);
    FXL_SKIP_UNLESS(b);
    MappedImage x, y, z;
    std::string error;
    FX_REQUIRE(map_image(a, {}, x, error));
    MapOptions reloc;
    reloc.force_relocation = true;
    FX_REQUIRE(map_image(a, reloc, y, error));   // same file, different base: same identity
    FX_REQUIRE(map_image(b, reloc, z, error));
    FX_CHECK_EQ(x.image_hash, y.image_hash);
    FX_CHECK(x.image_hash != z.image_hash);
    unmap_image(x);
    unmap_image(y);
    unmap_image(z);
}

FX_TEST(map_rejects_non_arm64_and_garbage) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    MappedImage image;
    std::string error;
    FX_CHECK(!map_image(self, {}, image, error));
    FX_CHECK(error.find("machine") != std::string::npos);
    FX_CHECK(!map_image(L"C:\\definitely\\missing.dll", {}, image, error));
    // A text file is not a PE image.
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring junk = std::wstring(temp) + L"fxl_junk.dll";
    { std::ofstream(junk) << "not a PE image at all, just text"; }
    FX_CHECK(!map_image(junk, {}, image, error));
    FX_CHECK_EQ(probe_pe_file(junk), 0);
    DeleteFileW(junk.c_str());
}

// ---- execution of real fixtures through the loader --------------------------------

FX_TEST(return42_and_relocated_pointer_execute) {
    FXL_SKIP_UNLESS(legacy_fixture(L"relocated_pointer_arm64.exe"));
    for (const bool force : {false, true}) {
        fresh_loader();
        const std::uint64_t base = exe_base(L"relocated_pointer_arm64.exe", force, true);
        FX_REQUIRE(base);
        const GuestModule* m = find_guest_module(base);
        FX_REQUIRE(m);
        FX_CHECK_EQ(m->base, base);
        FX_CHECK(!m->is_dll);
        FX_CHECK_EQ(m->tls_index, kNoTls);
        if (force) FX_CHECK(base != 0x140000000ull);
        FX_CHECK_EQ(run_guest(m->entry), 42u);
    }
    fresh_loader();
    const std::uint64_t base = exe_base(L"return42_arm64.exe", false, true);
    FX_REQUIRE(base);
    FX_CHECK_EQ(run_guest(find_guest_module(base)->entry), 42u);
}

FX_TEST(win32_import_binds_host_function) {
    FXL_SKIP_UNLESS(legacy_fixture(L"win32_import_arm64.exe"));
    fresh_loader();
    stubs().emulations["GetCurrentProcessId"] = [](GuestContext&) -> std::uint64_t { return 4242; };
    const std::uint64_t base = exe_base(L"win32_import_arm64.exe", true, true);
    FX_REQUIRE(base);
    const void* real = reinterpret_cast<const void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentProcessId"));
    bool found = false;
    for (const auto& b : stubs().host_binds) {
        if (b.function == "GetCurrentProcessId") {
            found = true;
            FX_CHECK_EQ(_stricmp(b.dll.c_str(), "KERNEL32.dll"), 0);
            FX_CHECK(b.host_address == real);
        }
    }
    FX_CHECK(found);
    FX_CHECK_EQ(loader_stats().host_imports, 1u);
    FX_CHECK_EQ(run_guest(find_guest_module(base)->entry), 4242u);
}

FX_TEST(guest_dll_chain_initializes_in_dependency_order) {
    FXL_SKIP_UNLESS(legacy_fixture(L"guest_app.exe"));
    fresh_loader();
    const std::uint64_t base = exe_base(L"guest_app.exe", false, true);
    FX_REQUIRE(base);
    const auto modules = guest_modules();
    FX_REQUIRE(modules.size() == 3);
    FX_CHECK(modules[0]->name == L"guest_app.exe");
    FX_CHECK(modules[1]->name == L"guest_library.dll");
    FX_CHECK(modules[2]->name == L"guest_leaf.dll");
    const GuestModule* library = modules[1];
    // guest_app and guest_library share the preferred base 0x140000000.
    FX_CHECK(library->base != base);
    FX_CHECK(library->is_dll);
    FX_CHECK(library->pdata_size != 0);
    // DllMain(base, DLL_PROCESS_ATTACH, static load) ran exactly once.
    int attaches = 0;
    for (const auto& call : stubs().guest_calls) {
        if (call.entry == library->entry) {
            ++attaches;
            FX_CHECK_EQ(call.args.size(), 3u);
            FX_CHECK_EQ(call.args[0], library->base);
            FX_CHECK_EQ(call.args[1], 1u);
            FX_CHECK_EQ(call.args[2], 1u);
        }
    }
    FX_CHECK_EQ(attaches, 1);
    FX_CHECK_EQ(stubs().cache_loaded.size(), 3u);
    const auto bias = proc(library->base, "guest_bias");
    FX_REQUIRE(bias);
    FX_CHECK_EQ(*reinterpret_cast<const int*>(bias), 0);   // DllMain wrote reason - 1
    FX_CHECK_EQ(run_guest(modules[0]->entry), 42u);
    FX_CHECK_EQ(loader_stats().guest_imports, 3u);
}

FX_TEST(failing_dllmain_fails_the_load_and_unmaps) {
    FXL_SKIP_UNLESS(legacy_fixture(L"guest_fail.dll"));
    fresh_loader();
    LoaderConfig config;
    config.application_path = legacy_fixture(L"guest_app.exe");
    loader_configure(config);
    SetLastError(0);
    const std::uint64_t h = guest_load_library(L"guest_fail.dll", 0);
    FX_CHECK_EQ(h, 0u);
    FX_CHECK_EQ(GetLastError(), static_cast<DWORD>(ERROR_DLL_INIT_FAILED));
    FX_REQUIRE(stubs().guest_calls.size() >= 2);
    const auto& attach = stubs().guest_calls[0];
    const auto& detach = stubs().guest_calls[1];
    FX_CHECK_EQ(attach.args[1], 1u);
    FX_CHECK_EQ(detach.args[1], 0u);
    const std::uint64_t base = attach.args[0];
    FX_CHECK(find_guest_module(base) == nullptr);
    FX_CHECK_EQ(state_at(base), static_cast<DWORD>(MEM_FREE));
    FX_CHECK_EQ(guest_get_module_handle(L"guest_fail.dll"), 0u);
    FX_CHECK(guest_modules().empty());
    bool invalidated = false;
    for (const auto& [a, size] : stubs().invalidated) invalidated |= a == base && size != 0;
    FX_CHECK(invalidated);
    bool diagnosed = false;
    for (const auto& d : loader_diagnostics()) diagnosed |= d.kind == LoaderDiagnostic::Kind::init_failed;
    FX_CHECK(diagnosed);
}

// ---- exports, forwarders, ordinals, sections ---------------------------------------

FX_TEST(exports_by_name_ordinal_data_and_forwarders) {
    FXL_SKIP_UNLESS(fixture(L"fxl_exports.dll"));
    fresh_loader();
    LoaderConfig config;
    config.application_path = fixture(L"fxl_importer.exe");
    loader_configure(config);
    const std::uint64_t h = guest_load_library(fixture(L"fxl_exports.dll"), 0);
    FX_REQUIRE(h);
    const GuestModule* m = find_guest_module(h);
    FX_REQUIRE(m);
    FX_CHECK(m->name == L"fxl_exports.dll");

    const auto named = proc(h, "named_one");
    FX_CHECK(named >= m->text_begin && named < m->text_end);
    FX_CHECK_EQ(run_guest(named), 1u);
    const auto by_ordinal = proc(h, reinterpret_cast<const char*>(5));
    FX_REQUIRE(by_ordinal);
    FX_CHECK(by_ordinal != named);
    FX_CHECK_EQ(run_guest(by_ordinal), 5u);
    FX_CHECK_EQ(proc(h, reinterpret_cast<const char*>(1)), named);
    FX_CHECK_EQ(proc(h, reinterpret_cast<const char*>(3)), 0u);   // gap in the ordinal table

    const auto data = proc(h, "data_value");
    FX_REQUIRE(data);
    FX_CHECK_EQ(*reinterpret_cast<const int*>(data), 77);
    FX_CHECK(writable_data(protection_at(data)));

    // Forwarders: to another guest DLL (loaded on demand), by ordinal, chained, to the host.
    const auto leaf = proc(h, "fwd_leaf");
    FX_REQUIRE(leaf);
    const GuestModule* target = find_guest_module(leaf);
    FX_REQUIRE(target);
    FX_CHECK(target->name == L"fxl_fwd_target.dll");
    FX_CHECK_EQ(run_guest(leaf), 42u);
    const auto ord = proc(h, "fwd_ord");
    FX_CHECK(find_guest_module(ord) == target);
    FX_CHECK_EQ(run_guest(ord), 33u);
    FX_CHECK_EQ(proc(h, "fwd_chain"), leaf);
    const auto host = proc(h, "fwd_host");
    FX_CHECK(is_fake_host(host));
    const auto& last = stubs().host_binds.back();
    FX_CHECK_EQ(last.function, std::string("GetCurrentProcessId"));
    FX_CHECK(last.host_address ==
             reinterpret_cast<const void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentProcessId")));
    FX_CHECK(loader_stats().forwarders >= 4);

    SetLastError(0);
    FX_CHECK_EQ(proc(h, "no_such_export"), 0u);
    FX_CHECK_EQ(GetLastError(), static_cast<DWORD>(ERROR_PROC_NOT_FOUND));

    ExportView view;
    FX_REQUIRE(view.open(reinterpret_cast<const std::uint8_t*>(h)));
    FX_CHECK(view.name_of_ordinal(1) == "named_one");
    FX_CHECK(view.name_of_ordinal(5).empty());
    FX_CHECK(view.by_name("fwd_leaf").kind == ExportLookup::Kind::forwarder);
    FX_CHECK(view.by_name("fwd_leaf").forwarder == "fxl_fwd_target.target_answer");
}

FX_TEST(section_protections_and_writable_code) {
    FXL_SKIP_UNLESS(fixture(L"fxl_exports.dll"));
    fresh_loader();
    const std::uint64_t h = guest_load_library(fixture(L"fxl_exports.dll"), LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    FX_REQUIRE(h);
    const GuestModule* m = find_guest_module(h);
    FX_REQUIRE(m);
    const PeHeaders headers = headers_at(h);
    const auto* text = find_section(headers, ".text");
    const auto* rdata = find_section(headers, ".rdata");
    const auto* data = find_section(headers, ".data");
    const auto* rwx = find_section(headers, ".rwx");
    FX_REQUIRE(text && rdata && data && rwx);
    FX_CHECK((rwx->characteristics & IMAGE_SCN_MEM_EXECUTE) && (rwx->characteristics & IMAGE_SCN_MEM_WRITE));
    FX_CHECK_EQ(protection_at(h), static_cast<DWORD>(PAGE_READONLY));
    FX_CHECK_EQ(protection_at(h + text->rva), static_cast<DWORD>(PAGE_READONLY));
    FX_CHECK_EQ(protection_at(h + rdata->rva), static_cast<DWORD>(PAGE_READONLY));
    FX_CHECK(writable_data(protection_at(h + data->rva)));
    FX_CHECK(writable_data(protection_at(h + rwx->rva)));
    FX_CHECK_EQ(m->text_begin, h + text->rva);
    FX_CHECK(m->text_end >= h + rwx->rva + rwx->virtual_size);   // union covers the RWX section
    // The writable code section holds data the guest can modify and run.
    const auto probe = proc(h, "rwx_probe");
    FX_CHECK_EQ(run_guest(probe), 5u);
    *reinterpret_cast<volatile int*>(h + rwx->rva) = 9;
    FX_CHECK_EQ(run_guest(probe), 9u);
}

// ---- the importer: every import kind ---------------------------------------------------

namespace {
std::vector<std::string> g_missing_calls;
std::uint64_t record_missing(const char* dll, const char* function) {
    g_missing_calls.push_back(std::string(dll) + "!" + function);
    return 5;
}
} // namespace

FX_TEST(importer_binds_every_import_kind) {
    FXL_SKIP_UNLESS(fixture(L"fxl_importer.exe"));
    fresh_loader();
    stubs().emulations["GetCurrentProcessId"] = [](GuestContext&) -> std::uint64_t { return 4242; };
    g_missing_calls.clear();
    loader_set_missing_import_handler(record_missing);
    const std::uint64_t base = exe_base(L"fxl_importer.exe");
    FX_REQUIRE(base);
    for (const wchar_t* name : {L"fxl_importer.exe", L"fxl_exports.dll", L"fxl_fwd_target.dll",
                                L"fxl_cycle_a.dll", L"fxl_cycle_b.dll", L"fxl_tls.dll"}) {
        FX_CHECK(module_named(name) != nullptr);
    }
    FX_CHECK(module_named(L"fxl_delay.dll") == nullptr);   // delay-loaded
    FX_CHECK(has_diagnostic(LoaderDiagnostic::Kind::missing_function, "KERNEL32.dll", "TransmuteFxMissingFunction"));
    FX_CHECK(has_diagnostic(LoaderDiagnostic::Kind::missing_dll, "fxl_missing_dll.dll"));
    bool cycle = false;
    for (const auto& d : loader_diagnostics()) cycle |= d.kind == LoaderDiagnostic::Kind::import_cycle;
    FX_CHECK(cycle);
    const auto stats = loader_stats();
    FX_CHECK_EQ(stats.missing_imports, 2u);
    FX_CHECK_EQ(stats.host_imports, 2u);    // GetCurrentProcessId, ResolveDelayLoadedAPI
    FX_CHECK(stats.guest_imports >= 11u);
    FX_CHECK_EQ(stats.modules_mapped, 6u);

    // Initialization: dependencies first; the EXE's TLS callback last.
    const GuestModule* tls = module_named(L"fxl_tls.dll");
    FX_REQUIRE(tls);
    const auto events = proc(tls->base, "g_events");
    const auto count = proc(tls->base, "g_event_count");
    FX_REQUIRE(events && count);
    FX_REQUIRE(*reinterpret_cast<const long*>(count) == 2);
    FX_CHECK_EQ(reinterpret_cast<const unsigned long*>(events)[0], 0x101ul);   // TLS callback, PROCESS_ATTACH
    FX_CHECK_EQ(reinterpret_cast<const unsigned long*>(events)[1], 0x201ul);   // DllMain, PROCESS_ATTACH
    FX_CHECK_EQ(*reinterpret_cast<const long*>(proc(base, "exe_event_count")), 1);
    FX_CHECK_EQ(*reinterpret_cast<const unsigned long*>(proc(base, "exe_events")), 1ul);
    std::size_t tls_attach = SIZE_MAX, exe_callback = SIZE_MAX;
    for (std::size_t i = 0; i < stubs().guest_calls.size(); ++i) {
        const auto& c = stubs().guest_calls[i];
        if (c.args.size() == 3 && c.args[0] == tls->base && tls_attach == SIZE_MAX) tls_attach = i;
        if (c.args.size() == 3 && c.args[0] == base && exe_callback == SIZE_MAX) exe_callback = i;
    }
    FX_CHECK(tls_attach < exe_callback && exe_callback != SIZE_MAX);

    // Run the imports.
    FX_CHECK_EQ(run_guest(proc(base, "fxl_sum")), 307u);
    FX_CHECK_EQ(run_guest(find_guest_module(base)->entry), 307u);
    FX_CHECK_EQ(run_guest(proc(base, "fxl_host_pid")), 8484u);
    FX_CHECK_EQ(run_guest(proc(base, "fxl_tls_value")), 1234u);
    FX_CHECK_EQ(run_guest(proc(base, "fxl_call_missing")), 10u);
    FX_REQUIRE(g_missing_calls.size() == 2);
    FX_CHECK_EQ(g_missing_calls[0], std::string("KERNEL32.dll!TransmuteFxMissingFunction"));
    FX_CHECK_EQ(g_missing_calls[1], std::string("fxl_missing_dll.dll!fxl_missing_function"));
    // Missing thunks are shared per function.
    FX_CHECK_EQ(missing_import_thunk("KERNEL32.dll", "TransmuteFxMissingFunction"),
                missing_import_thunk("KERNEL32.dll", "TransmuteFxMissingFunction"));
}

FX_TEST(delay_load_resolves_on_first_call) {
    FXL_SKIP_UNLESS(fixture(L"fxl_importer.exe"));
    fresh_loader();
    int resolves = 0;
    stubs().emulations["ResolveDelayLoadedAPI"] = [&resolves](GuestContext& ctx) -> std::uint64_t {
        ++resolves;
        return guest_resolve_delay_loaded_api(ctx.x[0], ctx.x[1], ctx.x[2], ctx.x[3], ctx.x[4],
                                              static_cast<std::uint32_t>(ctx.x[5]));
    };
    const std::uint64_t base = exe_base(L"fxl_importer.exe");
    FX_REQUIRE(base);
    FX_CHECK_EQ(guest_get_module_handle(L"fxl_delay.dll"), 0u);
    const auto call = proc(base, "fxl_call_delay");
    FX_CHECK_EQ(run_guest(call), 99u);
    FX_CHECK_EQ(resolves, 1);
    const std::uint64_t delay = guest_get_module_handle(L"fxl_delay.dll");
    FX_CHECK(delay != 0);
    FX_CHECK(find_guest_module(delay) != nullptr);
    FX_CHECK_EQ(run_guest(call), 99u);
    FX_CHECK_EQ(resolves, 1);   // the IAT slot now holds the export
}

FX_TEST(circular_imports_resolve_both_ways) {
    FXL_SKIP_UNLESS(fixture(L"fxl_cycle_a.dll"));
    fresh_loader();
    const std::uint64_t a = guest_load_library(fixture(L"fxl_cycle_a.dll"), LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    FX_REQUIRE(a);
    FX_CHECK(module_named(L"fxl_cycle_b.dll") != nullptr);
    FX_CHECK_EQ(run_guest(proc(a, "cycle_a_calls_b")), 30u);
    FX_CHECK(has_diagnostic(LoaderDiagnostic::Kind::import_cycle, "fxl_cycle_a.dll"));
    // A cycle pins both modules (as on Windows): FreeLibrary keeps them mapped.
    FX_CHECK(guest_free_library(a));
    FX_CHECK(find_guest_module(a) != nullptr);
}

FX_TEST(abi_import_replacement_wins_over_guest_export) {
    FXL_SKIP_UNLESS(fixture(L"fxl_importer.exe"));
    fresh_loader();
    stubs().replacements["named_one"] = fake_host([](GuestContext&) -> std::uint64_t { return 100; });
    // A host DLL lacking an export (ARM64-only CRT functions) asks the abi too.
    stubs().replacements["TransmuteFxMissingFunction"] = fake_host([](GuestContext&) -> std::uint64_t { return 3; });
    g_missing_calls.clear();
    loader_set_missing_import_handler(record_missing);
    const std::uint64_t base = exe_base(L"fxl_importer.exe");
    FX_REQUIRE(base);
    FX_CHECK_EQ(run_guest(proc(base, "fxl_sum")), 406u);
    const auto& q = stubs().replacement_queries;
    FX_CHECK(std::find(q.begin(), q.end(), std::string("fxl_exports.dll!named_one")) != q.end());
    FX_CHECK_EQ(loader_stats().replaced_imports, 2u);
    FX_CHECK_EQ(loader_stats().missing_imports, 1u);
    FX_CHECK_EQ(run_guest(proc(base, "fxl_call_missing")), 3u + 5u);
    FX_CHECK_EQ(g_missing_calls.size(), 1u);
    // GetProcAddress applies the same replacement.
    FX_CHECK_EQ(proc(guest_get_module_handle(L"fxl_exports.dll"), "named_one"), stubs().replacements["named_one"]);
}

// ---- module APIs ---------------------------------------------------------------------

FX_TEST(module_handles_names_and_refcounts) {
    FXL_SKIP_UNLESS(fixture(L"fxl_importer.exe"));
    fresh_loader();
    const std::uint64_t base = exe_base(L"fxl_importer.exe");
    FX_REQUIRE(base);
    const std::uint64_t exports = guest_get_module_handle(L"FXL_EXPORTS.DLL");
    FX_CHECK(exports != 0);
    FX_CHECK_EQ(guest_get_module_handle(L""), base);
    FX_CHECK_EQ(guest_get_module_handle(L"fxl_exports"), exports);
    FX_CHECK_EQ(guest_get_module_handle(fixture(L"fxl_exports.dll")), exports);
    FX_CHECK_EQ(guest_get_module_handle(L"kernel32.dll"),
                reinterpret_cast<std::uint64_t>(GetModuleHandleW(L"kernel32.dll")));
    FX_CHECK_EQ(guest_get_module_handle(L"no_such_module.dll"), 0u);
    FX_CHECK(guest_module_file_name(0) == fixture(L"fxl_importer.exe"));
    FX_CHECK(guest_module_file_name(exports) == fixture(L"fxl_exports.dll"));
    FX_CHECK_EQ(guest_module_from_address(exports + 0x1000), exports);
    FX_CHECK_EQ(proc(0, "fxl_sum"), proc(base, "fxl_sum"));   // NULL module = EXE
    // Loading a module of the static set returns the same handle; freeing it keeps it.
    FX_CHECK_EQ(guest_load_library(L"fxl_exports.dll", 0), exports);
    FX_CHECK(guest_free_library(exports));
    FX_CHECK(guest_free_library(exports));
    FX_CHECK(find_guest_module(exports) != nullptr);
}

FX_TEST(free_library_detaches_unmaps_and_invalidates) {
    FXL_SKIP_UNLESS(fixture(L"fxl_tls.dll"));
    fresh_loader();
    const std::uint64_t a = guest_load_library(fixture(L"fxl_tls.dll"), 0);
    FX_REQUIRE(a);
    FX_CHECK_EQ(guest_load_library(L"fxl_tls.dll", 0), a);   // refcount 2
    const GuestModule* m = find_guest_module(a);
    FX_REQUIRE(m);
    const std::uint64_t size = m->size;
    const std::uint64_t entry = m->entry;
    FX_CHECK(guest_free_library(a));
    FX_CHECK(find_guest_module(a) != nullptr);
    stubs().guest_calls.clear();
    FX_CHECK(guest_free_library(a));
    FX_CHECK(find_guest_module(a) == nullptr);
    FX_CHECK_EQ(state_at(a), static_cast<DWORD>(MEM_FREE));
    bool detach = false;
    for (const auto& c : stubs().guest_calls) detach |= c.entry == entry && c.args[1] == DLL_PROCESS_DETACH;
    FX_CHECK(detach);
    FX_CHECK(std::find(stubs().cache_unloaded.begin(), stubs().cache_unloaded.end(), L"fxl_tls.dll") !=
             stubs().cache_unloaded.end());
    bool invalidated = false;
    for (const auto& [address, length] : stubs().invalidated) invalidated |= address == a && length == size;
    FX_CHECK(invalidated);
    FX_CHECK(!guest_free_library(a));   // no longer a module
    // Reloading reuses the TLS slot.
    const std::uint64_t b = guest_load_library(fixture(L"fxl_tls.dll"), 0);
    FX_REQUIRE(b);
    FX_CHECK_EQ(find_guest_module(b)->tls_index, 0u);
}

FX_TEST(find_guest_module_is_lock_free_under_concurrent_loads) {
    FXL_SKIP_UNLESS(fixture(L"fxl_tls.dll"));
    fresh_loader();
    stubs().execute = false;   // no guest code: just mapping churn
    const std::uint64_t pinned = guest_load_library(fixture(L"fxl_exports.dll"), LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    FX_REQUIRE(pinned);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> lookups{0}, misses{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const GuestModule* m = find_guest_module(pinned + 0x1000);
                if (!m || m->base != pinned) misses.fetch_add(1);
                if (find_guest_module(0x10)) misses.fetch_add(1);
                lookups.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // Mapping is fast (image views): let the readers start before the churn.
    for (int i = 0; i < 5000 && lookups.load() == 0; ++i) Sleep(1);
    for (int i = 0; i < 25; ++i) {
        const std::uint64_t h = guest_load_library(fixture(L"fxl_tls.dll"), 0);
        FX_CHECK(h != 0);
        FX_CHECK(find_guest_module(h + 16) != nullptr);
        guest_free_library(h);
    }
    stop = true;
    for (auto& t : readers) t.join();
    FX_CHECK_EQ(misses.load(), 0u);
    FX_CHECK(lookups.load() > 0);
}

FX_TEST(host_libraries_and_proc_addresses) {
    fresh_loader();
    const std::uint64_t k32 = guest_load_library(L"kernel32.dll", 0);
    FX_CHECK_EQ(k32, reinterpret_cast<std::uint64_t>(GetModuleHandleW(L"kernel32.dll")));
    const auto tick = proc(k32, "GetTickCount");
    FX_CHECK(is_fake_host(tick));
    const void* real = reinterpret_cast<const void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTickCount"));
    FX_REQUIRE(!stubs().host_binds.empty());
    FX_CHECK(stubs().host_binds.back().host_address == real);
    FX_CHECK_EQ(_stricmp(stubs().host_binds.back().dll.c_str(), "kernel32.dll"), 0);
    // Security software may EAT-hook kernel32: export entries (or the whole
    // export directory) then point outside the image. Compare with what the
    // host resolves rather than with the image's own tables.
    ExportView view;
    const auto ordinal = view.open(reinterpret_cast<const std::uint8_t*>(k32)) ? view.by_name("GetTickCount").ordinal : 0;
    if (ordinal != 0) {
        const void* real_by_ordinal = reinterpret_cast<const void*>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), MAKEINTRESOURCEA(ordinal)));
        const auto by_ordinal = proc(k32, reinterpret_cast<const char*>(static_cast<std::uintptr_t>(ordinal)));
        FX_CHECK(is_fake_host(by_ordinal));
        FX_CHECK(stubs().host_binds.back().host_address == real_by_ordinal);
        if (real_by_ordinal == real) FX_CHECK_EQ(by_ordinal, tick);
        FX_CHECK_EQ(stubs().host_binds.back().function, std::string("GetTickCount"));
    } else {
        std::printf("  note: kernel32 export directory not readable in place (hooked?); ordinal checks skipped\n");
    }
    SetLastError(0);
    FX_CHECK_EQ(proc(k32, "NoSuchKernel32Function"), 0u);
    FX_CHECK_EQ(GetLastError(), static_cast<DWORD>(ERROR_PROC_NOT_FOUND));
    FX_CHECK(guest_load_library(L"api-ms-win-core-sysinfo-l1-1-0.dll", 0) != 0);
    FX_CHECK_EQ(guest_load_library(L"no_such_library_anywhere.dll", 0), 0u);
    FX_CHECK(guest_free_library(k32));
    FX_CHECK(is_known_dll(L"kernel32.dll"));
    FX_CHECK(is_known_dll(L"ntdll.dll"));
    FX_CHECK(!is_known_dll(L"guest_leaf.dll"));
}

FX_TEST(datafile_loads_use_the_host_resource_loader) {
    FXL_SKIP_UNLESS(fixture(L"fxl_exports.dll"));
    fresh_loader();
    const std::uint64_t h = guest_load_library(fixture(L"fxl_exports.dll"), LOAD_LIBRARY_AS_DATAFILE);
    FX_CHECK(h != 0);
    FX_CHECK((h & 3) != 0);
    FX_CHECK_EQ(guest_get_module_handle(L"fxl_exports.dll"), 0u);
    FX_CHECK(guest_modules().empty());
    FX_CHECK(guest_free_library(h));
    const std::uint64_t r = guest_load_library(fixture(L"fxl_exports.dll"), LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    FX_CHECK(r != 0);
    FX_CHECK(guest_free_library(r));
}

FX_TEST(pin_guest_library_retains_queued_callback_code) {
    FXL_SKIP_UNLESS(fixture(L"fxl_exports.dll"));
    fresh_loader();
    stubs().execute = false;
    const auto module = guest_load_library(fixture(L"fxl_exports.dll"), LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    FX_REQUIRE(module != 0);
    FX_CHECK(guest_pin_library(module));
    FX_CHECK(guest_free_library(module));
    FX_CHECK(find_guest_module(module) != nullptr);
    FX_CHECK(!guest_pin_library(0x1234));
}

FX_TEST(search_path_environment_for_guest_dependencies) {
    FXL_SKIP_UNLESS(fixture(L"fxl_exports.dll"));
    const DWORD size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring previous(size, L'\0');
    if (size) previous.resize(GetEnvironmentVariableW(L"PATH", previous.data(), size));
    const std::wstring path = L"C:\\fx-missing-directory;" + directory_of(fixture(L"fxl_exports.dll"));
    SetEnvironmentVariableW(L"PATH", path.c_str());
    fresh_loader();
    LoaderConfig config;
    config.application_path = L"C:\\fx-missing-application\\program.exe";
    loader_configure(config);
    stubs().execute = false;
    const auto found = guest_load_library(L"fxl_exports.dll", 0);
    FX_CHECK(found != 0);
    if (found) FX_CHECK(find_guest_module(found)->path == fixture(L"fxl_exports.dll"));
    fresh_loader();
    loader_configure(config);
    // Explicit secure search flags must continue excluding PATH.
    FX_CHECK_EQ(guest_load_library(L"fxl_exports.dll", LOAD_LIBRARY_SEARCH_APPLICATION_DIR), 0u);
    SetEnvironmentVariableW(L"PATH", size ? previous.c_str() : nullptr);
}

FX_TEST(search_path_directories) {
    FXL_SKIP_UNLESS(legacy_fixture(L"guest_library.dll"));
    // LaunchOptions::dll_directories.
    fresh_loader();
    LoaderConfig config;
    config.application_path = fixture(L"fxl_importer.exe");
    config.dll_directories = {L"" FXL_LEGACY_FIXTURE_DIR};
    loader_configure(config);
    const std::uint64_t leaf = guest_load_library(L"guest_leaf.dll", 0);
    FX_REQUIRE(leaf);
    FX_CHECK(find_guest_module(leaf)->path == legacy_fixture(L"guest_leaf.dll"));
    // AddDllDirectory + LOAD_LIBRARY_SEARCH_USER_DIRS.
    fresh_loader();
    config.dll_directories.clear();
    loader_configure(config);
    FX_CHECK_EQ(guest_load_library(L"guest_leaf.dll", LOAD_LIBRARY_SEARCH_USER_DIRS), 0u);
    const std::uint64_t cookie = guest_add_dll_directory(L"" FXL_LEGACY_FIXTURE_DIR);
    FX_CHECK(cookie != 0);
    FX_CHECK(guest_load_library(L"guest_leaf.dll", LOAD_LIBRARY_SEARCH_USER_DIRS) != 0);
    FX_CHECK(guest_remove_dll_directory(cookie));
    FX_CHECK(!guest_remove_dll_directory(cookie));
    FX_CHECK_EQ(guest_add_dll_directory(L"relative\\dir"), 0u);
    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR: dependencies next to the DLL.
    fresh_loader();
    loader_configure(config);
    const std::uint64_t lib = guest_load_library(legacy_fixture(L"guest_library.dll"), LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    FX_REQUIRE(lib);
    FX_CHECK(module_named(L"guest_leaf.dll") != nullptr);
    FX_CHECK_EQ(run_guest(proc(lib, "guest_answer")), 42u);
    // Without it the dependency is missing: the load still succeeds. The default
    // search includes the current directory, which under ctest can be the fixture
    // directory itself, so this case runs from the temporary directory.
    {
        const auto previous = std::filesystem::current_path();
        std::filesystem::current_path(std::filesystem::temp_directory_path());
        fresh_loader();
        loader_configure(config);
        const std::uint64_t lib2 = guest_load_library(legacy_fixture(L"guest_library.dll"), 0);
        const bool leaf_missing = module_named(L"guest_leaf.dll") == nullptr;
        const bool diagnosed = has_diagnostic(LoaderDiagnostic::Kind::missing_dll, "guest_leaf.dll");
        std::filesystem::current_path(previous);
        FX_CHECK(lib2 != 0);
        FX_CHECK(leaf_missing);
        FX_CHECK(diagnosed);
    }
    // LOAD_WITH_ALTERED_SEARCH_PATH also searches the DLL's directory.
    fresh_loader();
    loader_configure(config);
    FX_CHECK(guest_load_library(legacy_fixture(L"guest_library.dll"), LOAD_WITH_ALTERED_SEARCH_PATH) != 0);
    FX_CHECK(module_named(L"guest_leaf.dll") != nullptr);
    // SetDllDirectory.
    fresh_loader();
    loader_configure(config);
    FX_CHECK(guest_set_dll_directory(L"" FXL_LEGACY_FIXTURE_DIR));
    FX_CHECK(guest_load_library(L"guest_leaf.dll", 0) != 0);
}

FX_TEST(side_by_side_private_assemblies) {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + L"fxl_sxs_test";
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\my.assembly").c_str(), nullptr);
    {
        std::ofstream(dir + L"\\my.assembly\\my.assembly.manifest")
            << "<?xml version='1.0'?><assembly xmlns='urn:schemas-microsoft-com:asm.v1'>"
               "<assemblyIdentity type='win32' name='my.assembly' version='1.0.0.0'/>"
               "<file name='First.dll'/>\n<file  name=\"second.dll\" />"
               "</assembly>";
        std::ofstream(dir + L"\\flat.manifest")
            << "<assembly><file name=\"flat.dll\"/></assembly>";
    }
    fresh_loader();
    load_sxs_from_manifest(
        "<assembly><dependency><dependentAssembly>"
        "<assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\" version=\"6.0.0.0\" "
        "publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/></dependentAssembly></dependency>"
        "<dependency><dependentAssembly><assemblyIdentity type=\"win32\" name=\"my.assembly\" version=\"1.0.0.0\"/>"
        "</dependentAssembly></dependency>"
        "<dependency><dependentAssembly><asmv1:assemblyIdentity name='flat'/></dependentAssembly></dependency>"
        "</assembly>",
        dir);
    auto& sxs = state().sxs;
    FX_CHECK_EQ(sxs.size(), 3u);
    FX_CHECK(sxs[L"first.dll"] == dir + L"\\my.assembly\\First.dll");
    FX_CHECK(sxs[L"second.dll"] == dir + L"\\my.assembly\\second.dll");
    FX_CHECK(sxs[L"flat.dll"] == dir + L"\\flat.dll");
    // The shared Common-Controls dependency becomes a host activation context:
    // host comctl32 then resolves to v6 (TaskDialogIndirect, ordinal 345).
    FX_REQUIRE(guest_activation_context() != nullptr);
    const std::uint64_t comctl = guest_load_library(L"comctl32.dll", 0);
    FX_REQUIRE(comctl);
    FX_CHECK(GetProcAddress(reinterpret_cast<HMODULE>(comctl), "TaskDialogIndirect") != nullptr);
    FX_CHECK(GetProcAddress(reinterpret_cast<HMODULE>(comctl), MAKEINTRESOURCEA(345)) != nullptr);
    fresh_loader();
    FX_CHECK(guest_activation_context() == nullptr);
}

// ---- command line --------------------------------------------------------------------

FX_TEST(command_line_quoting_round_trips) {
    FX_CHECK(quote_argument(L"plain") == L"plain");
    FX_CHECK(quote_argument(L"a b") == L"\"a b\"");
    FX_CHECK(quote_argument(L"") == L"\"\"");
    FX_CHECK(quote_argument(L"a\"b") == L"\"a\\\"b\"");
    FX_CHECK(quote_argument(L"c:\\dir with space\\") == L"\"c:\\dir with space\\\\\"");
    FX_CHECK(quote_argument(L"c:\\no\\space\\") == L"c:\\no\\space\\");
    const std::vector<std::wstring> args = {L"simple",       L"with space", L"quote\"inside", L"trailing\\",
                                            L"trail space\\", L"",           L"back\\\\\"q",   L"tab\there",
                                            L"\\\\server\\share", L"--dll-dir", L"\"\""};
    const std::wstring program = L"C:\\Program Files\\App\\app.exe";
    const std::wstring line = build_guest_command_line(program, args);
    FX_CHECK(line.rfind(L"\"C:\\Program Files\\App\\app.exe\" ", 0) == 0);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(line.c_str(), &argc);
    FX_REQUIRE(argv);
    FX_REQUIRE(argc == static_cast<int>(args.size()) + 1);
    FX_CHECK(program == argv[0]);
    for (std::size_t i = 0; i < args.size(); ++i) FX_CHECK(args[i] == argv[i + 1]);
    LocalFree(argv);
}

FX_TEST(cli_option_parsing) {
    CliOptions cli;
    std::wstring error;
    const wchar_t* a[] = {L"transmute-fx", L"--dll-dir", L"C:\\x", L"--no-cache", L"--profile",
                          L"--dll-dir", L"D:\\y", L"prog.exe", L"--dll-dir", L"guest arg"};
    FX_REQUIRE(parse_cli(10, a, cli, error));
    FX_CHECK_EQ(cli.dll_directories.size(), 2u);
    FX_CHECK(cli.dll_directories[1] == L"D:\\y");
    FX_CHECK(cli.no_cache && cli.profile && !cli.verbose);
    FX_CHECK(cli.program == L"prog.exe");
    FX_REQUIRE(cli.arguments.size() == 2);
    FX_CHECK(cli.arguments[0] == L"--dll-dir");   // options after the program belong to the guest
    const wchar_t* b[] = {L"transmute-fx", L"--aot", L"blender.exe"};
    FX_REQUIRE(parse_cli(3, b, cli, error));
    FX_CHECK(cli.program.empty());
    FX_CHECK_EQ(cli.aot_images.size(), 1u);
    const wchar_t* c[] = {L"transmute-fx", L"--", L"--weird.exe", L"x"};
    FX_REQUIRE(parse_cli(4, c, cli, error));
    FX_CHECK(cli.program == L"--weird.exe");
    const wchar_t* d[] = {L"transmute-fx", L"--bogus", L"p.exe"};
    FX_CHECK(!parse_cli(3, d, cli, error));
    const wchar_t* e[] = {L"transmute-fx"};
    FX_CHECK(!parse_cli(1, e, cli, error));
    const wchar_t* f[] = {L"transmute-fx", L"--dll-dir"};
    FX_CHECK(!parse_cli(2, f, cli, error));
}

FX_TEST(external_modules_are_found_listed_and_keyed) {
    fresh_loader();
    void* memory = VirtualAlloc(nullptr, 0x40000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    FX_REQUIRE(memory);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    GuestCodeModule m;
    m.base = base;
    m.size = 0x40000;
    m.text_begin = base + 0x4000;
    m.text_end = base + 0x20000;
    m.image_hash = 0x5eed'0000'1234ull;
    m.path = L"/usr/lib/libfake.dylib";
    m.name = L"libfake.dylib";
    register_external_module(m);
    const GuestModule* found = find_guest_module(base + 0x5000);
    FX_REQUIRE(found != nullptr);
    FX_CHECK_EQ(found->base, base);
    FX_CHECK_EQ(found->size, std::uint64_t{0x40000});
    FX_CHECK_EQ(found->pdata_rva, 0u);
    FX_CHECK_EQ(found->text_begin, base + 0x4000);
    FX_CHECK_EQ(found->image_hash, m.image_hash);
    FX_CHECK(found->name == L"libfake.dylib");
    FX_CHECK(find_guest_module(base + 0x40000) == nullptr);
    const auto modules = guest_modules();
    FX_CHECK(std::find(modules.begin(), modules.end(), found) != modules.end());
    FX_REQUIRE(!stubs().cache_loaded.empty());
    FX_CHECK(stubs().cache_loaded.back() == L"libfake.dylib");   // the cache keys it
    // Overlaps and bad ranges are rejected.
    GuestCodeModule overlap = m;
    overlap.base = base + 0x1000;
    bool threw = false;
    try { register_external_module(overlap); } catch (const std::invalid_argument&) { threw = true; }
    FX_CHECK(threw);
    GuestCodeModule bad = m;
    bad.text_end = base + 0x50000;
    threw = false;
    try { register_external_module(bad); } catch (const std::invalid_argument&) { threw = true; }
    FX_CHECK(threw);
    // Unregistering removes it, unloads it from the cache and drops its translations.
    unregister_external_module(base);
    FX_CHECK(find_guest_module(base + 0x5000) == nullptr);
    FX_CHECK(found->base == base);   // old pointers stay valid
    FX_REQUIRE(!stubs().cache_unloaded.empty());
    FX_CHECK(stubs().cache_unloaded.back() == L"libfake.dylib");
    FX_REQUIRE(!stubs().invalidated.empty());
    FX_CHECK(stubs().invalidated.back() == std::make_pair(base, std::uint64_t{0x40000}));
    unregister_external_module(base);   // no-op
    VirtualFree(memory, 0, MEM_RELEASE);
}
