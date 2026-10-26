#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <map>

// CoreFoundation and libSystem interfaces QLab's images import under those
// identities (the integrator's doctor audit of mac/qlab 53c7857):
//   CFBundleGetVersionNumber   the bundle's CFBundleVersion as a 'vers'-style
//                              number (CoreFoundation's encoding, below)
//   CFFileDescriptor*          descriptors watched for readability and
//                              writability: a dispatch worker polls the
//                              descriptor while callbacks are enabled and signals
//                              the descriptor's run loop source; the callout runs
//                              on the run loop. Callbacks are one-shot, as
//                              documented: a delivered type stays disabled until
//                              CFFileDescriptorEnableCallBacks enables it again.
//   NSURLTagNamesKey           the resource key constant (CoreFoundation and Foundation)
//   _OBJC_CLASS_$_NSCache, NSInputStream, NSInvocation, NSTimer under the
//                              CoreFoundation identity (toll-free bridged
//                              re-exports on macOS) as well as Foundation's
// The libSystem part (_c_locale, fts, warn/err) is darwin/libsystem/misc/fts_err.cpp.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

// ---- CFBundleGetVersionNumber ----
// "MM.m.b<stage>nnn": major (BCD, up to 2 digits) << 24 | minor << 20 |
// bugfix << 16 | stage (development 0x20, alpha 0x40, beta 0x60, final/release
// 0x80) << 8 | non-release revision (0-255). Anything else is 0.
std::uint32_t version_number(std::string_view text) {
    std::size_t at = 0;
    const auto digits = [&](std::size_t max, unsigned& value) {
        const auto start = at;
        value = 0;
        while (at < text.size() && at - start < max && text[at] >= '0' && text[at] <= '9') value = value * 10 + static_cast<unsigned>(text[at++] - '0');
        return at > start;
    };
    unsigned major{}, minor{}, bugfix{}, revision{};
    if (!digits(2, major)) return 0;
    if (at < text.size() && text[at] == '.') { ++at; if (!digits(1, minor)) return 0; }
    if (at < text.size() && text[at] == '.') { ++at; if (!digits(1, bugfix)) return 0; }
    std::uint32_t stage = 0x80;
    if (at < text.size()) {
        switch (text[at]) {
        case 'd': stage = 0x20; break;
        case 'a': stage = 0x40; break;
        case 'b': stage = 0x60; break;
        case 'f': stage = 0x80; break;
        default: return 0;
        }
        ++at;
        if (!digits(3, revision) || revision > 255) return 0;
        if (stage == 0x80 && revision == 0) return 0;   // "f0" is not a final revision
    }
    if (at != text.size()) return 0;
    const auto bcd = ((major / 10) << 4) | (major % 10);
    return (bcd << 24) | (minor << 20) | (bugfix << 16) | (stage << 8) | revision;
}

// ---- CFFileDescriptor ----
constexpr std::uint64_t read_callback = 1, write_callback = 2;

struct Descriptor final : ObjectPayload {
    std::mutex mutex;
    std::int32_t fd{-1};
    bool close_on_invalidate{}, valid{true}, watching{};
    Address callout{}, info{}, retain{}, release{}, description{};
    std::uint64_t enabled{}, pending{};
    Address source{};   // the run loop source (+1), created on demand
};

struct Extras {
    explicit Extras(Support& value) : support(value) {}
    Support& support;
    Address descriptor_class{}, perform_entry{}, watch_entry{};
    std::mutex mutex;
    std::map<Address, std::uint64_t> watch_work;   // worker context -> descriptor object
    Address next_work{1};

    std::shared_ptr<Descriptor> descriptor_of(Address object) {
        auto state = object ? support.objc.find_host_state<Descriptor>(object) : nullptr;
        if (!state) fail("not a CFFileDescriptor");
        return state;
    }
    std::int64_t sys(std::string_view symbol, std::initializer_list<Address> arguments) {
        return static_cast<std::int64_t>(support.call(libsystem, symbol, arguments));
    }
    // Starts the watcher when callbacks are enabled and none is running.
    void watch(Address object) {
        const auto state = descriptor_of(object);
        {
            std::lock_guard lock(state->mutex);
            if (!state->valid || state->watching || !(state->enabled & ~state->pending)) return;
            state->watching = true;
        }
        static_cast<void>(objc_retain(support, object));
        Address id;
        { std::lock_guard lock(mutex); id = next_work++; watch_work[id] = object; }
        const auto queue = support.call(libsystem, "_dispatch_get_global_queue", {0, 0});
        static_cast<void>(support.call(libsystem, "_dispatch_async_f", {queue, id, watch_entry}));
    }
    void watcher(Address id) {
        Address object;
        {
            std::lock_guard lock(mutex);
            const auto found = watch_work.find(id);
            if (found == watch_work.end()) fail("CFFileDescriptor watcher without its descriptor");
            object = found->second;
            watch_work.erase(found);
        }
        const auto state = descriptor_of(object);
        GuestBlock entry(support, 8);
        for (;;) {
            std::uint64_t wanted;
            std::int32_t fd;
            Address source;
            {
                std::lock_guard lock(state->mutex);
                wanted = state->valid ? state->enabled & ~state->pending : 0;
                if (!wanted || support.process.requested_exit()) { state->watching = false; break; }
                fd = state->fd;
                source = state->source;
            }
            const std::uint32_t events = ((wanted & read_callback) ? 0x1u : 0u) | ((wanted & write_callback) ? 0x4u : 0u);   // POLLIN, POLLOUT
            support.memory().write32(entry, static_cast<std::uint32_t>(fd));
            support.memory().write32(entry + 4, events);
            const auto count = sys("_poll", {entry, 1, 50});
            if (count <= 0) continue;
            const auto revents = support.memory().read32(entry + 4) >> 16;
            std::uint64_t ready = 0;
            if (revents & (0x1 | 0x10 | 0x8)) ready |= read_callback;    // POLLIN, POLLHUP, POLLERR: a read does not block
            if (revents & (0x4 | 0x10 | 0x8)) ready |= write_callback;   // POLLOUT
            if (revents & 0x20) { std::lock_guard lock(state->mutex); state->watching = false; break; }   // POLLNVAL
            ready &= wanted;
            if (!ready) continue;
            {
                std::lock_guard lock(state->mutex);
                state->pending |= ready;
                state->enabled &= ~ready;   // one-shot until re-enabled
                source = state->source;
            }
            if (source) static_cast<void>(support.call(core_foundation, "_CFRunLoopSourceSignal", {source}));
        }
        objc_release(support, object);
    }
    // The run loop source's perform: deliver the pending callbacks.
    void perform(Address object) {
        const auto state = descriptor_of(object);
        std::uint64_t types;
        Address callout, info;
        {
            std::lock_guard lock(state->mutex);
            if (!state->valid) return;
            types = std::exchange(state->pending, 0);
            callout = state->callout;
            info = state->info;
        }
        if (types && callout) static_cast<void>(call_address(support, callout, {object, types, info}));
    }
    void invalidate(Address object) {
        const auto state = descriptor_of(object);
        Address source, info, release_callback;
        std::int32_t fd;
        bool close_it;
        {
            std::lock_guard lock(state->mutex);
            if (!state->valid) return;
            state->valid = false;
            source = std::exchange(state->source, 0);
            info = std::exchange(state->info, 0);
            release_callback = state->release;
            fd = state->fd;
            close_it = state->close_on_invalidate;
        }
        if (source) {
            static_cast<void>(support.call(core_foundation, "_CFRunLoopSourceInvalidate", {source}));
            support.release(source);
        }
        if (close_it && fd >= 0) static_cast<void>(sys("_close", {static_cast<Address>(fd)}));
        if (release_callback && info) static_cast<void>(call_address(support, release_callback, {info}));
    }
};

std::shared_ptr<Extras> extras_instance;

} // namespace

/// darwin/libsystem/misc/fts_err.cpp.
void install_fts_err(Support& support);

// The version number encoding (for the unit tests).
std::uint32_t bundle_version_number(std::string_view text) { return version_number(text); }

void install_cf_extras(Support& support) {
    auto self = std::make_shared<Extras>(support);
    extras_instance = self;

    // ---- class identities under CoreFoundation ----
    for (const auto* name : {"NSCache", "NSInputStream", "NSInvocation", "NSTimer"}) {
        const auto cls = support.objc.find_class(name);
        if (!cls) continue;   // a process without that Foundation class (unit fixtures)
        if (support.process.resolve(core_foundation, std::string("_OBJC_CLASS_$_") + name)) continue;   // already published (streams.cpp)
        support.objc.publish_class(name, cls, core_foundation);
    }
    for (const auto library : {std::string_view(core_foundation), std::string_view(foundation_library)})
        if (!support.process.resolve(library, "_NSURLTagNamesKey")) support.string_constant(library, "_NSURLTagNamesKey", "NSURLTagNamesKey");

    // ---- CFBundleGetVersionNumber ----
    support.function(core_foundation, "_CFBundleGetVersionNumber", [&support](CpuState& s, GuestMemory&) {
        const auto version = s.x[0] ? support.send_id(s.x[0], "objectForInfoDictionaryKey:", {string(support, "CFBundleVersion")}) : 0;
        s.x[0] = version && support.cf.kind(version) == ObjectKind::string ? version_number(support.cf.utf8(version)) : 0;
    });

    // ---- CFFileDescriptor ----
    auto cls = support.objc.find_class("__NSCFFileDescriptor");
    if (!cls) cls = support.objc.register_native_class("__NSCFFileDescriptor", class_named(support, "NSObject"), 16);
    self->descriptor_class = cls;
    support.method(cls, "dealloc", [self, cls](CpuState& s, GuestMemory&) {
        if (self->support.objc.find_host_state<Descriptor>(s.x[0])) self->invalidate(s.x[0]);
        static_cast<void>(self->support.objc.send_super(s.x[0], cls, self->support.objc.selector("dealloc")));
    }, "v@:");
    self->watch_entry = support.process.install_function("___alchemy_completion_cffiledescriptor_watch", [self](CpuState& s, GuestMemory&) {
        self->watcher(s.x[0]);
        s.x[0] = 0;
    }, "alchemy.internal");
    self->perform_entry = support.process.install_function("___alchemy_completion_cffiledescriptor_perform", [self](CpuState& s, GuestMemory&) {
        self->perform(s.x[0]);
        s.x[0] = 0;
    }, "alchemy.internal");
    // CFFileDescriptorCreate(allocator, fd, closeOnInvalidate, callout, CFFileDescriptorContext*)
    support.function(core_foundation, "_CFFileDescriptorCreate", [self](CpuState& s, GuestMemory& m) {
        auto state = std::make_shared<Descriptor>();
        state->fd = static_cast<std::int32_t>(s.x[1]);
        state->close_on_invalidate = (s.x[2] & 0xff) != 0;
        state->callout = s.x[3];
        if (s.x[4]) {   // { version, info, retain, release, copyDescription }
            state->retain = m.read64(s.x[4] + 16);
            state->release = m.read64(s.x[4] + 24);
            state->description = m.read64(s.x[4] + 32);
            state->info = m.read64(s.x[4] + 8);
            if (state->retain && state->info) state->info = call_address(self->support, state->retain, {state->info});
        }
        s.x[0] = make_object(self->support, self->descriptor_class, state);
    });
    support.function(core_foundation, "_CFFileDescriptorGetTypeID", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->descriptor_class;   // a unique value for the type
    });
    support.function(core_foundation, "_CFFileDescriptorGetNativeDescriptor", [self](CpuState& s, GuestMemory&) {
        const auto state = self->descriptor_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = static_cast<Address>(static_cast<std::uint32_t>(state->fd));
    });
    support.function(core_foundation, "_CFFileDescriptorGetContext", [self](CpuState& s, GuestMemory& m) {
        const auto state = self->descriptor_of(s.x[0]);
        if (!s.x[1]) return;
        std::lock_guard lock(state->mutex);
        m.write64(s.x[1], 0);
        m.write64(s.x[1] + 8, state->info);
        m.write64(s.x[1] + 16, state->retain);
        m.write64(s.x[1] + 24, state->release);
        m.write64(s.x[1] + 32, state->description);
    });
    support.function(core_foundation, "_CFFileDescriptorEnableCallBacks", [self](CpuState& s, GuestMemory&) {
        const auto state = self->descriptor_of(s.x[0]);
        { std::lock_guard lock(state->mutex); state->enabled |= s.x[1] & (read_callback | write_callback); }
        self->watch(s.x[0]);
    });
    support.function(core_foundation, "_CFFileDescriptorDisableCallBacks", [self](CpuState& s, GuestMemory&) {
        const auto state = self->descriptor_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->enabled &= ~s.x[1];
    });
    support.function(core_foundation, "_CFFileDescriptorInvalidate", [self](CpuState& s, GuestMemory&) { self->invalidate(s.x[0]); });
    support.function(core_foundation, "_CFFileDescriptorIsValid", [self](CpuState& s, GuestMemory&) {
        const auto state = self->descriptor_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->valid;
    });
    // CFRunLoopSourceRef CFFileDescriptorCreateRunLoopSource(allocator, f, order): +1, one source per descriptor.
    support.function(core_foundation, "_CFFileDescriptorCreateRunLoopSource", [self](CpuState& s, GuestMemory& m) {
        const auto state = self->descriptor_of(s.x[1]);
        {
            std::lock_guard lock(state->mutex);
            if (!state->valid) { s.x[0] = 0; return; }
            if (state->source) { s.x[0] = self->support.retain(state->source); return; }
        }
        // CFRunLoopSourceContext (version 0): version, info, retain, release, copyDescription, equal, hash, schedule, cancel, perform.
        GuestBlock context(self->support, 80);
        m.write64(context + 8, s.x[1]);
        m.write64(context + 72, self->perform_entry);
        const auto source = self->support.call(core_foundation, "_CFRunLoopSourceCreate", {0, s.x[2], context});
        bool pending;
        {
            std::lock_guard lock(state->mutex);
            state->source = source;   // the descriptor's reference
            pending = state->pending != 0;
        }
        if (pending) static_cast<void>(self->support.call(core_foundation, "_CFRunLoopSourceSignal", {source}));
        s.x[0] = self->support.retain(source);
    });

    // ---- _c_locale, fts, warn / err (libSystem: darwin/libsystem/misc/fts_err.cpp) ----
    install_fts_err(support);
}

} // namespace alchemy::gui::completion_kit
