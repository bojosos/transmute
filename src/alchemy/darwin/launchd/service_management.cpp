#include "alchemy/darwin/launchd/service_management.hpp"

#include "alchemy/darwin/kit/completion/runtime/common.hpp"
#include "alchemy/darwin/launchd/client.hpp"
#include "alchemy/darwin/launchd/protocol.hpp"

#include <vector>

// The job dictionaries cross between CoreFoundation and the session launchd
// as XML property lists (CFPropertyListCreateData / CreateWithData). Errors
// are CFErrors in the launchd domain with the SMErrors.h codes.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;
using launchd::Value;

constexpr std::string_view service_management = "/System/Library/Frameworks/ServiceManagement.framework/Versions/A/ServiceManagement";
constexpr std::string_view launchd_domain = "CFErrorDomainLaunchd";
// SMErrors.h
constexpr std::int64_t sm_internal_failure = 2, sm_authorization_failure = 4, sm_job_not_found = 6, sm_job_must_be_enabled = 9,
                       sm_invalid_plist = 10;
constexpr std::int64_t already_exists = 17;   // EEXIST, as launchd reports a label already loaded

bool system_domain(Support& support, Address domain) {
    return domain && text(support, domain) == "kSMDomainSystemLaunchd";
}

void fail_with(Support& support, Address out_error, std::int64_t code, std::string_view why) {
    if (!out_error) return;
    // Create rule: the caller releases the CFError.
    store(support, out_error, objc_retain(support, error(support, launchd_domain, code, why)));
}

// A CF property list's XML bytes, parsed on the host; nullopt when CF cannot write it.
std::optional<Value> host_value(Support& support, Address plist) {
    const auto data = support.call(core_foundation, "_CFPropertyListCreateData", {0, plist, 100 /* kCFPropertyListXMLFormat_v1_0 */, 0, 0});
    if (!data) return std::nullopt;
    const auto bytes = support.call(core_foundation, "_CFDataGetBytePtr", {data});
    const auto size = static_cast<std::size_t>(support.call(core_foundation, "_CFDataGetLength", {data}));
    std::vector<std::uint8_t> copy(size);
    if (size) support.memory().read_bytes(bytes, std::span(copy));
    support.call(core_foundation, "_CFRelease", {data});
    try { return launchd::parse_plist(copy); }
    catch (const std::exception&) { return std::nullopt; }
}

// A +1 CF property list made from a host value.
Address guest_value(Support& support, const Value& value) {
    const auto xml = launchd::to_xml(value);
    GuestBlock bytes(support, xml.size() ? xml.size() : 1, false);
    support.memory().write_bytes(bytes, std::span(reinterpret_cast<const std::uint8_t*>(xml.data()), xml.size()));
    const auto data = support.call(core_foundation, "_CFDataCreate", {0, bytes, xml.size()});
    const auto result = support.call(core_foundation, "_CFPropertyListCreateWithData", {0, data, 0, 0, 0});
    support.call(core_foundation, "_CFRelease", {data});
    return result;
}

std::unique_ptr<launchd::Session> session(Support& support, bool start) {
    if (!start && !launchd::Session::running()) return nullptr;
    std::string why;
    auto opened = launchd::Session::open(start, support.process.launch_options, &why);
    if (!opened && start) support_kit::diagnostic("ServiceManagement: cannot reach the session launchd: " + why);
    return opened;
}

} // namespace

void install_launchd_jobs(Support& support) {
    // Boolean SMJobSubmit(CFStringRef domain, CFDictionaryRef job, AuthorizationRef, CFErrorRef* outError)
    support.function(service_management, "_SMJobSubmit", [&support](CpuState& s, GuestMemory&) {
        const auto domain = s.x[0], job = s.x[1], out_error = s.x[3];
        s.x[0] = 0;
        if (system_domain(support, domain)) {
            fail_with(support, out_error, sm_authorization_failure, "The system domain needs root: Alchemy runs no system launchd");
            return;
        }
        const auto value = job ? host_value(support, job) : std::nullopt;
        if (!value || !value->is(Value::Kind::dictionary)) {
            fail_with(support, out_error, sm_invalid_plist, "The job is not a property-list dictionary");
            return;
        }
        auto launchd = session(support, true);
        if (!launchd) { fail_with(support, out_error, sm_internal_failure, "The session launchd could not be started"); return; }
        std::string message;
        const auto status = launchd->load(*value, "", launchd::wire::load_submit, message);
        if (status == 0) { s.x[0] = 1; return; }
        const auto code = status == launchd::wire::status_already_loaded ? already_exists
                        : status == launchd::wire::status_invalid_plist ? sm_invalid_plist
                        : status == launchd::wire::status_service_disabled ? sm_job_must_be_enabled : sm_internal_failure;
        fail_with(support, out_error, code, message.empty() ? "The job could not be submitted" : message);
    });
    // Boolean SMJobRemove(CFStringRef domain, CFStringRef label, AuthorizationRef, Boolean wait, CFErrorRef* outError):
    // the job's process has ended when it returns (wait or not).
    support.function(service_management, "_SMJobRemove", [&support](CpuState& s, GuestMemory&) {
        const auto domain = s.x[0], label = s.x[1], out_error = s.x[4];
        s.x[0] = 0;
        if (system_domain(support, domain)) {
            fail_with(support, out_error, sm_authorization_failure, "The system domain needs root: Alchemy runs no system launchd");
            return;
        }
        auto launchd = session(support, false);
        std::string message;
        const auto status = launchd && label ? launchd->remove(text(support, label), message) : launchd::wire::status_no_such_service;
        if (status == 0) { s.x[0] = 1; return; }
        fail_with(support, out_error, status == launchd::wire::status_no_such_service ? sm_job_not_found : sm_internal_failure,
                  message.empty() ? "The job was not found" : message);
    });
    // CFDictionaryRef SMJobCopyDictionary(CFStringRef domain, CFStringRef label): +1, or NULL.
    support.function(service_management, "_SMJobCopyDictionary", [&support](CpuState& s, GuestMemory&) {
        const auto domain = s.x[0], label = s.x[1];
        s.x[0] = 0;
        if (system_domain(support, domain) || !label) return;
        auto launchd = session(support, false);
        if (!launchd) return;
        if (const auto info = launchd->info(text(support, label))) s.x[0] = guest_value(support, info->dictionary);
    });
    // int launch_activate_socket(const char* name, int** fds, size_t* count): the
    // descriptors of the sockets the session launchd made for the job's Sockets
    // entry `name` (a malloc'd array the caller frees). ESRCH for a process
    // launchd did not start, ENOENT for a name the job does not have.
    support.function(rt::libsystem, "_launch_activate_socket", [&support](CpuState& s, GuestMemory& m) {
        const auto name = s.x[0], out_fds = s.x[1], out_count = s.x[2];
        if (!name || !out_fds || !out_count) { s.x[0] = 22; return; }
        GuestBlock key(support, "XPC_SERVICE_NAME");
        if (!support.call(rt::libsystem, "_getenv", {key})) { s.x[0] = 3; return; }
        auto launchd = session(support, false);
        std::vector<std::int32_t> descriptors;
        const auto status = launchd ? launchd->sockets(c_string(m, name), descriptors) : 3;
        if (status) { s.x[0] = static_cast<std::uint64_t>(status); return; }
        const auto array = support.call(rt::libsystem, "_malloc", {descriptors.size() * 4});
        if (!array) { s.x[0] = 12; return; }   // ENOMEM
        for (std::size_t index = 0; index < descriptors.size(); ++index) m.write32(array + index * 4, static_cast<std::uint32_t>(descriptors[index]));
        m.write64(out_fds, array);
        m.write64(out_count, descriptors.size());
        s.x[0] = 0;
    });
    // CFArrayRef SMCopyAllJobDictionaries(CFStringRef domain): +1 (empty for the system domain).
    support.function(service_management, "_SMCopyAllJobDictionaries", [&support](CpuState& s, GuestMemory&) {
        const auto domain = s.x[0];
        auto all = Value::of_array();
        if (!system_domain(support, domain)) {
            if (auto launchd = session(support, false))
                for (const auto& entry : launchd->list())
                    if (const auto info = launchd->info(entry.label)) all.array.push_back(info->dictionary);
        }
        s.x[0] = guest_value(support, all);
    });
}

} // namespace alchemy::gui::completion_kit
