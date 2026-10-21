# Google Chrome 155 (macOS arm64): import and Objective-C inventory

This is the complete static inventory of what Google Chrome for macOS
references and how Alchemy binds it today. It complements
[the Chrome audit](ALCHEMY_CHROME_AUDIT.md), which has the probe runs, the
area plan and the JIT analysis. Nothing was implemented for Chrome. The
numbers count binding decisions, not behavior.

## Input and method

* **Input.** The official universal `googlechrome.dmg` (275,971,452 bytes,
  sha256 `32036e8a…5d93c3`), Google Chrome 155.0.8059.85. It is installed
  into `artifacts/alchemy-root-chrome` with `scripts/alchemy_install_dmg.ps1`
  (6 symbolic links recreated). The installed bundle is 382 files and
  756,482,212 bytes. Only the arm64 slices' load commands and symbol tables are read. The image is a local
  test input and is not redistributed.
* **Binding side.** `alchemy-fx doctor --deps --scan-bundle --json` over the
  bundle, built from `mac/chrome-probe` (= `mac/qlab@02b6448` plus a
  long-path manifest, see the audit). A bundle scan expands every
  `@executable_path` against the main executable. Every other executable
  (18) is therefore inspected again on its own, and its result replaces the
  scan's. Without this, the four Aperitif helpers would report the framework
  as missing.
* **Objective-C side.** `scripts/alchemy_objc_gap_scan.py` reads class lists,
  categories, class and super references, selector references and protocols
  from the metadata sections, without reading code. The implementation side
  is the runtime's own class dump (`ALCHEMY_DUMP_OBJC`) from the probe run
  in which the whole framework was bound (run `p2` in the audit): 867
  classes, 693 of them host-implemented.
* **Post-processing.** `scripts/alchemy_import_inventory.py` gives every
  import one class. `scripts/alchemy_chrome_inventory.ps1` runs all of the
  above:

  ```powershell
  scripts/alchemy_chrome_inventory.ps1 -Alchemy build-c/Release/alchemy-fx.exe `
    -Root C:\dev\transmute\artifacts\alchemy-root-chrome -OutDirectory $scratch\inventory `
    -Dump $scratch\runs\p2-aperitif-zero\objc-dump.json -Probe $scratch\runs\p2-aperitif-zero\stderr.txt `
    -SymbolsMd docs/ALCHEMY_CHROME_GAPS_SYMBOLS.md
  ```

  It writes `inventory.json`, `imports.csv` (one row per image and import:
  6,716 rows), `libraries.csv`, the Objective-C `scan.json`/`gaps.json` and
  the generated name lists.

### Classes

| Class | Meaning |
| --- | --- |
| provided | Bound to an Alchemy provider export. |
| provided-error | Bound. A source heuristic says a call only returns an error: `failure(lib, "_x", code)`, or a short handler that sets `ENOTSUP`/`KERN_NOT_SUPPORTED`. |
| provided-stop | Bound. A source heuristic says a call stops the process (`unsupported(...)`, or a short handler that throws "unsupported"). |
| unsupported | Bound, but doctor marks it as a deferred API that fails at load (here `objc_begin_catch`/`objc_end_catch`: Objective-C exceptions). |
| guest | Bound to another image of the bundle. |
| missing-library | A strong import from a library identity that Alchemy does not have. |
| unresolved | A strong import that an existing identity does not export. |
| weak-unresolved | A weak import. It binds to NULL. |

The error/stop heuristic reads `src/alchemy` (128 explicit-failure names
plus short refusing handlers) and is approximate in both directions:
* It misses failure stubs that return a fixed Mach error from a longer
  handler. `mach_msg` is the main one: it is "provided", but every send
  fails with `MACH_SEND_INVALID_DEST` (machsys has no Mach port namespace).
* Some flagged names refuse only some arguments, for example `setbuf`,
  `class_getProperty` and `IOBluetoothPreferenceSetControllerPowerState`.

## Totals

* **27 images.** 26 are arm64:
  * the main executable;
  * `Google Chrome Framework` (2,697 import records);
  * 9 helper apps and tools: Alerts, GPU, Renderer, plain, the four
    Aperitif helpers, `app_mode_loader`, `chrome_crashpad_handler` and
    `web_app_shortcut_copier`;
  * 6 libraries: `libaperitif`, `libchromecompaneros`,
    `liboptimization_guide_internal`, `libvk_swiftshader`, `libvulkan` and
    the arm64 Widevine CDM;
  * 7 updater images: GoogleUpdater, its launcher, Keystone (`ksadmin`,
    `ksinstall`, the agent, the empty `GoogleSoftwareUpdate` stub) and the
    privileged helper.

  The x64-only Widevine CDM is rejected ("only ARM64 images are
  supported").
* **6,716 import records, 2,804 unique (library, symbol) pairs, 65 library
  identities.** No Swift library is linked. libc++ and libc++abi are
  statically linked into Chrome, so the only C++ runtime imports are the
  `_Unwind_*` names from libSystem.
* **15 missing identities.** 13 strong: AVFAudio, Accessibility,
  AuthenticationServices, CoreHaptics, CoreML, CoreMediaIO, CryptoTokenKit,
  LocalAuthenticationEmbeddedUI, MediaPlayer, Network, SafariServices,
  UserNotifications and Vision. 2 weak: ScreenCaptureKit and the private
  SkyLight. Every one of them is linked by the framework. CryptoTokenKit and
  Network are also linked by the updater images, and CryptoTokenKit by
  `app_mode_loader` and the arm64 Widevine CDM.

| Class | Unique symbols | Of which in the framework (records) |
| --- | --- | --- |
| provided | 2,144 | 2,087 |
| provided-error (heuristic) | 78 | 76 |
| provided-stop (heuristic) | 3 | 3 |
| unsupported | 2 | 2 |
| guest | 5 | 0 |
| missing-library | 102 | 102 |
| unresolved | 414 | 371 |
| weak-unresolved | 56 | 56 |

**Strong gaps: 518 unique names (102 + 414 + 2).** 475 of them are in the
framework. The main executable has 9 strong gaps (`malloc_zone_register`,
`malloc_zone_unregister` and 7 `_Unwind_*`), and each helper executable has
10 (the same 9 plus `libsandbox`'s `sandbox_apply`). In strict mode only
those names stand between the loader and the main executable's `main`. The
framework is not loaded at that point: it is `dlopen`ed from `main`.

## Per library

All images, unique symbols. Sorted by strong gaps. "Identity" is the library
status: `provider` (Alchemy has the identity), `missing` or `guest`.

| Library | Identity | Images | Imports | Provided | Errors / stops (heuristic) | Guest | Unsupported | Missing / unresolved | Weak-unresolved |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| AppKit | provider | 11 | 356 | 219 | 0 / 0 | 0 | 0 | 130 | 7 |
| libSystem.B.dylib | provider | 24 | 736 | 642 | 26 / 1 | 0 | 0 | 65 | 2 |
| ApplicationServices | provider | 1 | 73 | 35 | 7 / 0 | 0 | 0 | 31 | 0 |
| Security | provider | 10 | 131 | 107 | 1 / 0 | 0 | 0 | 23 | 0 |
| Network | missing | 1 | 22 | 0 | 0 / 0 | 0 | 0 | 22 | 0 |
| VideoToolbox | provider | 1 | 57 | 28 | 3 / 0 | 0 | 0 | 21 | 5 |
| Vision | missing | 1 | 20 | 0 | 0 / 0 | 0 | 0 | 20 | 0 |
| CoreFoundation | provider | 12 | 226 | 206 | 0 / 0 | 0 | 0 | 15 | 5 |
| libcups.2.dylib | provider | 1 | 51 | 36 | 0 / 0 | 0 | 0 | 15 | 0 |
| AuthenticationServices | missing | 1 | 20 | 0 | 0 / 0 | 0 | 0 | 14 | 6 |
| CoreVideo | provider | 1 | 75 | 56 | 5 / 0 | 0 | 0 | 14 | 0 |
| CoreServices | provider | 3 | 39 | 27 | 1 / 0 | 0 | 0 | 11 | 0 |
| GameController | provider | 1 | 12 | 1 | 0 / 0 | 0 | 0 | 11 | 0 |
| MediaPlayer | missing | 1 | 11 | 0 | 0 / 0 | 0 | 0 | 11 | 0 |
| UserNotifications | missing | 1 | 11 | 0 | 0 / 0 | 0 | 0 | 11 | 0 |
| CoreHaptics | missing | 1 | 9 | 0 | 0 / 0 | 0 | 0 | 9 | 0 |
| CoreMedia | provider | 1 | 55 | 48 | 0 / 0 | 0 | 0 | 7 | 0 |
| AVFoundation | provider | 1 | 27 | 18 | 0 / 0 | 0 | 0 | 6 | 3 |
| CFNetwork | provider | 2 | 17 | 11 | 0 / 0 | 0 | 0 | 6 | 0 |
| Carbon | provider | 1 | 25 | 17 | 2 / 0 | 0 | 0 | 6 | 0 |
| Foundation | provider | 11 | 92 | 86 | 0 / 0 | 0 | 0 | 6 | 0 |
| libsandbox.1.dylib | provider | 6 | 6 | 0 | 0 / 0 | 0 | 0 | 6 | 0 |
| AVFAudio | missing | 1 | 5 | 0 | 0 / 0 | 0 | 0 | 5 | 0 |
| CoreML | missing | 1 | 9 | 0 | 0 / 0 | 0 | 0 | 5 | 4 |
| CoreGraphics | provider | 2 | 163 | 155 | 4 / 0 | 0 | 0 | 4 | 0 |
| CoreMIDI | provider | 1 | 22 | 18 | 0 / 0 | 0 | 0 | 4 | 0 |
| CoreText | provider | 1 | 75 | 71 | 0 / 0 | 0 | 0 | 4 | 0 |
| CoreAudio | provider | 1 | 19 | 13 | 0 / 0 | 0 | 0 | 3 | 3 |
| OpenDirectory | provider | 4 | 12 | 9 | 0 / 0 | 0 | 0 | 3 | 0 |
| SystemConfiguration | provider | 1 | 36 | 33 | 0 / 0 | 0 | 0 | 3 | 0 |
| UniformTypeIdentifiers | provider | 4 | 11 | 8 | 0 / 0 | 0 | 0 | 3 | 0 |
| libbz2.1.0.dylib | provider | 1 | 3 | 0 | 0 / 0 | 0 | 0 | 3 | 0 |
| libobjc.A.dylib | provider | 11 | 60 | 54 | 0 / 1 | 0 | 2 | 3 | 0 |
| AudioToolbox | provider | 1 | 13 | 2 | 9 / 0 | 0 | 0 | 2 | 0 |
| DiskArbitration | provider | 1 | 28 | 26 | 0 / 0 | 0 | 0 | 2 | 0 |
| IOKit | provider | 3 | 78 | 69 | 7 / 0 | 0 | 0 | 2 | 0 |
| Metal | provider | 3 | 30 | 26 | 0 / 0 | 0 | 0 | 2 | 2 |
| Accessibility | missing | 1 | 3 | 0 | 0 / 0 | 0 | 0 | 1 | 2 |
| CoreMediaIO | missing | 1 | 1 | 0 | 0 / 0 | 0 | 0 | 1 | 0 |
| CryptoTokenKit | missing | 1 | 1 | 0 | 0 / 0 | 0 | 0 | 1 | 0 |
| IOSurface | provider | 1 | 38 | 37 | 0 / 0 | 0 | 0 | 1 | 0 |
| LocalAuthenticationEmbeddedUI | missing | 1 | 1 | 0 | 0 / 0 | 0 | 0 | 1 | 0 |
| SafariServices | missing | 1 | 1 | 0 | 0 / 0 | 0 | 0 | 1 | 0 |
| ServiceManagement | provider | 1 | 2 | 1 | 0 / 0 | 0 | 0 | 1 | 0 |
| libbsm.0.dylib | provider | 7 | 2 | 1 | 0 / 0 | 0 | 0 | 1 | 0 |
| Accelerate | provider | 1 | 12 | 12 | 0 / 0 | 0 | 0 | 0 | 0 |
| AudioUnit | provider | 1 | 12 | 8 | 4 / 0 | 0 | 0 | 0 | 0 |
| CoreBluetooth | provider | 1 | 10 | 10 | 0 / 0 | 0 | 0 | 0 | 0 |
| CoreImage | provider | 1 | 7 | 7 | 0 / 0 | 0 | 0 | 0 | 0 |
| CoreLocation | provider | 1 | 3 | 3 | 0 / 0 | 0 | 0 | 0 | 0 |
| CoreWLAN | provider | 1 | 1 | 1 | 0 / 0 | 0 | 0 | 0 | 0 |
| ForceFeedback | provider | 1 | 9 | 0 | 9 / 0 | 0 | 0 | 0 | 0 |
| IOBluetooth | provider | 1 | 8 | 7 | 0 / 1 | 0 | 0 | 0 | 0 |
| LocalAuthentication | provider | 1 | 1 | 1 | 0 / 0 | 0 | 0 | 0 | 0 |
| MediaAccessibility | provider | 1 | 11 | 11 | 0 / 0 | 0 | 0 | 0 | 0 |
| OpenGL | provider | 1 | 2 | 2 | 0 / 0 | 0 | 0 | 0 | 0 |
| QuartzCore | provider | 3 | 21 | 18 | 0 / 0 | 0 | 0 | 0 | 3 |
| ScreenCaptureKit | missing | 1 | 13 | 0 | 0 / 0 | 0 | 0 | 0 | 13 |
| SecurityInterface | provider | 1 | 1 | 1 | 0 / 0 | 0 | 0 | 0 | 0 |
| SkyLight | missing | 1 | 1 | 0 | 0 / 0 | 0 | 0 | 0 | 1 |
| libresolv.9.dylib | provider | 1 | 3 | 3 | 0 / 0 | 0 | 0 | 0 | 0 |
| Google Chrome Framework | guest | 4 | 1 | 0 | 0 / 0 | 1 | 0 | 0 | 0 |
| libaperitif.dylib | guest | 4 | 1 | 0 | 0 / 0 | 1 | 0 | 0 | 0 |
| Google Chrome Framework | guest | 1 | 1 | 0 | 0 / 0 | 1 | 0 | 0 | 0 |
| flat-lookup | ? | 6 | 2 | 0 | 0 / 0 | 2 | 0 | 0 | 0 |

"Google Chrome Framework" appears twice in the guest rows because the
helpers name it by two different `@executable_path` strings.

## Per area

These are the same unique symbols, grouped by
`alchemy_import_inventory.py`'s area rules. The rules match library and
symbol patterns, and the first match wins, so the grouping is approximate.
* **memory-jit** includes the malloc zone API.
* **crashpad** gets a symbol only when the crashpad handler is its only
  importer, or when the symbol is an exception-port or thread-state call.
* The last column drops symbols that only the updater images import.

| Area | Imports | Provided | Errors / stops (heuristic) | Missing / unresolved | Weak-unresolved | Browser-tree gaps (no updater) |
| --- | --- | --- | --- | --- | --- | --- |
| appkit-ui | 669 | 448 | 14 / 0 | 198 | 9 | 198 missing, 14 error/stop |
| crashpad | 20 | 12 | 7 / 0 | 1 | 0 | 1 missing, 7 error/stop |
| fonts-text | 88 | 84 | 0 / 0 | 4 | 0 | 4 missing, 0 error/stop |
| gpu-compositing | 112 | 102 | 1 / 0 | 3 | 6 | 3 missing, 1 error/stop |
| keychain-security | 155 | 109 | 1 / 0 | 39 | 6 | 39 missing, 1 error/stop |
| media | 379 | 203 | 29 / 0 | 119 | 28 | 119 missing, 29 error/stop |
| memory-jit | 27 | 23 | 0 / 0 | 4 | 0 | 4 missing, 0 error/stop |
| networking | 111 | 77 | 0 / 0 | 34 | 0 | 28 missing, 0 error/stop |
| other | 1129 | 1012 | 9 / 3 | 99 | 6 | 88 missing, 12 error/stop |
| process | 100 | 75 | 15 / 0 | 9 | 1 | 3 missing, 15 error/stop |
| sandbox | 14 | 4 | 2 / 0 | 8 | 0 | 8 missing, 2 error/stop |

## Objective-C

From the metadata of the 26 arm64 images, checked against the runtime class
dump.

* **Defined by Chrome.** The framework defines 174 classes and 16
  categories, and has 3,594 selector references and 263 imported system
  classes. The other images define 68 classes: the updater images 67, the crashpad handler 1.
* **Categories on system classes:** AVCaptureSession, NSMenu, NSMenuItem,
  NSObject, NSProcessInfo, NSString, NSToolbar, NSView and NSWindow.
* **System classes Chrome subclasses** (`_OBJC_METACLASS_$_` imports),
  each of which must be a real, subclassable host class:
  * NSApplication, NSWindow, NSWindowController, NSView, NSViewController,
    NSTitlebarAccessoryViewController;
  * NSThemeFrame and NSNextStepFrame (private AppKit frame views), and
    NSGlassEffectView;
  * NSAccessibilityElement, NSCursor, NSTrackingArea, NSAnimation;
  * NSInputStream, NSKeyedUnarchiver, NSURLProtocol;
  * CAMetalLayer, CAOpenGLLayer;
  * ASAuthorizationController, NSObject.
* **Referenced classes not registered: 82**, 5 of them subclassed:
  ASAuthorizationController, NSGlassEffectView, NSNextStepFrame,
  NSThemeFrame and NSURLProtocol. By family:
  * AuthenticationServices (13, passkeys);
  * UserNotifications (8);
  * Touch Bar and sharing (NSTouchBar, NSCustomTouchBarItem,
    NSGroupTouchBarItem, NSCandidateListTouchBarItem, NSSharingService,
    NSSharingServicePicker, NSPreviewRepresentingActivityItem);
  * ScreenCaptureKit (6) and CoreML (9);
  * AVFoundation/AVFAudio capture, renderers and speech (7);
  * Vision (3) and CoreHaptics (4);
  * GameController pads (3) and MediaPlayer (3);
  * AppKit: NSVisualEffectView, NSStatusBarButton, NSTextAttachment,
    NSThemeFrame, `_NSThemeWidget`, NSNextStepFrame, NSGlassEffectView,
    NSAccessibilityCustomAction;
  * Foundation: NSURLProtocol, NSURLQueryItem, NSUserActivity;
  * CAMetalDisplayLink, CATapDescription, MTLCommandQueueDescriptor,
    MTLLogStateDescriptor, TKTokenWatcher, LAAuthenticationView,
    SFUniversalLink, AXCustomContent.

  The NSThemeFrame/NSNextStepFrame imports are weak. Chrome checks for them
  and builds its custom window frame (`BrowserNativeWidgetWindow`) on top of
  them.
* **Selectors with no implementation: 643.** These are selectors in
  `__objc_selrefs` that no class or category of the bundle defines and that
  no registered host class implements. 6 more are delegate callbacks. The
  check is class-agnostic: a selector counts as implemented if any host
  class has it, so the real gap is larger. The attribution table is QLab's,
  so 636 have no owner guess. The full list is in
  [the generated file](ALCHEMY_CHROME_GAPS_SYMBOLS.md).
* **Nibs: none.** Chrome builds its menus and windows in code, so the nib
  loader is not involved.

### Dynamic names (strings, heuristic)

These are `__TEXT,__cstring` strings that look like library paths or class
names but are not imported. They are candidates for `dlopen` and
`NSClassFromString`, not proof of use.
* **Libraries no image links:**
  * `/System/Library/Frameworks/GSS.framework/GSS` (Kerberos/Negotiate
    authentication);
  * the OpenGL framework and `libGL.dylib` (ANGLE's GL backend);
  * `/System/Library/PrivateFrameworks/ViewBridge.framework` and
    `login.framework`;
  * `/usr/lib/libSystem.dylib` and `/usr/lib/libstdc++.6.dylib`;
  * `libvulkan.1.dylib`, `libMoltenVK.dylib`, and four `libLiteRt*`
    accelerator libraries (not in the bundle);
  * `libvulkan.dylib` and `libchromecompaneros.dylib` (in the bundle).
* **Class names nothing imports and the runtime does not register:** 71, with
  false positives (ffmpeg's `AVStream`, constant names). The likely real
  ones are private AppKit classes:
  * `NSRemoteView`, `NSToolTipManager`, `NSPopupMenuWindow`,
    `NSContextMenuItemView`, `NSPopoverWindow`, `NSToolbarFullScreenWindow`,
    `NSServiceEntry`;
  * the ScreenCaptureKit stream classes.

## Reached at run time

Probe mode (`ALCHEMY_PROBE_MISSING=zero`) binds each strong gap to a logging
stub.
* **The framework through an Aperitif helper (run `p2`).** The whole
  framework loads and binds: 344 function stubs, 97 data stubs and 82
  placeholder classes.
* **What runs before the stop.** The only stubs called are
  `malloc_zone_register` and `malloc_zone_unregister`, before any Chrome
  code past the allocator's early zone registration
  ([audit](ALCHEMY_CHROME_AUDIT.md#probe-frontier)). `ALCHEMY_TRACE_CALLS=_`
  shows the provided calls on that path:
  * `malloc_default_zone`, `malloc_default_purgeable_zone` and
    `malloc_get_all_zones` (one zone);
  * `strlen`, then `abort_report_np` and `abort`.

  Every other gap in this inventory is linked but was not reached.

## Missing names per library

Strong gaps only (missing-library, unresolved, unsupported). Error/stop
stubs and weak imports per library are in
[ALCHEMY_CHROME_GAPS_SYMBOLS.md](ALCHEMY_CHROME_GAPS_SYMBOLS.md).

- **AppKit** (130): 103 `NSAccessibility*` attribute, role, action and notification constants, plus `_NSApplicationLaunchUserNotificationKey` `_NSImageHintCTM` `_NSImageNameFolder` `_NSMarkedClauseSegmentAttributeName` `_NSMenuDidEndTrackingNotification` `_NSPerformService` `_NSSharingServiceNameAddToSafariReadingList` `_NSSharingServiceNameComposeEmail` `_NSTextInputReplacementRangeAttributeName` `_NSTouchBarItemIdentifierCandidateList` `_NSTouchBarItemIdentifierCharacterPicker` `_NSTouchBarItemIdentifierFlexibleSpace` `_NSWindowWillMoveNotification` `_NSWorkspaceScreensDidSleepNotification` `_NSWorkspaceScreensDidWakeNotification` `_NSWorkspaceWillPowerOffNotification` `_OBJC_CLASS_$_NSAccessibilityCustomAction` `_OBJC_CLASS_$_NSCandidateListTouchBarItem` `_OBJC_CLASS_$_NSCustomTouchBarItem` `_OBJC_CLASS_$_NSGroupTouchBarItem` `_OBJC_CLASS_$_NSPreviewRepresentingActivityItem` `_OBJC_CLASS_$_NSSharingService` `_OBJC_CLASS_$_NSSharingServicePicker` `_OBJC_CLASS_$_NSStatusBarButton` `_OBJC_CLASS_$_NSTextAttachment` `_OBJC_CLASS_$_NSTouchBar` `_OBJC_CLASS_$_NSVisualEffectView`
- **libSystem.B.dylib** (65): `__Unwind_Backtrace` `__Unwind_DeleteException` `__Unwind_GetCFA` `__Unwind_GetIP` `__Unwind_GetLanguageSpecificData` `__Unwind_GetRegionStart` `__Unwind_RaiseException` `__Unwind_Resume` `__Unwind_SetGR` `__Unwind_SetIP` `____mb_cur_max_l` `___deregister_frame` `___mac_syscall` `___maskrune_l` `___math_errhandling` `___register_frame` `__dirhelper` `_acl_free` `_acl_get_entry` `_acl_get_link_np` `_acl_get_permset_mask_np` `_acl_get_qualifier` `_acl_get_tag_type` `_bootstrap_parent` `_csops` `_csr_get_active_config` `_dispatch_queue_create_with_target$V2` `_err` `_errc` `_errx` `_fmaf` `_getattrlist` `_getopt_long` `_getprogname` `_mach_port_get_refs` `_mach_port_type` `_mach_vm_purgable_control` `_mach_vm_region_recurse` `_malloc_zone_register` `_malloc_zone_unregister` `_mbr_uid_to_uuid` `_mstats` `_nan` `_nanf` `_nearbyintf` `_optarg` `_optind` `_posix_spawnattr_setspecialport_np` `_pthread_get_qos_class_np` `_pthread_override_qos_class_end_np` `_pthread_override_qos_class_start_np` `_swap_fat_arch` `_swap_fat_header` `_swap_linkedit_data_command` `_swap_load_command` `_swap_mach_header` `_swap_mach_header_64` `_swap_segment_command` `_swap_segment_command_64` `_swap_symtab_command` `_task_create_identity_token` `_task_identity_token_get_task_port` `_task_set_special_port` `_task_threads` `_tmpnam`
- **ApplicationServices** (31): `_AXObserverAddNotification` `_AXObserverCreateWithInfoCallback` `_AXObserverGetRunLoopSource` `_AXTextMarkerCreate` `_AXTextMarkerGetBytePtr` `_AXTextMarkerGetLength` `_AXTextMarkerGetTypeID` `_AXTextMarkerRangeCopyEndMarker` `_AXTextMarkerRangeCopyStartMarker` `_AXTextMarkerRangeCreate` `_AXTextMarkerRangeGetTypeID` `_AXUIElementCopyActionNames` `_AXUIElementCopyAttributeNames` `_AXUIElementCopyAttributeValue` `_AXUIElementCopyParameterizedAttributeNames` `_AXUIElementCopyParameterizedAttributeValue` `_AXUIElementCreateApplication` `_AXUIElementGetTypeID` `_AXUIElementPerformAction` `_AXUIElementSetAttributeValue` `_AXValueCreate` `_AXValueGetType` `_AXValueGetTypeID` `_AXValueGetValue` `_PMCopyPrintSettings` `_PMPageFormatCreateDataRepresentation` `_PMPageFormatCreateWithDataRepresentation` `_PMPrintSettingsCreateDataRepresentation` `_PMPrintSettingsCreateWithDataRepresentation` `_PMSessionCopyDestinationFormat` `_UAZoomChangeFocus`
- **Security** (23): `_SecACLCopyContents` `_SecACLSetContents` `_SecAccessCopyACLList` `_SecAccessCreate` `_SecCodeSignerAddSignatureWithErrors` `_SecCodeSignerCreate` `_SecKeychainGetStatus` `_SecKeychainItemCopyKeychain` `_SecKeychainItemCreateFromContent` `_kSecCFErrorResourceAltered` `_kSecCFErrorResourceMissing` `_kSecCodeInfoCertificates` `_kSecCodeInfoFlags` `_kSecCodeInfoMainExecutable` `_kSecCodeInfoPList` `_kSecCodeInfoUnique` `_kSecCodeSignerEntitlements` `_kSecCodeSignerFlags` `_kSecCodeSignerIdentity` `_kSecGuestAttributeAudit` `_kSecGuestAttributeDynamicCode` `_kSecGuestAttributeDynamicCodeInfoPlist` `_kSecGuestAttributePid`
- **Network** (22, identity missing): `__nw_parameters_configure_protocol_default_configuration` `__nw_parameters_configure_protocol_disable` `_nw_browse_descriptor_create_bonjour_service` `_nw_browse_result_copy_endpoint` `_nw_browse_result_get_changes` `_nw_browser_cancel` `_nw_browser_create` `_nw_browser_set_browse_results_changed_handler` `_nw_browser_set_queue` `_nw_browser_set_state_changed_handler` `_nw_browser_start` `_nw_endpoint_get_bonjour_service_name` `_nw_error_get_error_code` `_nw_interface_get_type` `_nw_parameters_create_secure_tcp` `_nw_path_enumerate_interfaces` `_nw_path_get_status` `_nw_path_monitor_cancel` `_nw_path_monitor_create` `_nw_path_monitor_set_queue` `_nw_path_monitor_set_update_handler` `_nw_path_monitor_start`
- **VideoToolbox** (21): `_kVTCompressionPropertyKey_BaseLayerFrameRateFraction` `_kVTCompressionPropertyKey_ColorPrimaries` `_kVTCompressionPropertyKey_ContentLightLevelInfo` `_kVTCompressionPropertyKey_EncoderID` `_kVTCompressionPropertyKey_HDRMetadataInsertionMode` `_kVTCompressionPropertyKey_MasteringDisplayColorVolume` `_kVTCompressionPropertyKey_OutputBitDepth` `_kVTCompressionPropertyKey_PixelTransferProperties` `_kVTCompressionPropertyKey_ReferenceBufferCount` `_kVTCompressionPropertyKey_TransferFunction` `_kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder` `_kVTCompressionPropertyKey_YCbCrMatrix` `_kVTEncodeFrameOptionKey_BaseFrameQP` `_kVTHDRMetadataInsertionMode_Auto` `_kVTPixelTransferPropertyKey_ScalingMode` `_kVTProfileLevel_HEVC_Main10_AutoLevel` `_kVTProfileLevel_HEVC_Main42210_AutoLevel` `_kVTProfileLevel_HEVC_Main_AutoLevel` `_kVTScalingMode_CropSourceToCleanAperture` `_kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder` `_kVTVideoEncoderSpecification_EnableLowLatencyRateControl`
- **Vision** (20, identity missing): `_OBJC_CLASS_$_VNDetectBarcodesRequest` `_OBJC_CLASS_$_VNDetectFaceLandmarksRequest` `_OBJC_CLASS_$_VNImageRequestHandler` `_VNBarcodeSymbologyAztec` `_VNBarcodeSymbologyCode128` `_VNBarcodeSymbologyCode39` `_VNBarcodeSymbologyCode39Checksum` `_VNBarcodeSymbologyCode39FullASCII` `_VNBarcodeSymbologyCode39FullASCIIChecksum` `_VNBarcodeSymbologyCode93` `_VNBarcodeSymbologyCode93i` `_VNBarcodeSymbologyDataMatrix` `_VNBarcodeSymbologyEAN13` `_VNBarcodeSymbologyEAN8` `_VNBarcodeSymbologyI2of5` `_VNBarcodeSymbologyI2of5Checksum` `_VNBarcodeSymbologyITF14` `_VNBarcodeSymbologyPDF417` `_VNBarcodeSymbologyQR` `_VNBarcodeSymbologyUPCE`
- **CoreFoundation** (15): `_CFDictionaryApplyFunction` `_CFNotificationCenterGetDarwinNotifyCenter` `_CFNotificationCenterRemoveObserver` `_CFPreferencesAppValueIsForced` `_CFStringAppend` `_CFStringCreateArrayWithFindResults` `_CFStringGetRangeOfCharacterClusterAtIndex` `_NSURLHasHiddenExtensionKey` `_NSURLIsExcludedFromBackupKey` `_NSURLTagNamesKey` `_OBJC_CLASS_$_NSInputStream` `_OBJC_METACLASS_$_NSInputStream` `__CFURLCopyPropertyListRepresentation` `__CFURLCreateFromPropertyListRepresentation` `__kCFBundleNumericVersionKey`
- **libcups.2.dylib** (15): `_cupsGetResponse` `_cupsLocalizeDestValue` `_cupsSendRequest` `_cupsUser` `_cupsUserAgent` `_ippAddBoolean` `_ippAddCollection` `_ippAddCollections` `_ippAddRange` `_ippCopyAttribute` `_ippDeleteAttribute` `_ippFindAttribute` `_ippGetCollection` `_ippNew` `_pwgMediaForSize`
- **AuthenticationServices** (14, identity missing): `_ASAuthorizationErrorDomain` `_ASAuthorizationPublicKeyCredentialAttestationKindNone` `_ASAuthorizationPublicKeyCredentialUserVerificationPreferenceDiscouraged` `_ASAuthorizationPublicKeyCredentialUserVerificationPreferencePreferred` `_ASAuthorizationPublicKeyCredentialUserVerificationPreferenceRequired` `_ASWebAuthenticationSessionErrorDomain` `_OBJC_CLASS_$_ASAuthorizationController` `_OBJC_CLASS_$_ASAuthorizationPlatformPublicKeyCredentialAssertion` `_OBJC_CLASS_$_ASAuthorizationPlatformPublicKeyCredentialDescriptor` `_OBJC_CLASS_$_ASAuthorizationPlatformPublicKeyCredentialProvider` `_OBJC_CLASS_$_ASAuthorizationPlatformPublicKeyCredentialRegistration` `_OBJC_CLASS_$_ASAuthorizationSingleSignOnProvider` `_OBJC_CLASS_$_ASWebAuthenticationSessionWebBrowserSessionManager` `_OBJC_METACLASS_$_ASAuthorizationController`
- **CoreVideo** (14): `_CVBufferCopyAttachment` `_CVBufferHasAttachment` `_CVIsCompressedPixelFormatAvailable` `_CVPixelFormatDescriptionCreateWithPixelFormatType` `_kCVImageBufferAmbientViewingEnvironmentKey` `_kCVImageBufferCleanApertureHeightKey` `_kCVImageBufferCleanApertureHorizontalOffsetKey` `_kCVImageBufferCleanApertureKey` `_kCVImageBufferCleanApertureVerticalOffsetKey` `_kCVImageBufferCleanApertureWidthKey` `_kCVImageBufferColorPrimaries_DCI_P3` `_kCVImageBufferColorPrimaries_P3_D65` `_kCVPixelFormatComponentRange` `_kCVPixelFormatComponentRange_FullRange`
- **CoreServices** (11): `_AECreateAppleEvent` `_AECreateDesc` `_AEDisposeDesc` `_AEGetParamDesc` `_AEInstallEventHandler` `_AEPutParamDesc` `_AERemoveEventHandler` `_AESendMessage` `_LSRegisterURL` `_MDItemSetAttribute` `__kLSOpenOptionBackgroundLaunchKey`
- **GameController** (11): `_GCControllerDidConnectNotification` `_GCControllerDidDisconnectNotification` `_GCHapticDurationInfinite` `_GCHapticsLocalityDefault` `_GCHapticsLocalityLeftHandle` `_GCHapticsLocalityLeftTrigger` `_GCHapticsLocalityRightHandle` `_GCHapticsLocalityRightTrigger` `_OBJC_CLASS_$_GCDualSenseGamepad` `_OBJC_CLASS_$_GCDualShockGamepad` `_OBJC_CLASS_$_GCXboxGamepad`
- **MediaPlayer** (11, identity missing): `_MPMediaItemPropertyAlbumTitle` `_MPMediaItemPropertyArtist` `_MPMediaItemPropertyArtwork` `_MPMediaItemPropertyPlaybackDuration` `_MPMediaItemPropertyTitle` `_MPNowPlayingInfoPropertyCurrentPlaybackDate` `_MPNowPlayingInfoPropertyElapsedPlaybackTime` `_MPNowPlayingInfoPropertyPlaybackRate` `_OBJC_CLASS_$_MPMediaItemArtwork` `_OBJC_CLASS_$_MPNowPlayingInfoCenter` `_OBJC_CLASS_$_MPRemoteCommandCenter`
- **UserNotifications** (11, identity missing): `_OBJC_CLASS_$_UNMutableNotificationContent` `_OBJC_CLASS_$_UNNotificationAction` `_OBJC_CLASS_$_UNNotificationAttachment` `_OBJC_CLASS_$_UNNotificationCategory` `_OBJC_CLASS_$_UNNotificationRequest` `_OBJC_CLASS_$_UNTextInputNotificationAction` `_OBJC_CLASS_$_UNTextInputNotificationResponse` `_OBJC_CLASS_$_UNUserNotificationCenter` `_UNNotificationAttachmentOptionsTypeHintKey` `_UNNotificationDefaultActionIdentifier` `_UNNotificationDismissActionIdentifier`
- **CoreHaptics** (9, identity missing): `_CHHapticDynamicParameterIDHapticIntensityControl` `_CHHapticDynamicParameterIDHapticSharpnessControl` `_CHHapticEventParameterIDHapticIntensity` `_CHHapticEventParameterIDHapticSharpness` `_CHHapticEventTypeHapticContinuous` `_OBJC_CLASS_$_CHHapticDynamicParameter` `_OBJC_CLASS_$_CHHapticEvent` `_OBJC_CLASS_$_CHHapticEventParameter` `_OBJC_CLASS_$_CHHapticPattern`
- **CoreMedia** (7): `_CMBlockBufferAppendMemoryBlock` `_CMBlockBufferCreateEmpty` `_CMClockConvertHostTimeToSystemUnits` `_CMSetAttachment` `_CMVideoFormatDescriptionCreateFromHEVCParameterSets` `_CMVideoFormatDescriptionGetHEVCParameterSetAtIndex` `_kCMSampleBufferAttachmentKey_ResetDecoderBeforeDecoding`
- **AVFoundation** (6): `_AVVideoCodecKey` `_AVVideoCodecTypeJPEG` `_OBJC_CLASS_$_AVCapturePhotoOutput` `_OBJC_CLASS_$_AVCapturePhotoSettings` `_OBJC_CLASS_$_AVSampleBufferAudioRenderer` `_OBJC_CLASS_$_AVSampleBufferRenderSynchronizer`
- **CFNetwork** (6): `_NSURLSessionTaskPriorityHigh` `_OBJC_CLASS_$_NSHTTPURLResponse` `_OBJC_CLASS_$_NSMutableURLRequest` `_OBJC_CLASS_$_NSURLRequest` `_OBJC_CLASS_$_NSURLSession` `_OBJC_CLASS_$_NSURLSessionConfiguration`
- **Carbon** (6): `_AEProcessEvent` `_CallNextEventHandler` `_GetEventKind` `_ReceiveNextEvent` `_ReleaseEvent` `_RemoveEventHandler`
- **Foundation** (6): `_NSProgressFileOperationKindDownloading` `_NSUserActivityTypeBrowsingWeb` `_OBJC_CLASS_$_NSURLProtocol` `_OBJC_CLASS_$_NSURLQueryItem` `_OBJC_CLASS_$_NSUserActivity` `_OBJC_METACLASS_$_NSURLProtocol`
- **libsandbox.1.dylib** (6): `_sandbox_apply` `_sandbox_compile_string` `_sandbox_create_params` `_sandbox_free_params` `_sandbox_free_profile` `_sandbox_set_param`
- **AVFAudio** (5, identity missing): `_AVSpeechUtteranceDefaultSpeechRate` `_AVSpeechUtteranceMaximumSpeechRate` `_OBJC_CLASS_$_AVSpeechSynthesisVoice` `_OBJC_CLASS_$_AVSpeechSynthesizer` `_OBJC_CLASS_$_AVSpeechUtterance`
- **CoreML** (5, identity missing): `_OBJC_CLASS_$_MLFeatureValue` `_OBJC_CLASS_$_MLModel` `_OBJC_CLASS_$_MLModelConfiguration` `_OBJC_CLASS_$_MLMultiArray` `_OBJC_CLASS_$_MLPredictionOptions`
- **libobjc.A.dylib** (5): `_imp_implementationWithBlock` `_objc_copyCppObjectAtomic` `_objc_setExceptionPreprocessor` `_objc_begin_catch` `_objc_end_catch`
- **CoreGraphics** (4): `_CGDisplayIsInMirrorSet` `_CGRegionCreateWithRect` `_CGSSetWindowCaptureExcludeShape` `_kCGColorSpaceExtendedLinearITUR_2020`
- **CoreMIDI** (4): `_MIDIEventListAdd` `_MIDIEventListInit` `_MIDIInputPortCreateWithProtocol` `_MIDISendEventList`
- **CoreText** (4): `_CTFontCollectionCreateMatchingFontDescriptorsForFamily` `_CTFontCopyVariationAxesInternal` `_CTFontCreateForStringWithLanguage` `_kCTFontVariationAxesAttribute`
- **CoreAudio** (3): `_AudioHardwareCreateAggregateDevice` `_AudioHardwareDestroyAggregateDevice` `_OBJC_CLASS_$_CATapDescription`
- **OpenDirectory** (3): `_kODAttributeTypeStandardOnly` `_kODAttributeTypeUniqueID` `_kODRecordTypeGroups`
- **SystemConfiguration** (3): `_SCNetworkInterfaceCopyAll` `_SCNetworkInterfaceGetBSDName` `_SCNetworkInterfaceGetHardwareAddressString`
- **UniformTypeIdentifiers** (3): `_UTTagClassFilenameExtension` `_UTTagClassMIMEType` `_UTTypeInternetShortcut`
- **libbz2.1.0.dylib** (3): `_BZ2_bzDecompress` `_BZ2_bzDecompressEnd` `_BZ2_bzDecompressInit`
- **AudioToolbox** (2): `_AudioFileInitializeWithCallbacks` `_AudioFileWritePackets`
- **DiskArbitration** (2): `_DADiskClaim` `_DADiskUnclaim`
- **IOKit** (2): `_IOHIDCheckAccess` `_SCDynamicStoreCopyValue`
- **Metal** (2): `_MTLCommonCounterSetTimestamp` `_MTLCommonCounterTimestamp`
- **Accessibility** (1, identity missing): `_OBJC_CLASS_$_AXCustomContent`
- **CoreMediaIO** (1, identity missing): `_CMIOObjectGetPropertyData`
- **CryptoTokenKit** (1, identity missing): `_OBJC_CLASS_$_TKTokenWatcher`
- **IOSurface** (1): `_IOSurfaceSetPurgeable`
- **LocalAuthenticationEmbeddedUI** (1, identity missing): `_OBJC_CLASS_$_LAAuthenticationView`
- **SafariServices** (1, identity missing): `_OBJC_CLASS_$_SFUniversalLink`
- **ServiceManagement** (1): `_SMJobBless`
- **libbsm.0.dylib** (1): `_audit_token_to_euid`
