# Network

CFNetwork, Foundation's URL loading, Bonjour and Network.framework.

- **Covers:** network NSURLs (every scheme except file:) and the CFURL
  functions for them; NSURLRequest, NSURLResponse and NSURLSession data,
  upload and download tasks; cookies; CFNetwork proxies (PAC through the
  host's autoproxy) and CFHTTPMessage; CFSocket over guest sockets; libresolv's
  res_9_n* queries; dns_sd and NSNetService over the host `dns_sd` area;
  Network.framework's nw_path_monitor over the host's adapters
  (network_framework.cpp); NSOperationQueue for block operations.
- **Host dependencies:** the host `http` area (requests, PAC; WinHTTP on
  Windows), `dns_sd` (Bonjour; the Windows DNS-SD client) and `time`
  (timers), and `net` (interfaces, the DNS configuration and sockets for
  libresolv). Every request passes the ALCHEMY_NETWORK policy.
- **Unsupported, failing explicitly:** URL schemes without a host HTTP
  transport; NSURLProtocol registrations are recorded but never consulted.
- **Registration:** `install_network`, after `install_security`. File URLs
  stay with CoreFoundation and Foundation. Network URLs are layered over
  their exports through CoreFoundation's extension points (see
  ../corefoundation/README.md): `extend` for the CFURL functions CoreFoundation
  implements (domain "network URLs"), and `extend_url_strings` for
  +[NSURL URLWithString:] / -initWithString: (domain "URL schemes other than
  file:, and non-strings"). The CFURL functions CoreFoundation lacks are
  this provider's own exports.
- **Tests:** `tests/` (alchemy_gui_network, alchemy_gui_bonjour_fixture).
