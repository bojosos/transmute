#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"
#include "alchemy/darwin_frameworks.hpp"

// IOKit with an empty registry: matching dictionaries are real, and lookups
// consume them and find no service.
// Moved from darwin/identities/frameworks.cpp (the headless framework set);
// registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

constexpr std::string_view iokit = darwin_framework_libraries[10];

class EmptyRegistry final : public Provider {
public:
    using Provider::Provider;
    void install() {
        // const mach_port_t: the default main port is MACH_PORT_NULL.
        zero_symbol(iokit, "_kIOMainPortDefault", 8);
        zero_symbol(iokit, "_kIOMasterPortDefault", 8);
        function(iokit, "_IORegistryEntryIDMatching", [this](CpuState& s, GuestMemory& m) {
            const std::array<std::uint64_t, 4> create{0, 0, callbacks("_kCFTypeDictionaryKeyCallBacks"), callbacks("_kCFTypeDictionaryValueCallBacks")};
            const auto dictionary = cf.invoke("CFDictionaryCreateMutable", create);
            const auto key = cf.string("IORegistryEntryID");
            const auto storage = context.allocate_guest(8);
            Address number{};
            try {
                m.write64(storage, s.x[0]);
                const std::array<std::uint64_t, 3> make{0, 4 /* kCFNumberSInt64Type */, storage};
                number = cf.invoke("CFNumberCreate", make);
                const std::array<std::uint64_t, 3> set{dictionary, key, number};
                static_cast<void>(cf.invoke("CFDictionarySetValue", set));
            } catch (...) {
                context.release_guest(storage); context.objects().release(key);
                if (number) context.objects().release(number);
                context.objects().release(dictionary);
                throw;
            }
            context.release_guest(storage);
            context.objects().release(key); context.objects().release(number);
            s.x[0] = dictionary;
        });
        // IOServiceGetMatchingService and IORegistryEntryCreateCFProperty are
        // IOKit's headless shell (darwin/shells.hpp).
        function(iokit, "_IODisplayCreateInfoDictionary", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    }
};

} // namespace

void install_iokit_empty_registry(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<EmptyRegistry>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
