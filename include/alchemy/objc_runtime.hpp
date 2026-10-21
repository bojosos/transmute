#pragma once

#include "alchemy/call_regs.hpp"
#include "alchemy/object_store.hpp"
#include "alchemy/process_context.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace alchemy {
inline constexpr auto objc_library = "/usr/lib/libobjc.A.dylib";
inline constexpr auto foundation_library = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
class ObjcRuntime {
public:
    explicit ObjcRuntime(ProcessContext& process);
    ~ObjcRuntime();
    ObjcRuntime(const ObjcRuntime&) = delete;
    ObjcRuntime& operator=(const ObjcRuntime&) = delete;
    void prepare_images(std::span<const ImageRuntimeInfo> images);
    void rollback_images(std::span<const std::uint64_t> ids);
    void initialize_image(std::uint64_t id);
    void cleanup_thread(ThreadContext& thread);
    [[nodiscard]] std::uint64_t selector(std::string_view name);
    [[nodiscard]] std::uint64_t find_class(std::string_view name) const;
    // `where` attributes the registration for the registration audit
    // (alchemy/registration_audit.hpp); callers never pass it.
    [[nodiscard]] std::uint64_t register_native_class(std::string name,
        std::uint64_t superclass = 0, std::uint32_t instance_size = 16,
        const std::source_location& where = std::source_location::current());
    // register_native_class with the class record placed at `storage` (at
    // least 40 bytes, 16-aligned) instead of a new allocation: for a class whose
    // address another provider exported before the class existed
    // (CoreFoundation's ___CFConstantStringClassReference is NSString).
    [[nodiscard]] std::uint64_t register_native_class_in(std::uint64_t storage, std::string name,
        std::uint64_t superclass = 0, std::uint32_t instance_size = 16,
        const std::source_location& where = std::source_location::current());
    // Publishes class `cls` (named `name`) as _OBJC_CLASS_$_<name> and its
    // metaclass as _OBJC_METACLASS_$_<name> under `library`. A symbol that
    // `library` already binds to the same address is left alone:
    // register_native_class publishes every new class under libobjc and
    // Foundation itself, so publishing it there again would only add a second
    // owner of the same export. Every provider helper that publishes a class
    // goes through this.
    void publish_class(std::string_view name, std::uint64_t cls, std::string_view library,
        const std::source_location& where = std::source_location::current());
    [[nodiscard]] std::uint64_t add_native_method(std::uint64_t cls, std::string name,
        transmute::GuestCallHandler handler, bool class_method = false,
        std::string types = "@@:", const std::source_location& where = std::source_location::current());
    [[nodiscard]] std::uint64_t allocate(std::uint64_t cls);
    [[nodiscard]] std::uint64_t object_class(std::uint64_t object) const;

    // ---- host -> guest messaging (GUI contract, wave 0) ---------------------
    // Host providers that call a method a guest subclass may override, or a
    // delegate method (-[NSWindowDelegate windowShouldClose:], -drawRect:,
    // -[NSApplicationDelegate applicationDidFinishLaunching:]), must send it
    // through these functions, never call their own C++ handler directly, so
    // overrides and categories are honoured exactly as objc_msgSend would.
    //
    // Threading: callable on any guest thread from inside a synthetic call
    // handler (the call runs on the calling thread's engine). Not callable
    // from a host thread that has no guest engine.
    // Errors: a missing method fails with the same explicit
    // "unsupported Objective-C message -[Class selector]" error objc_msgSend
    // raises (no forwarding exists); C++ exceptions thrown by the callee
    // propagate to the caller.

    // The IMP objc_msgSend would call for [receiver sel], running +initialize
    // first like objc_msgSend. Returns the nil-message IMP for a nil receiver.
    [[nodiscard]] std::uint64_t lookup_imp(std::uint64_t receiver, std::uint64_t sel);
    // The IMP for [super sel] inside a method of `cls` (objc_msgSendSuper2:
    // lookup starts at cls's superclass). `cls` is the class (or metaclass
    // for class methods) whose implementation is calling super.
    [[nodiscard]] std::uint64_t lookup_super_imp(std::uint64_t receiver, std::uint64_t cls, std::uint64_t sel);
    // class_respondsToSelector(object_getClass(receiver), sel). Never runs
    // +initialize and never throws for a missing method; false for nil.
    [[nodiscard]] bool responds(std::uint64_t receiver, std::uint64_t sel) const;
    // [receiver sel ...]: registers.x[0] and x[1] are overwritten with
    // receiver and sel; arguments start at x[2] / v[0] / the stack exactly as
    // a compiled call site would pass them (see alchemy/call_regs.hpp). A nil
    // receiver returns all-zero registers without calling anything. If the
    // IMP is a host native method the C++ handler runs directly on the
    // calling thread (no guest transition) with the same register image.
    CallResult send(std::uint64_t receiver, std::uint64_t sel, CallRegs registers = {});
    CallResult send(std::uint64_t receiver, std::string_view sel, CallRegs registers = {}) {
        return send(receiver, selector(sel), std::move(registers));
    }
    // send() when responds(); std::nullopt otherwise (optional delegate methods).
    std::optional<CallResult> send_if_responds(std::uint64_t receiver, std::uint64_t sel, CallRegs registers = {});
    // [super sel ...] from a method of `cls`; see lookup_super_imp.
    CallResult send_super(std::uint64_t receiver, std::uint64_t cls, std::uint64_t sel, CallRegs registers = {});

    // Declares that host class `cls` adopts the Objective-C protocol named
    // `protocol` (for example CAMetalLayer and "CAMetalDrawable" providers,
    // MTLDevice objects and "MTLDevice"). conformsToProtocol: and
    // class_conformsToProtocol answer YES for protocol objects of that name
    // and the protocols they inherit from, including protocols that guest
    // images load later. Applies to `cls` itself; subclasses inherit through
    // conformsToProtocol:'s superclass walk as on Darwin.
    void add_native_protocol(std::uint64_t cls, std::string_view protocol);

    // ---- host state for host-implemented classes ----------------------------
    // Host classes keep no state inside the guest object (instances may be
    // guest subclasses whose ivars follow the host class's 8-byte layout).
    // Their state lives in the object's ObjectRecord::payload side table.
    //
    // host_state<T>(object) returns the object's T, creating it on first use
    // with `make()` (default: std::make_shared<T>()). Creation happens exactly
    // once per object even when threads race; the winner's value is returned
    // to every caller. The payload is destroyed with the object record
    // (after -dealloc), so T's destructor must not call guest code.
    // Fails explicitly when the object is unknown to the object store or its
    // payload holds a different type (two providers claiming one object).
    // find_host_state<T> never creates; it returns nullptr when absent.
    template <class T, class Make>
    std::shared_ptr<T> host_state(std::uint64_t object, Make&& make) {
        static_assert(std::is_base_of_v<ObjectPayload, T>, "host state derives from ObjectPayload");
        auto value = host_payload(object, [&]() -> std::shared_ptr<ObjectPayload> { return make(); });
        auto typed = std::dynamic_pointer_cast<T>(value);
        if (!typed) throw std::runtime_error("Objective-C object already carries host state of another type");
        return typed;
    }
    template <class T>
    std::shared_ptr<T> host_state(std::uint64_t object) {
        return host_state<T>(object, [] { return std::make_shared<T>(); });
    }
    template <class T>
    [[nodiscard]] std::shared_ptr<T> find_host_state(std::uint64_t object) const {
        return std::dynamic_pointer_cast<T>(host_payload(object, {}));
    }

    // ALCHEMY_TRACE_OBJC=<comma-separated class-name prefixes> logs every
    // message whose receiver class (or, for class messages, the class itself)
    // starts with one of the prefixes to stderr as
    //   [objc] -[Class selector] from <module>+0xoffset
    // ("*" matches every class). Parsed once; unset costs one load per send.
    [[nodiscard]] static bool tracing() noexcept;

    // ---- diagnostics (never a compatibility claim) --------------------------
    // Message probe: with ALCHEMY_PROBE_MISSING=zero (or set_probe_messages),
    // a message no class implements logs
    //   [alchemy probe] message -[Class selector]   (+[...] for classes)
    // once per (class, selector) to stderr and returns zero in x0/x1/v0-v3
    // instead of failing. Off (the default), such a message fails exactly as
    // described above.
    void set_probe_messages(bool enabled);
    // "-[Class selector]" / "+[Class selector]" in first-seen order.
    [[nodiscard]] std::vector<std::string> probed_messages() const;
    // The registered classes as JSON: {"classes":[{"name","super","native",
    // "instance":[...],"class":[...],"guest_instance":[...],"guest_class":[...]}]}.
    // "instance"/"class" list selectors with host (native) implementations;
    // "guest_*" those whose implementation is guest code (categories, guest
    // classes). ALCHEMY_DUMP_OBJC=<file> writes it at the first guest message
    // send, again when a probed message finds new classes, and at exit.
    [[nodiscard]] std::string dump_json() const;

private:
    std::shared_ptr<ObjectPayload> host_payload(std::uint64_t object,
        const std::function<std::shared_ptr<ObjectPayload>()>& make) const;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace alchemy
