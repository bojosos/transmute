# libdispatch (src/alchemy/darwin/dispatch/; owner: refactor group r1): queues,
# groups, sources, semaphores and dispatch data. The core provider is part of
# alchemy_core; see cmake/AlchemyDarwin.cmake for the fragment pattern.
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/dispatch/dispatch.cpp)

# GUI (alchemy-fx only):
#   install.cpp: libdispatch's rows of the GUI installer table (install.hpp).
#   dispatch_data.cpp: dispatch_data_t, fill-only
#     (dispatch::install_dispatch_data's row; shared kit src/alchemy/darwin/kit/export_fill/).
#   dispatch_objects.cpp: dispatch objects as Objective-C objects
#     (OS_dispatch_* classes; installed by kit/completion/runtime/install.cpp).
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/dispatch/install.cpp
    src/alchemy/darwin/dispatch/dispatch_data.cpp
    src/alchemy/darwin/dispatch/dispatch_objects.cpp)

function(alchemy_gui_dispatch_tests)
    # dispatch_data_t (split from the export-fill kit's tests).
    alchemy_gui_add_unit_test(alchemy_gui_dispatch_data src/alchemy/darwin/dispatch/tests/dispatch_data_tests.cpp TIMEOUT 120)
    # Dispatch objects as Objective-C objects (split from the QLab runtime tests).
    alchemy_gui_add_unit_test(alchemy_gui_dispatch_objects src/alchemy/darwin/dispatch/tests/dispatch_objects_tests.cpp TIMEOUT 120)
    # Dispatch blocks, assertions, attributes, target queues, finalizers, workloops, signal
    # sources and the I/O conveniences.
    alchemy_gui_add_unit_test(alchemy_gui_dispatch_api src/alchemy/darwin/dispatch/tests/dispatch_api_tests.cpp TIMEOUT 120)
endfunction()
alchemy_gui_register_tests(alchemy_gui_dispatch_tests)
