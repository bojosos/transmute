#include "alchemy/gui/fill_provider.hpp"

#include <array>
#include <utility>

// OpenDirectory: no directory service is running (nil + an error); the
// attribute and record type constants are real.
// The Qt WebEngine images first imported them (moved from
// darwin/identities/system_gaps.cpp); installed by the fill-only kit
// (kit/fwgaps/install.cpp) at the old position.
namespace alchemy::gui::fill {
namespace {

class Sessions final : public Provider {
public:
    using Provider::Provider;
    void install();
};

void Sessions::install() {
    for (const auto& [symbol, value] : std::array<std::pair<const char*, const char*>, 5>{{
             {"_kODAttributeTypeAllAttributes", "dsAttributesAll"},
             {"_kODAttributeTypeAltSecurityIdentities", "dsAttrTypeStandard:AltSecurityIdentities"},
             {"_kODAttributeTypeMetaRecordName", "dsAttrTypeStandard:AppleMetaRecordName"},
             {"_kODAttributeTypeRecordName", "dsAttrTypeStandard:RecordName"},
             {"_kODRecordTypeUsers", "dsRecTypeStandard:Users"}}})
        string(open_directory, symbol, value);
    // No Open Directory daemon runs: sessions and nodes cannot be opened.
    const auto unavailable = [this](GuestMemory& m, Address out) {
        if (out) m.write64(out, bridge->autorelease(bridge->error("com.apple.OpenDirectory", 1002, "The Open Directory daemon is not running")));
    };
    const auto session = cls(open_directory, "ODSession", "NSObject");
    method(session, "defaultSession", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true);
    method(session, "sessionWithOptions:error:", [unavailable](CpuState& s, GuestMemory& m) { unavailable(m, s.x[3]); s.x[0] = 0; }, true);
    const auto node = cls(open_directory, "ODNode", "NSObject");
    method(node, "nodeWithSession:type:error:", [unavailable](CpuState& s, GuestMemory& m) { unavailable(m, s.x[4]); s.x[0] = 0; }, true);
    method(node, "nodeWithSession:name:error:", [unavailable](CpuState& s, GuestMemory& m) { unavailable(m, s.x[4]); s.x[0] = 0; }, true);
    static_cast<void>(cls(open_directory, "ODQuery", "NSObject"));
    static_cast<void>(cls(open_directory, "ODRecord", "NSObject"));

    // The C API (CFOpenDirectory) answers the same way: sessions and nodes are NULL with a +1
    // CFError (kODErrorSessionDaemonNotRunning), and no node, query or record object can exist.
    // kODSessionDefault is the default session (NULL), which ODNodeCreate* accept.
    const auto unavailable_c = [this](GuestMemory& m, Address out) {
        if (out) m.write64(out, bridge->error("com.apple.OpenDirectory", 1002, "The Open Directory daemon is not running"));
    };
    value(open_directory, "_kODSessionDefault", 0);
    // ODSessionRef ODSessionCreate(allocator, CFDictionaryRef options, CFErrorRef* error)
    raw(open_directory, "_ODSessionCreate", [unavailable_c](CpuState& s, GuestMemory& m) { unavailable_c(m, s.x[2]); s.x[0] = 0; });
    // CFArrayRef ODSessionCopyNodeNames(allocator, session, CFErrorRef* error)
    raw(open_directory, "_ODSessionCopyNodeNames", [unavailable_c](CpuState& s, GuestMemory& m) { unavailable_c(m, s.x[2]); s.x[0] = 0; });
    // ODNodeRef ODNodeCreateWithNodeType(allocator, session, ODNodeType, CFErrorRef*) / ...WithName(allocator, session, CFStringRef, CFErrorRef*)
    for (const auto* name : {"_ODNodeCreateWithNodeType", "_ODNodeCreateWithName"})
        raw(open_directory, name, [unavailable_c](CpuState& s, GuestMemory& m) { unavailable_c(m, s.x[3]); s.x[0] = 0; });
    for (const auto* name : {"_ODNodeCopyRecord", "_ODNodeCopySubnodeNames", "_ODNodeCopyDetails", "_ODNodeCopySupportedRecordTypes",
                             "_ODNodeCopySupportedAttributes", "_ODNodeGetName", "_ODNodeCreateRecord", "_ODNodeSetCredentials",
                             "_ODQueryCopyResults", "_ODQueryScheduleWithRunLoop", "_ODQueryUnscheduleFromRunLoop", "_ODQuerySetDispatchQueue",
                             "_ODQuerySynchronize", "_ODRecordGetRecordName", "_ODRecordGetRecordType", "_ODRecordCopyValues",
                             "_ODRecordCopyDetails", "_ODRecordVerifyPassword", "_ODRecordContainsMember", "_ODRecordSetValue",
                             "_ODRecordAddValue", "_ODRecordRemoveValue", "_ODRecordDelete", "_ODRecordSynchronize",
                             "_ODRecordChangePassword", "_ODRecordAddMember", "_ODRecordRemoveMember"}) {
        const std::string what(name + 1);
        raw(open_directory, name, [what](CpuState& s, GuestMemory&) {
            if (s.x[0]) throw std::runtime_error("Alchemy: " + what + " received an Open Directory object that cannot exist under Alchemy");
            s.x[0] = 0;
        });
    }
    // ODQueryCreateWithNode(allocator, node, ...) / ...WithNodeType(allocator, ODNodeType, ...)
    raw(open_directory, "_ODQueryCreateWithNode", [](CpuState& s, GuestMemory&) {
        if (s.x[1]) throw std::runtime_error("Alchemy: ODQueryCreateWithNode received an Open Directory node that cannot exist under Alchemy");
        s.x[0] = 0;
    });
    // ODQueryCreateWithNodeType(allocator, nodeType, recordTypes, attribute, matchType, queryValues,
    // returnAttributes, maxResults, CFErrorRef* error): the error is the ninth argument (the stack).
    raw(open_directory, "_ODQueryCreateWithNodeType", [unavailable_c](CpuState& s, GuestMemory& m) {
        unavailable_c(m, m.read64(s.sp));
        s.x[0] = 0;
    });
}

} // namespace

void install_opendirectory_sessions(InstallContext& context) { std::make_shared<Sessions>(context)->install(); }

} // namespace alchemy::gui::fill
