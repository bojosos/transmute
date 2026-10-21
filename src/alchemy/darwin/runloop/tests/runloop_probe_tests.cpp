// Run-loop behaviour Qt's QCocoaEventDispatcher depends on (owner: qt-vfb).
// Qt drives its timers through a CFRunLoopTimer whose callback signals an
// "activate timers" source; that source's perform runs the Qt timers and
// re-arms the CF timer at once for zero-interval timers, and every event
// posted meanwhile signals a second "posted events" source. Both sources are
// in the common modes. The posted-events source must still perform while the
// zero-interval timer keeps firing, otherwise Qt never delivers posted events
// (a renderer's frame buffer stalled this way).
#include "runloop_internal.hpp"

#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/gui/runloop.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> bytes) {
        return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices services{process};
    alchemy::DarwinThreads threads{(alchemy::install_darwin_dispatch(process), process)};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    alchemy::FoundationBridge foundation{process, objc, cf};
    Fixture() {
        alchemy::install_darwin_frameworks(process, objc, cf);
        alchemy::gui::InstallContext context{process, objc, cf};
        alchemy::gui::install_runloop(context);
    }
    ~Fixture() {
        try { process.request_exit(0); threads.join_workers(); } catch (...) {}
    }
};

} // namespace

int main() {
    try {
        Fixture f;
        auto& loop = alchemy::gui::main_run_loop();
        using alchemy::gui::common_modes;
        int timer_fires = 0, activations = 0, posted_performs = 0;
        alchemy::gui::SourceId activate{}, posted{};
        alchemy::gui::TimerId timer{};
        // The CF timer: signals the activate-timers source.
        timer = loop.add_timer(alchemy::gui::monotonic_seconds(), 3600, common_modes, [&](alchemy::gui::TimerId) {
            ++timer_fires;
            loop.signal_source(activate);
        });
        activate = loop.add_source(common_modes, [&] {
            ++activations;
            loop.signal_source(posted);                                      // a Qt timer posted an event
            loop.set_timer_fire(timer, alchemy::gui::monotonic_seconds());   // a zero-interval Qt timer is due again
        });
        posted = loop.add_source(common_modes, [&] { ++posted_performs; });
        for (int step = 0; step < 200 && posted_performs < 5; ++step)
            static_cast<void>(loop.run_once(alchemy::gui::default_mode, 0.05, true));
        std::cout << "timer fires " << timer_fires << " activations " << activations << " posted performs "
                  << posted_performs << '\n';
        if (posted_performs < 5) throw std::runtime_error("posted-events source starved by a zero-interval timer source");
        std::cout << "Qt run-loop probe passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
