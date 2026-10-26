// libobjc's exception pieces: OBJC_EHTYPE_id and _objc_ehtype_vtable's can_catch (objc_runtime.cpp),
// objc_exception_throw entering libc++abi's __cxa_throw, and objc_begin_catch / objc_end_catch
// forwarding to libc++abi (stand-ins for the guest LLVM runtime here).
#include "alchemy/gui/tests/framework_case.hpp"

#include <array>

namespace {
using namespace fill_test;

constexpr std::string_view objc_lib = "/usr/lib/libobjc.A.dylib";
constexpr std::string_view cxxabi = "/usr/lib/libc++abi.dylib";

const Case objc_exception_types_case("objc_exception_types", [](Fixture& f) {
    const auto ehtype = f.symbol(objc_lib, "_OBJC_EHTYPE_id");
    require(f.read_text(f.engine.read64(ehtype + 8)) == "id", "OBJC_EHTYPE_id name");
    const auto vptr = f.engine.read64(ehtype);
    require(vptr == f.symbol(objc_lib, "_objc_ehtype_vtable") + 16, "OBJC_EHTYPE_id uses _objc_ehtype_vtable + 16");
    const auto can_catch = f.engine.read64(vptr + 32);   // can_catch: vtable slot 6 (the vptr points at slot 2)
    const auto adjusted = f.block(8);
    require(f.engine.invoke_callback(can_catch, std::array<Address, 3>{ehtype, 0, adjusted}) == 0, "no thrown type: no match");
    // A thrown Objective-C payload {object, {vptr, name, class}}.
    const auto object_class = f.cls("NSObject");
    const auto object = f.send(object_class, "new");
    const auto payload = f.block(32);
    f.engine.write64(payload, object); f.engine.write64(payload + 8, vptr); f.engine.write64(payload + 24, object_class);
    f.engine.write64(adjusted, payload);
    require(f.engine.invoke_callback(can_catch, std::array<Address, 3>{ehtype, payload + 8, adjusted}) == 1 &&
            f.engine.read64(adjusted) == object, "@catch (id) catches an object and receives it");
    const auto own_type = f.block(24);
    f.engine.write64(own_type, vptr); f.engine.write64(own_type + 16, object_class);
    f.engine.write64(adjusted, payload);
    require(f.engine.invoke_callback(can_catch, std::array<Address, 3>{own_type, payload + 8, adjusted}) == 1, "@catch (NSObject*) matches");
    const auto other = f.cls("NSException");
    f.engine.write64(own_type + 16, other);
    f.engine.write64(adjusted, payload);
    require(f.engine.invoke_callback(can_catch, std::array<Address, 3>{own_type, payload + 8, adjusted}) == 0 &&
            f.engine.read64(adjusted) == payload, "an unrelated class does not match");
    const auto cxx_type = f.block(24);   // a C++ type_info with some other vtable
    f.engine.write64(cxx_type, 0x1234);
    require(f.engine.invoke_callback(can_catch, std::array<Address, 3>{ehtype, cxx_type, adjusted}) == 0, "a C++ exception does not match");

    // objc_exception_throw allocates {object, type info} and enters __cxa_throw with the
    // destructor; the stand-in __cxa_throw records its arguments and returns.
    static Address allocated{}, thrown{}, thrown_type{}, destructor{};
    const auto allocation = f.block(32);
    allocated = allocation;
    const auto allocate = f.process.install_function("__fill_test_cxa_allocate_exception", [](transmute::CpuState& s, transmute::GuestMemory&) {
        require(s.x[0] == 32, "the payload holds the object and its type info");
        s.x[0] = allocated;
    });
    const auto cxa_throw = f.process.install_function("__fill_test_cxa_throw", [](transmute::CpuState& s, transmute::GuestMemory&) {
        thrown = s.x[0]; thrown_type = s.x[1]; destructor = s.x[2];
    });
    f.process.install_export(std::string(cxxabi), "___cxa_allocate_exception", {allocate, alchemy::ExportKind::function});
    f.process.install_export(std::string(cxxabi), "___cxa_throw", {cxa_throw, alchemy::ExportKind::function});
    static_cast<void>(f.call(objc_lib, "_objc_exception_throw", {object}));
    require(thrown == allocation && thrown_type == allocation + 8 && destructor != 0, "__cxa_throw(payload, type info, destructor)");
    require(f.engine.read64(allocation) == object && f.engine.read64(allocation + 8) == vptr && f.engine.read64(allocation + 24) == object_class &&
            f.read_text(f.engine.read64(allocation + 16)) == "NSObject", "the payload names the object's class");
    require(f.send(object, "retainCount") == 2, "the thrown object is retained");
    static_cast<void>(f.engine.invoke_callback(destructor, std::array<Address, 1>{allocation}));
    require(f.send(object, "retainCount") == 1, "the payload destructor releases it");
    require(f.call(objc_lib, "_objc_setUncaughtExceptionHandler", {0x1000}) == 0 && f.call(objc_lib, "_objc_setUncaughtExceptionHandler", {0}) == 0x1000,
            "objc_setUncaughtExceptionHandler returns the previous handler");

    // objc_begin_catch / objc_end_catch forward to libc++abi.
    static Address begun{};
    static int ended{};
    const auto begin_catch = f.process.install_function("__fill_test_cxa_begin_catch", [](transmute::CpuState& s, transmute::GuestMemory&) {
        begun = s.x[0];
        s.x[0] = s.x[0] + 0x20;   // the thrown object follows the exception header
    });
    const auto end_catch = f.process.install_function("__fill_test_cxa_end_catch", [](transmute::CpuState&, transmute::GuestMemory&) { ++ended; });
    f.process.install_export(std::string(cxxabi), "___cxa_begin_catch", {begin_catch, alchemy::ExportKind::function});
    f.process.install_export(std::string(cxxabi), "___cxa_end_catch", {end_catch, alchemy::ExportKind::function});
    require(f.call(objc_lib, "_objc_begin_catch", {0x7000}) == 0x7020 && begun == 0x7000, "objc_begin_catch is __cxa_begin_catch");
    static_cast<void>(f.call(objc_lib, "_objc_end_catch"));
    require(ended == 1, "objc_end_catch is __cxa_end_catch");
});

} // namespace
