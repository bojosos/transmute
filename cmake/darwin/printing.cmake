# Printing for strict mode (src/alchemy/darwin/printing/): libcups (no scheduler;
# real option, destination and IPP message data). Installed by
# gui::fill::install_printing (fill-only). PrintCore (PM*) is print_core.cpp below.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/printing/install.cpp
    src/alchemy/darwin/printing/cups.cpp)

# The values provider's PrintCore entry (print_core_gaps.cpp, called by
# darwin/coreservices/services.cpp's install_services; empty since print_core.cpp).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/printing/print_core_gaps.cpp)

function(alchemy_gui_printing_tests)
    alchemy_gui_add_unit_test(alchemy_gui_printing src/alchemy/darwin/printing/tests/printing_tests.cpp TIMEOUT 120)
endfunction()
alchemy_gui_register_tests(alchemy_gui_printing_tests)

# Headless additions (both builds; darwin/headless_additions.hpp), moved from
# darwin/identities/installer_frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/printing/print_core.cpp)
