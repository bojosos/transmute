#include "alchemy/module_loader.hpp"
#include "alchemy/process.hpp"
#include "alchemy/runtime_libraries.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t preferred = 0x100000000ULL;

void expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void fails(const std::function<void()>& operation, std::string_view expected) {
    try { operation(); }
    catch (const std::runtime_error& error) {
        expect(std::string_view(error.what()).find(expected) != std::string_view::npos, "wrong rejection diagnostic");
        return;
    }
    throw std::runtime_error("malformed loader input was accepted");
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) bytes.at(at + index) = static_cast<std::uint8_t>(value >> (index * 8));
}
void put64(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint64_t value) {
    put32(bytes, at, static_cast<std::uint32_t>(value));
    put32(bytes, at + 4, static_cast<std::uint32_t>(value >> 32));
}
std::uint32_t get32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) value |= static_cast<std::uint32_t>(bytes.at(at + index)) << (index * 8);
    return value;
}
void text(std::vector<std::uint8_t>& bytes, std::size_t at, std::string_view value) {
    for (const auto byte : value) bytes.at(at++) = static_cast<std::uint8_t>(byte);
    bytes.at(at) = 0;
}
std::size_t command(std::vector<std::uint8_t>& bytes, std::uint32_t kind, std::uint32_t size) {
    const auto at = static_cast<std::size_t>(32 + get32(bytes, 20));
    expect(at + size < 0x500, "test load commands overlap code");
    put32(bytes, at, kind); put32(bytes, at + 4, size);
    put32(bytes, 16, get32(bytes, 16) + 1);
    put32(bytes, 20, get32(bytes, 20) + size);
    return at;
}
std::vector<std::uint8_t> image(std::uint32_t type = 2, bool tls = false) {
    std::vector<std::uint8_t> bytes(0x3000);
    put32(bytes, 0, 0xfeedfacf); put32(bytes, 4, 0x0100000c);
    put32(bytes, 12, type); put32(bytes, 24, 0x85);
    for (unsigned index = 0; index < 3; ++index) {
        const auto count = tls && index == 1 ? 2U : 0U;
        const auto at = command(bytes, 0x19, 72 + count * 80);
        text(bytes, at + 8, index == 0 ? "__TEXT" : index == 1 ? "__DATA" : "__LINKEDIT");
        put64(bytes, at + 24, preferred + index * 0x1000ULL);
        put64(bytes, at + 32, 0x1000); put64(bytes, at + 40, index * 0x1000ULL);
        put64(bytes, at + 48, 0x1000);
        put32(bytes, at + 56, index == 0 ? 5 : index == 1 ? 3 : 1);
        put32(bytes, at + 60, index == 0 ? 5 : index == 1 ? 3 : 1);
        put32(bytes, at + 64, count);
        for (unsigned section = 0; section < count; ++section) {
            const auto pos = at + 72 + section * 80;
            text(bytes, pos, section == 0 ? "__thread_data" : "__thread_vars");
            text(bytes, pos + 16, "__DATA");
            put64(bytes, pos + 32, preferred + 0x1000 + section * 16);
            put64(bytes, pos + 40, section == 0 ? 16 : 24);
            put32(bytes, pos + 48, 0x1000 + section * 16);
            put32(bytes, pos + 52, 3); put32(bytes, pos + 64, section == 0 ? 0x11 : 0x13);
        }
    }
    if (type == 2) { const auto at = command(bytes, 0x80000028, 24); put64(bytes, at + 8, 0x500); }
    if (type == 6) {
        const auto at = command(bytes, 0xd, 56); put32(bytes, at + 8, 24);
        text(bytes, at + 24, "@loader_path/unit.dylib");
    }
    put32(bytes, 0x500, 0x528000e0); // mov w0, #7
    put32(bytes, 0x504, 0xd65f03c0); // ret
    return bytes;
}

void dependency(std::vector<std::uint8_t>& bytes, std::string_view name, bool reexport = false) {
    const auto size = static_cast<std::uint32_t>((24 + name.size() + 1 + 7) & ~7ULL);
    const auto at = command(bytes, reexport ? 0x8000001fU : 0xcU, size);
    put32(bytes, at + 8, 24); text(bytes, at + 24, name);
}

void uleb(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    do { const auto byte = static_cast<std::uint8_t>(value & 0x7fU); value >>= 7; bytes.push_back(static_cast<std::uint8_t>(byte | (value ? 0x80U : 0))); } while (value);
}

void exports(std::vector<std::uint8_t>& bytes, std::string_view name, unsigned flags,
             std::uint64_t value, std::string_view imported = {}) {
    std::vector<std::uint8_t> terminal;
    uleb(terminal, flags); uleb(terminal, value);
    if ((flags & 8U) != 0) { for (const auto byte : imported) terminal.push_back(static_cast<std::uint8_t>(byte)); terminal.push_back(0); }
    std::vector<std::uint8_t> trie{0, 1};
    for (const auto byte : name) trie.push_back(static_cast<std::uint8_t>(byte));
    trie.push_back(0); trie.push_back(static_cast<std::uint8_t>(trie.size() + 1));
    uleb(trie, terminal.size()); trie.insert(trie.end(), terminal.begin(), terminal.end()); trie.push_back(0);
    for (std::size_t offset = 0; offset < trie.size(); ++offset) bytes.at(0x2300 + offset) = trie[offset];
    const auto at = command(bytes, 0x80000033, 16); put32(bytes, at + 8, 0x2300); put32(bytes, at + 12, static_cast<std::uint32_t>(trie.size()));
}

std::size_t classic(std::vector<std::uint8_t>& bytes, const std::array<std::vector<std::uint8_t>, 4>& streams) {
    const auto at = command(bytes, 0x80000022, 48);
    for (unsigned index = 0; index < 4; ++index) {
        const auto offset = 0x2000 + index * 0x80;
        put32(bytes, at + 8 + index * 8, offset);
        put32(bytes, at + 12 + index * 8, static_cast<std::uint32_t>(streams[index].size()));
        for (std::size_t pos = 0; pos < streams[index].size(); ++pos) bytes.at(offset + pos) = streams[index][pos];
    }
    return at;
}

void parser_tests() {
    auto bytes = image(); dependency(bytes, "/usr/lib/libSystem.B.dylib");
    put64(bytes, 0x1000, preferred + 0x500);
    std::vector<std::uint8_t> bind{0x11, 0x40, '_', 'p', 'u', 't', 's', 0, 0x71, 8, 0x60, 0x7f, 0x90, 0};
    classic(bytes, {{{0x11, 0x21, 0, 0x51, 0}, bind, {}, {}}});
    auto parsed = alchemy::MachOImage::parse(bytes);
    transmute::ReferenceEngine engine(transmute::ExecutionStart{preferred + 0x500});
    parsed.map(engine, preferred);
    parsed.bind(engine, preferred, [](auto ordinal, auto name, auto) -> std::optional<std::uint64_t> {
        expect(ordinal == 1 && name == "_puts", "wrong classic binding symbol"); return 0x500000001ULL;
    });
    expect(engine.read64(preferred + 0x1000) == preferred + 0x500, "classic rebasing failed");
    expect(engine.read64(preferred + 0x1008) == 0x500000000ULL, "classic signed binding addend failed");
    auto invalid_ordinal = bytes; invalid_ordinal[0x2080] = 0x12;
    fails([&] { static_cast<void>(alchemy::MachOImage::parse(invalid_ordinal)); }, "ordinal");
    auto truncated = bytes; put32(truncated, 32 + get32(truncated, 20) - 48 + 12, 2);
    fails([&] { static_cast<void>(alchemy::MachOImage::parse(truncated)); }, "truncated");
    auto threaded = bytes; threaded[0x2080] = 0xd0;
    fails([&] { static_cast<void>(alchemy::MachOImage::parse(threaded)); }, "threaded");

    auto weak = image(); classic(weak, {{{}, {}, {0x40, '_', 'x', 0, 0x71, 0, 0x90, 0}, {}}});
    parsed = alchemy::MachOImage::parse(weak);
    expect(parsed.imports().size() == 1 && parsed.imports()[0].library_ordinal == -3 && parsed.imports()[0].weak,
           "weak lookup stream lost semantics");
    auto lazy = image(); dependency(lazy, "/usr/lib/libSystem.B.dylib");
    classic(lazy, {{{}, {}, {}, {0x11, 0x40, '_', 'x', 0, 0x71, 0, 0x90, 0, 0x11, 0x40, '_', 'y', 0, 0x71, 8, 0x90, 0}}});
    expect(alchemy::MachOImage::parse(lazy).imports().size() == 2, "lazy entries were not parsed eagerly");
    // A lazy pointer is first rebased to its helper and subsequently bound.
    auto lazy_rebase = image(); dependency(lazy_rebase, "/usr/lib/libSystem.B.dylib");
    put64(lazy_rebase, 0x1000, preferred + 0x500);
    classic(lazy_rebase, {{{0x11, 0x21, 0, 0x51}, {}, {}, {0x11, 0x40, '_', 'x', 0, 0x71, 0, 0x90, 0}}});
    expect(alchemy::MachOImage::parse(lazy_rebase).imports().size() == 1, "lazy rebase was not replaced by eager binding");
    // Apple's linker emits a backwards step using unsigned ULEB128 wrapping.
    auto backwards = image(); dependency(backwards, "/usr/lib/libSystem.B.dylib");
    std::vector<std::uint8_t> backwards_bind{0x11, 0x40, '_', 'x', 0, 0x71, 16, 0x90, 0x80};
    uleb(backwards_bind, UINT64_MAX - 23); backwards_bind.push_back(0x90); backwards_bind.push_back(0);
    classic(backwards, {{{}, backwards_bind, {}, {}}});
    expect(alchemy::MachOImage::parse(backwards).imports().size() == 2, "checked backwards bind step failed");
    auto underflow = backwards; underflow.at(0x2080 + 9) = 0xe7;
    fails([&] { static_cast<void>(alchemy::MachOImage::parse(underflow)); }, "underflows");
    auto duplicated = image(); dependency(duplicated, "/usr/lib/libSystem.B.dylib");
    const std::vector<std::uint8_t> same_binding{0x11, 0x40, '_', 'x', 0, 0x71, 0, 0x90,
        0x71, 0, 0x90, 0};
    classic(duplicated, {{{}, same_binding, {}, {}}});
    expect(alchemy::MachOImage::parse(duplicated).imports().size() == 2, "identical bind records rejected");
    auto conflicting = duplicated;
    const std::vector<std::uint8_t> conflict_binding{0x11, 0x40, '_', 'x', 0, 0x71, 0, 0x90,
        0x40, '_', 'y', 0, 0x71, 0, 0x90, 0};
    put32(conflicting, 32 + get32(conflicting, 20) - 48 + 20, static_cast<std::uint32_t>(conflict_binding.size()));
    for (std::size_t index = 0; index < conflict_binding.size(); ++index) conflicting[0x2080 + index] = conflict_binding[index];
    fails([&] { static_cast<void>(alchemy::MachOImage::parse(conflicting)); }, "conflicting classic");
    auto coalesced = image(8); dependency(coalesced, "/usr/lib/libSystem.B.dylib");
    classic(coalesced, {{{}, {0x11, 0x40, '_', 'x', 0, 0x71, 0, 0x90, 0},
        {0x40, '_', 'x', 0, 0x71, 0, 0x90, 0}, {0x11, 0x40, '_', 'x', 0, 0x71, 0, 0x90, 0}}});
    auto coalesced_image = alchemy::MachOImage::parse(coalesced);
    constexpr auto coalesced_base = 0x180000000ULL;
    coalesced_image.map(engine, coalesced_base);
    coalesced_image.bind(engine, coalesced_base, [](auto ordinal, auto, auto) -> std::optional<std::uint64_t> {
        return ordinal == -3 ? 0x500000020ULL : 0x500000010ULL;
    });
    expect(engine.read64(coalesced_base + 0x1000) == 0x500000020ULL, "lazy binding overwrote weak coalescing");

    auto bundle = image(8); exports(bundle, "_answer", 4, 0x500);
    parsed = alchemy::MachOImage::parse(bundle);
    expect(parsed.is_bundle() && parsed.exports()[0].weak_definition, "bundle weak export missing");
    fails([&] { static_cast<void>(parsed.entry(preferred)); }, "entry");
    auto absolute = image(8); exports(absolute, "_constant", 2, 27);
    expect(alchemy::MachOImage::parse(absolute).exported_symbol("_constant", preferred) == 27, "absolute export slid");
    auto renamed = image(8); dependency(renamed, "@rpath/other.dylib", true); exports(renamed, "_alias", 8, 1, "_answer");
    parsed = alchemy::MachOImage::parse(renamed);
    expect(parsed.dependencies()[0].reexport && parsed.exports()[0].imported_name == "_answer", "renamed reexport missing");
    auto repeated_identity = image(8);
    dependency(repeated_identity, "@rpath/first.dylib");
    dependency(repeated_identity, "@rpath/first.dylib", true);
    dependency(repeated_identity, "@rpath/second.dylib");
    classic(repeated_identity, {{{}, {0x12, 0x40, '_', 'x', 0, 0x71, 0, 0x90, 0}, {}, {}}});
    const auto repeated_image = alchemy::MachOImage::parse(repeated_identity);
    expect(repeated_image.dependencies().size() == 2 && repeated_image.dependencies()[0].reexport &&
           repeated_image.dependencies()[1].install_name == "@rpath/second.dylib" &&
           repeated_image.imports()[0].library_ordinal == 2, "reexport identity changed following library ordinals");
    struct ChainedCase { unsigned format; unsigned inline_addend; std::uint64_t import_addend; std::uint64_t expected; };
    for (const auto scenario : {ChainedCase{1, 128, 0, 1128}, ChainedCase{1, 255, 0, 1255},
                               ChainedCase{3, 0, 1ULL << 63, (1ULL << 63) + 1000},
                               ChainedCase{3, 0, UINT64_MAX - 6, 993}}) {
        auto chained = image(8); dependency(chained, "/usr/lib/libSystem.B.dylib");
        const auto at = command(chained, 0x80000034, 16);
        constexpr std::size_t payload = 0x2400;
        put32(chained, at + 8, payload); put32(chained, at + 12, 128);
        put32(chained, payload + 4, 28); put32(chained, payload + 8, 96);
        const auto symbols = scenario.format == 3 ? 112U : 100U;
        put32(chained, payload + 12, symbols); put32(chained, payload + 16, 1);
        put32(chained, payload + 20, scenario.format); put32(chained, payload + 28, 3);
        put32(chained, payload + 36, 16); put32(chained, payload + 44, 24);
        chained[payload + 48] = 0; chained[payload + 49] = 16; // 4096-byte page.
        chained[payload + 50] = 6;
        put64(chained, payload + 52, 0x1000); chained[payload + 64] = 1;
        put32(chained, payload + 96, 1); text(chained, payload + symbols, "_x");
        if (scenario.format == 3) put64(chained, payload + 104, scenario.import_addend);
        put64(chained, 0x1000, (1ULL << 63) | (static_cast<std::uint64_t>(scenario.inline_addend) << 24));
        const auto chained_image = alchemy::MachOImage::parse(chained);
        constexpr auto chained_base = 0x1c0000000ULL;
        transmute::ReferenceEngine chained_engine(transmute::ExecutionStart{chained_base + 0x500});
        chained_image.map(chained_engine, chained_base);
        chained_image.bind(chained_engine, chained_base, [](auto, auto, auto) -> std::optional<std::uint64_t> { return 1000; });
        expect(chained_engine.read64(chained_base + 0x1000) == scenario.expected,
               "chained addend lost its unsigned pointer-value representation");
    }
    auto tls = image(8, true); exports(tls, "_tls", 1, 0x1010);
    expect(alchemy::MachOImage::parse(tls).exports()[0].thread_local_descriptor, "TLS export missing");
    put64(tls, 104 + 72 + 80 + 40, 8);
    fails([&] { static_cast<void>(alchemy::MachOImage::parse(tls)); }, "misaligned TLS");

    std::vector<std::uint8_t> fat(0x4000);
    const auto big32 = [&](std::size_t at, std::uint32_t value) {
        for (unsigned index = 0; index < 4; ++index) fat.at(at + index) = static_cast<std::uint8_t>(value >> ((3 - index) * 8));
    };
    big32(0, 0xcafebabe); big32(4, 1); big32(8, 0x0100000c);
    big32(16, 0x1000); big32(20, static_cast<std::uint32_t>(bundle.size())); big32(24, 12);
    std::copy(bundle.begin(), bundle.end(), fat.begin() + 0x1000);
    expect(alchemy::MachOImage::parse(fat).is_bundle(), "universal ARM64 slice not selected");
    big32(16, 0xfffff000);
    fails([&] { static_cast<void>(alchemy::MachOImage::parse(fat)); }, "out-of-range");
    big32(0, 0xcafebabf); big32(16, 0); big32(20, 0x1000);
    big32(24, 0); big32(28, static_cast<std::uint32_t>(bundle.size())); big32(32, 12); big32(36, 0);
    expect(alchemy::MachOImage::parse(fat).is_bundle(), "64-bit universal ARM64 slice not selected");
    big32(4, 2); big32(40, 0x0100000c); big32(44, 0); big32(48, 0); big32(52, 0x1000);
    big32(56, 0); big32(60, static_cast<std::uint32_t>(bundle.size())); big32(64, 12); big32(68, 0);
    fails([&] { static_cast<void>(alchemy::MachOImage::parse(fat)); }, "overlapping");
}

void persistent_loader_tests() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("alchemy-loader-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
    const auto write = [&](std::string_view name, const std::vector<std::uint8_t>& bytes) {
        const auto path = directory / name;
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        expect(static_cast<bool>(output), "cannot write authored test image"); return path;
    };
    const auto main = write("main", image());
    auto plugin = image(8); exports(plugin, "_answer", 0, 0x500); const auto source = write("plugin.bundle", plugin);
    auto broken = image(8); dependency(broken, "@loader_path/missing.dylib"); const auto broken_source = write("broken.bundle", broken);
    transmute::ReferenceEngine engine(transmute::ExecutionStart{preferred + 0x500});
    alchemy::ProcessContext context(engine, [](unsigned, std::span<const std::uint8_t> bytes) { return alchemy::OutputResult{bytes.size(), 0}; });
    context.working_directory = directory;
    alchemy::ModuleLoader loader(context, main);
    expect(loader.load_main() == preferred + 0x500, "persistent loader entry incorrect");
    loader.initialize({});
    const auto before = context.images().size();
    fails([&] { static_cast<void>(loader.open(broken_source.string(), 2)); }, "missing guest dylib");
    expect(context.images().size() == before, "failed load published image metadata");
    auto strong = image(8); dependency(strong, "/usr/lib/libSystem.B.dylib");
    std::vector<std::uint8_t> missing_bind{0x11, 0x40};
    for (const auto byte : std::string("_strong_missing")) missing_bind.push_back(static_cast<std::uint8_t>(byte));
    missing_bind.insert(missing_bind.end(), {0, 0x71, 0, 0x90, 0});
    classic(strong, {{{}, missing_bind, {}, {}}});
    const auto strong_source = write("strong.bundle", strong);
    fails([&] { static_cast<void>(loader.open(strong_source.string(), 2)); }, "unresolved strong import");
    expect(context.images().size() == before && !engine.query_region(0x200000000ULL), "binding failure leaked guest mappings");
    const auto handle = loader.open(source.string(), 2 | 4);
    const auto answer = loader.symbol(handle, "answer");
    expect(answer && answer->kind == alchemy::ExportKind::function, "plugin function resolution failed");
    engine.set_jit_enabled(true); engine.set_jit_profiling(true);
    expect(engine.invoke_callback(answer->address, std::span<const std::uint64_t>{}) == 7, "mapped plugin code did not execute");
    expect(engine.jit_stats().native_guest_instructions > 0, "plugin's translated blocks did not execute");
    expect(!loader.symbol(UINT64_MAX - 1, "answer"), "RTLD_LOCAL symbol leaked globally");
    expect(loader.close(handle) && !loader.close(handle), "dynamic handle references incorrect");
    fails([&] { static_cast<void>(loader.symbol(handle, "answer")); }, "closed");
    const auto again = loader.open(source.string(), 2 | 0x10);
    expect(again != 0 && context.images().size() == before + 1, "resident module reopen duplicated image");
    expect(loader.symbol(UINT64_MAX - 1, "answer").has_value(), "RTLD_GLOBAL promotion failed");
    fails([&] { static_cast<void>(loader.open(source.string(), 0x80000000)); }, "flags");
    const auto tls_storage = context.allocate_guest(16);
    static_cast<void>(context.install_function("__tlv_bootstrap", [tls_storage](auto& state, auto&) { state.x[0] = tls_storage; }));
    auto tls_image = image(8, true); put64(tls_image, 0x1000, 42); exports(tls_image, "_tls", 1, 0x1010);
    const auto tls_source = write("tls.bundle", tls_image);
    const auto tls_handle = loader.open(tls_source.string(), 2 | 4);
    const auto tls_symbol = loader.symbol(tls_handle, "tls");
    expect(tls_symbol && tls_symbol->kind == alchemy::ExportKind::thread_local_descriptor, "TLS descriptor export lost kind");
    const auto published = context.images().back();
    expect(published.tls.size() == 1 && published.tls[0].initial_data.size() == 16 &&
           published.tls[0].initial_data[0] == 42, "relocated TLS template not published");
    expect(engine.read64(tls_symbol->address + 8) == published.module_id && engine.read64(tls_symbol->address + 16) == 0,
           "TLS descriptor module or template offset incorrect");
    const auto name = context.allocate_guest(4);
    transmute::GuestMemory& memory = engine;
    memory.write8(name, 't'); memory.write8(name + 1, 'l'); memory.write8(name + 2, 's'); memory.write8(name + 3, 0);
    const auto dlsym = context.resolve("/usr/lib/libSystem.B.dylib", "_dlsym");
    const std::array<std::uint64_t, 2> lookup{tls_handle, name};
    expect(dlsym && engine.invoke_callback(dlsym->address, lookup) == tls_storage,
           "guest dlsym returned a TLS descriptor instead of calling-thread storage");
}

// Darwin's libSystem re-exports libunwind: a libSystem-ordinal import of an
// unwinder symbol binds to the resident guest LLVM libunwind, ahead of the
// provider's (diagnostic) fallback resolver, for ordinal and dlsym lookups.
void libsystem_unwinder_reexport_tests() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("alchemy-unwind-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory / "runtime");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
    const auto write = [&](const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        expect(static_cast<bool>(output), "cannot write authored test image"); return path;
    };
    auto unwinder = image(6);
    text(unwinder, 32 + 3 * 72 + 24, "/usr/lib/libunwind.dylib");
    exports(unwinder, "__Unwind_Resume", 0, 0x504);
    write(directory / "runtime" / "libunwind.dylib", unwinder);
    {   // Apple runtime identities are accepted only with matching provenance.
        std::ofstream provenance(directory / "runtime" / "provenance.json", std::ios::binary);
        provenance << "{\"source\":\"https://github.com/llvm/llvm-project\",\"outputs\":[{\"name\":\"libunwind.dylib\",\"sha256\":\""
                   << alchemy::sha256_file(directory / "runtime" / "libunwind.dylib") << "\"}]}";
    }
    const auto bind = [](std::uint8_t ordinal, std::string_view name) {
        std::vector<std::uint8_t> stream{static_cast<std::uint8_t>(0x10 | ordinal), 0x40};
        for (const auto byte : name) stream.push_back(static_cast<std::uint8_t>(byte));
        stream.insert(stream.end(), {0, 0x71, 0, 0x90, 0});
        return stream;
    };
    auto user = image(8); dependency(user, "/usr/lib/libunwind.dylib"); dependency(user, "/usr/lib/libSystem.B.dylib");
    exports(user, "_slot", 0, 0x1000);
    classic(user, {{{}, bind(2, "__Unwind_Resume"), {}, {}}});
    const auto user_source = write(directory / "user.bundle", user);
    auto unrelated = image(8); dependency(unrelated, "/usr/lib/libSystem.B.dylib");
    classic(unrelated, {{{}, bind(1, "__Unwind_NotExported"), {}, {}}});
    const auto unrelated_source = write(directory / "unrelated.bundle", unrelated);
    const auto main = write(directory / "main", image());
    transmute::ReferenceEngine engine(transmute::ExecutionStart{preferred + 0x500});
    alchemy::ProcessContext context(engine, [](unsigned, std::span<const std::uint8_t> bytes) { return alchemy::OutputResult{bytes.size(), 0}; });
    context.working_directory = directory;
    constexpr std::uint64_t fallback = 0xdead0000ULL;
    context.base_resolver = [](std::string_view library, std::string_view symbol) -> std::optional<std::uint64_t> {
        if (library == "/usr/lib/libSystem.B.dylib" && symbol == "__Unwind_NotExported") return fallback;
        if (library == "/usr/lib/libSystem.B.dylib" && symbol == "__Unwind_Resume") return fallback;
        return std::nullopt;
    };
    alchemy::ModuleLoader loader(context, main, {}, {}, {directory / "runtime"});
    static_cast<void>(loader.load_main());
    loader.initialize({});
    const auto user_handle = loader.open(user_source.string(), 2 | 4);
    const auto unwind_handle = loader.open("/usr/lib/libunwind.dylib", 2 | 4 | 0x10);
    const auto resume = loader.symbol(unwind_handle, "_Unwind_Resume");
    expect(resume && resume->address != fallback && engine.read32(resume->address) == 0xd65f03c0, "guest libunwind export missing");
    const auto slot = loader.symbol(user_handle, "slot");
    expect(slot && engine.read64(slot->address) == resume->address, "libSystem ordinal did not bind the guest libunwind export");
    const auto system = loader.open("/usr/lib/libSystem.B.dylib", 2);
    const auto via_system = loader.symbol(system, "_Unwind_Resume");
    expect(via_system && via_system->address == resume->address, "dlsym(libSystem) missed the libunwind re-export");
    const auto via_default = loader.symbol(UINT64_MAX - 1, "_Unwind_Resume");
    expect(via_default && via_default->address == resume->address, "dlsym(RTLD_DEFAULT) missed the libunwind re-export");
    const auto other = loader.open(unrelated_source.string(), 2 | 4);
    expect(other != 0, "non-unwinder libSystem import lost its provider fallback");
}

void fixture_tests(const std::filesystem::path& main, const std::string& plugin) {
    for (const bool jit : {false, true}) for (const auto base : {preferred, 0x180000000ULL}) {
        alchemy::RunOptions options; options.jit = jit; options.jit_stats = jit; options.load_base = base;
        std::string output;
        const std::array<std::string, 1> arguments{plugin};
        const auto result = alchemy::run_program(main, arguments, options,
            [&](unsigned descriptor, std::span<const std::uint8_t> bytes) {
                expect(descriptor == 1, "plugin fixture emitted stderr");
                output.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                return alchemy::OutputResult{bytes.size(), 0};
            });
        expect(result.exit_code == 0 && output == "Plugin: 25\n", "linked plugin fixture failed");
        if (jit) expect(result.jit.native_guest_instructions > 0, "plugin workload did not execute translated code");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        parser_tests();
        persistent_loader_tests();
        libsystem_unwinder_reexport_tests();
        if (argc == 3) fixture_tests(argv[1], argv[2]);
        else if (argc == 2) {
            const auto inspected = alchemy::MachOImage::load(argv[1]);
            std::cout << "Parsed " << inspected.imports().size() << " imports, " << inspected.exports().size() << " exports\n";
        } else expect(argc == 1, "expected executable and plugin fixture paths");
        std::cout << "Alchemy loader expansion tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
