#include "alchemy/darwin/network/network_internal.hpp"
#include "alchemy/darwin/network/mdns.hpp"
#include "alchemy/host/time.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>


// Foundation's Bonjour classes, NSNetService and NSNetServiceBrowser (QLab
// discovers and advertises workspaces with them), over the host mDNS engine
// (mdns.hpp). Defined only when no other provider defines the classes.
//
// Delivery. Each object owns a version-0 CFRunLoopSource (made through the
// guest's CFRunLoopSourceCreate, info = the object, not retained). At init it
// is added to the current run loop in kCFRunLoopDefaultMode, as Foundation
// does; scheduleInRunLoop:forMode: / removeFromRunLoop:forMode: change that.
// Host mDNS callbacks queue events and have the guest pump signal the source
// (and wake the scheduled run loops); the source's perform runs the delegate
// messages on the run loop's thread. The "will" messages
// (netServiceBrowserWillSearch:, netServiceWillPublish:,
// netServiceWillResolve:) are sent synchronously by the starting call.
//
// Behaviour notes (Windows DNS-SD backend):
// - Only the "local." domain exists; domain searches report it once.
// - Resolves report one result (Windows resolves are one-shot); a timeout
//   of 0 means no timeout, as on macOS. -resolve uses 5 s.
// - Publishing keeps the requested name (no conflict renames are reported);
//   NSNetServiceListenForConnections and stream access are not provided
//   (didNotPublish: with NSNetServicesBadArgumentError).
// - setTXTRecordData: on a published service re-registers it.
// - includesPeerToPeer is stored; there is no peer-to-peer transport.
// - TXT keys without a value appear as empty NSData in
//   +dictionaryFromTXTRecordData:.
// Not provided (unrecognized selectors): startMonitoring/stopMonitoring,
// getInputStream:outputStream:, publishWithOptions: listening.
namespace alchemy::gui::network {
namespace {

constexpr std::string_view core = core_foundation_library;
constexpr std::int64_t stream_error_domain_net_services = 10;   // kCFStreamErrorDomainNetServices

namespace ns_error {
inline constexpr std::int64_t unknown = -72000, collision = -72001, not_found = -72002, activity_in_progress = -72003,
    bad_argument = -72004, cancelled = -72005, invalid = -72006, timeout = -72007;
} // namespace ns_error

std::int64_t map_error(std::int32_t error) {
    switch (error) {
    case mdns::error::name_conflict: case mdns::error::already_registered: return ns_error::collision;
    case mdns::error::no_such_name: case mdns::error::no_such_record: return ns_error::not_found;
    case mdns::error::bad_param: case mdns::error::bad_flags: case mdns::error::unsupported: return ns_error::bad_argument;
    case mdns::error::timeout: return ns_error::timeout;
    case mdns::error::invalid: return ns_error::invalid;
    default: return ns_error::unknown;
    }
}

// A queued notification for the run-loop thread.
struct Note {
    enum class Kind { event, stopped, failed, busy, timeout };
    Kind kind{Kind::event};
    mdns::Event event;
    std::int64_t error{};
    std::uint64_t generation{};       // the activity it belongs to (stale notes are dropped)
};

struct Delivery {
    std::mutex mutex;
    std::deque<Note> queue;
    bool signal_requested{};
    std::mutex signal;                // held while the pump signals `source`; cleared under it at dealloc
    Address source{};                 // +1 CFRunLoopSource
    std::vector<std::pair<Address, Address>> schedules;   // (CFRunLoop +1, mode +1)
};

enum class Activity { idle, searching, publishing, published, resolving };

struct NetState final : ObjectPayload {
    std::shared_ptr<Delivery> delivery = std::make_shared<Delivery>();
    std::mutex mutex;
    Address delegate{};
    Activity activity{Activity::idle};
    std::uint64_t generation{};
    std::unique_ptr<mdns::Operation> operation;
    std::shared_ptr<void> lease;
    bool peer_to_peer{};
    // NSNetService
    Address name{}, type{}, domain{}, host{}, addresses{}, txt{};   // +1
    std::int64_t port{-1};
    bool republishing{};
    host::time::Timer timer;                    // the resolve timeout
    // NSNetServiceBrowser: found services by lower-case name (+1 NSNetService).
    std::map<std::string, Address> services;
};

std::string lower(std::string_view text) { return security::lower(text); }

class NetServices : public std::enable_shared_from_this<NetServices> {
public:
    explicit NetServices(std::shared_ptr<Bridge> bridge) : b(std::move(bridge)), pump(guest_pump()) {}
    std::shared_ptr<Bridge> b;
    std::shared_ptr<GuestPump> pump;
    Address service_class{}, browser_class{}, perform{};
    Address error_code_key{}, error_domain_key{};

    std::shared_ptr<NetState> state(Address object) {
        const auto value = object ? b->objc.find_host_state<NetState>(object) : nullptr;
        if (!value) security::fail("expected an initialized NSNetService or NSNetServiceBrowser");
        return value;
    }

    // ---- run-loop delivery ----------------------------------------------------------------------
    void attach(Address object, const std::shared_ptr<NetState>& value) {
        auto& m = b->memory();
        const auto context = b->process.allocate_guest(80);
        const std::array<Address, 10> fields{0, object, 0, 0, 0, 0, 0, 0, 0, perform};
        for (std::size_t index = 0; index < fields.size(); ++index) m.write64(context + index * 8, fields[index]);
        Address source{};
        try { source = b->call(core, "_CFRunLoopSourceCreate", {0, 0, context}); }
        catch (...) { b->process.release_guest(context); throw; }
        b->process.release_guest(context);
        {
            std::lock_guard lock(value->delivery->signal);
            value->delivery->source = source;
        }
        schedule(value, b->call(core, "_CFRunLoopGetCurrent", {}), b->constant(core, "_kCFRunLoopDefaultMode"));
    }

    void schedule(const std::shared_ptr<NetState>& value, Address run_loop, Address mode) {
        if (!run_loop || !mode) return;
        Address source;
        {
            std::lock_guard lock(value->delivery->signal);
            source = value->delivery->source;
            if (!source) return;
            for (const auto& [loop, existing] : value->delivery->schedules)
                if (loop == run_loop && b->call(core, "_CFEqual", {existing, mode})) return;
            value->delivery->schedules.emplace_back(b->retain(run_loop), b->retain(mode));
        }
        static_cast<void>(b->call(core, "_CFRunLoopAddSource", {run_loop, source, mode}));
    }

    void unschedule(const std::shared_ptr<NetState>& value, Address run_loop, Address mode) {
        Address source{};
        std::pair<Address, Address> removed{};
        {
            std::lock_guard lock(value->delivery->signal);
            source = value->delivery->source;
            auto& list = value->delivery->schedules;
            for (auto at = list.begin(); at != list.end(); ++at) {
                if (at->first == run_loop && b->call(core, "_CFEqual", {at->second, mode})) { removed = *at; list.erase(at); break; }
            }
        }
        if (!removed.first) return;
        if (source) static_cast<void>(b->call(core, "_CFRunLoopRemoveSource", {run_loop, source, mode}));
        b->release(removed.first);
        b->release(removed.second);
    }

    // Queues a note and has the pump signal the source (any thread).
    void post(const std::shared_ptr<Delivery>& delivery, Note note) {
        bool signal{};
        {
            std::lock_guard lock(delivery->mutex);
            delivery->queue.push_back(std::move(note));
            if (!delivery->signal_requested) { delivery->signal_requested = true; signal = true; }
        }
        if (!signal) return;
        const std::weak_ptr<Delivery> weak = delivery;
        const auto self = shared_from_this();
        pump->post([self, weak] {
            const auto alive = weak.lock();
            if (!alive) return;
            std::lock_guard lock(alive->signal);
            if (!alive->source) return;
            static_cast<void>(self->b->call(core, "_CFRunLoopSourceSignal", {alive->source}));
            for (const auto& [loop, mode] : alive->schedules) static_cast<void>(self->b->call(core, "_CFRunLoopWakeUp", {loop}));
        });
    }

    mdns::Sink sink(const std::shared_ptr<NetState>& value, std::uint64_t generation) {
        const std::weak_ptr<Delivery> weak = value->delivery;
        const auto self = shared_from_this();
        return [self, weak, generation](mdns::Event event) {
            if (const auto delivery = weak.lock()) self->post(delivery, {Note::Kind::event, std::move(event), 0, generation});
        };
    }

    // Ends the current activity (no more mdns events); returns whether one was running.
    bool end_activity(const std::shared_ptr<NetState>& value) {
        std::unique_ptr<mdns::Operation> operation;
        host::time::Timer timer;
        bool running;
        {
            std::lock_guard lock(value->mutex);
            running = value->activity != Activity::idle;
            value->activity = Activity::idle;
            ++value->generation;
            operation = std::move(value->operation);
            timer = std::exchange(value->timer, host::time::Timer{});
            value->republishing = false;
        }
        if (operation) operation->cancel();
        timer.cancel();
        return running;
    }

    Address error_dictionary(std::int64_t code) {
        const auto number = b->number(code), domain = b->number(stream_error_domain_net_services);
        const auto result = b->dictionary({{error_code_key, number}, {error_domain_key, domain}});
        b->release(number);
        b->release(domain);
        return result;
    }

    void tell(Address delegate, std::string_view selector, Address object) {
        if (!delegate || !b->objc.responds(delegate, b->objc.selector(selector))) return;
        CallRegs registers;
        registers.x[2] = object;
        static_cast<void>(b->objc.send(delegate, selector, registers));
    }
    void tell_error(Address delegate, std::string_view selector, Address object, std::int64_t code) {
        if (!delegate || !b->objc.responds(delegate, b->objc.selector(selector))) return;
        const auto dictionary = error_dictionary(code);
        CallRegs registers;
        registers.x[2] = object;
        registers.x[3] = dictionary;
        try { static_cast<void>(b->objc.send(delegate, selector, registers)); } catch (...) { b->release(dictionary); throw; }
        b->release(dictionary);
    }
    void tell_more(Address delegate, std::string_view selector, Address object, Address item, bool more) {
        if (!delegate || !b->objc.responds(delegate, b->objc.selector(selector))) return;
        CallRegs registers;
        registers.x[2] = object;
        registers.x[3] = item;
        registers.x[4] = more ? 1 : 0;
        static_cast<void>(b->objc.send(delegate, selector, registers));
    }

    // The source's perform (run-loop thread).
    void deliver(Address object) {
        const auto value = object ? b->objc.find_host_state<NetState>(object) : nullptr;
        if (!value) return;
        std::deque<Note> notes;
        {
            std::lock_guard lock(value->delivery->mutex);
            notes.swap(value->delivery->queue);
            value->delivery->signal_requested = false;
        }
        const bool browser = is_browser(object);
        b->retain(object);   // a delegate may release the last reference
        try {
            for (auto& note : notes) {
                if (browser) deliver_browser(object, value, note);
                else deliver_service(object, value, note);
            }
        } catch (...) { b->release(object); throw; }
        b->release(object);
    }

    bool is_browser(Address object) {
        for (auto cls = b->objc.object_class(object); cls; cls = b->objc.send(cls, "superclass").x0)
            if (cls == browser_class) return true;
        return false;
    }

    void deliver_browser(Address object, const std::shared_ptr<NetState>& value, Note& note) {
        Address delegate;
        {
            std::lock_guard lock(value->mutex);
            if (note.generation != value->generation) return;   // from a search that was stopped
            delegate = value->delegate;
        }
        switch (note.kind) {
        case Note::Kind::stopped: tell(delegate, "netServiceBrowserDidStopSearch:", object); return;
        case Note::Kind::failed:
            end_activity(value);
            tell_error(delegate, "netServiceBrowser:didNotSearch:", object, note.error);
            return;
        case Note::Kind::busy: tell_error(delegate, "netServiceBrowser:didNotSearch:", object, note.error); return;
        case Note::Kind::timeout: return;
        case Note::Kind::event: break;
        }
        auto& event = note.event;
        switch (event.kind) {
        case mdns::Event::Kind::domain: {
            const auto domain = b->string(event.name);
            try { tell_more(delegate, "netServiceBrowser:didFindDomain:moreComing:", object, domain, event.more_coming); }
            catch (...) { b->release(domain); throw; }
            b->release(domain);
            return;
        }
        case mdns::Event::Kind::found: {
            const auto key = lower(event.name);
            Address service{};
            {
                std::lock_guard lock(value->mutex);
                if (value->services.contains(key)) return;
            }
            service = make_service(event.domain, event.type, event.name, -1);
            {
                std::lock_guard lock(value->mutex);
                value->services[key] = service;
            }
            tell_more(delegate, "netServiceBrowser:didFindService:moreComing:", object, service, event.more_coming);
            return;
        }
        case mdns::Event::Kind::lost: {
            Address service{};
            {
                std::lock_guard lock(value->mutex);
                const auto found = value->services.find(lower(event.name));
                if (found == value->services.end()) return;
                service = found->second;
                value->services.erase(found);
            }
            try { tell_more(delegate, "netServiceBrowser:didRemoveService:moreComing:", object, service, event.more_coming); }
            catch (...) { b->release(service); throw; }
            b->release(service);
            return;
        }
        case mdns::Event::Kind::failed:
            end_activity(value);
            tell_error(delegate, "netServiceBrowser:didNotSearch:", object, map_error(event.error));
            return;
        default: return;
        }
    }

    void deliver_service(Address object, const std::shared_ptr<NetState>& value, Note& note) {
        Address delegate;
        Activity activity;
        bool republishing;
        {
            std::lock_guard lock(value->mutex);
            if (note.generation != value->generation) return;
            delegate = value->delegate;
            activity = value->activity;
            republishing = value->republishing;
        }
        switch (note.kind) {
        case Note::Kind::stopped: tell(delegate, "netServiceDidStop:", object); return;
        case Note::Kind::failed: {
            const bool publishing = activity == Activity::publishing || activity == Activity::published;
            end_activity(value);
            tell_error(delegate, publishing ? "netService:didNotPublish:" : "netService:didNotResolve:", object, note.error);
            return;
        }
        case Note::Kind::busy: {
            const bool publishing = activity == Activity::publishing || activity == Activity::published;
            tell_error(delegate, publishing ? "netService:didNotPublish:" : "netService:didNotResolve:", object, note.error);
            return;
        }
        case Note::Kind::timeout:
            if (activity != Activity::resolving) return;
            end_activity(value);
            tell_error(delegate, "netService:didNotResolve:", object, ns_error::timeout);
            return;
        case Note::Kind::event: break;
        }
        auto& event = note.event;
        switch (event.kind) {
        case mdns::Event::Kind::registered: {
            {
                std::lock_guard lock(value->mutex);
                value->activity = Activity::published;
                value->republishing = false;
            }
            if (republishing) return;
            set_string(value, &NetState::name, event.name);
            tell(delegate, "netServiceDidPublish:", object);
            return;
        }
        case mdns::Event::Kind::resolved: {
            std::vector<Address> datas;
            for (const auto& address : event.addresses) datas.push_back(b->data(address));
            const auto addresses = b->array(datas);
            b->release_all(datas);
            const auto host = b->string(event.host);
            const auto txt = b->data(event.txt);
            Address old_host, old_addresses, old_txt;
            {
                std::lock_guard lock(value->mutex);
                value->activity = Activity::idle;   // one result per resolve
                old_host = std::exchange(value->host, host);
                old_addresses = std::exchange(value->addresses, addresses);
                old_txt = std::exchange(value->txt, txt);
                value->port = event.port;
            }
            end_activity(value);
            b->release(old_host); b->release(old_addresses); b->release(old_txt);
            tell(delegate, "netServiceDidResolveAddress:", object);
            return;
        }
        case mdns::Event::Kind::failed: {
            const bool publishing = activity == Activity::publishing || activity == Activity::published;
            end_activity(value);
            tell_error(delegate, publishing ? "netService:didNotPublish:" : "netService:didNotResolve:", object, map_error(event.error));
            return;
        }
        default: return;
        }
    }

    void set_string(const std::shared_ptr<NetState>& value, Address NetState::*field, std::string_view text) {
        const auto string = b->string(text);
        Address old;
        {
            std::lock_guard lock(value->mutex);
            old = std::exchange(value.get()->*field, string);
        }
        b->release(old);
    }

    // +1 NSNetService (browse results and -init*).
    Address make_service(std::string_view domain, std::string_view type, std::string_view name, std::int64_t port) {
        const auto object = b->objc.allocate(service_class);
        init_service(object, b->string(domain), b->string(type), b->string(name), port, true);
        return object;
    }

    // Takes the +1 strings when `owned`, retains them otherwise.
    void init_service(Address object, Address domain, Address type, Address name, std::int64_t port, bool owned) {
        auto value = std::make_shared<NetState>();
        value->domain = owned ? domain : b->call(core, "_CFStringCreateCopy", {0, domain});
        value->type = owned ? type : b->call(core, "_CFStringCreateCopy", {0, type});
        value->name = owned ? name : b->call(core, "_CFStringCreateCopy", {0, name});
        value->port = port;
        static_cast<void>(b->objc.host_state<NetState>(object, [&] { return value; }));
        attach(object, value);
    }

    void init_browser(Address object) {
        auto value = std::make_shared<NetState>();
        static_cast<void>(b->objc.host_state<NetState>(object, [&] { return value; }));
        attach(object, value);
    }

    // -dealloc for both classes.
    void destroy(Address object) {
        const auto value = b->objc.find_host_state<NetState>(object);
        if (!value) return;
        end_activity(value);
        Address source{};
        std::vector<std::pair<Address, Address>> schedules;
        {
            std::lock_guard lock(value->delivery->signal);
            source = std::exchange(value->delivery->source, 0);
            schedules.swap(value->delivery->schedules);
        }
        if (source) {
            static_cast<void>(b->call(core, "_CFRunLoopSourceInvalidate", {source}));
            b->release(source);
        }
        for (const auto& [loop, mode] : schedules) { b->release(loop); b->release(mode); }
        std::vector<Address> owned;
        {
            std::lock_guard lock(value->mutex);
            for (auto* field : {&value->name, &value->type, &value->domain, &value->host, &value->addresses, &value->txt})
                if (*field) owned.push_back(std::exchange(*field, 0));
            for (const auto& [key, service] : value->services) owned.push_back(service);
            value->services.clear();
            value->lease.reset();
        }
        b->release_all(owned);
    }

    // Starts an activity: `start` makes the operation (guest thread).
    void begin(Address object, const std::shared_ptr<NetState>& value, Activity activity,
               const std::function<std::unique_ptr<mdns::Operation>(mdns::Sink, std::int32_t&)>& start,
               std::string_view will, double timeout = 0) {
        Address delegate;
        std::uint64_t generation;
        {
            std::lock_guard lock(value->mutex);
            delegate = value->delegate;
            if (value->activity != Activity::idle) {
                generation = value->generation;
                post(value->delivery, {Note::Kind::busy, {}, ns_error::activity_in_progress, generation});
                return;
            }
            generation = ++value->generation;
            value->activity = activity;
            if (!value->lease) value->lease = pump->lease();
        }
        tell(delegate, will, object);
        std::int32_t status{};
        auto operation = start(sink(value, generation), status);
        if (!operation) {
            post(value->delivery, {Note::Kind::failed, {}, map_error(status), generation});
            return;
        }
        std::lock_guard lock(value->mutex);
        if (value->generation != generation) { operation->cancel(); return; }   // stopped by the will-message
        value->operation = std::move(operation);
        if (timeout > 0) {
            auto timer = host::time::Timer::create([self = weak_from_this(), delivery = std::weak_ptr(value->delivery), generation] {
                const auto owner = self.lock();
                const auto target = delivery.lock();
                if (owner && target) owner->post(target, {Note::Kind::timeout, {}, 0, generation});
            });
            if (timer && timer->schedule(static_cast<std::uint64_t>(timeout * 1e9))) value->timer = std::move(*timer);
        }
    }

    void stop(Address object, bool browser) {
        const auto value = state(object);
        std::uint64_t generation;
        const bool running = end_activity(value);
        {
            std::lock_guard lock(value->mutex);
            generation = value->generation;
        }
        if (running) post(value->delivery, {Note::Kind::stopped, {}, 0, generation});
        static_cast<void>(browser);
    }

    // +1 NSData TXT record for an NSDictionary (NSString -> NSData or NSString); 0 when invalid.
    Address txt_from_dictionary(Address dictionary) {
        if (!dictionary || !b->is_dictionary(dictionary)) return 0;
        const auto count = b->call(core, "_CFDictionaryGetCount", {dictionary});
        const auto keys = b->process.allocate_guest(std::max<std::uint64_t>(count, 1) * 8);
        const auto values = b->process.allocate_guest(std::max<std::uint64_t>(count, 1) * 8);
        static_cast<void>(b->call(core, "_CFDictionaryGetKeysAndValues", {dictionary, keys, values}));
        std::vector<mdns::TxtEntry> entries;
        bool valid = true;
        auto& m = b->memory();
        for (std::uint64_t index = 0; index < count && valid; ++index) {
            const auto key = b->maybe_text(m.read64(keys + index * 8));
            const auto item = m.read64(values + index * 8);
            if (!key || key->empty() || key->find('=') != std::string::npos) { valid = false; break; }
            mdns::TxtEntry entry{*key, std::nullopt};
            if (b->is_data(item)) entry.value = b->bytes(item);
            else if (const auto text = b->maybe_text(item)) entry.value = Bytes(text->begin(), text->end());
            else { valid = false; break; }
            entries.push_back(std::move(entry));
        }
        b->process.release_guest(keys);
        b->process.release_guest(values);
        if (!valid) return 0;
        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) { return left.key < right.key; });
        return b->data(mdns::build_txt(entries));
    }

    // +1 NSDictionary (NSString -> NSData) for TXT data; 0 when malformed.
    Address dictionary_from_txt(Address data) {
        if (!data || !b->is_data(data)) return 0;
        const auto entries = mdns::parse_txt(b->bytes(data));
        if (!entries) return 0;
        std::vector<std::pair<Address, Address>> pairs;
        std::vector<Address> owned;
        for (const auto& entry : *entries) {
            const auto key = b->string(entry.key);
            const auto value = b->data(entry.value ? *entry.value : Bytes{});
            pairs.emplace_back(key, value);
            owned.push_back(key);
            owned.push_back(value);
        }
        const auto result = b->dictionary(pairs);
        b->release_all(owned);
        return result;
    }
};

} // namespace

void install_net_services(const std::shared_ptr<Bridge>& b) {
    if (b->objc.find_class("NSNetService") || b->exported(foundation, "_OBJC_CLASS_$_NSNetService")) return;
    const auto self = std::make_shared<NetServices>(b);
    static std::shared_ptr<NetServices> keep;
    keep = self;
    self->service_class = b->define_class("NSNetService", b->object_class, foundation);
    self->browser_class = b->define_class("NSNetServiceBrowser", b->object_class, foundation);
    self->error_code_key = b->export_string(foundation, "_NSNetServicesErrorCode", "NSNetServicesErrorCode");
    self->error_domain_key = b->export_string(foundation, "_NSNetServicesErrorDomain", "NSNetServicesErrorDomain");
    self->perform = b->process.install_function("_alchemy_netservice_perform", [self](CpuState& s, GuestMemory&) {
        self->deliver(s.x[0]);
        s.x[0] = 0;
    }, "alchemy.netservice");

    const auto sv = self->service_class, br = self->browser_class;
    const auto value = [self](Address object) { return self->state(object); };

    // ---- shared ------------------------------------------------------------------------------------
    for (const auto cls : {sv, br}) {
        b->method(cls, "dealloc", [self, cls](CpuState& s, GuestMemory&) {
            self->destroy(s.x[0]);
            static_cast<void>(self->b->objc.send_super(s.x[0], cls, self->b->objc.selector("dealloc")));
        }, false, "v@:");
        b->method(cls, "delegate", [value](CpuState& s, GuestMemory&) {
            const auto v = value(s.x[0]);
            std::lock_guard lock(v->mutex);
            s.x[0] = v->delegate;
        });
        b->method(cls, "setDelegate:", [value](CpuState& s, GuestMemory&) {
            const auto v = value(s.x[0]);
            std::lock_guard lock(v->mutex);
            v->delegate = s.x[2];   // assign (not retained), as Foundation does
        }, false, "v@:@");
        b->method(cls, "scheduleInRunLoop:forMode:", [self, value](CpuState& s, GuestMemory&) {
            const auto loop = self->b->objc.send(s.x[2], "getCFRunLoop").x0;
            self->schedule(value(s.x[0]), loop, s.x[3]);
        }, false, "v@:@@");
        b->method(cls, "removeFromRunLoop:forMode:", [self, value](CpuState& s, GuestMemory&) {
            const auto loop = self->b->objc.send(s.x[2], "getCFRunLoop").x0;
            self->unschedule(value(s.x[0]), loop, s.x[3]);
        }, false, "v@:@@");
        b->method(cls, "includesPeerToPeer", [value](CpuState& s, GuestMemory&) {
            const auto v = value(s.x[0]);
            std::lock_guard lock(v->mutex);
            s.x[0] = v->peer_to_peer ? 1 : 0;
        }, false, "B@:");
        b->method(cls, "setIncludesPeerToPeer:", [value](CpuState& s, GuestMemory&) {
            const auto v = value(s.x[0]);
            std::lock_guard lock(v->mutex);
            v->peer_to_peer = (s.x[2] & 0xff) != 0;
        }, false, "v@:B");
    }

    // ---- NSNetServiceBrowser -----------------------------------------------------------------------
    b->method(br, "init", [self](CpuState& s, GuestMemory&) { self->init_browser(s.x[0]); });
    b->method(br, "searchForServicesOfType:inDomain:", [self, value](CpuState& s, GuestMemory&) {
        const auto object = s.x[0];
        const auto v = value(object);
        const auto type = self->b->maybe_text(s.x[2]).value_or("");
        const auto domain = self->b->maybe_text(s.x[3]).value_or("");
        self->begin(object, v, Activity::searching, [&](mdns::Sink sink, std::int32_t& status) -> std::unique_ptr<mdns::Operation> {
            if (!mdns::is_local_domain(domain)) { status = mdns::error::unsupported; return nullptr; }
            return mdns::browse(type, 0, std::move(sink), status);
        }, "netServiceBrowserWillSearch:");
    }, false, "v@:@@");
    const auto domains = [self, value](CpuState& s, GuestMemory&) {
        const auto object = s.x[0];
        self->begin(object, value(object), Activity::searching, [](mdns::Sink sink, std::int32_t& status) {
            return mdns::browse_domains(std::move(sink), status);
        }, "netServiceBrowserWillSearch:");
    };
    b->method(br, "searchForBrowsableDomains", domains, false, "v@:");
    b->method(br, "searchForRegistrationDomains", domains, false, "v@:");
    b->method(br, "stop", [self](CpuState& s, GuestMemory&) { self->stop(s.x[0], true); }, false, "v@:");

    // ---- NSNetService ------------------------------------------------------------------------------
    // initWithDomain:type:name:port: (publishing) / initWithDomain:type:name: (resolving).
    b->method(sv, "initWithDomain:type:name:port:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3] || !s.x[4]) security::fail("-[NSNetService initWithDomain:type:name:port:] requires non-nil strings");
        self->init_service(s.x[0], s.x[2], s.x[3], s.x[4], static_cast<std::int32_t>(s.x[5]), false);
    }, false, "@@:@@@i");
    b->method(sv, "initWithDomain:type:name:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3] || !s.x[4]) security::fail("-[NSNetService initWithDomain:type:name:] requires non-nil strings");
        self->init_service(s.x[0], s.x[2], s.x[3], s.x[4], -1, false);
    }, false, "@@:@@@");
    const auto getter = [value](Address NetState::*field) {
        return [value, field](CpuState& s, GuestMemory&) {
            const auto v = value(s.x[0]);
            std::lock_guard lock(v->mutex);
            s.x[0] = v.get()->*field;
        };
    };
    b->method(sv, "name", getter(&NetState::name));
    b->method(sv, "type", getter(&NetState::type));
    b->method(sv, "domain", getter(&NetState::domain));
    b->method(sv, "hostName", getter(&NetState::host));
    b->method(sv, "addresses", getter(&NetState::addresses));
    b->method(sv, "TXTRecordData", getter(&NetState::txt));
    b->method(sv, "port", [value](CpuState& s, GuestMemory&) {
        const auto v = value(s.x[0]);
        std::lock_guard lock(v->mutex);
        s.x[0] = static_cast<std::uint64_t>(v->port);
    }, false, "q@:");
    b->method(sv, "setTXTRecordData:", [self, value](CpuState& s, GuestMemory&) {
        const auto object = s.x[0];
        const auto v = value(object);
        if (s.x[2] && !self->b->is_data(s.x[2])) { s.x[0] = 0; return; }
        const auto data = s.x[2] ? self->b->call(core, "_CFDataCreateCopy", {0, s.x[2]}) : 0;
        Address old;
        bool republish;
        {
            std::lock_guard lock(v->mutex);
            old = std::exchange(v->txt, data);
            republish = v->activity == Activity::published;
        }
        self->b->release(old);
        s.x[0] = 1;
        if (!republish) return;
        // Windows cannot update a registered TXT record: withdraw and register again.
        std::string name, type;
        std::int64_t port;
        std::uint64_t generation;
        std::unique_ptr<mdns::Operation> previous;
        {
            std::lock_guard lock(v->mutex);
            name = self->b->text(v->name);
            type = self->b->text(v->type);
            port = v->port;
            generation = v->generation;
            previous = std::move(v->operation);
            v->republishing = true;
        }
        if (previous) previous->cancel();
        const auto txt = data ? self->b->bytes(data) : Bytes{};
        std::int32_t status{};
        auto operation = mdns::register_service(name, type, static_cast<std::uint16_t>(port), txt, 0, self->sink(v, generation), status);
        std::lock_guard lock(v->mutex);
        if (operation && v->generation == generation) v->operation = std::move(operation);
        else if (!operation) self->post(v->delivery, {Note::Kind::failed, {}, map_error(status), generation});
    }, false, "B@:@");
    const auto publish = [self, value](Address object, std::uint64_t options) {
        const auto v = value(object);
        std::string name, type, domain;
        std::int64_t port;
        Bytes txt;
        {
            std::lock_guard lock(v->mutex);
            name = self->b->text(v->name);
            type = self->b->text(v->type);
            domain = self->b->text(v->domain);
            port = v->port;
            if (v->txt) txt = self->b->bytes(v->txt);
        }
        self->begin(object, v, Activity::publishing, [&](mdns::Sink sink, std::int32_t& status) -> std::unique_ptr<mdns::Operation> {
            // NSNetServiceListenForConnections (2) needs NSStream listening, which is not provided.
            if (port < 0 || port > 0xffff || (options & 2) || !mdns::is_local_domain(domain)) { status = mdns::error::bad_param; return nullptr; }
            return mdns::register_service(name, type, static_cast<std::uint16_t>(port), txt, 0, std::move(sink), status);
        }, "netServiceWillPublish:");
    };
    b->method(sv, "publish", [publish](CpuState& s, GuestMemory&) { publish(s.x[0], 0); }, false, "v@:");
    b->method(sv, "publishWithOptions:", [publish](CpuState& s, GuestMemory&) { publish(s.x[0], s.x[2]); }, false, "v@:Q");
    const auto resolve = [self, value](Address object, double timeout) {
        const auto v = value(object);
        std::string name, type, domain;
        {
            std::lock_guard lock(v->mutex);
            name = self->b->text(v->name);
            type = self->b->text(v->type);
            domain = self->b->text(v->domain);
        }
        self->begin(object, v, Activity::resolving, [&](mdns::Sink sink, std::int32_t& status) -> std::unique_ptr<mdns::Operation> {
            if (!mdns::is_local_domain(domain)) { status = mdns::error::unsupported; return nullptr; }
            return mdns::resolve(name, type, 0, std::move(sink), status);
        }, "netServiceWillResolve:", timeout);
    };
    b->method(sv, "resolve", [resolve](CpuState& s, GuestMemory&) { resolve(s.x[0], 5.0); }, false, "v@:");
    b->method(sv, "resolveWithTimeout:", [resolve](CpuState& s, GuestMemory&) {
        const auto timeout = std::bit_cast<double>(s.v[0][0]);
        resolve(s.x[0], std::isfinite(timeout) && timeout > 0 ? timeout : 0);
    }, false, "v@:d");
    b->method(sv, "stop", [self](CpuState& s, GuestMemory&) { self->stop(s.x[0], false); }, false, "v@:");
    b->method(sv, "isEqual:", [self, sv](CpuState& s, GuestMemory&) {
        const auto left = s.x[0], right = s.x[2];
        if (left == right) { s.x[0] = 1; return; }
        const auto a = self->b->objc.find_host_state<NetState>(left);
        const auto other = right ? self->b->objc.find_host_state<NetState>(right) : nullptr;
        if (!a || !other || self->is_browser(right)) { s.x[0] = 0; return; }
        std::string la, lb;
        const auto key = [self](const std::shared_ptr<NetState>& v) {
            std::lock_guard lock(v->mutex);
            return lower(self->b->text(v->name)) + "|" + lower(self->b->text(v->type)) + "|" + lower(self->b->text(v->domain));
        };
        s.x[0] = key(a) == key(other) ? 1 : 0;
        static_cast<void>(sv);
    }, false, "B@:@");
    b->method(sv, "hash", [self, value](CpuState& s, GuestMemory&) {
        const auto v = value(s.x[0]);
        std::string text;
        {
            std::lock_guard lock(v->mutex);
            text = lower(self->b->text(v->name)) + "|" + lower(self->b->text(v->type));
        }
        s.x[0] = std::hash<std::string>{}(text);
    }, false, "Q@:");
    b->method(sv, "dataFromTXTRecordDictionary:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->b->autorelease(self->txt_from_dictionary(s.x[2]));
    }, true, "@@:@");
    b->method(sv, "dictionaryFromTXTRecordData:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->b->autorelease(self->dictionary_from_txt(s.x[2]));
    }, true, "@@:@");
}

} // namespace alchemy::gui::network
