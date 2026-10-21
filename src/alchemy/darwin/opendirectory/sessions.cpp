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
}

} // namespace

void install_opendirectory_sessions(InstallContext& context) { std::make_shared<Sessions>(context)->install(); }

} // namespace alchemy::gui::fill
