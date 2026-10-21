#include "alchemy/darwin/shell_kit.hpp"
#include "alchemy/darwin/shells.hpp"

// alchemy.exe only (darwin/shells.hpp): IOKit lookups in an empty registry
// (from darwin/identities/frameworks.cpp): a matching dictionary is consumed
// and no service is found.
namespace alchemy::darwin_shells {
namespace {
constexpr std::string_view iokit = "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit";
} // namespace

void install_iokit_shell(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    ShellKit kit{context, objc, cf};
    kit.function(iokit, "_IOServiceGetMatchingService", [&context](CpuState& s, GuestMemory&) {
        if (s.x[1]) context.objects().release(s.x[1]);
        s.x[0] = 0;
    });
    kit.function(iokit, "_IORegistryEntryCreateCFProperty", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
}

} // namespace alchemy::darwin_shells
