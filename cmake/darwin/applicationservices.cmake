# ApplicationServices (src/alchemy/darwin/applicationservices/): see its README.
# See cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/applicationservices/install.cpp
    src/alchemy/darwin/applicationservices/icon_services.cpp)
alchemy_gui_add_framework_case(applicationservices src/alchemy/darwin/applicationservices/tests/applicationservices_tests.cpp)

# Headless additions (both builds; darwin/headless_additions.hpp), moved from
# darwin/identities/installer_frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/applicationservices/process_manager.cpp)
