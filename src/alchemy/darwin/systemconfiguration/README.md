# SystemConfiguration

- **Covers:** SCDynamicStoreCopyProxies and the kSCPropNetProxies* keys over
  the user's proxy configuration (system_configuration.cpp); the
  dynamic store and SCNetworkReachability over the host's network state
  (sysconfig_store.cpp, installed with the network provider).
- **Host dependencies:** the host `http` area (the user's proxy settings),
  `net` (interfaces, DNS configuration, best interface, names) and `system`
  (computer and user names).
- **Unsupported, failing explicitly:** writing to the dynamic store;
  configuration sets and preferences.
- **Registration:** `install_system_configuration`; the store is part of
  `install_network`.
- **Extension points:** none. A provider layering behaviour over another
  framework's export uses that framework's extension point
  (../corefoundation/README.md), never a second registration.
- **Tests:** `tests/` (alchemy_gui_sysconfig).

## Moved from darwin/identities/

* `errors_and_aliases.cpp`: SCError/SCErrorString, the proxies key, reachability for an address pair, IOKit's SCDynamicStore aliases (fill-only, kit/fwgaps; ctest `alchemy_gui_systemconfiguration_errors_and_aliases`).
