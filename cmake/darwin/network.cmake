# Network: CFNetwork, Foundation URL loading, Bonjour and Network.framework
# (src/alchemy/darwin/network/; group r2 foundation). Included by
# cmake/AlchemyDarwin.cmake.
#
# Network NSURLs (http, https and other non-file schemes) and the CFURL
# functions for them, NSOperationQueue for block operations (when no other
# provider defines it), NSURLRequest / NSURLResponse / NSURLSession with data,
# upload and download tasks over WinHTTP (system proxy settings through the
# SystemConfiguration provider, server trust through SecTrust and
# NSURLSession challenges), CFNetwork proxy functions (PAC scripts through
# WinHTTP autoproxy) and CFHTTPMessage, CoreFoundation's CFSocket over guest
# sockets, libresolv's res_9_n* DNS queries, and Bonjour: libSystem's dns_sd
# API and Foundation's NSNetService / NSNetServiceBrowser over the Windows
# DNS-SD client (each export only when no other provider exports it).
# Network.framework (network_framework.cpp): nw_path_monitor over the host's
# adapters. Installed by install_network after install_security
# (include/alchemy/gui/install.hpp). GUI source set for the same reasons as
# security.cmake; winhttp.dll is delay-loaded.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/network/url.cpp
    src/alchemy/darwin/network/operation_queue.cpp
    src/alchemy/darwin/network/http.cpp
    src/alchemy/darwin/network/loading.cpp
    src/alchemy/darwin/network/cookies.cpp
    src/alchemy/darwin/network/cookie_classes.cpp
    src/alchemy/darwin/network/cfnetwork.cpp
    src/alchemy/darwin/network/pac.cpp
    src/alchemy/darwin/network/socket.cpp
    src/alchemy/darwin/network/resolver.cpp
    src/alchemy/darwin/network/pump.cpp
    src/alchemy/darwin/network/mdns.cpp
    src/alchemy/darwin/network/dns_sd.cpp
    src/alchemy/darwin/network/net_service.cpp
    src/alchemy/darwin/network/install.cpp
    src/alchemy/darwin/network/network_framework.cpp
    src/alchemy/darwin/network/nw_connections.cpp
    src/alchemy/darwin/network/cfsocket_fx.cpp)
# Headless shell (darwin/foundation/headless_shells.hpp): alchemy.exe's CFSocket
# placeholders; in alchemy-fx socket.cpp owns CFSocket (cfsocket_fx.cpp is empty).
target_sources(alchemy_core PRIVATE src/alchemy/darwin/network/cfsocket_headless.cpp)
list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES src/alchemy/darwin/network/cfsocket_headless.cpp)
# dnsapi's DNS-SD entry points are resolved at run time (mdns.cpp).
list(APPEND ALCHEMY_GUI_LINK_LIBRARIES winhttp ws2_32 iphlpapi -DELAYLOAD:winhttp.dll)

function(alchemy_darwin_network_tests)
    alchemy_gui_add_unit_test(alchemy_gui_network src/alchemy/darwin/network/tests/network_tests.cpp TIMEOUT 180)
    target_include_directories(alchemy_gui_network PRIVATE "${PROJECT_SOURCE_DIR}/src/alchemy/darwin/network"
        "${PROJECT_SOURCE_DIR}/src/alchemy/darwin/security")
    # Host-only: the HTTP cookie jar (cookies.cpp).
    alchemy_gui_add_unit_test(alchemy_gui_network_cookies src/alchemy/darwin/network/tests/cookie_tests.cpp)
    # Network.framework's path monitor over the full provider set (fill_fixture.hpp).
    alchemy_gui_add_unit_test(alchemy_gui_network_framework src/alchemy/darwin/network/tests/network_framework_tests.cpp TIMEOUT 180)
    if(NOT ALCHEMY_GUI_FIXTURES_ENABLED)
        return()
    endif()
    # Compiled ARM64 dns_sd client (libndi's usage) under alchemy-fx.
    alchemy_gui_object(gui_bonjour_fixture ../../../src/alchemy/darwin/network/tests/bonjour_fixture.c)
    alchemy_gui_program(gui_bonjour_fixture gui_bonjour_fixture)
    add_custom_target(alchemy_gui_network_fixtures ALL DEPENDS "${macos_dir}/gui_bonjour_fixture")
    add_dependencies(alchemy_gui_network_fixtures alchemy_gui_link_stubs)
    alchemy_gui_add_fixture_test(alchemy_gui_bonjour_fixture "${macos_dir}/gui_bonjour_fixture"
        PROGRESS "Alchemy Bonjour fixture: launched" COMPLETE "Alchemy Bonjour fixture: complete")
endfunction()
alchemy_gui_register_tests(alchemy_darwin_network_tests)
