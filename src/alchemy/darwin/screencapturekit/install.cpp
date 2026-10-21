#include "alchemy/darwin/screencapturekit/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// ScreenCaptureKit (weakly linked): shareable content and screenshots fail
// with SCStreamErrorUserDeclined.
// String constants carry the values of Apple's public headers; values
// marked "(unverified)" follow the framework's naming convention and could
// not be checked against a header. Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
namespace alchemy::screencapturekit {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view screen_capture_kit = "/System/Library/Frameworks/ScreenCaptureKit.framework/Versions/A/ScreenCaptureKit";
constexpr std::int64_t sc_stream_error_user_declined = -3801;

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(screen_capture_kit));
    const auto nothing = [](CpuState&, GuestMemory&) {};
    string(screen_capture_kit, "_SCStreamErrorDomain", "com.apple.ScreenCaptureKit.SCStreamErrorDomain");
    const auto shareable = cls(screen_capture_kit, "SCShareableContent", "NSObject");
    const auto declined = [this](Address block) {
        complete_with_error(block, "com.apple.ScreenCaptureKit.SCStreamErrorDomain", sc_stream_error_user_declined,
                            "Screen capture is not permitted");
    };
    method(shareable, "getShareableContentWithCompletionHandler:", [declined](CpuState& s, GuestMemory&) { declined(s.x[2]); }, true, "v@:@?");
    method(shareable, "getShareableContentExcludingDesktopWindows:onScreenWindowsOnly:completionHandler:",
           [declined](CpuState& s, GuestMemory&) { declined(s.x[4]); }, true, "v@:BB@?");
    method(shareable, "getCurrentProcessShareableContentWithCompletionHandler:", [declined](CpuState& s, GuestMemory&) { declined(s.x[2]); }, true, "v@:@?");
    const auto screenshots = cls(screen_capture_kit, "SCScreenshotManager", "NSObject");
    method(screenshots, "captureImageWithFilter:configuration:completionHandler:", [declined](CpuState& s, GuestMemory&) { declined(s.x[4]); }, true, "v@:@@@?");
    method(screenshots, "captureSampleBufferWithFilter:configuration:completionHandler:", [declined](CpuState& s, GuestMemory&) { declined(s.x[4]); }, true, "v@:@@@?");
    for (const auto* name : {"SCContentFilter", "SCStreamConfiguration", "SCStream"})
        static_cast<void>(cls(screen_capture_kit, name, "NSObject"));
    const auto picker = cls(screen_capture_kit, "SCContentSharingPicker", "NSObject");
    singleton(picker, "sharedPicker");
    integer_properties(picker, {"active", "maximumStreamCount"});
    method(picker, "addObserver:", nothing, false, "v@:@");
    method(picker, "removeObserver:", nothing, false, "v@:@");
}

} // namespace

void install_screencapturekit(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::screencapturekit
