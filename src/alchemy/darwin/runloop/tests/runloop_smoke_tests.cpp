// Run loop contract smoke test (owner: A2).
#include "alchemy/gui/runloop.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        const auto first = alchemy::gui::monotonic_seconds();
        const auto second = alchemy::gui::monotonic_seconds();
        require(first > 0 && second >= first, "monotonic clock is not monotonic");
        // No run loop exists until install_runloop creates one.
        if (!alchemy::gui::main_run_loop_available()) {
            bool threw = false;
            try { static_cast<void>(alchemy::gui::main_run_loop()); } catch (const std::runtime_error&) { threw = true; }
            require(threw, "main_run_loop() did not fail explicitly");
        }
        require(static_cast<int>(alchemy::gui::RunResult::timed_out) == 3, "CFRunLoopRunResult numbering");
        std::cout << "Alchemy GUI run loop smoke passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
