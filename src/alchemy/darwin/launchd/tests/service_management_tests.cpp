// ServiceManagement's job calls without a session launchd (darwin/launchd/service_management.cpp):
// nothing is started by the read-only calls, the system domain is refused, and
// launch_activate_socket says the process is not a job. The calls that start
// the session launchd run end to end in alchemy_launchd_session_fx.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view sm = "/System/Library/Frameworks/ServiceManagement.framework/Versions/A/ServiceManagement";
constexpr std::string_view sys = "/usr/lib/libSystem.B.dylib";
constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case launchd_jobs_case("launchd_service_management", [](Fixture& f) {
    const auto user = f.cf.string("kSMDomainUserLaunchd");
    const auto system = f.cf.string("kSMDomainSystemLaunchd");
    const auto label = f.cf.string("com.example.never-loaded");
    // No session launchd runs for this process: nothing is loaded, and none is started.
    const auto all = f.call(sm, "_SMCopyAllJobDictionaries", {user});
    require(all && f.call(cf, "_CFArrayGetCount", {all}) == 0, "SMCopyAllJobDictionaries: an empty array");
    require(f.call(sm, "_SMJobCopyDictionary", {user, label}) == 0, "SMJobCopyDictionary of an unknown label: NULL");
    const auto error = f.block(8);
    require(f.call(sm, "_SMJobRemove", {user, label, 0, 1, error}) == 0 && f.engine.read64(error), "SMJobRemove of an unknown label fails");
    require(f.call(cf, "_CFErrorGetCode", {f.engine.read64(error)}) == 6, "kSMErrorJobNotFound");
    // The system domain needs root.
    f.engine.write64(error, 0);
    const auto keys = f.block(8), values = f.block(8);
    f.engine.write64(keys, f.cf.string("Label"));
    f.engine.write64(values, label);
    const auto job = f.call(cf, "_CFDictionaryCreate", {0, keys, values, 1, 0, 0});
    require(f.call(sm, "_SMJobSubmit", {system, job, 0, error}) == 0 && f.engine.read64(error), "SMJobSubmit in the system domain fails");
    require(f.call(cf, "_CFErrorGetCode", {f.engine.read64(error)}) == 4, "kSMErrorAuthorizationFailure");
    const auto none = f.call(sm, "_SMCopyAllJobDictionaries", {system});
    require(none && f.call(cf, "_CFArrayGetCount", {none}) == 0, "the system domain lists no jobs");
    // launch_activate_socket: not a job (ESRCH).
    const auto fds = f.block(8), count = f.block(8);
    require(f.call(sys, "_launch_activate_socket", {f.text("Listeners"), fds, count}) == 3, "launch_activate_socket: ESRCH");
});

} // namespace
