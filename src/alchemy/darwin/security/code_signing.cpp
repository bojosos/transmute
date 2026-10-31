#include "alchemy/darwin/security/security_internal.hpp"
#include "alchemy/darwin/security/code_signature.hpp"
#include "alchemy/darwin/security/code_requirements.hpp"

#include "alchemy/darwin/corefoundation/cf_plist.hpp"
#include "alchemy/path.hpp"

#include <fstream>
#include <map>
#include <mutex>

// Code signing (SecCode / SecStaticCode / SecRequirement), Authorization
// Services and SecTransform, answered for a Windows host:
// - Static code (code on disk, a bundle or a Mach-O) is read from its
//   embedded signature (code_signature.cpp): SecCodeCopySigningInformation
//   reports the identifier, team, flags, cdhashes, digest algorithms, format,
//   Info.plist, entitlements and the signing certificates and time;
//   SecStaticCodeCheckValidity(WithErrors) checks the page hashes, the
//   special slots, the sealed resources (CodeResources, nested code by its
//   cdhash, or fully with kSecCSCheckNestedCode) and the CMS signature with
//   its certificate chain (errSecCSUnsigned for unsigned code; resource
//   failures carry kSecCFErrorResourceAltered / Missing).
// - Running code: SecCodeCopySelf and SecCodeCopyGuestWithAttributes (a pid
//   or an audit token, through proc_pidpath; dynamic code attributes answer
//   errSecCSUnimplemented) give code objects whose static
//   code and signing information are their executable's. Alchemy does not
//   validate code as it runs (csops reports no CS_VALID), so
//   SecCodeCheckValidity and the dynamic status answer that: status 0, and
//   errSecCSUnimplemented for dynamic validity.
// - Code requirements (code_requirements.cpp): SecRequirementCreateWithString
//   (AndErrors) / WithData, CopyString / CopyData, the designated requirement
//   (the signature's, else the implicit one), and static validity against a
//   requirement (errSecCSReqFailed when it is not satisfied).
// - Alchemy runs every guest as one unprivileged user without an
//   authorization policy database: empty authorizations can be created, but
//   no right can be obtained, defined or read (errAuthorizationDenied), and
//   tools never run with privileges.
// - SecTransform graphs are not provided: creating one fails with an
//   errSecUnimplemented CFError.
namespace alchemy::gui::security {
namespace {

constexpr std::string_view lib = security_library;
constexpr std::uint32_t authorization_flag_extend_rights = 1U << 1;
// SecCSFlags for signing information.
constexpr std::uint32_t info_signing = 1U << 1, info_requirements = 1U << 2, info_dynamic = 1U << 3;

struct RequirementState final : ObjectPayload { Bytes blob; };

struct CodeState final : ObjectPayload {
    std::string path;     // guest path: the bundle or the file
    bool self{};          // running code (SecCode), else static code
    std::mutex mutex;
    std::shared_ptr<codesign::StaticCode> code;   // parsed on first use
    std::int32_t status{};
};

Address status_value(std::int32_t value) { return static_cast<Address>(static_cast<std::int64_t>(value)); }

// The host location of a guest path.
std::filesystem::path host_of(const std::string& guest) {
    if (guest.size() > 10 && guest.starts_with("/Volumes/") && guest[10] == '/')
        return utf8_path(std::string(1, guest[9]) + ":" + guest.substr(10));
    if (auto mapped = guest_root_host_path(guest)) return *mapped;
    return utf8_path(guest);
}

// The bundle around a main executable (".../X.app/Contents/MacOS/x"), else the executable itself.
std::string code_root_of(const std::string& executable) {
    const auto macos = executable.rfind("/Contents/MacOS/");
    if (macos != std::string::npos && executable.find('/', macos + 16) == std::string::npos) return executable.substr(0, macos);
    return executable;
}

} // namespace

void install_code_signing(Security& sec) {
    auto b = sec.b;
    const auto static_code = b->define_class("_AlchemySecStaticCode", b->object_class, lib);
    const auto code = b->define_class("_AlchemySecCode", b->object_class, lib);
    const auto authorization = b->define_class("_AlchemyAuthorization", b->object_class, lib);
    b->register_type_id(static_code, TypeId::static_code);
    b->register_type_id(code, TypeId::code);
    const auto requirement_class = b->define_class("_AlchemySecRequirement", b->object_class, lib);
    b->register_type_id(requirement_class, TypeId::requirement);
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"kSecCodeInfoIdentifier", "identifier"}, {"kSecCodeInfoFormat", "format"}, {"kSecCodeInfoDigestAlgorithm", "digest-algorithm"},
             {"kSecCodeInfoDigestAlgorithms", "digest-algorithms"}, {"kSecCodeInfoPlatformIdentifier", "platform-identifier"},
             {"kSecCodeInfoCdHashes", "cdhashes"}, {"kSecCodeInfoSource", "source"}, {"kSecCodeInfoTeamIdentifier", "teamid"},
             {"kSecCodeInfoEntitlements", "entitlements"}, {"kSecCodeInfoEntitlementsDict", "entitlements-dict"},
             {"kSecCodeInfoTime", "signing-time"}, {"kSecCodeInfoStatus", "status"}, {"kSecCodeInfoCertificates", "certificates"},
             {"kSecCodeInfoFlags", "flags"}, {"kSecCodeInfoMainExecutable", "main-executable"}, {"kSecCodeInfoPList", "info-plist"},
             {"kSecCodeInfoUnique", "unique"}, {"kSecCodeInfoRequirements", "requirements"},
             {"kSecCodeInfoRequirementData", "requirement-data"}, {"kSecCodeInfoDesignatedRequirement", "designated-requirement"},
             {"kSecCodeInfoImplicitDesignatedRequirement", "implicit-requirement"}, {"kSecCFErrorResourceAltered", "SecCSResourceAltered"},
             {"kSecCFErrorResourceMissing", "SecCSResourceMissing"}})
        sec.export_constant(name, value);

    const auto make = [b](Address cls, std::string path, bool self) {
        const auto object = b->objc.allocate(cls);
        auto state = std::make_shared<CodeState>();
        state->path = std::move(path);
        state->self = self;
        static_cast<void>(b->objc.host_state<CodeState>(object, [&] { return state; }));
        return object;
    };
    const auto state_of = [b](Address object) { return object ? b->objc.find_host_state<CodeState>(object) : nullptr; };
    const auto make_requirement = [b, requirement_class](Bytes blob) {
        const auto object = b->objc.allocate(requirement_class);
        auto state = std::make_shared<RequirementState>();
        state->blob = std::move(blob);
        static_cast<void>(b->objc.host_state<RequirementState>(object, [&] { return state; }));
        return object;
    };
    const auto requirement_of = [b](Address object) { return object ? b->objc.find_host_state<RequirementState>(object) : nullptr; };
    // The parsed code of a code object (nullptr with the status when it is not code).
    const auto parsed = [](const std::shared_ptr<CodeState>& state, std::int32_t& status) -> std::shared_ptr<codesign::StaticCode> {
        std::lock_guard lock(state->mutex);
        if (!state->code && !state->status) {
            if (auto value = codesign::open(host_of(state->path), state->status)) state->code = std::make_shared<codesign::StaticCode>(std::move(*value));
        }
        status = state->status;
        return state->code;
    };
    // The guest path of a process's executable (proc_pidpath), empty when there is none.
    const auto process_path = [b](Address pid) -> std::string {
        const auto buffer = b->call(libsystem_library, "_malloc", {4096});
        const auto length = static_cast<std::int64_t>(b->call(libsystem_library, "_proc_pidpath", {pid, buffer, 4096}));
        std::string path;
        for (std::int64_t index = 0; index < length; ++index) path += static_cast<char>(b->memory().read8(buffer + static_cast<Address>(index)));
        static_cast<void>(b->call(libsystem_library, "_free", {buffer}));
        return path;
    };
    const auto self_pid = [b] { return b->call(libsystem_library, "_getpid", {}); };
    const auto store_status_error = [b](Address out, const codesign::Validation& result) {
        if (!out) return;
        std::vector<std::pair<std::string, Address>> info;
        Address list{};
        if (!result.resource.empty()) {
            const auto url = b->string(result.resource);
            list = b->array({url});
            b->release(url);
            info.emplace_back(result.missing ? "SecCSResourceMissing" : "SecCSResourceAltered", list);
        }
        b->store_error(out, b->error("NSOSStatusErrorDomain", result.status, result.detail, info), false);
        b->release(list);
    };

    b->function(lib, "_SecStaticCodeGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::static_code); });
    b->function(lib, "_SecCodeGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::code); });
    b->function(lib, "_SecRequirementGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::requirement); });
    // References to code on disk / running code.
    const auto create_static = [b, make, static_code](CpuState& s, GuestMemory& m, Address out) {
        if (!s.x[0] || !out) { s.x[0] = status_value(status::param); return; }
        m.write64(out, make(static_code, b->cf.url_path(s.x[0]), false));
        s.x[0] = 0;
    };
    b->function(lib, "_SecStaticCodeCreateWithPath", [create_static](CpuState& s, GuestMemory& m) { create_static(s, m, s.x[2]); });
    b->function(lib, "_SecStaticCodeCreateWithPathAndAttributes", [create_static](CpuState& s, GuestMemory& m) { create_static(s, m, s.x[3]); });
    b->function(lib, "_SecCodeCopySelf", [make, code, process_path, self_pid](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = status_value(status::param); return; }
        m.write64(s.x[1], make(code, code_root_of(process_path(self_pid())), true));
        s.x[0] = 0;
    });
    // (host, attributes, flags, SecCodeRef *guest): the guest named by kSecGuestAttributePid or kSecGuestAttributeAudit.
    b->function(lib, "_SecCodeCopyGuestWithAttributes", [b, make, code, process_path](CpuState& s, GuestMemory& m) {
        if (!s.x[3]) { s.x[0] = status_value(status::param); return; }
        std::optional<Address> pid;
        if (s.x[1]) {
            for (const auto& [key, value] : b->cf.dictionary_entries(s.x[1])) {
                const auto name = b->maybe_text(key).value_or("");
                if (name == "pid") {
                    if (const auto number = b->integer(value)) pid = static_cast<Address>(*number);
                } else if (name == "audit") {
                    if (!b->is_data(value)) { s.x[0] = status_value(codesign::cs::invalid_attribute_values); return; }
                    const auto token = b->bytes(value);   // audit_token_t: eight 32-bit words, the pid is the sixth
                    if (token.size() < 32) { s.x[0] = status_value(codesign::cs::invalid_attribute_values); return; }
                    pid = token[20] | (static_cast<Address>(token[21]) << 8) | (static_cast<Address>(token[22]) << 16) | (static_cast<Address>(token[23]) << 24);
                } else if (name == "dynamicCode" || name == "dynamicCodeInfoPlist") {
                    // Dynamic (in-memory) code with its own signature: not provided by Alchemy (Chrome passes it and
                    // treats errSecCSUnimplemented as "no answer", as before code signing was read at all).
                    s.x[0] = status_value(codesign::cs::unimplemented);
                    return;
                } else {
                    s.x[0] = status_value(codesign::cs::unsupported_guest_attributes);
                    return;
                }
            }
        }
        if (!pid) { s.x[0] = status_value(codesign::cs::no_such_code); return; }
        const auto path = process_path(*pid);
        if (path.empty()) { s.x[0] = status_value(codesign::cs::no_such_code); return; }
        m.write64(s.x[3], make(code, code_root_of(path), true));
        s.x[0] = 0;
    });
    b->function(lib, "_SecCodeCopyStaticCode", [make, static_code, state_of](CpuState& s, GuestMemory& m) {
        const auto state = state_of(s.x[0]);
        if (!state || !s.x[2]) { s.x[0] = status_value(status::param); return; }
        m.write64(s.x[2], make(static_code, state->path, false));
        s.x[0] = 0;
    });
    // (SecStaticCodeRef, flags, CFURLRef *path): the bundle or the file.
    b->function(lib, "_SecCodeCopyPath", [b, state_of](CpuState& s, GuestMemory& m) {
        const auto state = state_of(s.x[0]);
        if (!state || !s.x[2]) { s.x[0] = status_value(status::param); return; }
        if (state->path.empty()) { s.x[0] = status_value(codesign::cs::no_such_code); return; }
        std::error_code error;
        m.write64(s.x[2], b->cf.file_url(state->path, std::filesystem::is_directory(host_of(state->path), error)));
        s.x[0] = 0;
    });
    // (code, flags, CFDictionaryRef *information)
    b->function(lib, "_SecCodeCopySigningInformation", [&sec, b, state_of, parsed, make_requirement](CpuState& s, GuestMemory& m) {
        const auto state = state_of(s.x[0]);
        if (!state || !s.x[2]) { s.x[0] = status_value(status::param); return; }
        std::int32_t status{};
        const auto value = parsed(state, status);
        if (!value) { s.x[0] = status_value(status); return; }
        const auto flags = static_cast<std::uint32_t>(s.x[1]);
        std::vector<std::pair<Address, Address>> entries;
        std::vector<Address> owned;
        const auto add = [&](std::string_view key, Address object) {
            if (!object) return;
            const auto name = b->string(key);
            owned.push_back(name);
            owned.push_back(object);
            entries.emplace_back(name, object);
        };
        const auto plist_object = [&](std::span<const std::uint8_t> bytes) -> Address {
            const auto plist = export_fill::plist_parse(bytes);
            return plist ? plist_to_guest(b->process, b->cf, *plist) : 0;
        };
        add("main-executable", b->cf.file_url(guest_path_string(value->executable), false));
        add("format", b->string(codesign::format_of(*value)));
        if (value->info_plist)
            if (std::ifstream in(*value->info_plist, std::ios::binary); in)
                add("info-plist", plist_object(Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>())));
        if (value->is_signed()) {
            const auto& signature = *value->preferred().signature;
            const auto& primary = signature.directories.front();
            const auto& best = signature.best();
            add("identifier", b->string(primary.identifier));
            if (!primary.team.empty()) add("teamid", b->string(primary.team));
            add("flags", b->number(primary.flags));
            add("unique", b->data(best.cdhash()));
            std::vector<Address> hashes, algorithms;
            for (const auto& cd : signature.directories) {
                hashes.push_back(b->data(cd.cdhash()));
                algorithms.push_back(b->number(cd.hash_type));
            }
            add("cdhashes", b->array(hashes));
            add("digest-algorithms", b->array(algorithms));
            b->release_all(hashes);
            b->release_all(algorithms);
            add("digest-algorithm", b->number(best.hash_type));
            add("source", b->string("embedded"));
            if (primary.platform) add("platform-identifier", b->number(primary.platform));
            if ((flags & info_requirements) && signature.entitlements.size() > 8) {
                add("entitlements", b->data(signature.entitlements));
                add("entitlements-dict", plist_object(std::span(signature.entitlements).subspan(8)));
            }
            if (flags & info_requirements) {
                if (!signature.requirements.empty()) {
                    add("requirement-data", b->data(signature.requirements));
                    add("requirements", b->string(codesign::requirement_set_text(signature.requirements)));
                }
                bool implicit = false;
                if (auto designated = codesign::designated_requirement(*value, &implicit)) {
                    add("designated-requirement", make_requirement(*designated));
                    if (implicit) add("implicit-requirement", make_requirement(std::move(*designated)));
                }
            }
            if ((flags & info_signing) && !(primary.flags & codesign::flag_adhoc) && !signature.cms.empty()) {
                const auto cms = codesign::check_cms(signature);
                std::vector<Address> certificates;
                for (const auto& certificate : cms.certificates) certificates.push_back(sec.make_certificate(certificate));
                add("certificates", b->array(certificates));
                b->release_all(certificates);
                if (cms.signing_time) add("signing-time", b->cf.date(*cms.signing_time, false));
            }
        }
        // Running code: Alchemy does not validate pages as they run (csops reports no CS_VALID).
        if (state->self && (flags & info_dynamic)) add("status", b->number(0));
        m.write64(s.x[2], b->dictionary(entries));
        b->release_all(owned);
        s.x[0] = 0;
    });
    // Static validity: (code, flags, requirement[, CFErrorRef *errors]).
    const auto check_static = [state_of, parsed, requirement_of, store_status_error](CpuState& s, Address errors) {
        const auto state = state_of(s.x[0]);
        if (!state) { s.x[0] = status_value(codesign::cs::invalid_object); return; }
        const auto requirement = s.x[2] ? requirement_of(s.x[2]) : nullptr;
        if (s.x[2] && !requirement) { s.x[0] = status_value(codesign::cs::invalid_object); return; }
        std::int32_t status{};
        const auto value = parsed(state, status);
        codesign::Validation result;
        if (!value) result = {status, "not code", {}, false};
        else result = codesign::validate(*value, static_cast<std::uint32_t>(s.x[1]));
        if (!result.status && requirement) result.status = codesign::evaluate(requirement->blob, *value, &result.detail);
        if (result.status) store_status_error(errors, result);
        s.x[0] = status_value(result.status);
    };
    b->function(lib, "_SecStaticCodeCheckValidity", [check_static](CpuState& s, GuestMemory&) { check_static(s, 0); });
    b->function(lib, "_SecStaticCodeCheckValidityWithErrors", [check_static](CpuState& s, GuestMemory&) { check_static(s, s.x[3]); });
    // Running code is not validated by Alchemy.
    b->function(lib, "_SecCodeCheckValidity", [](CpuState& s, GuestMemory&) { s.x[0] = status_value(codesign::cs::unimplemented); });
    b->function(lib, "_SecCodeCheckValidityWithErrors", [b](CpuState& s, GuestMemory&) {
        if (s.x[3]) b->store_error(s.x[3], b->os_status_error(codesign::cs::unimplemented, "Alchemy does not validate running code"), false);
        s.x[0] = status_value(codesign::cs::unimplemented);
    });
    // ---- code requirements ----
    // (text, flags[, CFErrorRef *errors], SecRequirementRef *requirement)
    const auto create_from_text = [b, make_requirement](CpuState& s, GuestMemory& m, Address errors, Address out) {
        if (!s.x[0] || !out || !b->is_string(s.x[0])) { s.x[0] = status_value(status::param); return; }
        std::string error;
        auto blob = codesign::compile(b->text(s.x[0]), error);
        if (!blob) {
            if (errors) b->store_error(errors, b->os_status_error(codesign::cs::requirement_invalid, "invalid code requirement: " + error), false);
            s.x[0] = status_value(codesign::cs::requirement_invalid);
            return;
        }
        m.write64(out, make_requirement(std::move(*blob)));
        s.x[0] = 0;
    };
    b->function(lib, "_SecRequirementCreateWithString", [create_from_text](CpuState& s, GuestMemory& m) { create_from_text(s, m, 0, s.x[2]); });
    b->function(lib, "_SecRequirementCreateWithStringAndErrors", [create_from_text](CpuState& s, GuestMemory& m) {
        create_from_text(s, m, s.x[2], s.x[3]);
    });
    // (CFDataRef, flags, SecRequirementRef *)
    b->function(lib, "_SecRequirementCreateWithData", [b, make_requirement](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[2] || !b->is_data(s.x[0])) { s.x[0] = status_value(status::param); return; }
        auto blob = b->bytes(s.x[0]);
        if (!codesign::requirement_text(blob)) { s.x[0] = status_value(codesign::cs::requirement_invalid); return; }
        m.write64(s.x[2], make_requirement(std::move(blob)));
        s.x[0] = 0;
    });
    b->function(lib, "_SecRequirementCopyData", [b, requirement_of](CpuState& s, GuestMemory& m) {
        const auto state = requirement_of(s.x[0]);
        if (!state || !s.x[2]) { s.x[0] = status_value(status::param); return; }
        m.write64(s.x[2], b->data(state->blob));
        s.x[0] = 0;
    });
    b->function(lib, "_SecRequirementCopyString", [b, requirement_of](CpuState& s, GuestMemory& m) {
        const auto state = requirement_of(s.x[0]);
        if (!state || !s.x[2]) { s.x[0] = status_value(status::param); return; }
        const auto text = codesign::requirement_text(state->blob);
        if (!text) { s.x[0] = status_value(codesign::cs::requirement_invalid); return; }
        m.write64(s.x[2], b->string(*text));
        s.x[0] = 0;
    });
    // (code, flags, SecRequirementRef *)
    b->function(lib, "_SecCodeCopyDesignatedRequirement", [state_of, parsed, make_requirement](CpuState& s, GuestMemory& m) {
        const auto state = state_of(s.x[0]);
        if (!state || !s.x[2]) { s.x[0] = status_value(status::param); return; }
        std::int32_t status{};
        const auto value = parsed(state, status);
        if (!value) { s.x[0] = status_value(status); return; }
        auto designated = codesign::designated_requirement(*value);
        if (!designated) { s.x[0] = status_value(codesign::cs::unsigned_code); return; }
        m.write64(s.x[2], make_requirement(std::move(*designated)));
        s.x[0] = 0;
    });

    // CFStringRef SecCopyErrorMessageString(OSStatus, void* reserved): a +1
    // description of the Security status values Alchemy itself returns, NULL
    // for any other (as documented when no message is available). The texts
    // are Alchemy's own wording.
    b->function(lib, "_SecCopyErrorMessageString", [b](CpuState& s, GuestMemory&) {
        static const std::map<std::int32_t, std::string_view> messages{
            {status::success, "No error."}, {status::unimplemented, "Function or operation not implemented."},
            {status::param, "One or more parameters passed to a function were not valid."},
            {status::allocate, "Failed to allocate memory."}, {status::not_available, "No keychain is available."},
            {status::duplicate_item, "The specified item already exists in the keychain."},
            {status::item_not_found, "The specified item could not be found in the keychain."},
            {status::interaction_not_allowed, "User interaction is not allowed."},
            {status::decode, "Unable to decode the provided data."},
            {status::cs_unimplemented, "Code signing operation is not implemented."},
            {status::authorization_denied, "The authorization was denied."},
            {status::not_trusted, "The certificate is not trusted."},
            {status::verify_failed, "A cryptographic verification failure has occurred."}};
        const auto found = messages.find(static_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[0])));
        s.x[0] = found == messages.end() ? 0 : b->string(found->second);
    });

    // ---- Authorization Services --------------------------------------------------------------------
    // AuthorizationRights: {UInt32 count; AuthorizationItem *items}.
    const auto right_count = [](GuestMemory& m, Address rights) -> std::uint32_t { return rights ? m.read32(rights) : 0; };
    b->function(lib, "_AuthorizationCreate", [b, authorization, right_count](CpuState& s, GuestMemory& m) {
        const auto rights = right_count(m, s.x[0]);
        if (rights && (s.x[2] & authorization_flag_extend_rights)) { s.x[0] = status_value(status::authorization_denied); return; }
        if (s.x[3]) m.write64(s.x[3], b->objc.allocate(authorization));
        s.x[0] = 0;
    });
    b->function(lib, "_AuthorizationFree", [b, authorization](CpuState& s, GuestMemory&) {
        if (!s.x[0] || b->objc.object_class(s.x[0]) != authorization) { s.x[0] = status_value(-60002); return; }   // errAuthorizationInvalidRef
        b->process.objects().release(s.x[0]);
        s.x[0] = 0;
    });
    b->function(lib, "_AuthorizationCopyRights", [right_count](CpuState& s, GuestMemory& m) {
        if (s.x[4]) m.write64(s.x[4], 0);
        s.x[0] = right_count(m, s.x[1]) ? status_value(status::authorization_denied) : 0;
    });
    for (const auto* name : {"_AuthorizationRightGet", "_AuthorizationRightSet", "_AuthorizationRightRemove",
                             "_AuthorizationExecuteWithPrivileges"})
        b->function(lib, name, [](CpuState& s, GuestMemory&) { s.x[0] = status_value(status::authorization_denied); });

    // ---- SecTransform --------------------------------------------------------------------------------------
    sec.export_constant("kSecTransformInputAttributeName", "INPUT");
    sec.export_constant("kSecTransformOutputAttributeName", "OUTPUT");
    sec.export_constant("kSecTransformDebugAttributeName", "DEBUG");
    sec.export_constant("kSecTransformTransformName", "NAME");
    sec.export_constant("kSecTransformAbortAttributeName", "ABORT");
    sec.export_constant("kSecDigestSHA1", "SHA1");
    sec.export_constant("kSecDigestSHA2", "SHA2");
    sec.export_constant("kSecDigestMD5", "MD5");
    sec.export_constant("kSecDigestTypeAttribute", "DigestType");
    sec.export_constant("kSecDigestLengthAttribute", "DigestLength");
    const auto transform_error = [b](Address out) {
        if (out) b->store_error(out, b->os_status_error(status::unimplemented, "SecTransform is not implemented by Alchemy"), false);
    };
    // (type, length, error*) / (key, signature, error*) / (transform, error*).
    b->function(lib, "_SecDigestTransformCreate", [transform_error](CpuState& s, GuestMemory&) { transform_error(s.x[2]); s.x[0] = 0; });
    b->function(lib, "_SecVerifyTransformCreate", [transform_error](CpuState& s, GuestMemory&) { transform_error(s.x[2]); s.x[0] = 0; });
    b->function(lib, "_SecSignTransformCreate", [transform_error](CpuState& s, GuestMemory&) { transform_error(s.x[1]); s.x[0] = 0; });
    b->function(lib, "_SecTransformExecute", [transform_error](CpuState& s, GuestMemory&) { transform_error(s.x[1]); s.x[0] = 0; });
    b->function(lib, "_SecTransformCreateReadTransformWithReadStream", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    b->function(lib, "_SecTransformCreateGroupTransform", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    // (fromTransform, fromName, toTransform, toName, group, error*)
    b->function(lib, "_SecTransformConnectTransforms", [transform_error](CpuState& s, GuestMemory&) { transform_error(s.x[5]); s.x[0] = 0; });
    b->function(lib, "_SecTransformSetAttribute", [transform_error](CpuState& s, GuestMemory&) { transform_error(s.x[3]); s.x[0] = 0; });
}

} // namespace alchemy::gui::security
