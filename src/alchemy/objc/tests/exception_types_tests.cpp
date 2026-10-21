// libobjc pieces moved from darwin/identities/system_gaps.cpp (exception_types.cpp):
// OBJC_EHTYPE_id, and objc_begin_catch / objc_end_catch forwarding to libc++abi.
#include "alchemy/gui/tests/framework_case.hpp"

#include <array>

namespace {
using namespace fill_test;

constexpr std::string_view objc_lib = "/usr/lib/libobjc.A.dylib";

const Case objc_exception_types_case("objc_exception_types", [](Fixture& f) {
    const auto ehtype = f.symbol(objc_lib, "_OBJC_EHTYPE_id");
    require(f.read_text(f.engine.read64(ehtype + 8)) == "id", "OBJC_EHTYPE_id name");
    const auto can_catch = f.engine.read64(f.engine.read64(ehtype) + 32);   // can_catch: vtable slot 6 (the vptr points at slot 2)
    require(f.engine.invoke_callback(can_catch, std::array<Address, 4>{ehtype, 0, 0, 0}) == 0, "can_catch is false");
    // objc_begin_catch / objc_end_catch forward to libc++abi (a stand-in here).
    static Address begun{};
    static int ended{};
    const auto begin_catch = f.process.install_function("__fill_test_cxa_begin_catch", [](transmute::CpuState& s, transmute::GuestMemory&) {
        begun = s.x[0];
        s.x[0] = s.x[0] + 0x20;   // the thrown object follows the exception header
    });
    const auto end_catch = f.process.install_function("__fill_test_cxa_end_catch", [](transmute::CpuState&, transmute::GuestMemory&) { ++ended; });
    f.process.install_export("/usr/lib/libc++abi.dylib", "___cxa_begin_catch", {begin_catch, alchemy::ExportKind::function});
    f.process.install_export("/usr/lib/libc++abi.dylib", "___cxa_end_catch", {end_catch, alchemy::ExportKind::function});
    require(f.call(objc_lib, "_objc_begin_catch", {0x7000}) == 0x7020 && begun == 0x7000, "objc_begin_catch is __cxa_begin_catch");
    static_cast<void>(f.call(objc_lib, "_objc_end_catch"));
    require(ended == 1, "objc_end_catch is __cxa_end_catch");
});

} // namespace
