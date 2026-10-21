#include "alchemy/engine_adapter.hpp"
#include "alchemy/module_loader.hpp"
#include "transmute/jit.hpp"

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

constexpr std::uint64_t default_scope = UINT64_MAX - 1;

void expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class Operation>
void fails(Operation operation, std::string_view diagnostic) {
    try { operation(); }
    catch (const std::runtime_error& error) {
        expect(std::string_view(error.what()).find(diagnostic) != std::string_view::npos,
               "native extension failure reported the wrong diagnostic");
        return;
    }
    throw std::runtime_error("native extension failure was accepted");
}

void run(const std::filesystem::path& executable, const std::filesystem::path& extension,
         const std::filesystem::path& consumer, const std::filesystem::path& missing,
         const std::filesystem::path& flat, bool jit, std::uint64_t base) {
    const auto image = alchemy::MachOImage::load(executable);
    transmute::ReferenceEngine engine(transmute::ExecutionStart{image.entry(base)});
    engine.set_jit_enabled(jit);
    engine.set_jit_profiling(jit);
    alchemy::ProcessContext context(engine, [](unsigned, std::span<const std::uint8_t>) -> alchemy::OutputResult {
        throw std::runtime_error("native extension emitted unexpected output");
    });
    context.jit = jit;
    context.instruction_limit = 1'000'000;
    // A provider function in a framework the executable links (AudioUnit), for
    // the flat-namespace add-on below.
    static_cast<void>(context.install_function("_AudioComponentFindNext", [](transmute::CpuState& state, transmute::GuestMemory&) {
        state.x[0] = 0x2a;
    }, "/System/Library/Frameworks/AudioUnit.framework/Versions/A/AudioUnit"));
    alchemy::ModuleLoader loader(context, executable, base);
    static_cast<void>(loader.load_main());
    loader.initialize({});
    const auto counter = loader.symbol(default_scope, "python_loader_constructor_count");
    const auto add = loader.symbol(default_scope, "python_loader_add");
    expect(counter && counter->kind == alchemy::ExportKind::data && add &&
           add->kind == alchemy::ExportKind::function, "executable C API exports lost their kinds");
    expect(engine.read32(counter->address) == 0, "unexpected initial extension constructor count");
    const auto initial_images = context.images().size();
    fails([&] { static_cast<void>(loader.open(missing.string(), 2 | 4)); }, "python_loader_missing_api");
    expect(context.images().size() == initial_images && engine.read32(counter->address) == 0,
           "unresolved extension published metadata or ran a constructor");
    fails([&] { static_cast<void>(loader.open(extension.string(), 2 | 4 | 0x10)); }, "RTLD_NOLOAD");

    const auto handle = loader.open(extension.string(), 2 | 4); // Darwin RTLD_NOW | RTLD_LOCAL.
    expect(engine.read32(counter->address) == 1, "extension constructor did not bind executable data");
    const auto init = loader.symbol(handle, "PyInit_alchemy_loader");
    expect(init && init->kind == alchemy::ExportKind::function, "extension initialization export was not found");
    expect(alchemy::EngineAdapter(engine).call(init->address, {}) == 27,
           "extension did not execute the guest executable's C API function");
    expect(!loader.symbol(default_scope, "PyInit_alchemy_loader"), "RTLD_LOCAL extension leaked into global lookup");
    fails([&] { static_cast<void>(loader.open(consumer.string(), 2 | 4)); }, "python_loader_extension");
    expect(context.images().size() == initial_images + 1 && engine.read32(counter->address) == 1,
           "failed dependent extension changed resident modules");

    const auto reopened = loader.open(extension.string(), 2 | 4 | 0x10);
    expect(reopened == handle && engine.read32(counter->address) == 1,
           "resident extension reopened with a duplicate constructor or identity");
    expect(loader.close(reopened) && loader.close(handle), "extension handle references were not counted");
    fails([&] { static_cast<void>(loader.symbol(handle, "PyInit_alchemy_loader")); }, "closed");
    const auto promoted = loader.open(extension.string(), 2 | 8 | 0x10); // RTLD_GLOBAL, NOLOAD.
    expect(loader.symbol(default_scope, "PyInit_alchemy_loader").has_value(), "extension global promotion failed");
    expect(engine.read32(counter->address) == 1, "global promotion repeated the extension constructor");
    const auto dependent = loader.open(consumer.string(), 2 | 4);
    const auto invoke = loader.symbol(dependent, "python_loader_consumer");
    expect(invoke && alchemy::EngineAdapter(engine).call(invoke->address, {}) == 41,
           "second extension did not call through the first extension and executable");
    expect(context.images().size() == initial_images + 2, "extension failures leaked published images");
    expect(loader.close(dependent) && loader.close(promoted), "extension shutdown did not retire handles");
    expect(!loader.close(dependent), "already closed extension handle was accepted");
    // A dynamic_lookup import binds to a provider library a loaded image links.
    const auto flat_handle = loader.open(flat.string(), 2 | 4);
    const auto flat_entry = loader.symbol(flat_handle, "python_loader_flat");
    expect(flat_entry && alchemy::EngineAdapter(engine).call(flat_entry->address, {}) == 42,
           "a flat-namespace import did not bind to the executable's provider framework");
    expect(loader.close(flat_handle), "flat add-on close");
    if (jit) expect(engine.jit_stats().native_guest_instructions > 0,
                    "native extension JIT test did not execute translated guest blocks");
}

} // namespace

int main(int argc, char** argv) {
    try {
        expect(argc == 6, "expected executable, extension, consumer, unresolved extension and flat add-on fixture paths");
        for (const bool jit : {false, true}) for (const auto base : {0x100000000ULL, 0x180000000ULL}) {
            run(argv[1], argv[2], argv[3], argv[4], argv[5], jit, base);
        }
        std::cout << "Alchemy native extension loader tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
