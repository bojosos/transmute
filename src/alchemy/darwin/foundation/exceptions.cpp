#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <array>
#include <cstdio>

// NSException (CoreFoundation) and the exception name constants.
//
// Exceptions are value objects: name, reason, user info. -raise,
// +raise:format: and +raise:format:arguments: throw the exception as
// objc_exception_throw does (ObjcRuntime::throw_exception: the guest's
// libc++abi unwinds to the matching @catch; an exception nothing catches
// reaches objc_setUncaughtExceptionHandler's handler and stops with its name
// and reason). Raised from a method host code invoked, an exception cannot
// unwind the host frame and stops as uncaught. -callStackReturnAddresses and
// -callStackSymbols are nil: the raise point's stack is not recorded.
//
// Exports: NSGenericException, NSRangeException, NSInvalidArgumentException,
// NSInternalInconsistencyException, NSMallocException, NSObjectInaccessibleException,
// NSObjectNotAvailableException, NSDestinationInvalidException,
// NSPortTimeoutException, NSInvalidSendPortException,
// NSInvalidReceivePortException, NSPortSendException, NSPortReceiveException,
// NSCharacterConversionException, NSFileHandleOperationException,
// NSUndefinedKeyException (only those no provider exports yet), and
// OBJC_EHTYPE_$_NSException, the type descriptor @catch (NSException*) clauses
// name: { vtable, "NSException", class }.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

struct ExceptionState final : ObjectPayload {
    std::mutex mutex;
    Address name{}, reason{}, user_info{};   // +1
};

} // namespace

void install_exceptions(Support& support) {
    const auto cls = support.declare_class("NSException", "NSObject", core_foundation);
    support.objc.publish_class("NSException", cls, foundation_library);
    const auto state_of = [&support](Address object) {
        auto state = support.objc.find_host_state<ExceptionState>(object);
        if (!state) fail("NSException is not initialized");
        return state;
    };
    const auto initialize = [&support](Address object, Address name, Address reason, Address user_info) {
        auto state = support.objc.host_state<ExceptionState>(object);
        const auto name_copy = name ? support.send_id(name, "copy") : 0;
        const auto reason_copy = reason ? support.send_id(reason, "copy") : 0;
        const auto info = support.retain(user_info);
        Address old_name, old_reason, old_info;
        {
            std::lock_guard lock(state->mutex);
            old_name = std::exchange(state->name, name_copy);
            old_reason = std::exchange(state->reason, reason_copy);
            old_info = std::exchange(state->user_info, info);
        }
        support.release(old_name);
        support.release(old_reason);
        support.release(old_info);
        return object;
    };
    support.method(cls, "initWithName:reason:userInfo:", [initialize](CpuState& s, GuestMemory&) {
        s.x[0] = initialize(s.x[0], s.x[2], s.x[3], s.x[4]);
    }, "@@:@@@");
    support.method(cls, "exceptionWithName:reason:userInfo:", [&support, initialize](CpuState& s, GuestMemory&) {
        const auto object = support.send_id(s.x[0], "alloc");
        s.x[0] = autoreleased(support, initialize(object, s.x[2], s.x[3], s.x[4]));
    }, "@@:@@@", true);
    support.method(cls, "init", [initialize](CpuState& s, GuestMemory&) { s.x[0] = initialize(s.x[0], 0, 0, 0); }, "@@:");
    support.method(cls, "dealloc", [&support, cls](CpuState& s, GuestMemory&) {
        if (const auto state = support.objc.find_host_state<ExceptionState>(s.x[0])) {
            Address name, reason, info;
            {
                std::lock_guard lock(state->mutex);
                name = std::exchange(state->name, 0);
                reason = std::exchange(state->reason, 0);
                info = std::exchange(state->user_info, 0);
            }
            support.release(name);
            support.release(reason);
            support.release(info);
        }
        static_cast<void>(support.objc.send_super(s.x[0], cls, support.objc.selector("dealloc")));
    }, "v@:");
    const auto field = [state_of](Address object, Address ExceptionState::*member) {
        const auto state = state_of(object);
        std::lock_guard lock(state->mutex);
        return (*state).*member;
    };
    support.method(cls, "name", [field](CpuState& s, GuestMemory&) { s.x[0] = field(s.x[0], &ExceptionState::name); }, "@@:");
    support.method(cls, "reason", [field](CpuState& s, GuestMemory&) { s.x[0] = field(s.x[0], &ExceptionState::reason); }, "@@:");
    support.method(cls, "userInfo", [field](CpuState& s, GuestMemory&) { s.x[0] = field(s.x[0], &ExceptionState::user_info); }, "@@:");
    // Not recorded (see the top of this file).
    support.method(cls, "callStackReturnAddresses", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    support.method(cls, "callStackSymbols", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    support.method(cls, "description", [&support, field](CpuState& s, GuestMemory&) {
        const auto reason = field(s.x[0], &ExceptionState::reason);
        s.x[0] = reason ? reason : string(support, "");
    }, "@@:");
    support.method(cls, "copyWithZone:", [&support](CpuState& s, GuestMemory&) { s.x[0] = support.retain(s.x[0]); }, "@@:^v");
    // The handler returns right after throw_exception; the guest then throws.
    support.method(cls, "raise", [&support](CpuState& s, GuestMemory&) { support.objc.throw_exception(s, s.x[0]); }, "v@:");
    // +raise:format:, ...: the reason is formatted like +[NSString stringWithFormat:].
    const auto raise_formatted = [&support](CpuState& s, Address name, Address format, Address arguments) {
        const auto string_class = class_named(support, "NSString");
        const auto reason = support.send_id(support.send_id(string_class, "alloc"), "initWithFormat:arguments:", {format, arguments});
        const auto exception = support.send_id(support.send_id(s.x[0], "alloc"), "initWithName:reason:userInfo:", {name, reason, 0});
        support.release(reason);
        support.objc.throw_exception(s, autoreleased(support, exception));
    };
    support.method(cls, "raise:format:", [raise_formatted](CpuState& s, GuestMemory&) {
        raise_formatted(s, s.x[2], s.x[3], s.sp);   // variadic arguments start at the stack pointer
    }, "v@:@@", true);
    support.method(cls, "raise:format:arguments:", [raise_formatted](CpuState& s, GuestMemory&) {
        raise_formatted(s, s.x[2], s.x[3], s.x[4]);
    }, "v@:@@*", true);

    for (const auto* name : {"NSGenericException", "NSRangeException", "NSInvalidArgumentException", "NSInternalInconsistencyException",
                             "NSMallocException", "NSObjectInaccessibleException", "NSObjectNotAvailableException",
                             "NSDestinationInvalidException", "NSPortTimeoutException", "NSInvalidSendPortException",
                             "NSInvalidReceivePortException", "NSPortSendException", "NSPortReceiveException",
                             "NSCharacterConversionException", "NSFileHandleOperationException", "NSUndefinedKeyException"}) {
        const auto symbol = "_" + std::string(name);
        for (const auto library : {std::string_view(core_foundation), std::string_view(foundation_library)}) {
            if (support.process.resolve(library, symbol)) continue;
            support.string_constant(library, symbol, name);
        }
    }
    // struct objc_typeinfo { const void* const* vtable; const char* name; Class cls; }:
    // libobjc's _objc_ehtype_vtable + 2 when exported, the class name, the class.
    if (!support.process.resolve(core_foundation, "_OBJC_EHTYPE_$_NSException")) {
        const auto type_info = support.process.allocate_guest(24 + 16, 16);
        auto& memory = support.process.main_engine();
        memory.fill_bytes(type_info, 0, 40);
        const auto vtable = support.process.resolve(libobjc, "_objc_ehtype_vtable");
        memory.write64(type_info, vtable ? vtable->address + 16 : 0);
        constexpr std::string_view text_name = "NSException";
        memory.write_bytes(type_info + 24, std::span(reinterpret_cast<const std::uint8_t*>(text_name.data()), text_name.size()));
        memory.write64(type_info + 8, type_info + 24);
        memory.write64(type_info + 16, cls);
        support.process.install_export(std::string(core_foundation), "_OBJC_EHTYPE_$_NSException", {type_info, ExportKind::data});
        support.process.install_export(std::string(foundation_library), "_OBJC_EHTYPE_$_NSException", {type_info, ExportKind::data});
    }
}

} // namespace alchemy::gui::completion_kit
