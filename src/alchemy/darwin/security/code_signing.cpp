#include "alchemy/darwin/security/security_internal.hpp"

// Code signing (SecCode / SecStaticCode / SecRequirement), Authorization
// Services and SecTransform, answered honestly for a Windows host:
// - Code-signature checks are not implemented (Apple's code-signing anchors
//   are Apple's, and Alchemy does not evaluate requirement programs): every
//   validity / requirement / signing-information query fails with
//   errSecCSUnimplemented, so callers (Sparkle) treat code as unverified.
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

struct CodeState final : ObjectPayload { std::string path; bool self{}; };

Address status_value(std::int32_t value) { return static_cast<Address>(static_cast<std::int64_t>(value)); }

} // namespace

void install_code_signing(Security& sec) {
    auto b = sec.b;
    const auto static_code = b->define_class("_AlchemySecStaticCode", b->object_class, lib);
    const auto code = b->define_class("_AlchemySecCode", b->object_class, lib);
    const auto authorization = b->define_class("_AlchemyAuthorization", b->object_class, lib);
    b->register_type_id(static_code, TypeId::static_code);
    b->register_type_id(code, TypeId::code);

    const auto make = [b](Address cls, std::string path, bool self) {
        const auto object = b->objc.allocate(cls);
        auto state = std::make_shared<CodeState>();
        state->path = std::move(path);
        state->self = self;
        static_cast<void>(b->objc.host_state<CodeState>(object, [&] { return state; }));
        return object;
    };
    const auto unimplemented_error = [b](Address out) {
        if (out) b->store_error(out, b->os_status_error(status::cs_unimplemented,
            "code signature validation is not implemented by Alchemy"), false);
    };

    b->function(lib, "_SecStaticCodeGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::static_code); });
    b->function(lib, "_SecCodeGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::code); });
    b->function(lib, "_SecRequirementGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::requirement); });
    // References to code on disk / the running process are plain handles.
    const auto create_static = [b, make, static_code](CpuState& s, GuestMemory& m, Address out) {
        if (!s.x[0] || !out) { s.x[0] = status_value(status::param); return; }
        m.write64(out, make(static_code, b->cf.url_path(s.x[0]), false));
        s.x[0] = 0;
    };
    b->function(lib, "_SecStaticCodeCreateWithPath", [create_static](CpuState& s, GuestMemory& m) { create_static(s, m, s.x[2]); });
    b->function(lib, "_SecStaticCodeCreateWithPathAndAttributes", [create_static](CpuState& s, GuestMemory& m) { create_static(s, m, s.x[3]); });
    b->function(lib, "_SecCodeCopySelf", [b, make, code](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = status_value(status::param); return; }
        m.write64(s.x[1], make(code, {}, true));
        s.x[0] = 0;
    });
    b->function(lib, "_SecCodeCopyStaticCode", [b, make, static_code](CpuState& s, GuestMemory& m) {
        const auto state = s.x[0] ? b->objc.find_host_state<CodeState>(s.x[0]) : nullptr;
        if (!state || !s.x[2]) { s.x[0] = status_value(status::param); return; }
        m.write64(s.x[2], make(static_code, state->path, state->self));
        s.x[0] = 0;
    });
    for (const auto* name : {"_SecStaticCodeCheckValidity", "_SecCodeCheckValidity"})
        b->function(lib, name, [](CpuState& s, GuestMemory&) { s.x[0] = status_value(status::cs_unimplemented); });
    for (const auto* name : {"_SecStaticCodeCheckValidityWithErrors", "_SecCodeCheckValidityWithErrors"})
        b->function(lib, name, [unimplemented_error](CpuState& s, GuestMemory&) {
            unimplemented_error(s.x[3]);
            s.x[0] = status_value(status::cs_unimplemented);
        });
    // No requirement object can be created, so none can be copied out either.
    for (const auto* name : {"_SecCodeCopySigningInformation", "_SecCodeCopyDesignatedRequirement", "_SecRequirementCopyString",
                             "_SecRequirementCopyData", "_SecRequirementCreateWithString", "_SecRequirementCreateWithData",
                             "_SecCodeCopyGuestWithAttributes"})
        b->function(lib, name, [](CpuState& s, GuestMemory&) { s.x[0] = status_value(status::cs_unimplemented); });

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
