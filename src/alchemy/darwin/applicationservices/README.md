# ApplicationServices

`/System/Library/Frameworks/ApplicationServices.framework` (the umbrella).

* **Real:** the identity, and its re-exports: a symbol bound to the umbrella
  resolves through the CoreGraphics, CoreText, ImageIO, ColorSync and
  CoreServices providers (`install.cpp`, a `base_resolver` link). HIServices'
  Process Manager (GetCurrentProcess, SetFrontProcess, TransformProcessType,
  GetFrontProcess, SameProcess, GetProcessPID, CopyProcessName,
  ProcessInformationCopyDictionary) is a headless addition
  (`process_manager.cpp`, both builds). The guest is the only Mac process:
  GetFrontProcess names it while its application is active and otherwise
  answers procNotFound (a host program is in front); the name is the
  bundle's CFBundleName, else CFBundleExecutable, else the executable's file
  name, else the program name (empty when there is none of these); the information dictionary holds CFBundleName, CFBundleIdentifier,
  CFBundleExecutable (the path), BundlePath (for an .app), LSBackgroundOnly,
  LSUIElement, pid, PSN and IsHiddenAttr (bundle keys only when bundled), and
  the other processes get procNotFound / NULL.
* **Real, for this process only:** HIServices' accessibility client
  (`ax_client.cpp`: AXUIElement, AXObserver) over the process's own
  accessibility tree. AXUIElementCreateApplication(getpid()) is the
  application; its windows, their views and NSAccessibilityElements follow.
  * Attribute values come from an object's -accessibilityAttributeValue:
    override, else its NSAccessibility properties, else AppKit's defaults:
    the role of the standard classes, a window's or button's title, a
    field's string and a slider's number, a control's enabled state, the
    first responder's focus, AXPosition / AXSize on the screen (AX
    coordinates: top-left origin), and children that look through plain
    NSViews. The application answers AXWindows, AXFocusedWindow,
    AXMainWindow, AXFocusedUIElement and AXFrontmost;
    AXUIElementCreateSystemWide answers AXFocusedApplication and
    AXFocusedUIElement.
  * Actions: AXPress (a custom -accessibilityPerformPress, else a button's
    -performClick:), AXRaise (a window), and the informal action names.
    Settable: AXValue (a field's string, a slider's number, else the informal
    setter) and AXFocused. Parameterized attributes go to the informal
    methods.
  * Observers: NSAccessibilityPostNotification / ...WithUserInfo for an
    observed element (an observed application element gets every element's)
    calls the callback on the main run loop. The run loop source an observer
    answers exists so it can be added, but nothing depends on it.
* **Unavailable, as documented:** other processes' accessibility. No
  accessibility server exists on the host, so an element of another process
  answers kAXErrorCannotComplete and observers of another process cannot be
  made (kAXErrorCannotComplete). The process is not a trusted accessibility
  client (AXIsProcessTrusted NO, `darwin/coreservices/services.cpp`, which
  also keeps AXValue and the text markers), and nothing outside the process
  (a screen reader on the host) sees the tree.
* **Unavailable, as documented:** Icon Services draws nothing:
  `PlotIconRefInContext` stops with an explicit "unsupported"
  (`icon_services.cpp`).
* **Host dependencies:** none.
* **Registration:** `install_applicationservices`, one installer-table row
  in the position of the former `install_framework_gaps` (a renderer plug-in's images
  first bound through the umbrella); `icon_services.cpp` and `ax_client.cpp`
  are installed by the support kit (`kit/support/install.cpp`), Icon Services
  at its old position and the accessibility client after it.
* **Tests:** `tests/applicationservices_tests.cpp` (ctest
  `alchemy_gui_applicationservices`); the accessibility client is tested over
  real AppKit windows in `darwin/appkit/tests/controls_behaviour_tests.cpp`
  (`ax_client`, ctest `alchemy_gui_appkit_controls_behaviour`).
