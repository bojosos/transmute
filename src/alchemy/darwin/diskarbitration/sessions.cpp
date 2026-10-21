#include "alchemy/darwin/kit/completion/completion_kit.hpp"

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

// DiskArbitration as a local arbiter of the guest's volumes (QLab checks its
// license against the mounted volumes and registers eject/unmount approval):
// macOS always has the Disk Arbitration daemon, so applications use its
// sessions without checking for NULL. Installed before the strict-mode fill
// provider (src/alchemy/devices, which answers "no daemon"), so these win;
// the kDADiskDescription* key constants come from that provider.
//   DASessionCreate / DAApprovalSessionCreate  session objects.
//   DADiskCreateFromVolumePath                 a disk for "/" or an existing
//                                              /Volumes/<name>; NULL otherwise.
//   DADiskCopyDescription                      DAVolumePath (URL), DAVolumeName,
//       DAVolumeMountable, DAMediaWritable, DAMediaRemovable (NO),
//       DAMediaEjectable (NO), DADeviceInternal (YES), DADeviceProtocol
//       ("Virtual Interface"), DAMediaBSDName ("disk1s<n>"), DADeviceModel and
//       DADeviceVendor ("Alchemy"), DAVolumeUUID / DAMediaUUID (stable, from the
//       path), DAMediaSize (guest statfs), DAVolumeKind ("apfs"),
//       DAVolumeNetwork (NO).
//   DADiskCopyWholeDisk                        the disk itself (no partition map).
//   DADiskCopyIOMedia                          IO_OBJECT_NULL (no IOKit media).
//   Approval callbacks and run-loop scheduling are recorded; no volume is
//   ever ejected or unmounted, so they are never called. DADissenterCreate
//   makes a dissenter object (status and string kept).
namespace alchemy::gui::completion_kit {
namespace {

constexpr std::string_view disk_arbitration_library = "/System/Library/Frameworks/DiskArbitration.framework/Versions/A/DiskArbitration";

struct DiskState final : ObjectPayload { std::string path; std::size_t index{}; };
struct DissenterState final : ObjectPayload { std::int64_t status{}; Address text{}; };

} // namespace

void install_disk_arbitration(Support& support) {
    auto* sp = &support;
    // The kDADiskDescription*Key constants (DiskArbitration's values).
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"VolumeKind", "DAVolumeKind"}, {"VolumeMountable", "DAVolumeMountable"}, {"VolumeName", "DAVolumeName"},
             {"VolumeNetwork", "DAVolumeNetwork"}, {"VolumePath", "DAVolumePath"}, {"VolumeType", "DAVolumeType"},
             {"VolumeUUID", "DAVolumeUUID"}, {"MediaBlockSize", "DAMediaBlockSize"}, {"MediaBSDMajor", "DAMediaBSDMajor"},
             {"MediaBSDMinor", "DAMediaBSDMinor"}, {"MediaBSDName", "DAMediaBSDName"}, {"MediaBSDUnit", "DAMediaBSDUnit"},
             {"MediaContent", "DAMediaContent"}, {"MediaEjectable", "DAMediaEjectable"}, {"MediaIcon", "DAMediaIcon"},
             {"MediaKind", "DAMediaKind"}, {"MediaLeaf", "DAMediaLeaf"}, {"MediaName", "DAMediaName"}, {"MediaPath", "DAMediaPath"},
             {"MediaRemovable", "DAMediaRemovable"}, {"MediaSize", "DAMediaSize"}, {"MediaType", "DAMediaType"},
             {"MediaUUID", "DAMediaUUID"}, {"MediaWhole", "DAMediaWhole"}, {"MediaWritable", "DAMediaWritable"},
             {"DeviceGUID", "DADeviceGUID"}, {"DeviceInternal", "DADeviceInternal"}, {"DeviceModel", "DADeviceModel"},
             {"DevicePath", "DADevicePath"}, {"DeviceProtocol", "DADeviceProtocol"}, {"DeviceRevision", "DADeviceRevision"},
             {"DeviceUnit", "DADeviceUnit"}, {"DeviceVendor", "DADeviceVendor"}, {"BusName", "DABusName"}, {"BusPath", "DABusPath"}}) {
        const auto symbol = "_kDADiskDescription" + std::string(name) + "Key";
        if (!support.process.resolve(disk_arbitration_library, symbol)) support.string_constant(disk_arbitration_library, symbol, value);
    }
    const auto session_class = support.declare_class("_AlchemyDASession", "NSObject");
    const auto disk_class = support.declare_class("_AlchemyDADisk", "NSObject");
    const auto dissenter_class = support.declare_class("_AlchemyDADissenter", "NSObject");
    const auto library = disk_arbitration_library;
    for (const auto* name : {"_DASessionCreate", "_DAApprovalSessionCreate"})
        support.function(library, name, [sp, session_class](CpuState& s, GuestMemory&) {
            s.x[0] = sp->create(session_class, std::make_shared<ObjectPayload>());
        });
    // Scheduling and callbacks: recorded nowhere that could fire; there are no events.
    for (const auto* name : {"_DASessionScheduleWithRunLoop", "_DASessionUnscheduleFromRunLoop", "_DASessionSetDispatchQueue",
             "_DAApprovalSessionScheduleWithRunLoop", "_DAApprovalSessionUnscheduleFromRunLoop",
             "_DARegisterDiskEjectApprovalCallback", "_DARegisterDiskUnmountApprovalCallback", "_DARegisterDiskMountApprovalCallback",
             "_DARegisterDiskAppearedCallback", "_DARegisterDiskDisappearedCallback", "_DARegisterDiskDescriptionChangedCallback",
             "_DAUnregisterApprovalCallback", "_DAUnregisterCallback"}) {
        const std::string what(name + 1);
        support.function(library, name, [what](CpuState& s, GuestMemory&) {
            if (!s.x[0]) fail(what + " with a NULL session");
        });
    }
    auto next_index = std::make_shared<std::size_t>(1);
    support.function(library, "_DADiskCreateFromVolumePath", [sp, disk_class, next_index](CpuState& s, GuestMemory&) {
        if (!s.x[1]) fail("DADiskCreateFromVolumePath with a NULL session");
        s.x[0] = 0;
        if (!s.x[2]) return;
        auto path = sp->cf.url_path(s.x[2]);
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        const bool candidate = path == "/" || (path.starts_with("/Volumes/") && path.find('/', 9) == std::string::npos && path.size() > 9);
        if (!candidate) return;
        const auto manager = sp->send_id(sp->objc.find_class("NSFileManager"), "defaultManager");
        const auto text = sp->send_id(sp->cf.string(path), "autorelease");
        if (!(sp->send_id(manager, "fileExistsAtPath:", {text}) & 0xff)) return;
        auto state = std::make_shared<DiskState>();
        state->path = path;
        state->index = path == "/" ? 1 : ++*next_index;
        s.x[0] = sp->create(disk_class, state);
    });
    support.function(library, "_DADiskCreateFromBSDName", [sp, disk_class](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) fail("DADiskCreateFromBSDName with a NULL session");
        std::string name;
        for (auto at = s.x[2]; at && name.size() < 64; ++at) { const auto c = m.read8(at); if (!c) break; name.push_back(static_cast<char>(c)); }
        s.x[0] = 0;
        if (name != "disk1s1" && name != "disk1") return;   // the root volume
        auto state = std::make_shared<DiskState>();
        state->path = "/";
        state->index = 1;
        s.x[0] = sp->create(disk_class, state);
    });
    support.function(library, "_DADiskCopyWholeDisk", [sp](CpuState& s, GuestMemory&) {
        if (!s.x[0]) fail("DADiskCopyWholeDisk(NULL)");
        s.x[0] = sp->retain(s.x[0]);
    });
    support.function(library, "_DADiskCopyIOMedia", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });   // IO_OBJECT_NULL
    support.function(library, "_DADiskGetBSDName", [sp](CpuState& s, GuestMemory&) {
        const auto state = sp->objc.find_host_state<DiskState>(s.x[0]);
        if (!state) fail("DADiskGetBSDName: not a disk");
        static thread_local Address buffer{};
        if (!buffer) buffer = sp->process.allocate_guest(32);
        const auto name = "disk1s" + std::to_string(state->index);
        sp->memory().write_bytes(buffer, std::span(reinterpret_cast<const std::uint8_t*>(name.c_str()), name.size() + 1));
        s.x[0] = buffer;
    });
    support.function(library, "_DADiskCopyDescription", [sp](CpuState& s, GuestMemory&) {
        const auto state = sp->objc.find_host_state<DiskState>(s.x[0]);
        if (!state) fail("DADiskCopyDescription: not a disk");
        auto& cf = sp->cf;
        const auto key = [&cf](const char* text) { return cf.constant_string(text); };
        const auto name = state->path == "/" ? std::string("Macintosh HD") : state->path.substr(9);
        std::vector<std::pair<Address, Address>> entries{
            {key("DAVolumePath"), cf.file_url(state->path, true)},
            {key("DAVolumeName"), cf.string(name)},
            {key("DAVolumeMountable"), cf.boolean(true)},
            {key("DAMediaWritable"), cf.boolean(true)},
            {key("DAMediaRemovable"), cf.boolean(false)},
            {key("DAMediaEjectable"), cf.boolean(false)},
            {key("DADeviceInternal"), cf.boolean(true)},
            {key("DADeviceProtocol"), cf.string("Virtual Interface")},
            {key("DAMediaBSDName"), cf.string("disk1s" + std::to_string(state->index))},
            {key("DADeviceModel"), cf.string("Alchemy")},
            {key("DADeviceVendor"), cf.string("Alchemy")},
            {key("DADevicePath"), cf.string("IODeviceTree:/alchemy/volume@" + std::to_string(state->index))},
            {key("DAMediaName"), cf.string(name)},
            {key("DAMediaWhole"), cf.boolean(true)},
            {key("DAMediaLeaf"), cf.boolean(true)}};
        // A stable volume UUID from the path (FNV-1a), as an RFC 4122 v4-shaped value.
        std::array<std::uint8_t, 16> bytes{};
        std::uint64_t hash = 1469598103934665603ULL;
        for (int round = 0; round < 2; ++round)
            for (std::size_t i = 0; i < 8; ++i) {
                for (const char c : state->path + static_cast<char>('0' + round)) hash = (hash ^ static_cast<std::uint8_t>(c)) * 1099511628211ULL;
                bytes[static_cast<std::size_t>(round) * 8 + i] = static_cast<std::uint8_t>(hash >> (8 * i));
            }
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
        CallRegs uuid_call;
        for (std::size_t i = 0; i < 8; ++i) {
            uuid_call.x[1] |= static_cast<Address>(bytes[i]) << (8 * i);
            uuid_call.x[2] |= static_cast<Address>(bytes[8 + i]) << (8 * i);
        }
        const auto uuid = sp->call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFUUIDCreateFromUUIDBytes", uuid_call).x0;
        if (!uuid) fail("DADiskCopyDescription: CFUUIDCreateFromUUIDBytes failed");
        entries.emplace_back(key("DAVolumeUUID"), uuid);
        entries.emplace_back(key("DAMediaUUID"), sp->retain(uuid));
        // The volume's size from the guest statfs (f_blocks * f_bsize).
        std::int64_t size = 0;
        const auto info = sp->process.allocate_guest(2176);
        const auto path_text = sp->process.allocate_guest(state->path.size() + 1);
        try {
            sp->memory().write_bytes(path_text, std::span(reinterpret_cast<const std::uint8_t*>(state->path.c_str()), state->path.size() + 1));
            if (static_cast<std::int32_t>(sp->call("/usr/lib/libSystem.B.dylib", "_statfs", {path_text, info})) == 0)
                size = static_cast<std::int64_t>(sp->memory().read64(info + 8) * sp->memory().read32(info));
        } catch (...) { sp->process.release_guest(info); sp->process.release_guest(path_text); throw; }
        sp->process.release_guest(info);
        sp->process.release_guest(path_text);
        entries.emplace_back(key("DAMediaSize"), cf.integer(size));
        entries.emplace_back(key("DAVolumeKind"), cf.string("apfs"));
        entries.emplace_back(key("DAVolumeNetwork"), cf.boolean(false));
        const auto result = cf.dictionary(entries);
        for (const auto& entry : entries) sp->release(entry.second);
        s.x[0] = result;
    });
    support.function(library, "_DADissenterCreate", [sp, dissenter_class](CpuState& s, GuestMemory&) {
        auto state = std::make_shared<DissenterState>();
        state->status = static_cast<std::int32_t>(s.x[1]);
        state->text = s.x[2] ? sp->cf.copy(s.x[2]) : 0;
        s.x[0] = sp->create(dissenter_class, state, [sp, state](Address) { if (state->text) sp->release(state->text); });
    });
    support.function(library, "_DADissenterGetStatus", [sp](CpuState& s, GuestMemory&) {
        const auto state = sp->objc.find_host_state<DissenterState>(s.x[0]);
        if (!state) fail("DADissenterGetStatus: not a dissenter");
        s.x[0] = static_cast<std::uint32_t>(state->status);
    });
    support.function(library, "_DADissenterGetStatusString", [sp](CpuState& s, GuestMemory&) {
        const auto state = sp->objc.find_host_state<DissenterState>(s.x[0]);
        if (!state) fail("DADissenterGetStatusString: not a dissenter");
        s.x[0] = state->text;
    });
}

} // namespace alchemy::gui::completion_kit
