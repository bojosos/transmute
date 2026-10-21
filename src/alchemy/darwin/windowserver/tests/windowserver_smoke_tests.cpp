// Window server contract smoke test (owner: A3).
#include "alchemy/gui/window_server.hpp"

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
        namespace gui = alchemy::gui;
        // install_windowserver has not run in this process.
        if (!gui::window_server_available()) {
            bool threw = false;
            try { static_cast<void>(gui::window_server()); } catch (const std::runtime_error&) { threw = true; }
            require(threw, "window_server() did not fail explicitly");
        }
        require(gui::event_type::key_down == 10 && gui::event_type::scroll_wheel == 22, "NSEventType numbering");
        require(gui::modifier::command == (1ULL << 20), "NSEventModifierFlags numbering");
        require(gui::style::resizable == 8, "NSWindowStyleMask numbering");
        std::cout << "Alchemy GUI window server smoke passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
