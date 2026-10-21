#include "alchemy/gui/fill_provider.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

// libobjc: the Objective-C exception type info and personality are real for a
// runtime that never throws Objective-C exceptions (C++ exceptions pass
// through Objective-C frames).
// The Qt WebEngine images first imported them (moved from
// darwin/identities/system_gaps.cpp); installed by the fill-only kit
// (kit/fwgaps/install.cpp) at the old position.
namespace alchemy::gui::fill {
namespace {

class ExceptionTypes final : public Provider {
public:
    using Provider::Provider;
    void install();
};

void ExceptionTypes::install() {
    // method_exchangeImplementations and class_getProperty are the Objective-C runtime's (objc_runtime.cpp).
    // OBJC_EHTYPE_id: objc_typeinfo {vtable, "id", cls}. The vtable follows
    // libc++abi's type_info layout; can_catch answers false because Alchemy
    // never throws Objective-C exceptions, so only catch(...) and cleanups run
    // for the C++ exceptions that unwind through Objective-C frames.
    if (!has(libobjc, "_OBJC_EHTYPE_id")) {
        const auto noop = process.install_function("__alchemy_objc_ehtype_noop", [](CpuState&, GuestMemory&) {}, "alchemy.internal");
        const auto never = process.install_function("__alchemy_objc_ehtype_false", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "alchemy.internal");
        std::vector<Address> slots{0, 0, noop, noop, never, never, never, never, never, never};
        const auto vtable = process.allocate_guest(slots.size() * 8, 16);
        for (std::size_t index = 0; index < slots.size(); ++index) process.main_engine().write64(vtable + index * 8, slots[index]);
        std::vector<std::uint8_t> record(24, 0);
        const Address vptr = vtable + 16, name = cstring("id");
        std::memcpy(record.data(), &vptr, 8);
        std::memcpy(record.data() + 8, &name, 8);
        static_cast<void>(data(libobjc, "_OBJC_EHTYPE_id", record, 8));
    }
    // __objc_personality_v0: Objective-C exceptions are C++ ABI exceptions, so
    // the personality is the C++ one (as in objc4).
    raw(libobjc, "___objc_personality_v0", [this](CpuState& s, GuestMemory&) {
        std::optional<RuntimeExport> found;
        for (const auto library : {std::string_view("/usr/lib/libc++abi.dylib"), std::string_view("/usr/lib/libc++.1.dylib"), libsystem})
            if ((found = process.resolve(library, "___gxx_personality_v0"))) break;
        if (!found) throw std::runtime_error("Alchemy: __objc_personality_v0 needs libc++abi's __gxx_personality_v0");
        const std::array<Address, 5> arguments{s.x[0], s.x[1], s.x[2], s.x[3], s.x[4]};
        s.x[0] = bridge->call_address(found->address, arguments);
    });
}

} // namespace

void install_objc_exception_types(InstallContext& context) { std::make_shared<ExceptionTypes>(context)->install(); }

} // namespace alchemy::gui::fill
