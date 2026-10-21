# SystemConfiguration (src/alchemy/darwin/systemconfiguration/; group r2
# foundation). Included by cmake/AlchemyDarwin.cmake.
#
# SCDynamicStoreCopyProxies and the kSCPropNetProxies* keys (CPython's
# _scproxy, urllib.request on darwin) over the Windows user's proxy
# configuration (system_configuration.cpp; winhttp.dll is loaded at run time,
# no link dependency), and the dynamic store and reachability over the
# Windows network state (sysconfig_store.cpp, part of the network provider's
# install, cmake/darwin/network.cmake). The identity is listed in
# darwin_framework_libraries.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/systemconfiguration/system_configuration.cpp
    src/alchemy/darwin/systemconfiguration/sysconfig_store.cpp
    src/alchemy/darwin/systemconfiguration/network_interfaces.cpp
    src/alchemy/darwin/systemconfiguration/install.cpp)

function(alchemy_darwin_systemconfiguration_tests)
    alchemy_gui_add_unit_test(alchemy_gui_sysconfig src/alchemy/darwin/systemconfiguration/tests/sysconfig_tests.cpp)
    target_include_directories(alchemy_gui_sysconfig PRIVATE "${PROJECT_SOURCE_DIR}/src/alchemy/darwin/systemconfiguration")
endfunction()
alchemy_gui_register_tests(alchemy_darwin_systemconfiguration_tests)

# Moved from darwin/identities/system_gaps.cpp: SCError, SCErrorString,
# reachability for an address pair and IOKit's SCDynamicStore aliases
# (fill-only, kit/fwgaps).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/systemconfiguration/errors_and_aliases.cpp)
alchemy_gui_add_framework_case(systemconfiguration_errors_and_aliases src/alchemy/darwin/systemconfiguration/tests/errors_and_aliases_tests.cpp)
