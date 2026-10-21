# Security (src/alchemy/darwin/security/; group r2 foundation). Included by
# cmake/AlchemyDarwin.cmake.
#
# Security.framework: SecCertificate / SecPolicy / SecTrust over the Windows
# chain engine, SecKey over CNG, SecItem* keychain items in a DPAPI-protected
# file, SecureTransport (SSLContext) over SChannel, SecRandomCopyBytes, honest
# SecCode / Authorization / SecTransform answers, CommonCrypto digests / HMAC /
# random bytes / CCCryptor, MD4, and NSError / CFError when no other provider
# exports them. GSS.framework: the GSS-API C bindings (RFC 2744) without any
# mechanism. CryptoKit's host core (SHA-2, HMAC, HKDF, ChaCha20-Poly1305,
# P-521) behind the CryptoKit Swift overlay (swift/CryptoKit, built by
# scripts/build_alchemy_swift.ps1).
#
# Placement: the GUI source set (alchemy-fx only), not alchemy_core. The
# providers are headless (nothing is created at install time) but URL loading
# and asynchronous trust evaluation deliver callbacks through the run loop's
# completion thread and dispatch queues, which exist only in this set, and
# alchemy.exe stays free of the crypto / TLS / HTTP system libraries.
#
# crypt32, ncrypt, secur32 (and winhttp, network.cmake) are delay-loaded so a
# process that never touches Security does not map them.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/security/bridge.cpp
    src/alchemy/darwin/security/errors.cpp
    src/alchemy/darwin/security/x509.cpp
    src/alchemy/darwin/security/tls.cpp
    src/alchemy/darwin/security/keys.cpp
    src/alchemy/darwin/security/certificates.cpp
    src/alchemy/darwin/security/keychain.cpp
    src/alchemy/darwin/security/secure_transport.cpp
    src/alchemy/darwin/security/code_signing.cpp
    src/alchemy/darwin/security/common_crypto.cpp
    src/alchemy/darwin/security/security.cpp
    src/alchemy/darwin/security/gss.cpp
    src/alchemy/darwin/security/cc_cryptor.cpp
    src/alchemy/darwin/security/crypto_core.cpp
    src/alchemy/darwin/security/crypto_exports.cpp
    src/alchemy/darwin/security/legacy_keychain.cpp
    src/alchemy/darwin/security/install.cpp
    src/alchemy/darwin/security/authorization_fx.cpp)
# Headless shell (darwin/foundation/headless_shells.hpp): alchemy.exe's
# Authorization Services; in alchemy-fx code_signing.cpp owns them.
target_sources(alchemy_core PRIVATE src/alchemy/darwin/security/authorization_headless.cpp)
list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES src/alchemy/darwin/security/authorization_headless.cpp)
list(APPEND ALCHEMY_GUI_LINK_LIBRARIES crypt32 ncrypt secur32 delayimp
    -DELAYLOAD:crypt32.dll -DELAYLOAD:ncrypt.dll -DELAYLOAD:secur32.dll)

function(alchemy_darwin_security_tests)
    alchemy_gui_add_unit_test(alchemy_gui_security src/alchemy/darwin/security/tests/security_tests.cpp TIMEOUT 120)
    target_include_directories(alchemy_gui_security PRIVATE "${PROJECT_SOURCE_DIR}/src/alchemy/darwin/security")
    alchemy_gui_add_unit_test(alchemy_gui_gss src/alchemy/darwin/security/tests/gss_tests.cpp TIMEOUT 120)
    # SHA-2, HMAC, HKDF, ChaCha20-Poly1305 and P-521 against RFC / FIPS vectors.
    alchemy_gui_add_unit_test(alchemy_gui_security_crypto src/alchemy/darwin/security/tests/crypto_tests.cpp)
    # MD4 against RFC 1320 (md4.hpp).
    alchemy_gui_add_unit_test(alchemy_gui_security_md4 src/alchemy/darwin/security/tests/md4_tests.cpp)
endfunction()
alchemy_gui_register_tests(alchemy_darwin_security_tests)

# Moved from darwin/identities/system_gaps.cpp: CSSM, SecTask, access control
# and keychain-list answers (fill-only, kit/fwgaps).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/security/cssm_and_tasks.cpp)
alchemy_gui_add_framework_case(security_cssm_and_tasks src/alchemy/darwin/security/tests/cssm_and_tasks_tests.cpp)
