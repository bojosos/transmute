// GSS.framework provider tests: the identity, the exported OID descriptors,
// names (import, display, compare, duplicate, release), the empty mechanism
// set, and the explicit GSS_S_UNAVAILABLE answers of every call that needs a
// Kerberos, NTLM or SPNEGO mechanism, as Qt Network's QAuthenticator makes
// them.
#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/gui/install.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Address = std::uint64_t;
constexpr std::string_view gss = "/System/Library/Frameworks/GSS.framework/Versions/A/GSS";
constexpr std::uint32_t gss_s_complete = 0, gss_s_bad_name = 2u << 16, gss_s_no_cred = 7u << 16,
    gss_s_no_context = 8u << 16, gss_s_unavailable = 16u << 16;

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> bytes) {
        return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices services{process};
    alchemy::DarwinThreads threads{(alchemy::install_darwin_dispatch(process), process)};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    alchemy::FoundationBridge foundation{process, objc, cf};

    Fixture() {
        alchemy::install_darwin_frameworks(process, objc, cf);
        alchemy::gui::InstallContext context{process, objc, cf};
        alchemy::gui::install_gss(context);
    }
    ~Fixture() {
        try { process.request_exit(0); threads.join_workers(); } catch (...) {}
    }
    Address symbol(std::string_view name) {
        const auto found = process.resolve(gss, name);
        require(found.has_value(), "missing GSS symbol " + std::string(name));
        return found->address;
    }
    std::uint32_t call(std::string_view name, std::initializer_list<Address> arguments) {
        return static_cast<std::uint32_t>(engine.invoke_callback(symbol(name), std::span(arguments.begin(), arguments.size())));
    }
    Address block(std::size_t size) {
        const auto address = process.allocate_guest(size, 16);
        engine.write_bytes(address, std::vector<std::uint8_t>(size, 0xcc));
        return address;
    }
    Address buffer(std::string_view text) {   // gss_buffer_desc pointing at a copy of `text`
        const auto bytes = block(text.size() + 1), desc = block(16);
        engine.write_bytes(bytes, std::vector<std::uint8_t>(text.begin(), text.end()));
        engine.write64(desc, text.size());
        engine.write64(desc + 8, bytes);
        return desc;
    }
    std::string text(Address desc) {
        const auto length = engine.read64(desc), value = engine.read64(desc + 8);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        if (length) engine.read_bytes(value, bytes);
        return {bytes.begin(), bytes.end()};
    }
};

void identity_and_oids(Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(gss), "GSS is not a provider identity");
    require(alchemy::darwin_framework_library(gss), "GSS is not in darwin_framework_libraries");
    const auto oid = f.symbol("___gss_c_nt_hostbased_service_oid_desc");
    require(f.engine.read32(oid) == 10, "hostbased service OID length");
    std::vector<std::uint8_t> bytes(10);
    f.engine.read_bytes(f.engine.read64(oid + 8), bytes);
    const std::vector<std::uint8_t> expected{0x2a, 0x86, 0x48, 0x86, 0xf7, 0x12, 0x01, 0x02, 0x01, 0x04};
    require(bytes == expected, "hostbased service OID 1.2.840.113554.1.2.1.4");
}

void names(Fixture& f) {
    const auto minor = f.block(4), name = f.block(8), other = f.block(8), copy = f.block(8), equal = f.block(4),
        out = f.block(16);
    const auto type = f.symbol("___gss_c_nt_hostbased_service_oid_desc");
    require(f.call("_gss_import_name", {minor, f.buffer("HTTP@proxy.example"), type, name}) == gss_s_complete,
            "gss_import_name");
    require(f.engine.read32(minor) == 0 && f.engine.read64(name) != 0, "imported name");
    require(f.call("_gss_display_name", {minor, f.engine.read64(name), out, 0}) == gss_s_complete &&
            f.text(out) == "HTTP@proxy.example", "gss_display_name");
    require(f.call("_gss_release_buffer", {minor, out}) == gss_s_complete && f.engine.read64(out + 8) == 0,
            "gss_release_buffer");
    require(f.call("_gss_duplicate_name", {minor, f.engine.read64(name), copy}) == gss_s_complete, "gss_duplicate_name");
    require(f.call("_gss_compare_name", {minor, f.engine.read64(name), f.engine.read64(copy), equal}) == gss_s_complete &&
            f.engine.read32(equal) == 1, "duplicate compares equal");
    require(f.call("_gss_import_name", {minor, f.buffer("HTTP@other.example"), type, other}) == gss_s_complete &&
            f.call("_gss_compare_name", {minor, f.engine.read64(name), f.engine.read64(other), equal}) == gss_s_complete &&
            f.engine.read32(equal) == 0, "different names compare unequal");
    for (const auto handle : {name, copy, other})
        require(f.call("_gss_release_name", {minor, handle}) == gss_s_complete && f.engine.read64(handle) == 0,
                "gss_release_name");
    require(f.call("_gss_release_name", {minor, name}) == gss_s_complete, "releasing GSS_C_NO_NAME");
    require(f.call("_gss_import_name", {minor, f.buffer(""), type, name}) == gss_s_bad_name, "empty name");
}

void mechanisms(Fixture& f) {
    const auto minor = f.block(4), set = f.block(8), cred = f.block(8), context = f.block(8), out = f.block(16),
        name = f.block(8), message = f.block(4);
    require(f.call("_gss_indicate_mechs", {minor, set}) == gss_s_complete && f.engine.read64(f.engine.read64(set)) == 0,
            "no mechanisms");
    require(f.call("_gss_release_oid_set", {minor, set}) == gss_s_complete && f.engine.read64(set) == 0, "release set");
    // Qt: gss_acquire_cred(&minor, GSS_C_NO_NAME, GSS_C_INDEFINITE, GSS_C_NO_OID_SET, GSS_C_INITIATE, &cred, 0, 0)
    require(f.call("_gss_acquire_cred", {minor, 0, 0xffffffff, 0, 1, cred, 0, 0}) == gss_s_unavailable &&
            f.engine.read64(cred) == 0, "gss_acquire_cred is unavailable");
    const auto status = f.engine.read32(minor);
    require(status != 0, "a minor status explains the failure");
    // gss_init_sec_context with its four stack arguments (actual mech, output token, ret flags, time).
    const auto target = f.block(8);
    require(f.call("_gss_import_name", {minor, f.buffer("HTTP@proxy.example"),
                                        f.symbol("___gss_c_nt_hostbased_service_oid_desc"), target}) == gss_s_complete,
            "target name");
    f.engine.write64(context, 0);
    alchemy::CallRegs registers;
    registers.x = {minor, 0, context, f.engine.read64(target), f.symbol("___gss_spnego_mechanism_oid_desc"), 0x22, 0, 0};
    registers.push_stack<std::uint64_t>(0);     // input token
    registers.push_stack<std::uint64_t>(0);     // actual mech
    registers.push_stack<std::uint64_t>(out);   // output token
    registers.push_stack<std::uint64_t>(0);     // ret flags
    registers.push_stack<std::uint64_t>(0);     // time rec
    const auto result = alchemy::engine_call_regs(f.engine, f.symbol("_gss_init_sec_context"), registers).x0;
    require(static_cast<std::uint32_t>(result) == gss_s_unavailable, "gss_init_sec_context is unavailable");
    require(f.engine.read64(context) == 0 && f.engine.read64(out) == 0, "no context, no token");
    require(f.call("_gss_delete_sec_context", {minor, context, 0}) == gss_s_no_context, "no context to delete");
    require(f.call("_gss_release_cred", {minor, cred}) == gss_s_complete, "releasing GSS_C_NO_CREDENTIAL");
    require(f.call("_gss_inquire_cred", {minor, 0, name, 0, 0, 0}) == gss_s_no_cred, "no default credential");
    require(f.call("_gss_wrap", {minor, 0, 0, 0, 0, 0, 0}) == gss_s_no_context, "gss_wrap needs a context");
    // gss_display_status for the major and the minor code.
    f.engine.write32(message, 0);
    require(f.call("_gss_display_status", {minor, gss_s_unavailable, 1, 0, message, out}) == gss_s_complete &&
            f.text(out).find("unavailable") != std::string::npos, "major status text");
    f.call("_gss_release_buffer", {minor, out});
    require(f.call("_gss_display_status", {minor, status, 2, 0, message, out}) == gss_s_complete &&
            f.text(out).find("Kerberos") != std::string::npos && f.engine.read32(message) == 0, "minor status text");
    f.call("_gss_release_buffer", {minor, out});
    f.call("_gss_release_name", {minor, target});
}

} // namespace

int main() {
    try {
        Fixture f;
        identity_and_oids(f);
        names(f);
        mechanisms(f);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "gss tests passed\n";
    return 0;
}
