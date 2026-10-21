# Security

Security.framework, GSS.framework, CommonCrypto and CryptoKit's host core.

- **Covers:** SecCertificate, SecPolicy and SecTrust over the Windows chain
  engine; SecKey over CNG; SecItem keychain items in a DPAPI-protected file;
  SecureTransport (SSLContext) over SChannel; SecRandomCopyBytes;
  CommonCrypto digests, HMAC, random bytes and CCCryptor; MD4; the GSS-API C
  bindings; CryptoKit's SHA-2, HMAC, HKDF, ChaCha20-Poly1305 and P-521
  (crypto_core.cpp, behind the CryptoKit Swift overlay).
- **Host dependencies:** crypt32, ncrypt, secur32 and bcrypt (delay-loaded),
  DPAPI. Phase 2 moves them behind the host `crypto` area.
- **Unsupported, failing explicitly:** GSS mechanisms (Kerberos, NTLM and
  SPNEGO answer GSS_S_UNAVAILABLE); code signing, Authorization and
  SecTransform answer honestly that nothing is signed or authorized.
- **Registration:** `install_security` (install.cpp, one row in
  src/alchemy/gui/install.cpp): Security.framework, then GSS. Security's own CF types answer
  CFGetTypeID through `CoreFoundation::extend`, domain "Security and
  networking CF types" (see ../corefoundation/README.md); CoreFoundation
  keeps the export.
- **Tests:** `tests/` (alchemy_gui_security, alchemy_gui_gss,
  alchemy_gui_security_crypto).

## Moved from darwin/identities/

* `cssm_and_tasks.cpp`: CSSM OIDs and unimplemented calls, SecTask (unsigned, no entitlements), SecAccessControl values, the keychain search list and callbacks (fill-only, kit/fwgaps; ctest `alchemy_gui_security_cssm_and_tasks`).
