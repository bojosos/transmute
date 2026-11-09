// SystemExtensions (src/alchemy/darwin/systemextensions/): the identity, the request object, and the documented
// failure a submitted request reports to its delegate on its queue.
#include "alchemy/gui/tests/framework_case.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/SystemExtensions.framework/Versions/A/SystemExtensions"

const Case systemextensions_case("systemextensions", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "SystemExtensions identity");
    require(f.string_constant(LIBRARY, "_OSSystemExtensionErrorDomain") == "OSSystemExtensionErrorDomain", "OSSystemExtensionErrorDomain");
    const auto identifier = f.cf.string("com.example.camera-extension");
    const auto queue = f.call("/usr/lib/libSystem.B.dylib", "_dispatch_queue_create", {f.text("systemextensions.test"), 0});
    const auto request = f.send(f.cls("OSSystemExtensionRequest"), "activationRequestForExtension:queue:", {identifier, queue});
    require(request != 0 && f.cf.utf8(f.send(request, "identifier")) == "com.example.camera-extension", "the request keeps its identifier");

    std::mutex mutex;
    std::string domain;
    std::int64_t code = 0;
    Address seen = 0;
    std::atomic<bool> failed{};
    const auto delegate_class = f.objc.register_native_class("SystemExtensionsTestDelegate", f.cls("NSObject"), 8);
    static_cast<void>(f.objc.add_native_method(delegate_class, "request:didFailWithError:", [&](transmute::CpuState& s, transmute::GuestMemory&) {
        const auto error = s.x[3];
        {
            std::lock_guard lock(mutex);
            seen = s.x[2];
            domain = f.cf.utf8(f.send(error, "domain"));
            code = static_cast<std::int64_t>(f.send(error, "code"));
        }
        failed = true;
    }, false, "v@:@@"));
    const auto delegate = f.send(f.send(delegate_class, "alloc"), "init");
    static_cast<void>(f.send(request, "setDelegate:", {delegate}));
    const auto manager = f.send(f.cls("OSSystemExtensionManager"), "sharedManager");
    require(manager != 0 && manager == f.send(f.cls("OSSystemExtensionManager"), "sharedManager"), "one shared manager");
    static_cast<void>(f.send(manager, "submitRequest:", {request}));
    for (int wait = 0; wait < 500 && !failed; ++wait) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(failed, "the delegate heard request:didFailWithError: on the request's queue");
    std::lock_guard lock(mutex);
    require(seen == request && domain == "OSSystemExtensionErrorDomain" && code == 10,
            "OSSystemExtensionErrorForbiddenBySystemPolicy: " + domain + " " + std::to_string(code));
});

#undef LIBRARY
} // namespace
