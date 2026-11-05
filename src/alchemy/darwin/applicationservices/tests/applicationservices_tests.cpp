// ApplicationServices (src/alchemy/darwin/applicationservices/): the umbrella identity
// re-exports its subframeworks; the Process Manager answers; icon plotting
// fails explicitly.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view as = "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices", cg = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";

const Case applicationservices_case("applicationservices", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(as), "ApplicationServices identity");
    const auto through_umbrella = f.process.resolve(as, "_CGColorSpaceCreateDeviceRGB");
    require(through_umbrella && through_umbrella->address == f.symbol(cg, "_CGColorSpaceCreateDeviceRGB"),
            "a CoreGraphics symbol bound to the umbrella resolves to CoreGraphics' export");
    const auto psn = f.block(8);
    require(f.call(as, "_GetCurrentProcess", {psn}) == 0 && f.engine.read32(psn + 4) == 2, "GetCurrentProcess");
    // No active application here: no Mac process is in front.
    const auto front = f.block(8, 0xff);
    require((f.call(as, "_GetFrontProcess", {front}) & 0xffff) == 0xfda8 && f.engine.read32(front + 4) == 0, "GetFrontProcess without an active application");
    const auto same = f.block(8);
    require(f.call(as, "_SameProcess", {psn, front, same}) == 0 && (f.engine.read32(same) & 0xff) == 0, "SameProcess: different processes");
    require(f.call(as, "_SameProcess", {psn, psn, same}) == 0 && (f.engine.read32(same) & 0xff) == 1, "SameProcess: the same process");
    const auto pid = f.block(4);
    const auto own = static_cast<std::uint32_t>(f.call("/usr/lib/libSystem.B.dylib", "_getpid"));
    require(f.call(as, "_GetProcessPID", {psn, pid}) == 0 && f.engine.read32(pid) == own && own != 0, "GetProcessPID");
    require((f.call(as, "_GetProcessPID", {front, pid}) & 0xffff) == 0xfda8, "GetProcessPID of no process");
    const auto name_out = f.block(8);
    // This fixture's main bundle has neither a name nor an executable, and there is no program name: the name is empty.
    require(f.call(as, "_CopyProcessName", {psn, name_out}) == 0 && f.engine.read64(name_out) != 0, "CopyProcessName");
    const auto info = f.call(as, "_ProcessInformationCopyDictionary", {psn, 0xffffffff});
    require(info != 0, "ProcessInformationCopyDictionary");
    const auto value = [&f, info](std::string_view key) { return f.cf.dictionary_value(info, f.cf.string(key)); };
    require(f.cf.number_value(value("pid")).value_or(0) == own && f.cf.utf8(value("CFBundleName")) == f.cf.utf8(f.engine.read64(name_out)) &&
                f.cf.number_value(value("PSN")).value_or(0) == 2 && value("IsHiddenAttr") != 0,
            "the process information dictionary");
    require(f.call(as, "_ProcessInformationCopyDictionary", {front, 0xffffffff}) == 0, "no dictionary for no process");    require(f.process.resolve(as, "_PlotIconRefInContext").has_value(), "PlotIconRefInContext binds (and fails when called)");
});

} // namespace
