#include "alchemy/darwin/foundation/headless_shells.hpp"

#include "alchemy/objc_runtime.hpp"
#include "alchemy/process_context.hpp"

#include <stdexcept>
#include <string>
#include <utility>

// alchemy.exe only (darwin/foundation/headless_shells.hpp): NSRunLoop for a
// headless process, which registers no input sources or timers, so running a
// mode returns at once, as a source-less run loop does.
namespace alchemy::foundation_shells {

void install_run_loop_shell(ObjcRuntime& objc, ProcessContext& process, std::uint64_t run_loop_class,
                            std::function<std::uint64_t(std::uint64_t)> run_loop_for) {
    const auto method = [&objc, run_loop_class](std::string name, transmute::GuestCallHandler handler, bool class_method = false,
                                                std::string types = "@@:") {
        static_cast<void>(objc.add_native_method(run_loop_class, std::move(name), std::move(handler), class_method, std::move(types)));
    };
    method("currentRunLoop", [&process, run_loop_for](auto& state, auto&) { state.x[0] = run_loop_for(process.current_thread()->identity); }, true);
    method("mainRunLoop", [&process, run_loop_for](auto& state, auto&) { state.x[0] = run_loop_for(process.thread(process.main_engine())->identity); }, true);
    method("runMode:beforeDate:", [](auto& state, auto&) { state.x[0] = 0; }, false, "B@:@@");
    method("runUntilDate:", [](auto&, auto&) {}, false, "v@:@");
    method("run", [](auto&, auto&) {}, false, "v@:");
    method("currentMode", [](auto& state, auto&) { state.x[0] = 0; });
    method("limitDateForMode:", [](auto& state, auto&) { state.x[0] = 0; });
    method("addTimer:forMode:", [](auto&, auto&) { throw std::runtime_error("Alchemy Foundation: NSRunLoop timers are unsupported"); }, false, "v@:@@");
}

} // namespace alchemy::foundation_shells
