# The export-fill kit (src/alchemy/darwin/kit/export_fill/, a shared kit: its
# providers live in their frameworks under src/alchemy/darwin/, and the former
# gap-fill group is one row per framework in src/alchemy/gui/install.cpp).
# Only the shared test remains here.

function(alchemy_gui_guest_services_tests)
    alchemy_gui_add_unit_test(alchemy_gui_guest_services src/alchemy/darwin/kit/export_fill/tests/guest_services_tests.cpp TIMEOUT 120)
    target_include_directories(alchemy_gui_guest_services PRIVATE "${PROJECT_SOURCE_DIR}/src/alchemy/darwin/kit/export_fill")
endfunction()
alchemy_gui_register_tests(alchemy_gui_guest_services_tests)
