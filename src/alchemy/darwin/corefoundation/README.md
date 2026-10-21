# CoreFoundation

- **Covers:** the object store and the CoreFoundation provider, headless in
  alchemy.exe and alchemy-fx (core_foundation.cpp, object_store.cpp): strings,
  collections, numbers, dates, URLs (file URLs only), property lists, bundles,
  run loop sources. In alchemy-fx only: the additions over Foundation's
  classes, namely the CFSet and CFNotificationCenter bridges, CFCharacterSet,
  CFBundle over NSBundle, CFLocale, CFPreferences, CFFileDescriptor and
  CFString clusters.
- **Extension points** (the way to layer behaviour over a CoreFoundation
  export without a second owner): `CoreFoundation::extend(function, domain,
  handler)` lets another provider answer a CF function for the objects of
  one domain it owns. Security does this for CFGetTypeID ("Security and
  networking CF types"), network for the CFURL functions ("network URLs").
  Every extension is asked; a handler answers nullopt outside its domain;
  two extensions of one domain are refused when registered; a call two
  extensions claim fails explicitly. So install order never decides the
  result. `extend_url_strings` applies the same rules to Foundation's NSURL
  string constructors. The registration audit lists each extension (kind
  "extension"), and it does not count as a duplicate.
- **Layering with Foundation:** CoreFoundation owns
  `___CFConstantStringClassReference`: a 40-byte class record. Alone, as in
  CoreFoundation's own tests, it identifies constant strings. With the
  Foundation bridge, NSString's class is built in that very record
  (`ObjcRuntime::register_native_class_in`), so the address guest literals
  bind to is NSString and the export is never registered twice. The bridge
  sets each CF object kind's class (`set_class`) and publishes the class
  symbols with `ObjcRuntime::publish_class`.
- **Host dependencies:** time zone and locale data (std::chrono tzdb, host::locale).
  Phase 2 moves them behind the host `time` and `locale` areas.
- **Unsupported, failing explicitly:** custom CFSet callbacks; non-file URLs
  without the network provider; managed preferences (none exist).
- **Registration:** headless when the process starts; the GUI-set files are
  still called from the export-fill, fwgaps, qlab and values-kit aggregators.
- **Tests:** tests/alchemy_core_foundation_tests.cpp (headless); the
  GUI-set cases live in the shared aggregator tests until they are split out.

## Moved from darwin/identities/

* `time_zones_and_uuids.cpp`: CFTimeZone, CFUUID, CFStringHasPrefix/HasSuffix and kCFAllocatorNull when missing (its own row; ctest `alchemy_gui_corefoundation_time_zones_and_uuids`).
* `strings_and_bundles.cpp`: headless, every build: the system encoding, CFStringNormalize, NoCopy strings, CFBundle over NSBundle, CFURL over NSURL, CFUserNotificationDisplayAlert.
