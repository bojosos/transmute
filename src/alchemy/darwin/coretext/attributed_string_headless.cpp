#include "alchemy/darwin/foundation/headless_shells.hpp"

#include "alchemy/core_foundation.hpp"
#include "alchemy/objc_runtime.hpp"
#include "alchemy/process_context.hpp"

#include <initializer_list>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

// alchemy.exe only (darwin/foundation/headless_shells.hpp): NSAttributedString
// over a plain string; attributes are neither kept nor reported.
namespace alchemy::foundation_shells {
namespace {

struct AttributedState final : ObjectPayload { std::uint64_t string{}; };

[[noreturn]] void fail(std::string_view why) { throw std::runtime_error("Alchemy Foundation: " + std::string(why)); }

struct Shell {
    Shell(ProcessContext& process, ObjcRuntime& objc, CoreFoundation& cf) : process(process), objc(objc), cf(cf) {}
    ProcessContext& process;
    ObjcRuntime& objc;
    CoreFoundation& cf;
    std::mutex mutex;

    std::uint64_t call(std::string_view library, std::string_view symbol, std::initializer_list<std::uint64_t> arguments) {
        const auto service = process.resolve(library, symbol);
        if (!service) fail("missing runtime service " + std::string(symbol));
        return process.calling_engine().invoke_callback(service->address, std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t autorelease(std::uint64_t object) {
        if (!object) return 0;
        try { return call(objc_library, "_objc_autorelease", {object}); }
        catch (...) { process.objects().release(object); throw; }
    }
    std::shared_ptr<AttributedState> state_of(std::uint64_t object, bool required = true) {
        std::lock_guard lock(mutex);
        auto result = std::dynamic_pointer_cast<AttributedState>(process.objects().require(object)->payload);
        if (!result && required) fail("object is not initialized");
        return result;
    }
    std::shared_ptr<AttributedState> attach(std::uint64_t object) {
        auto value = std::make_shared<AttributedState>();
        const auto record = process.objects().require(object);
        std::lock_guard lock(mutex);
        if (record->payload) fail("object initialized twice");
        record->payload = value;
        return value;
    }
    void method(std::uint64_t cls, std::string name, transmute::GuestCallHandler handler, std::string types = "@@:") {
        static_cast<void>(objc.add_native_method(cls, std::move(name), std::move(handler), false, std::move(types)));
    }
};

} // namespace

void install_attributed_string_shell(ProcessContext& process, ObjcRuntime& objc, CoreFoundation& cf, std::uint64_t a) {
    const auto self = std::make_shared<Shell>(process, objc, cf);
    const auto initialize = [self](std::uint64_t object, std::uint64_t string) {
        if (!string || self->cf.kind(string) != ObjectKind::string) fail("NSAttributedString requires an NSString");
        self->attach(object)->string = self->cf.copy(string);
    };
    self->method(a, "initWithString:", [initialize](auto& state, auto&) { initialize(state.x[0], state.x[2]); });
    self->method(a, "initWithString:attributes:", [initialize](auto& state, auto&) { initialize(state.x[0], state.x[2]); }); // Attributes are not retained.
    self->method(a, "init", [self, initialize](auto& state, auto&) {
        const auto empty = self->cf.string("");
        try { initialize(state.x[0], empty); } catch (...) { self->process.objects().release(empty); throw; }
        self->process.objects().release(empty);
    });
    self->method(a, "string", [self](auto& state, auto&) { state.x[0] = self->state_of(state.x[0])->string; });
    self->method(a, "length", [self](auto& state, auto&) { state.x[0] = self->cf.length(self->state_of(state.x[0])->string); }, "Q@:");
    self->method(a, "attributesAtIndex:effectiveRange:", [self](auto& state, auto& memory) {
        const auto size = self->cf.length(self->state_of(state.x[0])->string);
        if (state.x[2] >= size) fail("attributed string index out of range");
        if (state.x[3]) { memory.write64(state.x[3], 0); memory.write64(state.x[3] + 8, size); }
        state.x[0] = self->autorelease(self->cf.dictionary({}));
    }, "@@:Q^{_NSRange=QQ}");
    self->method(a, "dealloc", [self](auto& state, auto&) {
        if (const auto value = self->state_of(state.x[0], false)) self->process.objects().release(std::exchange(value->string, 0));
        static_cast<void>(self->call(objc_library, "_objc_destructInstance", {state.x[0]}));
    }, "v@:");
}

} // namespace alchemy::foundation_shells
