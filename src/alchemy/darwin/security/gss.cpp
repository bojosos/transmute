#include "alchemy/gui/install.hpp"

#include "alchemy/engine_adapter.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// GSS.framework provider (identity
// /System/Library/Frameworks/GSS.framework/Versions/A/GSS, listed in
// darwin_framework_libraries). Qt Network (libQt6Network, loaded by a renderer
// plug-in's server) binds it for HTTP "Negotiate" authentication.
//
// Behaviour follows RFC 2743/2744 (the GSS-API C bindings) and Apple's GSS
// headers; no Apple or Heimdal code is used. Alchemy has no security
// mechanism behind the API: neither Kerberos (no KDC, no credential cache)
// nor NTLM nor SPNEGO is available. So:
//   - names are real mechanism-independent objects (gss_import_name,
//     gss_display_name, gss_compare_name, gss_duplicate_name,
//     gss_release_name), as the bindings allow without any mechanism;
//   - gss_indicate_mechs reports an empty mechanism set;
//   - everything that needs a mechanism (credentials, contexts, per-message
//     tokens) fails with GSS_S_UNAVAILABLE and minor status
//     alchemy_minor_no_mechanism; gss_display_status explains it;
//   - release calls accept the null handles these failures leave behind.
// Callers (Qt's QAuthenticator) therefore report "Negotiate unavailable" and
// fall back to other authentication methods, as on a Mac without Kerberos.
namespace alchemy::gui {
namespace {

using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view gss_library = "/System/Library/Frameworks/GSS.framework/Versions/A/GSS";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

// <GSS/gssapi.h>: routine errors are shifted by GSS_C_ROUTINE_ERROR_OFFSET.
constexpr std::uint32_t routine(std::uint32_t code) { return code << 16; }
constexpr std::uint32_t gss_s_complete = 0, gss_s_bad_mech = routine(1), gss_s_bad_name = routine(2),
    gss_s_bad_nametype = routine(3), gss_s_bad_status = routine(5), gss_s_no_cred = routine(7),
    gss_s_no_context = routine(8), gss_s_failure = routine(13), gss_s_unavailable = routine(16);
constexpr std::uint32_t gss_c_gss_code = 1, gss_c_mech_code = 2;
// A private minor status: "no GSS mechanism is available".
constexpr std::uint32_t alchemy_minor_no_mechanism = 0x414c0001;   // 'AL' 1

// A gss_name_t: a guest malloc block "GSSN" | type length | name length | type OID bytes | name bytes.
constexpr std::uint32_t name_magic = 0x4e535347;

struct Gss {
    ProcessContext& process;

    std::uint64_t call(std::string_view name, std::initializer_list<std::uint64_t> arguments) const {
        const auto found = process.resolve(libsystem, name);
        if (!found) throw std::runtime_error("GSS provider: libSystem " + std::string(name) + " is missing");
        return EngineAdapter(process.calling_engine()).call(found->address, std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t malloc(std::uint64_t size) const {
        const auto address = call("_malloc", {size ? size : 1});
        if (!address) throw std::bad_alloc();
        return address;
    }
    void free(std::uint64_t address) const { if (address) static_cast<void>(call("_free", {address})); }

    static void minor(GuestMemory& m, std::uint64_t address, std::uint32_t value) { if (address) m.write32(address, value); }

    // gss_buffer_desc {size_t length; void* value} filled with a malloc copy of `text`.
    void fill_buffer(GuestMemory& m, std::uint64_t buffer, std::string_view text) const {
        if (!buffer) return;
        const auto value = malloc(text.size() + 1);
        std::vector<std::uint8_t> bytes(text.begin(), text.end());
        bytes.push_back(0);
        m.write_bytes(value, bytes);
        m.write64(buffer, text.size());
        m.write64(buffer + 8, value);
    }
    static void empty_buffer(GuestMemory& m, std::uint64_t buffer) {
        if (!buffer) return;
        m.write64(buffer, 0);
        m.write64(buffer + 8, 0);
    }
    static std::vector<std::uint8_t> read_buffer(const GuestMemory& m, std::uint64_t buffer, std::size_t limit = 1 << 16) {
        if (!buffer) return {};
        const auto length = m.read64(buffer), value = m.read64(buffer + 8);
        if (!length) return {};
        if (!value || length > limit) throw std::invalid_argument("bad buffer");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        m.read_bytes(value, bytes);
        return bytes;
    }
    // gss_OID_desc {OM_uint32 length; void* elements}.
    static std::vector<std::uint8_t> read_oid(const GuestMemory& m, std::uint64_t oid) {
        if (!oid) return {};
        const auto length = m.read32(oid);
        const auto elements = m.read64(oid + 8);
        if (length > 64 || (length && !elements)) throw std::invalid_argument("bad OID");
        std::vector<std::uint8_t> bytes(length);
        if (length) m.read_bytes(elements, bytes);
        return bytes;
    }

    struct Name { std::vector<std::uint8_t> type, text; };
    std::uint64_t make_name(GuestMemory& m, const Name& name) const {
        const auto block = malloc(12 + name.type.size() + name.text.size());
        m.write32(block, name_magic);
        m.write32(block + 4, static_cast<std::uint32_t>(name.type.size()));
        m.write32(block + 8, static_cast<std::uint32_t>(name.text.size()));
        if (!name.type.empty()) m.write_bytes(block + 12, name.type);
        if (!name.text.empty()) m.write_bytes(block + 12 + name.type.size(), name.text);
        return block;
    }
    static std::optional<Name> read_name(const GuestMemory& m, std::uint64_t block) {
        if (!block || m.read32(block) != name_magic) return std::nullopt;
        Name name;
        name.type.resize(m.read32(block + 4));
        name.text.resize(m.read32(block + 8));
        if (name.type.size() > 64 || name.text.size() > (1 << 16)) return std::nullopt;
        if (!name.type.empty()) m.read_bytes(block + 12, name.type);
        if (!name.text.empty()) m.read_bytes(block + 12 + name.type.size(), name.text);
        return name;
    }
};

std::string major_text(std::uint32_t status) {
    static constexpr std::array<const char*, 19> routine_errors{
        "No error", "An unsupported mechanism was requested", "An invalid name was supplied",
        "A supplied name was of an unsupported type", "Incorrect channel bindings were supplied",
        "An invalid status code was supplied", "A token had an invalid signature",
        "No credentials were supplied, or the credentials were unavailable or inaccessible",
        "No context has been established", "A token was invalid", "A credential was invalid",
        "The referenced credentials have expired", "The context has expired",
        "Unspecified GSS failure.  Minor code may provide more information",
        "The quality-of-protection requested could not be provided", "The operation is forbidden by local security policy",
        "The operation or option is unavailable", "The requested credential element already exists",
        "The provided name was not a mechanism name"};
    const auto code = (status >> 16) & 0xff;
    if (code < routine_errors.size()) return routine_errors[code];
    return "Unknown GSS routine error " + std::to_string(code);
}

} // namespace

void install_gss(InstallContext& context) {
    auto& process = context.process;
    ProcessContext::add_provider_library(std::string(gss_library));
    const auto gss = std::make_shared<Gss>(Gss{process});
    const auto install = [&process](const char* name, auto handler) {
        static_cast<void>(process.install_function(name, [handler](CpuState& s, GuestMemory& m) {
            s.x[0] = handler(s, m);
        }, std::string(gss_library)));
    };
    // Exported OID descriptors (gss_OID_desc + elements), RFC 2743/4121/4178.
    const auto oid = [&process](const char* name, std::initializer_list<std::uint8_t> elements) {
        const auto block = process.allocate_guest(16 + elements.size(), 16);
        auto& engine = process.main_engine();
        engine.write32(block, static_cast<std::uint32_t>(elements.size()));
        engine.write32(block + 4, 0);
        engine.write64(block + 8, block + 16);
        engine.write_bytes(block + 16, std::vector<std::uint8_t>(elements));
        process.install_export(std::string(gss_library), name, {block, ExportKind::data});
    };
    oid("___gss_c_nt_hostbased_service_oid_desc", {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x12, 0x01, 0x02, 0x01, 0x04});
    oid("___gss_c_nt_user_name_oid_desc", {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x12, 0x01, 0x02, 0x01, 0x01});
    oid("___gss_c_nt_export_name_oid_desc", {0x2b, 0x06, 0x01, 0x05, 0x06, 0x04});
    oid("___gss_krb5_mechanism_oid_desc", {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x12, 0x01, 0x02, 0x02});
    oid("___gss_spnego_mechanism_oid_desc", {0x2b, 0x06, 0x01, 0x05, 0x05, 0x02});
    oid("___gss_ntlm_mechanism_oid_desc", {0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x02, 0x0a});

    // OM_uint32 gss_import_name(OM_uint32* minor, gss_buffer_t name, gss_OID type, gss_name_t* out)
    install("_gss_import_name", [gss](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Gss::minor(m, s.x[0], 0);
        if (!s.x[3]) return gss_s_failure;
        m.write64(s.x[3], 0);
        Gss::Name name;
        try {
            name.text = Gss::read_buffer(m, s.x[1]);
            name.type = Gss::read_oid(m, s.x[2]);
        } catch (const std::invalid_argument&) { return gss_s_bad_name; }
        if (name.text.empty()) return gss_s_bad_name;
        m.write64(s.x[3], gss->make_name(m, name));
        return gss_s_complete;
    });
    // OM_uint32 gss_display_name(OM_uint32* minor, gss_name_t, gss_buffer_t out, gss_OID* type)
    install("_gss_display_name", [gss](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Gss::minor(m, s.x[0], 0);
        const auto name = Gss::read_name(m, s.x[1]);
        if (!name) return gss_s_bad_name;
        gss->fill_buffer(m, s.x[2], std::string_view(reinterpret_cast<const char*>(name->text.data()), name->text.size()));
        if (s.x[3]) m.write64(s.x[3], 0);   // the input type is not kept as an exported OID
        return gss_s_complete;
    });
    // OM_uint32 gss_compare_name(OM_uint32* minor, gss_name_t, gss_name_t, int* equal)
    install("_gss_compare_name", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Gss::minor(m, s.x[0], 0);
        const auto a = Gss::read_name(m, s.x[1]), b = Gss::read_name(m, s.x[2]);
        if (!a || !b) return gss_s_bad_name;
        if (s.x[3]) m.write32(s.x[3], a->text == b->text && a->type == b->type ? 1 : 0);
        return gss_s_complete;
    });
    // OM_uint32 gss_duplicate_name(OM_uint32* minor, gss_name_t, gss_name_t* out)
    install("_gss_duplicate_name", [gss](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Gss::minor(m, s.x[0], 0);
        const auto name = Gss::read_name(m, s.x[1]);
        if (!name) return gss_s_bad_name;
        if (!s.x[2]) return gss_s_failure;
        m.write64(s.x[2], gss->make_name(m, *name));
        return gss_s_complete;
    });
    // OM_uint32 gss_release_name(OM_uint32* minor, gss_name_t* name)
    install("_gss_release_name", [gss](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Gss::minor(m, s.x[0], 0);
        if (!s.x[1]) return gss_s_failure;
        const auto block = m.read64(s.x[1]);
        if (!block) return gss_s_complete;
        if (!Gss::read_name(m, block)) return gss_s_bad_name;
        m.write32(block, 0);
        gss->free(block);
        m.write64(s.x[1], 0);
        return gss_s_complete;
    });
    // OM_uint32 gss_release_buffer(OM_uint32* minor, gss_buffer_t buffer)
    install("_gss_release_buffer", [gss](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Gss::minor(m, s.x[0], 0);
        if (!s.x[1]) return gss_s_complete;
        gss->free(m.read64(s.x[1] + 8));
        Gss::empty_buffer(m, s.x[1]);
        return gss_s_complete;
    });
    // OM_uint32 gss_indicate_mechs(OM_uint32* minor, gss_OID_set* mechs): an empty set
    // (gss_OID_set_desc {size_t count; gss_OID elements}).
    install("_gss_indicate_mechs", [gss](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Gss::minor(m, s.x[0], 0);
        if (!s.x[1]) return gss_s_failure;
        const auto set = gss->malloc(16);
        m.write64(set, 0);
        m.write64(set + 8, 0);
        m.write64(s.x[1], set);
        return gss_s_complete;
    });
    // OM_uint32 gss_release_oid_set(OM_uint32* minor, gss_OID_set* set)
    install("_gss_release_oid_set", [gss](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Gss::minor(m, s.x[0], 0);
        if (!s.x[1]) return gss_s_failure;
        const auto set = m.read64(s.x[1]);
        if (set) { gss->free(m.read64(set + 8)); gss->free(set); }
        m.write64(s.x[1], 0);
        return gss_s_complete;
    });
    // Everything that needs a mechanism. The output handles are cleared.
    // gss_acquire_cred(minor, name, time_req, mechs, usage, cred* out, mechs* actual, time* rec)
    install("_gss_acquire_cred", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Gss::minor(m, s.x[0], alchemy_minor_no_mechanism);
        if (s.x[5]) m.write64(s.x[5], 0);
        if (s.x[6]) m.write64(s.x[6], 0);
        if (s.x[7]) m.write32(s.x[7], 0);
        return gss_s_unavailable;
    });
    // gss_init_sec_context(minor, cred, ctx*, target, mech, flags, time_req, bindings | input,
    //                      mech* actual, output, flags* ret, time* rec) - the last five on the stack.
    install("_gss_init_sec_context", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Gss::minor(m, s.x[0], alchemy_minor_no_mechanism);
        if (s.x[2] && m.read64(s.x[2])) return gss_s_no_context;   // a context we never created
        const auto actual = m.read64(s.sp + 8), output = m.read64(s.sp + 16), flags = m.read64(s.sp + 24),
            time = m.read64(s.sp + 32);
        if (actual) m.write64(actual, 0);
        Gss::empty_buffer(m, output);
        if (flags) m.write32(flags, 0);
        if (time) m.write32(time, 0);
        return gss_s_unavailable;
    });
    // gss_accept_sec_context(minor, ctx*, cred, input, bindings, name* src, mech* actual, output,
    //                        flags* ret, time* rec, cred* delegated) - arguments 9.. on the stack.
    install("_gss_accept_sec_context", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Gss::minor(m, s.x[0], alchemy_minor_no_mechanism);
        if (s.x[1] && m.read64(s.x[1])) return gss_s_no_context;
        if (s.x[5]) m.write64(s.x[5], 0);
        if (s.x[6]) m.write64(s.x[6], 0);
        Gss::empty_buffer(m, s.x[7]);
        const auto flags = m.read64(s.sp), time = m.read64(s.sp + 8), delegated = m.read64(s.sp + 16);
        if (flags) m.write32(flags, 0);
        if (time) m.write32(time, 0);
        if (delegated) m.write64(delegated, 0);
        return gss_s_unavailable;
    });
    // gss_delete_sec_context(minor, ctx*, output token)
    install("_gss_delete_sec_context", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Gss::minor(m, s.x[0], 0);
        Gss::empty_buffer(m, s.x[2]);
        return gss_s_no_context;   // no context can exist (GSS_C_NO_CONTEXT included, RFC 2744)
    });
    // gss_release_cred(minor, cred*)
    install("_gss_release_cred", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Gss::minor(m, s.x[0], 0);
        if (!s.x[1] || !m.read64(s.x[1])) return gss_s_complete;   // GSS_C_NO_CREDENTIAL
        return gss_s_no_cred;   // no credential can exist
    });
    // gss_inquire_cred(minor, cred, name*, lifetime*, usage*, mechs*)
    install("_gss_inquire_cred", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Gss::minor(m, s.x[0], alchemy_minor_no_mechanism);
        if (s.x[2]) m.write64(s.x[2], 0);
        if (s.x[3]) m.write32(s.x[3], 0);
        if (s.x[4]) m.write32(s.x[4], 0);
        if (s.x[5]) m.write64(s.x[5], 0);
        return gss_s_no_cred;
    });
    // Per-message calls need an established context, which never exists.
    for (const auto* name : {"_gss_wrap", "_gss_unwrap", "_gss_get_mic", "_gss_verify_mic", "_gss_inquire_context",
                             "_gss_context_time", "_gss_export_sec_context", "_gss_import_sec_context"}) {
        install(name, [](CpuState& s, GuestMemory& m) -> std::uint64_t {
            Gss::minor(m, s.x[0], 0);
            return gss_s_no_context;
        });
    }
    // OM_uint32 gss_display_status(OM_uint32* minor, OM_uint32 status, int type, gss_OID mech,
    //                              OM_uint32* message_context, gss_buffer_t string)
    install("_gss_display_status", [gss](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Gss::minor(m, s.x[0], 0);
        const auto status = static_cast<std::uint32_t>(s.x[1]);
        const auto type = static_cast<std::uint32_t>(s.x[2]);
        if (s.x[4]) m.write32(s.x[4], 0);   // one message per status
        std::string text;
        if (type == gss_c_gss_code) {
            // Supplementary bits are not reported separately.
            text = major_text(status);
        } else if (type == gss_c_mech_code) {
            text = status == alchemy_minor_no_mechanism
                ? "No GSS-API mechanism is available under Alchemy (Kerberos, NTLM and SPNEGO are unsupported)"
                : status == 0 ? "Success" : "Unknown minor status " + std::to_string(status);
        } else {
            Gss::empty_buffer(m, s.x[5]);
            return gss_s_bad_status;
        }
        gss->fill_buffer(m, s.x[5], text);
        return gss_s_complete;
    });
    static_cast<void>(gss_s_bad_mech);
    static_cast<void>(gss_s_bad_nametype);
}

} // namespace alchemy::gui
