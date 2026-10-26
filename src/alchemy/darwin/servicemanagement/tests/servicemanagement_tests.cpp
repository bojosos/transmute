// ServiceManagement (src/alchemy/darwin/servicemanagement/): SMJobBless fails (no launchd);
// SMAppService reports services as not registered.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view sm = "/System/Library/Frameworks/ServiceManagement.framework/Versions/A/ServiceManagement";

const Case servicemanagement_case("servicemanagement", [](Fixture& f) {
    const auto error = f.block(8);
    require(f.call(sm, "_SMJobBless", {0, f.cf.string("com.example.helper"), 0, error}) == 0 && f.engine.read64(error), "SMJobBless fails with an error");
    // SMAppService: never registered; registering fails with an error.
    const auto service_class = f.cls("SMAppService");
    const auto main = f.send(service_class, "mainAppService");
    require(main && main == f.send(service_class, "mainAppService") && f.send(main, "status") == 0, "mainAppService is not registered");
    const auto agent = f.send(service_class, "agentServiceWithPlistName:", {f.cf.string("com.example.agent.plist")});
    require(agent && agent != main && f.send(agent, "status") == 0, "agentServiceWithPlistName:");
    const auto failure = f.block(8);
    require(f.send(agent, "registerAndReturnError:", {failure}) == 0 && f.engine.read64(failure), "registerAndReturnError: fails");
    f.engine.write64(failure, 0);
    require(f.send(agent, "unregisterAndReturnError:", {failure}) == 0 && f.engine.read64(failure), "unregisterAndReturnError: fails");
});

} // namespace
