#include "alchemy/darwin/kit/completion/completion_kit.hpp"
#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/gui/fill_provider.hpp"

#include <array>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

// DiskArbitration as a local arbiter of the guest's volumes (QLab checks its
// license against the mounted volumes and registers eject/unmount approval):
// macOS always has the Disk Arbitration daemon, so applications use its
// sessions without checking for NULL. Installed before the strict-mode fill
// provider (darwin/iokit/iokit.cpp, which adds two constants), so these win.
//   Sessions                  DASessionCreate / DAApprovalSessionCreate; scheduled on run loops
//                             (their callbacks run as CFRunLoopPerformBlock work) or a dispatch
//                             queue. Work for a session that is not scheduled waits until it is.
//   Volumes                   "/" and each existing /Volumes/<name>, with a stable BSD name
//                             (disk1s<n>) per path. Once a session is scheduled, each peek and
//                             disk-appeared callback receives the volumes its match dictionary
//                             selects (a later registration receives them then), and the idle
//                             callbacks run after them. No volume appears, disappears or changes
//                             later, so disappeared and description-changed callbacks never run.
//   DADiskCopyDescription     DAVolumePath (URL), DAVolumeName, DAVolumeMountable, DAMediaWritable,
//                             DAMediaRemovable (NO), DAMediaEjectable (NO), DADeviceInternal (YES),
//                             DADeviceProtocol ("Virtual Interface"), DAMediaBSDName, DADeviceModel and
//                             DADeviceVendor ("Alchemy"), DAVolumeUUID / DAMediaUUID (stable, from the
//                             path), DAMediaSize (guest statfs), DAVolumeKind ("apfs"), DAVolumeNetwork (NO).
//   Requests                  the volumes are host volumes Alchemy maps, so DADiskMount answers
//                             kDAReturnBusy (already mounted), DADiskUnmount and DADiskRename
//                             kDAReturnNotPermitted and DADiskEject kDAReturnUnsupported (no media
//                             is ejectable), each as a dissenter given to the request's callback on
//                             the disk's session. Approval callbacks are recorded; since no request
//                             proceeds they never run.
//   DADiskCopyWholeDisk       the disk itself (no partition map); DADiskCopyIOMedia IO_OBJECT_NULL;
//                             DADiskCreateFromIOMedia NULL (no IOKit media object exists).
//   DADiskGetOptions/SetOptions  kept per disk.
namespace alchemy::gui::completion_kit {
namespace {

constexpr std::string_view disk_arbitration_library = "/System/Library/Frameworks/DiskArbitration.framework/Versions/A/DiskArbitration";
constexpr std::string_view cf_library = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

// DAReturn (err_local | err_local_diskarbitration | code).
constexpr std::uint32_t da_return_success = 0, da_return_busy = 0xf8da0002, da_return_bad_argument = 0xf8da0003,
    da_return_not_permitted = 0xf8da0008, da_return_unsupported = 0xf8da000c;

// The callback kinds a session keeps.
enum class Kind { appeared, disappeared, description_changed, peek, idle, mount_approval, unmount_approval, eject_approval };

struct Callback { Kind kind{}; Address function{}, context{}, match{}; };   // match retained

struct SessionState final : ObjectPayload {
    std::mutex mutex;
    Address queue{};                                  // retained
    std::vector<std::pair<Address, Address>> loops;   // (run loop, mode), retained
    std::vector<Callback> callbacks;
    std::vector<std::function<void()>> pending;       // work waiting for scheduling
    std::vector<std::function<void()>> loop_work;     // work for the run loop's next perform
    Address loop_block{};                             // the block CFRunLoopPerformBlock runs (drains loop_work)
};

struct DiskState final : ObjectPayload {
    std::string path;
    std::size_t index{};
    Address session{};            // retained
    std::uint32_t options{};
};
struct DissenterState final : ObjectPayload { std::int64_t status{}; Address text{}; };

class Arbiter : public std::enable_shared_from_this<Arbiter> {
public:
    Arbiter(Support& support, Address session_class, Address disk_class, Address dissenter_class)
        : sp(support), session_class(session_class), disk_class(disk_class), dissenter_class(dissenter_class), bridge(security::bridge()) {}
    Support& sp;
    Address session_class, disk_class, dissenter_class;
    std::shared_ptr<security::Bridge> bridge;
    std::mutex mutex;
    std::map<std::string, std::size_t> indices{{"/", 1}};   // stable BSD unit per volume path

    std::shared_ptr<SessionState> session(Address object, std::string_view what) {
        const auto state = object ? sp.objc.find_host_state<SessionState>(object) : nullptr;
        if (!state) fail(std::string(what) + " needs a DiskArbitration session");
        return state;
    }
    std::shared_ptr<DiskState> disk(Address object, std::string_view what) {
        const auto state = object ? sp.objc.find_host_state<DiskState>(object) : nullptr;
        if (!state) fail(std::string(what) + " needs a disk");
        return state;
    }
    // A disk object for `path` on `session` (+1).
    Address make_disk(const std::string& path, Address session_object) {
        auto state = std::make_shared<DiskState>();
        state->path = path;
        {
            std::lock_guard lock(mutex);
            const auto found = indices.find(path);
            state->index = found != indices.end() ? found->second : (indices[path] = indices.size() + 1);
        }
        state->session = sp.retain(session_object);
        return sp.create(disk_class, state, [this, state](Address) { sp.release(std::exchange(state->session, 0)); });
    }
    bool volume_exists(const std::string& path) {
        const auto manager = sp.send_id(sp.objc.find_class("NSFileManager"), "defaultManager");
        const auto text = sp.send_id(sp.cf.string(path), "autorelease");
        return (sp.send_id(manager, "fileExistsAtPath:", {text}) & 0xff) != 0;
    }
    // "/" and each /Volumes/<name> that exists.
    std::vector<std::string> volumes() {
        std::vector<std::string> paths{"/"};
        const auto manager = sp.send_id(sp.objc.find_class("NSFileManager"), "defaultManager");
        const auto directory = sp.send_id(sp.cf.string("/Volumes"), "autorelease");
        const auto names = sp.send_id(manager, "contentsOfDirectoryAtPath:error:", {directory, 0});
        if (names)
            for (const auto name : sp.cf.array_values(names)) {
                const auto text = sp.cf.utf8(name);
                if (!text.empty() && text.front() != '.' && volume_exists("/Volumes/" + text)) paths.push_back("/Volumes/" + text);
            }
        return paths;
    }
    // DADiskCopyDescription's dictionary (+1).
    Address describe(const DiskState& state) {
        auto& cf = sp.cf;
        const auto key = [&cf](const char* text) { return cf.constant_string(text); };
        const auto name = state.path == "/" ? std::string("Macintosh HD") : state.path.substr(9);
        std::vector<std::pair<Address, Address>> entries{
            {key("DAVolumePath"), cf.file_url(state.path, true)},
            {key("DAVolumeName"), cf.string(name)},
            {key("DAVolumeMountable"), cf.boolean(true)},
            {key("DAMediaWritable"), cf.boolean(true)},
            {key("DAMediaRemovable"), cf.boolean(false)},
            {key("DAMediaEjectable"), cf.boolean(false)},
            {key("DADeviceInternal"), cf.boolean(true)},
            {key("DADeviceProtocol"), cf.string("Virtual Interface")},
            {key("DAMediaBSDName"), cf.string("disk1s" + std::to_string(state.index))},
            {key("DADeviceModel"), cf.string("Alchemy")},
            {key("DADeviceVendor"), cf.string("Alchemy")},
            {key("DADevicePath"), cf.string("IODeviceTree:/alchemy/volume@" + std::to_string(state.index))},
            {key("DAMediaName"), cf.string(name)},
            {key("DAMediaWhole"), cf.boolean(true)},
            {key("DAMediaLeaf"), cf.boolean(true)}};
        // A stable volume UUID from the path (FNV-1a), as an RFC 4122 v4-shaped value.
        std::array<std::uint8_t, 16> bytes{};
        std::uint64_t hash = 1469598103934665603ULL;
        for (int round = 0; round < 2; ++round)
            for (std::size_t i = 0; i < 8; ++i) {
                for (const char c : state.path + static_cast<char>('0' + round)) hash = (hash ^ static_cast<std::uint8_t>(c)) * 1099511628211ULL;
                bytes[static_cast<std::size_t>(round) * 8 + i] = static_cast<std::uint8_t>(hash >> (8 * i));
            }
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
        CallRegs uuid_call;
        for (std::size_t i = 0; i < 8; ++i) {
            uuid_call.x[1] |= static_cast<Address>(bytes[i]) << (8 * i);
            uuid_call.x[2] |= static_cast<Address>(bytes[8 + i]) << (8 * i);
        }
        const auto uuid = sp.call(cf_library, "_CFUUIDCreateFromUUIDBytes", uuid_call).x0;
        if (!uuid) fail("DADiskCopyDescription: CFUUIDCreateFromUUIDBytes failed");
        entries.emplace_back(key("DAVolumeUUID"), uuid);
        entries.emplace_back(key("DAMediaUUID"), sp.retain(uuid));
        // The volume's size from the guest statfs (f_blocks * f_bsize).
        std::int64_t size = 0;
        const auto info = sp.process.allocate_guest(2176);
        const auto path_text = sp.process.allocate_guest(state.path.size() + 1);
        try {
            sp.memory().write_bytes(path_text, std::span(reinterpret_cast<const std::uint8_t*>(state.path.c_str()), state.path.size() + 1));
            if (static_cast<std::int32_t>(sp.call("/usr/lib/libSystem.B.dylib", "_statfs", {path_text, info})) == 0)
                size = static_cast<std::int64_t>(sp.memory().read64(info + 8) * sp.memory().read32(info));
        } catch (...) { sp.process.release_guest(info); sp.process.release_guest(path_text); throw; }
        sp.process.release_guest(info);
        sp.process.release_guest(path_text);
        entries.emplace_back(key("DAMediaSize"), cf.integer(size));
        entries.emplace_back(key("DAVolumeKind"), cf.string("apfs"));
        entries.emplace_back(key("DAVolumeNetwork"), cf.boolean(false));
        const auto result = cf.dictionary(entries);
        for (const auto& entry : entries) sp.release(entry.second);
        return result;
    }
    // Whether a disk's description has every key of `match` (NULL matches every disk).
    bool matches(const DiskState& state, Address match) {
        if (!match) return true;
        const auto description = describe(state);
        bool ok = true;
        for (const auto& [key, expected] : sp.cf.dictionary_entries(match)) {
            const std::array<Address, 2> lookup{description, key};
            const auto own = sp.cf.invoke("CFDictionaryGetValue", lookup);
            if (!own || !sp.cf.equal(own, expected)) { ok = false; break; }
        }
        sp.release(description);
        return ok;
    }
    Address dissenter(std::uint32_t status, std::string_view text) {
        auto state = std::make_shared<DissenterState>();
        state->status = static_cast<std::int32_t>(status);
        state->text = sp.cf.string(text);
        return sp.create(dissenter_class, state, [this, state](Address) { sp.release(state->text); });
    }

    // Runs `work` on the session's queue or run loops (or once it is scheduled).
    void deliver(const std::shared_ptr<SessionState>& state, std::function<void()> work) {
        Address queue{};
        std::vector<std::pair<Address, Address>> loops;
        {
            std::lock_guard lock(state->mutex);
            queue = state->queue;
            loops = state->loops;
            if (!queue && loops.empty()) { state->pending.push_back(std::move(work)); return; }
        }
        if (queue) { bridge->dispatch_async(queue, std::move(work)); return; }
        // The first run loop it is scheduled on runs it: the session's block (made once, never
        // dropped) runs the queued work on each CFRunLoopPerformBlock.
        Address block{};
        {
            std::lock_guard lock(state->mutex);
            state->loop_work.push_back(std::move(work));
            if (!state->loop_block) {
                const std::weak_ptr<SessionState> weak = state;
                state->loop_block = bridge->make_block([weak](CpuState& s, GuestMemory&) {
                    s.x[0] = 0;
                    const auto held = weak.lock();
                    if (!held) return;
                    std::vector<std::function<void()>> work;
                    { std::lock_guard inner(held->mutex); work.swap(held->loop_work); }
                    for (auto& item : work) item();
                });
            }
            block = state->loop_block;
        }
        static_cast<void>(sp.call(cf_library, "_CFRunLoopPerformBlock", {loops.front().first, loops.front().second, block}));
        static_cast<void>(sp.call(cf_library, "_CFRunLoopWakeUp", {loops.front().first}));
    }
    // Once a session is scheduled: the work that waited for it.
    void flush(const std::shared_ptr<SessionState>& state) {
        std::vector<std::function<void()>> pending;
        { std::lock_guard lock(state->mutex); pending.swap(state->pending); }
        for (auto& work : pending) deliver(state, std::move(work));
    }
    // The volumes `callbacks` select, as peek then appeared calls, then the session's idle callbacks.
    void announce(Address session_object, const std::shared_ptr<SessionState>& state, std::vector<Callback> callbacks) {
        const auto self = shared_from_this();
        sp.retain(session_object);
        deliver(state, [self, session_object, state, callbacks] {
            std::vector<Address> disks;
            for (const auto& path : self->volumes()) disks.push_back(self->make_disk(path, session_object));
            for (const auto kind : {Kind::peek, Kind::appeared})
                for (const auto& callback : callbacks) {
                    if (callback.kind != kind) continue;
                    for (const auto disk : disks)
                        if (self->matches(*self->sp.objc.find_host_state<DiskState>(disk), callback.match))
                            static_cast<void>(self->bridge->call_address(callback.function, std::array<Address, 2>{disk, callback.context}));
                }
            for (const auto disk : disks) self->sp.release(disk);
            std::vector<Callback> idle;
            { std::lock_guard lock(state->mutex); for (const auto& callback : state->callbacks) if (callback.kind == Kind::idle) idle.push_back(callback); }
            for (const auto& callback : idle) static_cast<void>(self->bridge->call_address(callback.function, std::array<Address, 1>{callback.context}));
            self->sp.release(session_object);
        });
    }
    void schedule(Address session_object, const std::shared_ptr<SessionState>& state) {
        std::vector<Callback> callbacks;
        bool first{};
        {
            std::lock_guard lock(state->mutex);
            first = (state->queue ? 1 : 0) + state->loops.size() == 1;
            callbacks = state->callbacks;
        }
        flush(state);
        if (first) announce(session_object, state, std::move(callbacks));
    }
    void add_callback(Address session_object, Kind kind, Address function, Address context, Address match) {
        const auto state = session(session_object, "DARegister...Callback");
        if (!function) fail("a DiskArbitration callback registration without a callback");
        bool scheduled{};
        {
            std::lock_guard lock(state->mutex);
            state->callbacks.push_back(Callback{kind, function, context, match ? sp.retain(match) : 0});
            scheduled = state->queue || !state->loops.empty();
        }
        // A session that is running announces the volumes to a new peek/appeared callback, and idles.
        if (scheduled && (kind == Kind::appeared || kind == Kind::peek)) announce(session_object, state, {Callback{kind, function, context, match}});
        else if (scheduled && kind == Kind::idle) {
            const auto self = shared_from_this();
            deliver(state, [self, function, context] { static_cast<void>(self->bridge->call_address(function, std::array<Address, 1>{context})); });
        }
    }
    void remove_callback(Address session_object, bool approval, Address function, Address context) {
        const auto state = session(session_object, approval ? "DAUnregisterApprovalCallback" : "DAUnregisterCallback");
        std::vector<Address> matches;
        {
            std::lock_guard lock(state->mutex);
            std::erase_if(state->callbacks, [&](const Callback& callback) {
                const bool kind = callback.kind == Kind::mount_approval || callback.kind == Kind::unmount_approval || callback.kind == Kind::eject_approval;
                if (kind != approval || callback.function != function || callback.context != context) return false;
                if (callback.match) matches.push_back(callback.match);
                return true;
            });
        }
        for (const auto match : matches) sp.release(match);
    }
    // A request's answer: callback(disk, dissenter, context) on the disk's session.
    void answer(Address disk_object, Address callback, Address context, std::uint32_t status, std::string text) {
        const auto state = disk(disk_object, "a DiskArbitration request");
        if (!callback) return;
        const auto session_state = session(state->session, "a DiskArbitration request");
        const auto self = shared_from_this();
        sp.retain(disk_object);
        deliver(session_state, [self, disk_object, callback, context, status, text] {
            const auto value = self->dissenter(status, text);
            static_cast<void>(self->bridge->call_address(callback, std::array<Address, 3>{disk_object, value, context}));
            self->sp.release(value);
            self->sp.release(disk_object);
        });
    }
};

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
    const auto arbiter = std::make_shared<Arbiter>(support, session_class, disk_class, dissenter_class);
    const auto bridge = security::bridge();
    bridge->register_type_id(session_class, static_cast<security::TypeId>(fill::TypeId::da_session));
    bridge->register_type_id(disk_class, static_cast<security::TypeId>(fill::TypeId::da_disk));
    bridge->register_type_id(dissenter_class, static_cast<security::TypeId>(fill::TypeId::da_dissenter));
    const auto library = disk_arbitration_library;
    for (const auto& [name, id] : std::initializer_list<std::pair<const char*, fill::TypeId>>{
             {"_DASessionGetTypeID", fill::TypeId::da_session}, {"_DAApprovalSessionGetTypeID", fill::TypeId::da_session},
             {"_DADiskGetTypeID", fill::TypeId::da_disk}, {"_DADissenterGetTypeID", fill::TypeId::da_dissenter}})
        support.function(library, name, [value = static_cast<Address>(id)](CpuState& s, GuestMemory&) { s.x[0] = value; });

    // ---- sessions ----
    for (const auto* name : {"_DASessionCreate", "_DAApprovalSessionCreate"})
        support.function(library, name, [sp, session_class, arbiter](CpuState& s, GuestMemory&) {
            const auto state = std::make_shared<SessionState>();
            s.x[0] = sp->create(session_class, state, [sp, state](Address) {
                std::vector<Address> owned;
                Address queue{};
                {
                    std::lock_guard lock(state->mutex);
                    for (const auto& callback : state->callbacks) if (callback.match) owned.push_back(callback.match);
                    for (const auto& [loop, mode] : state->loops) { owned.push_back(loop); owned.push_back(mode); }
                    queue = std::exchange(state->queue, 0);
                    state->callbacks.clear(); state->loops.clear();
                }
                for (const auto object : owned) sp->release(object);
                if (queue) static_cast<void>(sp->call("/usr/lib/libSystem.B.dylib", "_dispatch_release", {queue}));
            });
        });
    for (const auto* name : {"_DASessionScheduleWithRunLoop", "_DAApprovalSessionScheduleWithRunLoop"})
        support.function(library, name, [sp, arbiter, what = std::string(name + 1)](CpuState& s, GuestMemory&) {
            const auto state = arbiter->session(s.x[0], what);
            if (!s.x[1] || !s.x[2]) fail(what + " without a run loop and mode");
            {
                std::lock_guard lock(state->mutex);
                state->loops.emplace_back(sp->retain(s.x[1]), sp->retain(s.x[2]));
            }
            arbiter->schedule(s.x[0], state);
        });
    for (const auto* name : {"_DASessionUnscheduleFromRunLoop", "_DAApprovalSessionUnscheduleFromRunLoop"})
        support.function(library, name, [sp, arbiter, what = std::string(name + 1)](CpuState& s, GuestMemory&) {
            const auto state = arbiter->session(s.x[0], what);
            std::vector<Address> owned;
            {
                std::lock_guard lock(state->mutex);
                std::erase_if(state->loops, [&](const auto& entry) {
                    if (entry.first != s.x[1] || !sp->cf.equal(entry.second, s.x[2])) return false;
                    owned.push_back(entry.first); owned.push_back(entry.second);
                    return true;
                });
            }
            for (const auto object : owned) sp->release(object);
        });
    // DASessionSetDispatchQueue(session, queue): NULL unschedules.
    support.function(library, "_DASessionSetDispatchQueue", [sp, arbiter](CpuState& s, GuestMemory&) {
        const auto state = arbiter->session(s.x[0], "DASessionSetDispatchQueue");
        if (s.x[1]) static_cast<void>(sp->call("/usr/lib/libSystem.B.dylib", "_dispatch_retain", {s.x[1]}));
        Address previous{};
        { std::lock_guard lock(state->mutex); previous = std::exchange(state->queue, s.x[1]); }
        if (previous) static_cast<void>(sp->call("/usr/lib/libSystem.B.dylib", "_dispatch_release", {previous}));
        if (s.x[1]) arbiter->schedule(s.x[0], state);
    });
    // DARegisterDisk{Appeared,Disappeared,EjectApproval,UnmountApproval,MountApproval}Callback(session, match, callback, context)
    for (const auto& [name, kind] : std::initializer_list<std::pair<const char*, Kind>>{
             {"_DARegisterDiskAppearedCallback", Kind::appeared}, {"_DARegisterDiskDisappearedCallback", Kind::disappeared},
             {"_DARegisterDiskEjectApprovalCallback", Kind::eject_approval}, {"_DARegisterDiskUnmountApprovalCallback", Kind::unmount_approval},
             {"_DARegisterDiskMountApprovalCallback", Kind::mount_approval}})
        support.function(library, name, [arbiter, kind = kind](CpuState& s, GuestMemory&) { arbiter->add_callback(s.x[0], kind, s.x[2], s.x[3], s.x[1]); });
    // DARegisterDiskDescriptionChangedCallback(session, match, watch, callback, context)
    support.function(library, "_DARegisterDiskDescriptionChangedCallback", [arbiter](CpuState& s, GuestMemory&) {
        arbiter->add_callback(s.x[0], Kind::description_changed, s.x[3], s.x[4], s.x[1]);
    });
    // DARegisterDiskPeekCallback(session, match, CFIndex order, callback, context)
    support.function(library, "_DARegisterDiskPeekCallback", [arbiter](CpuState& s, GuestMemory&) {
        arbiter->add_callback(s.x[0], Kind::peek, s.x[3], s.x[4], s.x[1]);
    });
    // DARegisterIdleCallback(session, callback, context)
    support.function(library, "_DARegisterIdleCallback", [arbiter](CpuState& s, GuestMemory&) { arbiter->add_callback(s.x[0], Kind::idle, s.x[1], s.x[2], 0); });
    support.function(library, "_DAUnregisterCallback", [arbiter](CpuState& s, GuestMemory&) { arbiter->remove_callback(s.x[0], false, s.x[1], s.x[2]); });
    support.function(library, "_DAUnregisterApprovalCallback", [arbiter](CpuState& s, GuestMemory&) { arbiter->remove_callback(s.x[0], true, s.x[1], s.x[2]); });

    // ---- disks ----
    support.function(library, "_DADiskCreateFromVolumePath", [sp, arbiter](CpuState& s, GuestMemory&) {
        static_cast<void>(arbiter->session(s.x[1], "DADiskCreateFromVolumePath"));
        s.x[0] = 0;
        if (!s.x[2]) return;
        auto path = sp->cf.url_path(s.x[2]);
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        const bool candidate = path == "/" || (path.starts_with("/Volumes/") && path.find('/', 9) == std::string::npos && path.size() > 9);
        if (!candidate || !arbiter->volume_exists(path)) return;
        s.x[0] = arbiter->make_disk(path, s.x[1]);
    });
    support.function(library, "_DADiskCreateFromBSDName", [arbiter](CpuState& s, GuestMemory& m) {
        static_cast<void>(arbiter->session(s.x[1], "DADiskCreateFromBSDName"));
        std::string name;
        for (auto at = s.x[2]; at && name.size() < 64; ++at) { const auto c = m.read8(at); if (!c) break; name.push_back(static_cast<char>(c)); }
        s.x[0] = 0;
        if (name == "disk1") name = "disk1s1";   // the whole disk is the root volume (no partition map)
        for (const auto& path : arbiter->volumes()) {
            std::size_t index{};
            {
                std::lock_guard lock(arbiter->mutex);
                const auto found = arbiter->indices.find(path);
                index = found != arbiter->indices.end() ? found->second : (arbiter->indices[path] = arbiter->indices.size() + 1);
            }
            if (name == "disk1s" + std::to_string(index)) { s.x[0] = arbiter->make_disk(path, s.x[1]); return; }
        }
    });
    // DADiskCreateFromIOMedia(allocator, session, io_service_t media): no IOKit media object exists.
    support.function(library, "_DADiskCreateFromIOMedia", [arbiter](CpuState& s, GuestMemory&) {
        static_cast<void>(arbiter->session(s.x[1], "DADiskCreateFromIOMedia"));
        s.x[0] = 0;
    });
    support.function(library, "_DADiskCopyWholeDisk", [sp, arbiter](CpuState& s, GuestMemory&) {
        static_cast<void>(arbiter->disk(s.x[0], "DADiskCopyWholeDisk"));
        s.x[0] = sp->retain(s.x[0]);
    });
    support.function(library, "_DADiskCopyIOMedia", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });   // IO_OBJECT_NULL
    support.function(library, "_DADiskGetBSDName", [sp, arbiter](CpuState& s, GuestMemory&) {
        const auto state = arbiter->disk(s.x[0], "DADiskGetBSDName");
        static thread_local Address buffer{};
        if (!buffer) buffer = sp->process.allocate_guest(32);
        const auto name = "disk1s" + std::to_string(state->index);
        sp->memory().write_bytes(buffer, std::span(reinterpret_cast<const std::uint8_t*>(name.c_str()), name.size() + 1));
        s.x[0] = buffer;
    });
    support.function(library, "_DADiskCopyDescription", [arbiter](CpuState& s, GuestMemory&) {
        s.x[0] = arbiter->describe(*arbiter->disk(s.x[0], "DADiskCopyDescription"));
    });
    support.function(library, "_DADiskGetOptions", [arbiter](CpuState& s, GuestMemory&) { s.x[0] = arbiter->disk(s.x[0], "DADiskGetOptions")->options; });
    // DAReturn DADiskSetOptions(disk, DADiskOptions options, Boolean value)
    support.function(library, "_DADiskSetOptions", [arbiter](CpuState& s, GuestMemory&) {
        const auto state = arbiter->disk(s.x[0], "DADiskSetOptions");
        const auto options = static_cast<std::uint32_t>(s.x[1]);
        if (s.x[2] & 0xff) state->options |= options; else state->options &= ~options;
        s.x[0] = da_return_success;
    });
    // ---- requests (each answered on the disk's session) ----
    // DADiskMount(disk, CFURLRef path, options, callback, context) and ...WithArguments(..., arguments)
    for (const auto* name : {"_DADiskMount", "_DADiskMountWithArguments"})
        support.function(library, name, [arbiter](CpuState& s, GuestMemory&) {
            arbiter->answer(s.x[0], s.x[3], s.x[4], da_return_busy, "The volume is already mounted");
        });
    // DADiskUnmount(disk, options, callback, context) / DADiskEject(disk, options, callback, context)
    support.function(library, "_DADiskUnmount", [arbiter](CpuState& s, GuestMemory&) {
        arbiter->answer(s.x[0], s.x[2], s.x[3], da_return_not_permitted, "Alchemy's volumes are host volumes and cannot be unmounted");
    });
    support.function(library, "_DADiskEject", [arbiter](CpuState& s, GuestMemory&) {
        arbiter->answer(s.x[0], s.x[2], s.x[3], da_return_unsupported, "The media is not ejectable");
    });
    // DADiskRename(disk, CFStringRef name, options, callback, context)
    support.function(library, "_DADiskRename", [arbiter](CpuState& s, GuestMemory&) {
        if (!s.x[1]) { arbiter->answer(s.x[0], s.x[3], s.x[4], da_return_bad_argument, "No name"); return; }
        arbiter->answer(s.x[0], s.x[3], s.x[4], da_return_not_permitted, "Alchemy's volumes are host volumes and cannot be renamed");
    });
    // ---- dissenters ----
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
