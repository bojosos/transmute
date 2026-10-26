#include "alchemy/gui/fill_provider.hpp"

#include <array>
#include <stdexcept>

// libobjc: the Objective-C exception personality (Objective-C exceptions are
// C++ ABI exceptions; objc_runtime.cpp throws them and owns their type info).
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
    // method_exchangeImplementations, class_getProperty, _objc_ehtype_vtable and OBJC_EHTYPE_id
    // are the Objective-C runtime's (objc_runtime.cpp).
    // __objc_personality_v0: Objective-C exceptions are C++ ABI exceptions, so
    // the personality is the C++ one (as in objc4).
    raw(libobjc, "___objc_personality_v0", [this](CpuState& s, GuestMemory&) {
        const auto found = objc.cxx_runtime("___gxx_personality_v0");
        if (!found) throw std::runtime_error("Alchemy: __objc_personality_v0 needs libc++abi's __gxx_personality_v0");
        const std::array<Address, 5> arguments{s.x[0], s.x[1], s.x[2], s.x[3], s.x[4]};
        s.x[0] = bridge->call_address(found, arguments);
    });
}

} // namespace

void install_objc_exception_types(InstallContext& context) { std::make_shared<ExceptionTypes>(context)->install(); }

} // namespace alchemy::gui::fill
