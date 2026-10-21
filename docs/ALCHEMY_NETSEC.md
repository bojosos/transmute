# Alchemy network and security providers

Owner: v-netsec (wave 1). Sources: `src/alchemy/security/` (`cmake/gui/security.cmake`) and `src/alchemy/network/` (`cmake/gui/network.cmake`). These are project-authored providers over Windows CryptoAPI, CNG, SChannel, DPAPI, WinHTTP, Winsock and IP Helper. No Apple code is used.

## Placement

Both directories belong to the GUI source set, so they ship in `alchemy-fx` and not in `alchemy_core`. They still work headless: installing them creates only classes and constant strings, and nothing opens a window. They need the GUI set for these reasons:

- URL loading, asynchronous trust evaluation, CFSocket and PAC results call back through dispatch queues and run-loop sources. Those exist only in the GUI set (the `runloop` provider).
- `alchemy.exe` stays free of the crypto, TLS and HTTP system DLLs. `crypt32`, `ncrypt`, `secur32` and `winhttp` are delay-loaded, so a process that never touches these APIs never maps them.

Install order (`src/alchemy/gui/install.cpp`): `install_system_configuration` (proxy keys), then `install_security`, then `install_network`.

The following CoreFoundation, libSystem and SystemConfiguration exports are installed per symbol, and only when no other provider exports that symbol: CFSocket, CommonCrypto, NSError/CFError, the SCDynamicStore/SCNetworkReachability exports, the libresolv exports and the dns_sd exports. NSNetService and NSNetServiceBrowser are defined only when no other provider defines them.

## Audit (what the targets import)

| Target | Network/security imports | Status |
|---|---|---|
| An installer (its `*.bin` executable, payload `installer.bin`/`uninstaller.bin`) | `AuthorizationCreate`, `AuthorizationExecuteWithPrivileges`, `SCDynamicStoreCopyProxies`, `CFSocket*` (wxWidgets socket layer) | Resolved. Authorization denies rights (see below). |
| A renderer add-on's payload, 492 Mach-Os | 163 distinct Security/CFNetwork/CommonCrypto/CFSocket/libresolv symbols. `CFSocket*` appears in 19 images. Its licensing libraries use SecTrust, SecCertificate, SecPolicy, SecKey, SecItem, `CCRandomGenerateBytes`, `CC_MD4_*` and `res_9_*` | Resolved, except `CC_MD4_*` (two licensing libraries) and `CCCryptor*` (a browser service), which the export-fill provider supplies, and Qt WebEngine's legacy CSSM / SecKeychain / SecTask / SecIdentitySearch set (Chromium, out of scope) |
| Qt6Network (payload) | `CFNetworkExecuteProxyAutoConfigurationURL`, `CFNetworkCopyProxiesForURL`, `kCFProxy*`, `kSecAttrProtocol*Proxy` | Resolved |
| QLab 5.6.3 + Sparkle | SecureTransport `SSL*` (GCDAsyncSocket), `kCFStreamSSL*`, `SecItemImport`, `SecKeyCreateDecryptedData`, `SecStaticCode*`/`SecCode*`/`SecRequirement*`, `SecTransform*`, Authorization, `CC_MD5`/`CC_SHA1_*`/`CC_SHA256`, NSURLSession family, `SCDynamicStore*`, `SCNetworkReachability*`, `NSNetService`/`NSNetServiceBrowser` (`_qlab._tcp`, `_qlab._udp`, `_state._tcp`) | Resolved (doctor `--deps` on the bundle shows no unresolved Security/CFNetwork/SystemConfiguration/CommonCrypto/Bonjour import). Still missing: CryptoKit (Swift) |
| QLab `libndi.dylib` (NDI) | dns_sd: `DNSServiceCreateConnection`, `RefSockFD`, `ProcessResult`, `RefDeallocate`, `Browse`, `Resolve`, `Register`, `GetAddrInfo`, `ConstructFullName`, `TXTRecord*` | Resolved |

## Security.framework

- **Certificates, policies and trust.** SecCertificate parses DER in-house (`x509.cpp`). SecPolicy covers basic X.509, SSL host names and revocation. SecTrust uses the Windows chain engine (`CertGetCertificateChain` + `CertVerifyCertificateChainPolicy`). Custom anchors are evaluated in-house against the given set, and the trust settings report the Windows roots. Supported calls include `SecTrustEvaluateWithError`, `SecTrustEvaluate`, `SecTrustEvaluateAsync*`, `SecTrustSetExceptions`, `SecTrustSetVerifyDate` and `SecTrustCopyCertificateChain`.
- **Keys.** SecKey is implemented over CNG: RSA and EC (P-256/384/521) generation, import and export in Apple's external layouts, sign/verify, encrypt/decrypt, and ECDH. `SecItemImport` accepts PEM and DER keys and certificates, and PKCS#12 (identities whose keys stay in CNG).
- **Keychain.** `SecItem*` and `SecKeychain*GenericPassword` handle generic and internet passwords. Items are stored in one DPAPI-protected file per Windows user: `<Alchemy root>/.alchemy/keychain/<user>.alchemy-keychain` when an Alchemy root is configured, otherwise `%LOCALAPPDATA%/Alchemy/keychain/login.alchemy-keychain` (the `ALCHEMY_KEYCHAIN` environment variable overrides the path). Adding keys, certificates or identities fails with `errSecUnimplemented`.
- **SecureTransport.** `SSLContext` runs over SChannel through the guest's `SSLReadFunc`/`SSLWriteFunc`, so `errSSLWouldBlock` appears at the same points as on macOS. Cipher selection, DH parameters and DTLS return `errSecUnimplemented`.
- **Code signing, Authorization and SecTransform** give explicit failures:
  - Every validity, requirement and signing-information query returns `errSecCSUnimplemented`.
  - Empty authorizations can be created. Any right, including `AuthorizationExecuteWithPrivileges`, is denied with `errAuthorizationDenied`.
  - SecTransform returns an `errSecUnimplemented` CFError.
- **Other APIs.** `SecRandomCopyBytes` and `CCRandomGenerateBytes` use BCryptGenRandom. CommonCrypto `CC_MD5`/`SHA1`/`SHA224`/`SHA256`/`SHA384`/`SHA512` (one-shot and Init/Update/Final, with contexts in guest memory at Apple's sizes) and `CCHmac*` are implemented in-house. `CCCrypt` and `CCCryptor` are not provided because no audited image imports them.

## CFNetwork and Foundation URL loading

- **URLs.** Network NSURLs cover http, https and other non-file schemes, with RFC 3986 parsing and resolution, plus the CFURL functions for them.
- **Sessions.** NSURLRequest, NSURLResponse, NSHTTPURLResponse and NSURLSession support data, upload and download tasks over WinHTTP:
  - Transfers run on a guest dispatch worker.
  - Delegate messages and completion handlers run in order on the session's delegate NSOperationQueue.
  - Server trust goes through SecTrust and the `NSURLAuthenticationMethodServerTrust` challenge. WinHTTP never judges the certificates.
  - Redirects, HTTP Basic/Digest/NTLM challenges and cancellation are supported.
  - Not provided (explicit failures): background sessions, stream and WebSocket tasks, custom NSURLProtocol classes, body streams and client certificates.
- **Proxies.** `CFNetworkCopySystemProxySettings` and `CFNetworkCopyProxiesForURL` are provided. `CFNetworkExecuteProxyAutoConfigurationURL` evaluates the script with WinHTTP autoproxy on a worker and delivers the result through a version-0 run-loop source.
- **CFHTTPMessage** supports building, serializing and incremental parsing.
- **CFSocket** works over guest BSD sockets: one dispatch watcher polls the descriptors and signals each socket's run-loop source. Read, accept, data, connect and write callbacks are delivered, and `kCFSocketAutomaticallyReenable*` is honoured. `CFSocketConnectToAddress` and `CFSocketSendData` are not provided. `CFStream` socket pairs are not provided either, because nothing in the audit imports them.
- **SystemConfiguration:**
  - The dynamic store covers Global IPv4/IPv6, interfaces, computer name, host names and console user. `CopyValue`, `CopyMultiple` and `CopyKeyList` are provided.
  - Queue notifications come from a 1 s watcher that diffs the watched keys.
  - Reachability (`CreateWithName`, `CreateWithAddress`, `GetFlags`) resolves the target and checks `GetBestInterfaceEx`.
  - Interface names follow Alchemy's guest names (`lo`, `if<index>`).
- **libresolv** (`/usr/lib/libresolv.9.dylib`, new identity in `darwin_framework_libraries`): `res_9_ninit`, `nquery`, `nsearch`, `nclose`, `ndestroy` and `getservers` make real DNS exchanges with the Windows DNS servers. Queries go over UDP, with TCP when the answer is truncated. The BIND ndots:1 search runs over the Windows suffixes. Calls return the raw answer and report `res_h_errno` in the res_state.

## Bonjour (DNS-SD)

QLab advertises and discovers workspaces with NSNetService/NSNetServiceBrowser, and its NDI library uses the dns_sd C API. Both run over the Windows DNS-SD client in `dnsapi.dll` (`DnsServiceBrowse`, `DnsServiceResolve`, `DnsServiceRegister`, available from Windows 10 1809). The engine is `mdns.cpp`. It resolves these functions at run time, so an older Windows reports `kDNSServiceErr_ServiceNotRunning` instead of failing to load.

- **Engine.** A browse reports a service when the responder announces its PTR record, and reports a removal when the responder sends its goodbye (the same record with TTL 0). A resolve returns the target host, port, addresses and TXT record. A registration publishes the service under `<host>.local`. Address lookups use the host resolver, which answers `.local` names over mDNS.
- **Threads.** Windows delivers results on thread-pool threads, which cannot call guest code. Those threads queue the result and post a small job to the guest pump (`pump.cpp`). The pump is one guest dispatch worker that wakes the guest.
- **dns_sd** (`dns_sd.cpp`, libSystem exports):
  - A primary `DNSServiceRef` owns a guest AF_UNIX socket pair. `DNSServiceRefSockFD` returns its read end.
  - When results are queued, the pump writes one byte, so the descriptor becomes readable as it does with mDNSResponder.
  - `DNSServiceProcessResult` reads that byte. When nothing is pending, it blocks like the real call. It then runs the reply callbacks on the calling thread.
  - `kDNSServiceFlagsShareConnection` subordinates share the primary's socket. Deallocating the primary ends them all.
  - `TXTRecordCreate`, `Deallocate`, `SetValue`, `RemoveValue`, `GetLength`, `GetBytesPtr`, `ContainsKey`, `GetValuePtr`, `GetCount` and `GetItemAtIndex` follow the documented semantics, including case-insensitive keys and `kDNSServiceErr_NoMemory` when a caller's buffer is full.
- **NSNetService / NSNetServiceBrowser** (`net_service.cpp`):
  - Each object has a version-0 run-loop source. At init the source is scheduled in the current run loop's default mode, and `scheduleInRunLoop:forMode:` and `removeFromRunLoop:forMode:` change that.
  - Delegate messages run on that run loop. The "will" messages are sent synchronously by the call that starts the activity.
  - Errors use the `NSNetServicesErrorCode`/`NSNetServicesErrorDomain` dictionaries.
  - `+dataFromTXTRecordDictionary:`, `+dictionaryFromTXTRecordData:`, `isEqual:` and `hash` are provided.
- **Limits** (each answers explicitly):
  - Only the `local.` domain exists. Other domains, host-proxy registrations, and the LocalOnly/Unicast/P2P/BLE interface indexes return `kDNSServiceErr_Unsupported` (NSNetServicesBadArgumentError for the Foundation classes).
  - A domain search reports only `local.`.
  - Windows resolves are one-shot, so a resolve answers once.
  - Registrations keep their requested name, because Windows reports no conflict rename.
  - TXT values cross the Windows API as UTF-16 text, so binary TXT values are not preserved.
  - `setTXTRecordData:` on a published NSNetService registers the service again.
  - `includesPeerToPeer` is stored but has no effect, because Windows has no peer-to-peer transport.
  - Not provided, so these calls are unresolved or unrecognized:
    - `NSNetServiceListenForConnections`, stream access and `startMonitoring`
    - `DNSServiceQueryRecord`, the record add/update/remove calls, `EnumerateDomains` and `NATPortMapping`

## Tests

- **`alchemy_gui_security`:** random bytes and errors, CommonCrypto vectors (FIPS/RFC digests and incremental contexts, RFC 2104/4231 HMAC), and trust:
  - custom anchors, host-name mismatch, exceptions and the Windows root store
  - keys: generate, export, sign/verify, encrypt and ECDH
  - a keychain round trip (add, find, update, delete, duplicate)
  - SecureTransport against an in-process SChannel server
  - honest code-signing and Authorization answers
- **`alchemy_gui_network`:**
  - URLs, NSOperationQueue and CFHTTPMessage
  - CFSocket accept, data, write and EOF through the run loop
  - PAC evaluation against a local PAC server (PROXY lists, DIRECT, unreachable script)
  - SystemConfiguration store and reachability
  - libresolv: NXDOMAIN, plus an online query
  - NSURLSession against a local HTTPS server with a self-signed certificate: rejection without a delegate (no request bytes sent), trust override through the challenge, redirect, upload, download, delegate streaming, Basic authentication and cancellation
  - a real HTTPS GET of https://example.com evaluated against the Windows roots
  - Bonjour: the dns_sd TXT helpers and full names, and the NSNetService TXT dictionaries. A published NSNetService is then found by both NSNetServiceBrowser and a shared-connection `DNSServiceBrowse`, resolved, and looked up with `DNSServiceGetAddrInfo`. The test also registers and resolves through dns_sd with stack-passed arguments, and checks that both browsers report each withdrawal.

  Setting `ALCHEMY_TEST_OFFLINE=1` skips the Internet cases. Setting `ALCHEMY_TEST_NO_MDNS=1` skips multicast discovery.
- **`alchemy_gui_bonjour_fixture`:** a compiled ARM64 dns_sd client under `alchemy-fx`, used the way libndi uses it. It shares one connection for register, browse, resolve and address lookup, and drives everything with `poll()` and `DNSServiceProcessResult`.
