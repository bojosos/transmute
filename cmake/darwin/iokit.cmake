# Device frameworks for strict mode (src/alchemy/darwin/iokit/): IOKit HID, power
# and notification gaps, ForceFeedback, DiskArbitration, IOBluetooth,
# CoreBluetooth, GameController, CoreWLAN, CoreLocation, LocalAuthentication.
# Installed by gui::fill::install_devices (fill-only).
# iokit_registry.cpp: the read-only IOKit registry describing the host
# (installed by the install_iokit_registry row, src/alchemy/gui/install.cpp).
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/iokit/install.cpp
    src/alchemy/darwin/iokit/iokit.cpp
    src/alchemy/darwin/iokit/peripherals.cpp
    src/alchemy/darwin/iokit/iokit_registry.cpp)

# Headless shell (src/alchemy/darwin/shells.hpp): alchemy.exe's answers for
# names this framework's GUI provider owns in alchemy-fx, where the _fx twin
# defines the same entry point empty.
target_sources(alchemy_core PRIVATE src/alchemy/darwin/iokit/iokit_headless.cpp)
list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES src/alchemy/darwin/iokit/iokit_headless.cpp)
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/iokit/iokit_fx.cpp)

# IOKit gaps of Chrome's images (iokit_gaps.cpp, installed by
# darwin/coreservices/services.cpp's install_services).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/iokit/iokit_gaps.cpp)

function(alchemy_gui_devices_tests)
    alchemy_gui_add_unit_test(alchemy_gui_devices src/alchemy/darwin/iokit/tests/devices_tests.cpp TIMEOUT 120)
    # The registry's GPU entries from injected GPU identities.
    alchemy_gui_add_unit_test(alchemy_gui_iokit_gpu src/alchemy/darwin/iokit/tests/gpu_registry_tests.cpp TIMEOUT 120)
endfunction()
alchemy_gui_register_tests(alchemy_gui_devices_tests)

# IOKit's registry as a renderer plug-in's images query it (moved from the former
# darwin/identities/tests/framework_gaps_tests.cpp).
alchemy_gui_add_framework_case(iokit_registry src/alchemy/darwin/iokit/tests/iokit_registry_tests.cpp)

# Headless framework set (both builds; darwin/headless_additions.hpp), moved
# from darwin/identities/frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/iokit/empty_registry.cpp)
