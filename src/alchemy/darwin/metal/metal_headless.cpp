#include "alchemy/darwin/shell_kit.hpp"
#include "alchemy/darwin/shells.hpp"

#include <array>

// alchemy.exe only (darwin/shells.hpp): no Metal device exists in a headless
// process (from darwin/identities/frameworks.cpp; the class shells stay there).
namespace alchemy::darwin_shells {
namespace {
constexpr std::string_view metal = "/System/Library/Frameworks/Metal.framework/Versions/A/Metal";
} // namespace

void install_metal_shell(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    ShellKit kit{context, objc, cf};
    kit.function(metal, "_MTLCreateSystemDefaultDevice", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    kit.function(metal, "_MTLCopyAllDevices", [&context, &cf](CpuState& s, GuestMemory&) {
        const auto callbacks = context.resolve(core_foundation_library, "_kCFTypeArrayCallBacks");
        if (!callbacks) throw std::runtime_error("Alchemy frameworks: CoreFoundation lacks _kCFTypeArrayCallBacks");
        const std::array<std::uint64_t, 4> arguments{0, 0, 0, callbacks->address};
        s.x[0] = cf.invoke("CFArrayCreate", arguments);
    });
}

} // namespace alchemy::darwin_shells
