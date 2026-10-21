# Alchemy providers: how the macOS layer is organized

Alchemy runs macOS arm64 programs on Windows by implementing the macOS
frameworks itself, as host-side **providers** (Wine-style). This page is
the pattern for reading, changing and extending them. The refactor wave
([ALCHEMY_BRIEF_REFACTOR.md](ALCHEMY_BRIEF_REFACTOR.md)) introduced it.

```text
guest Mach-O ─▶ loader / dyld (macho, module_loader) ─▶ exports
                                                        │
   macOS layer: src/alchemy/darwin/<framework>/          │  Darwin semantics only
                                                        ▼
   host API:    include/alchemy/host/*.hpp                host-neutral calls
                                                        │
   backends:    src/alchemy/host/win32/ (null/, later linux/)  the only OS code
engine: fx (src/fx/...), with its own host seams (src/fx/host/)
```

## Where things are

* **`src/alchemy/darwin/<framework>/`:** one directory per macOS framework
  or system library. Each has:
  * `install.cpp` with the framework's install entry point;
  * `README.md`: what the framework covers, its host dependencies, and
    what is explicitly unsupported;
  * `tests/`;
  * subdirectories for large areas (`libsystem/{vm,mach,net,fs,process,malloc,misc}/`,
    `appkit/controls/`).
* **`cmake/darwin/<framework>.cmake`:** the framework's sources and tests,
  picked up automatically by `cmake/AlchemyDarwin.cmake`.
* **`src/alchemy/darwin/kit/`:** the provider helper kits that several
  frameworks share (`completion/`, `support/`, `export_fill/`, `fwgaps/`; see its README).
  * Each kit's install group is one ordered row of the installer table. It
    calls the frameworks' entry points with the kit's shared state.
  * Merging the kits into one helper is planned for a later wave.
* **Provider helpers** next to the installer table, in `src/alchemy/gui/`:
  `fill_provider.hpp` (`gui::fill::Provider`, fill-only registration) and
  `value_provider.hpp` (`gui::values::Provider`: fill-only plus value-class
  properties, singletons and failing completion handlers).
* **The headless framework set** (every build, at process start):
  `src/alchemy/darwin/frameworks.cpp` (`install_darwin_frameworks`) runs each
  framework's headless pieces in order, then the headless additions
  (`darwin/headless_additions.hpp`) and the headless shells
  (`darwin/shells.hpp`). The pieces are in their frameworks' directories and
  use `darwin/headless_provider.hpp`.
* **No organisation by app.** Directories, files, namespaces and functions
  are named by framework and area, never by the program that first needed
  them; comments may say which program first exercised something.
* **The loader and runtime core** stay in `src/alchemy/`: `macho`,
  `module_loader`, `process_context`, `objc_runtime`, `objc_metadata`,
  `bundle`, `runtime_libraries`, `inspection`, `process`.
  * Public headers live in `include/alchemy/`.
  * `include/alchemy/darwin_providers.hpp` is the one ordered install of
    the whole macOS layer, shared by `run_program`, `alchemy doctor` and
    the provider tests.
* **GUI providers** (alchemy-fx only, `ALCHEMY_WITH_GUI`) are installed
  by `install_darwin_gui` from one ordered table in
  `src/alchemy/gui/install.cpp`. Each row is one framework installer, and
  the registration audit reports owners by row name.

## One owner per name

* **Every export** (function or data), **every Objective-C class** and
  **every native method** (class, selector, + or -) has exactly one
  registering owner: the framework it belongs to.
* **The registration audit** checks it. Run it with
  `ALCHEMY_REGISTRATION_AUDIT=1 alchemy-fx doctor <any Mach-O>`, or
  `=<file>.json` to write JSON. It lists every name registered more than
  once, with all owners as `<installer row> <file>:<line>`.
* **The test** `alchemy_gui_registration_audit` holds two ratchets:
  duplicates and skipped fill-only registrations. The target and the
  end state of the refactor are 0 for both.
* **Install order then doesn't matter,** because no two rows register the
  same name. The table keeps its order only where one framework uses
  another during installation, for example AppKit drawing through the
  window server.

### Registering

* **Use the owning framework's helpers.** They pass their caller's source
  location to the audit, so owners show up as provider lines.
* **Functions and data:** `ProcessContext::install_function` and
  `install_export`.
* **Classes:** `ObjcRuntime::register_native_class`, then
  `ObjcRuntime::publish_class(cls, library)` to publish
  `_OBJC_CLASS_$_`/`_OBJC_METACLASS_$_` under the framework's install name.
  `publish_class` skips an export already bound to the same class.
* **Methods:** `ObjcRuntime::add_native_method` on the class the framework
  owns.
* **Host state** goes in `ObjcRuntime::host_state`, never in the guest
  instance. Methods a guest subclass may override are called through
  `ObjcRuntime::send`.

### When two frameworks touch the same name

* **A plain override.** A headless shell exists for `alchemy.exe` and the
  GUI provider replaces it in alchemy-fx, for example NSRunLoop.
  * The shell goes in `<x>_headless.cpp` and is listed in
    `ALCHEMY_HEADLESS_ONLY_SOURCES` by the framework's fragment, so it is
    compiled into `alchemy.exe` only.
  * One function, `install_<x>_shell`, is declared in the framework
    header. It is defined for real in `<x>_headless.cpp`, and empty in an
    fx-only `<x>_fx.cpp` that names the GUI provider owning the names.
  * Each build has one owner per name.
* **A decorator.** One framework adds behaviour to another's export, for
  example Security's CF types in `CFGetTypeID`, or network URLs in
  `CFURL*`/`NSURL`.
  * The owning framework keeps the single export and offers an extension
    point (`CoreFoundation::extend`).
  * Each extension claims a disjoint domain, such as a type-id range or
    URL schemes. Registration rejects overlaps, so order cannot change
    the outcome.
  * The audit lists extensions as their own kind.
* **Fill-only** registration ("install only if nobody has it") is only for
  stubs clearly marked as stubs:
  * the explicit failure values and explicit "unsupported" stops of
    `gui::fill::Provider`;
  * every fill-only registration is listed by the audit (`fill-only`), and
    one that loses to another owner (`fill-skipped`) is a bug: dead code
    or a second implementation.

## Adding an API

1. **Find its framework**, by its install name, such as
   `/System/Library/Frameworks/AppKit.framework/...` or
   `/usr/lib/libSystem.B.dylib`. The code goes in that framework's
   directory, in the file for its area, or in a new file listed in the
   framework's `cmake/darwin/<framework>.cmake`.
2. **Write Darwin semantics only.** Use Apple's public-header constants and
   arm64 layouts, test sizes and offsets, and keep Darwin's error values
   (errno, `kern_return_t`, `OSStatus`).
3. **Use the host API for OS work:** files, sockets, processes, memory,
   time, crypto, UI and audio (`include/alchemy/host/<area>.hpp`).
   * Translate `host::Error` into the Darwin error at the call site.
   * Never include an OS header outside `src/alchemy/host/<backend>/`;
     `alchemy_host_boundary` fails if you do.
   * If the host API lacks a call, add it to the area's header and
     implement it in **every** backend, `null` included, in the same
     commit.
4. **Register it in the framework's installer.** Run the audit: the new
   name must have one owner, and the ratchets must not rise.
5. **Test it** in the framework's `tests/`. A bug fix gets a test that
   fails on the old code.
6. **Real behaviour first.** Otherwise fail explicitly, with the documented
   failure value or an "unsupported" stop. Never invent a success.

## Frameworks with no host service

Some frameworks have nothing on the host to stand on, yet programs link
them: CoreML, Vision, UserNotifications, AuthenticationServices,
ScreenCaptureKit, MediaPlayer and others. Each is still its own provider:

* `src/alchemy/darwin/<framework>/install.cpp`: a small `values::Provider`
  subclass that registers the identity and answers the documented
  "unavailable" way: nil plus an NSError in the framework's domain, a
  completion handler called with that error, "authorization denied", an
  empty list, or an explicit "unsupported" stop. Value classes (content,
  requests, settings) are real value objects. Constants carry the values of
  Apple's public headers; a value that could not be checked against a header
  is marked "(unverified)".
* Its own row of the installer table (`install_<framework>`).
* `README.md` says what is real, what is unavailable as documented and why,
  and what host service would make it real (for example UserNotifications
  through Windows toast notifications).
* `tests/<framework>_tests.cpp`: a case of the shared test executable (see
  below).

Making one real later replaces the answers in that one directory; nothing
else changes.

## Adding a framework

1. Create `src/alchemy/darwin/<framework>/` with `install.cpp`,
   `README.md` and `tests/`.
2. Add `cmake/darwin/<framework>.cmake`:
   * headless sources go in `target_sources(alchemy_core PRIVATE ...)`;
   * GUI sources are appended to `ALCHEMY_GUI_SOURCES`;
   * tests are registered through `alchemy_gui_register_tests` for GUI
     tests, or `add_test` for headless ones;
   * a small framework registers its case in the shared executable instead
     of linking one more test program:
     `alchemy_gui_add_framework_case(<name> <source>)`. The source defines
     `const fill_test::Case <name>_case("<name>", [](Fixture& f) {...});`
     (`src/alchemy/gui/tests/framework_case.hpp`). Every case runs alone as
     ctest `alchemy_gui_<name>`, and all of them together as
     `alchemy_gui_framework_cases`.
3. Add one row to the installer table in `src/alchemy/gui/install.cpp`,
   or call its core installer from `DarwinProviders` for a headless
   framework. Make its identity known to the loader with
   `ProcessContext::add_provider_library` when no guest image provides it.
4. Run `ctest -R "^alchemy_"`, cloaked, and the audit.

## Host layer

* **The API:** `include/alchemy/host/host.hpp` has the rules and the
  common types:
  * `host::Error`, which names POSIX error kinds, not their values;
  * `Result<T>`, `Status`, `Handle` and `OwnedHandle`.
* **Areas:** `vm`, `sections`, `files`, `net`, `dns_sd`, `http`,
  `process`, `ipc`, `sync`, `time`, `locale`, `crypto`, `system`, `ui`,
  `audio`, `midi`, `gpu` and `debug` (crash reports).
* **Owners:** each header names its owning group, and calls written for
  another group are listed in that header.
* **Backend code:** `src/alchemy/host/<backend>/gui/` holds the fx-only
  areas: the window server's Win32 half, D2D, DirectWrite and WIC, WASAPI,
  WinMM and Vulkan surfaces.
* **Backends:** CMake picks one with `ALCHEMY_HOST=win32|null`. Calls are
  free functions or small concrete classes, with no virtual dispatch.
* **The network policy** (`ALCHEMY_NETWORK`) sits above the backends, in
  `src/alchemy/host/common/`.
* **The null backend** fails every call. A null build puts poison copies
  of the OS headers first on the macOS layer's include path. That build
  succeeding proves the boundary.
* **Linux:** see [ALCHEMY_HOST_LINUX.md](ALCHEMY_HOST_LINUX.md) for what a
  Linux backend would map each area to.

## Paths before the refactor

Older documents (audits, gap lists and briefs of earlier waves) name the
paths from before the refactor. Read them through this map.

| Before | Now |
| --- | --- |
| `src/alchemy/darwin_*.cpp`, `darwin_network.inc`, `darwin_process.inc`, `cross_process_memory.cpp`, `shared_wait.cpp`, `machsys/` | `src/alchemy/darwin/libsystem/{vm,mach,net,fs,process,malloc,misc}/` (`darwin_services.cpp` is split there; see its README) |
| `darwin_dispatch.cpp` | `src/alchemy/darwin/dispatch/` |
| `darwin_frameworks.cpp`, `frameworks/installer_frameworks.cpp`, `darwin/identities/{frameworks,installer_frameworks}.cpp` | `src/alchemy/darwin/frameworks.cpp` and `headless_additions.cpp`; each piece in its framework (`darwin/headless_additions.hpp` lists them) |
| `darwin/identities/identities.cpp` | one directory per framework: `darwin/{avfaudio,accessibility,authenticationservices,corehaptics,coreml,coremediaio,cryptotokenkit,localauthenticationembeddedui,mediaplayer,safariservices,usernotifications,vision,screencapturekit,skylight}/` |
| `darwin/identities/framework_gaps.cpp` | `darwin/applicationservices/install.cpp`, `diskarbitration/install.cpp`, `coreservices/carbon_core.cpp`, `corefoundation/time_zones_and_uuids.cpp`, `foundation/file_attribute_keys.cpp`, `accelerate/bnns.cpp` |
| `darwin/identities/system_gaps.cpp` | `darwin/coreservices/launch_services.cpp`, `security/cssm_and_tasks.cpp`, `securityinterface/`, `systemconfiguration/errors_and_aliases.cpp`, `opendirectory/sessions.cpp`, `src/alchemy/objc/exception_types.cpp` |
| `darwin/identities/qt_imports.cpp` | `darwin/appkit/constants_and_shells.cpp`, `foundation/threads_and_localizations.cpp`, `coretext/font_data.cpp`, `quartzcore/shape_layer_shell.cpp`, `metal/stage_input_output_shell.cpp`, `coregraphics/color_space_property_list.cpp`, `coreservices/icon_refs.cpp`, `applicationservices/icon_services.cpp` |
| `darwin/identities/{gl_stubs,accelerate,disk_arbitration,open_directory,service_management}.cpp` | `darwin/opengl/no_context.cpp`, `accelerate/vdsp.cpp`, `diskarbitration/{sessions,claims}.cpp`, `opendirectory/constants.cpp`, `servicemanagement/job_bless.cpp` |
| `darwin/kit/chrome/` (`chrome::Frameworks`, `chromesupport::mach`) | `src/alchemy/gui/value_provider.{hpp,cpp}` (`values::Provider`); Mach IPC is `libsystem::mach` |
| `core_foundation.cpp`, `object_store.cpp` | `src/alchemy/darwin/corefoundation/` |
| `foundation_bridge.cpp`, `foundation_extra.cpp` | `src/alchemy/darwin/foundation/` |
| `appkit/`, `windowserver/`, `runloop/`, `quartzcore/`, `metal/`, `msl/`, `printing/` | `src/alchemy/darwin/<same name>/` (the window server's Win32 half is in `src/alchemy/host/win32/gui/`) |
| `quartz2d/`, `imageio/` | `src/alchemy/darwin/coregraphics/` (D2D, DirectWrite and WIC are behind `host::gpu`) |
| `coretext/` | `src/alchemy/darwin/coretext/` |
| `media/`, `kit/completion/media/` | `src/alchemy/darwin/media/` (WASAPI and WinMM are behind `host::audio` and `host::midi`) |
| `devices/`, `export_fill/iokit_registry.cpp` | `src/alchemy/darwin/iokit/` |
| `security/`, `gss/` | `src/alchemy/darwin/security/` |
| `network/` | `src/alchemy/darwin/network/` (WinHTTP is behind `host::http`) |
| `sysconfig/` | `src/alchemy/darwin/systemconfiguration/` |
| `qlabsupport/`, `qtsupport/`, `chromesupport/`, the dependency kit, `fwgaps/` | Their providers are in their frameworks. The shared kits and install groups are in `src/alchemy/darwin/kit/{completion,support,export_fill,fwgaps}/`; `chromesupport`'s is `gui::values::Provider`. |
| `qlabsupport/graphics/swift/` | `src/alchemy/darwin/swift/` |
| `chromesupport/libsystem/network_policy.cpp` | `src/alchemy/host/common/network_policy.cpp` |
| `cmake/gui/<x>.cmake` | `cmake/darwin/<framework>.cmake` and `cmake/darwin/kit_*.cmake`; only `cmake/gui/objc.cmake` is left |
