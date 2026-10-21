# AppKit

**Covers:** NSApplication, NSWindow, NSView, NSEvent, resources (NSImage,
NSColor, NSCursor, pasteboards), standard controls, text, tables and
outlines, panels, documents, nib loading, asset catalogs and RTF; Carbon /
HIToolbox input. Also the additions installed by other entry points, still
called from their aggregators: QLab's controls (`controls/`) and windows
(`windows/`), Qt's (`appkit_extra.cpp`, `accessibility.cpp`,
`pasteboard.cpp`), Chrome's (`appkit_gaps.cpp`, `menus.cpp`,
`window_additions.cpp`, `spelling.cpp`, `selectors.cpp`), and
`application_services.cpp` and `bindings.cpp`. NSColor's named colours are
AppKit's (`resources.cpp`, `graphics.cpp`, `controls/appearance.cpp`).
Drawing (NSGraphicsContext, NSBezierPath, string and image drawing) is
CoreGraphics' `appkit_drawing.cpp`; NSFont and the text attribute keys are
CoreText's.

**Headless:** `appkit_headless.cpp` (alchemy.exe only), empty
`appkit_fx.cpp` in alchemy-fx (`darwin/shells.hpp`).

**Host dependencies:** the window server (`darwin/windowserver/`) and
`host::ui` (alerts, file dialogs, the shell, appearance and accessibility
settings, spell checking), `host::gpu` (image files, system text
rasterizing), `host::locale` (case mapping). On win32: user32, gdi32,
shell32, ole32, windowscodecs, comctl32 (TaskDialogIndirect), the Windows
Spell Checking API.

**Unsupported (explicit failures):** for example NSCollectionView sections
other than 0, horizontal NSCollectionViewFlowLayout scrolling, planar
NSBitmapImageRep data, and NSWindow frame autosave without NSUserDefaults.

## Moved from darwin/identities/

* `constants_and_shells.cpp`: pasteboard, document and notification constants, NSBitsPerPixelFromDepth, NSStatusBar/NSStackView shells (the support kit).
* `base_classes.cpp`, `input_sources.cpp`, `color_alpha.cpp`, `carbon_events.cpp`, `class_shells.cpp`: headless, every build: the base classes and NSApp, TIS keys, +[NSColor ignoresAlpha], Carbon events (none delivered), and the AppKit class shells.
