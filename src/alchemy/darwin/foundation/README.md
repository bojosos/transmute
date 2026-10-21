# Foundation

- **Covers:** the Foundation bridge and its extra classes, headless in
  alchemy.exe and alchemy-fx (foundation_bridge.cpp, foundation_extra.cpp).
  In alchemy-fx only: value classes, collections, strings, scanners,
  predicates, regular expressions, decimal numbers, calendars, formatters,
  JSON, property lists, keyed archives and coding, XML, KVC, KVO (kvo.cpp,
  the one implementation), notification queues, block notification
  observers and distributed notifications, undo, invocations, exceptions,
  bundles, file manager additions and Cocoa scripting.
- **Also here for now:** libobjc's associated objects (objc_assoc.cpp). They
  belong to the Objective-C runtime (src/alchemy/objc/), which stays put in
  this wave.
- **Headless shells** (headless_shells.hpp): Foundation's own answers for
  names a GUI provider owns in alchemy-fx (NSRunLoop, block notification
  observers). Each `<name>_headless.cpp` is in alchemy.exe
  only, and its `<name>_fx.cpp` twin is empty.
- **Extension points:** NSURL's string constructors consult
  CoreFoundation's `extend_url_strings` handlers (network URLs) before
  Foundation's own file-URL parsing (../corefoundation/README.md).
- **Host dependencies:** files and time through the guest's libSystem;
  Unicode and locale data (normalization, character classes, the system
  time zone, Mac OS Roman text) through the host `locale` area.
- **Unsupported, failing explicitly:** Objective-C exceptions cannot unwind
  guest frames, so -raise stops the program with the exception's name and
  reason; distributed notifications stay in the process.
- **Registration:** headless when the process starts; the GUI-set files are
  still called from the qt, export-fill, qlab and fwgaps aggregators.
- **Tests:** tests/alchemy_foundation_services_tests.cpp (headless); the
  completion kit core, data and runtime tests until they are split out.

## Moved from darwin/identities/

* `file_attribute_keys.cpp`: NSFileModificationDate (its own row; ctest `alchemy_gui_foundation_file_attribute_keys`).
* `threads_and_localizations.cpp`: NSThread main-thread queries, NSBundle localizations, NSFileTypeForHFSTypeCode and class shells (the support kit).
* `class_shells_and_events.cpp`, `geometry.cpp`: headless, every build: the NSUserDefaults class, class shells, NSAppleEventManager (handlers recorded, no events) and NSGeometry.
