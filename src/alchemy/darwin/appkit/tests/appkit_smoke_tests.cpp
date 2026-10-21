// AppKit smoke test (owner: A4): the contracts AppKit builds on compile
// together.
#include "alchemy/gui/install.hpp"
#include "alchemy/gui/runloop.hpp"
#include "alchemy/gui/window_server.hpp"

#include <iostream>

int main() {
    alchemy::gui::Event event;
    event.kind = alchemy::gui::EventKind::input;
    event.type = alchemy::gui::event_type::left_mouse_down;
    if (event.click_count != 0 || alchemy::gui::default_mode != "kCFRunLoopDefaultMode") {
        std::cerr << "AppKit contract defaults changed\n";
        return 1;
    }
    std::cout << "Alchemy GUI AppKit smoke passed\n";
    return 0;
}
