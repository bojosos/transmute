// Host-only tests of QLab gap group G4 (src/alchemy/darwin/kit/completion/windows/):
// the Auto Layout constraint solver and the stack view, grid view and
// popover geometry.
#include "alchemy/darwin/appkit/tests/windows_test_support.hpp"

#include <iostream>

int main() {
    try {
        windows_tests::solver_tests();
        windows_tests::geometry_tests();
    } catch (const std::exception& error) {
        std::cerr << "alchemy_gui_appkit_windows_unit: " << error.what() << '\n';
        return 1;
    }
    std::cout << "alchemy_gui_appkit_windows_unit: passed\n";
    return 0;
}
