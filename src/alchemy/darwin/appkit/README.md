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
Also -[NSSavePanel ok:] / -cancel: while the Windows file dialog is open,
-[NSImageRep CGImageForProposedRect:context:hints:] of a representation that
is not a bitmap, and +[NSColor colorWithColorSpace:components:count:] in
colour spaces other than gray, RGB and CMYK.

**Unavailable, as documented:** remote notifications (no Apple Push
Notification service: -registerForRemoteNotifications fails through the
delegate with an NSCocoaErrorDomain error), and Finder tags in save panels
(-tagNames are the ones the application set); restorable state is encoded
and restored on request but not kept between launches.

**Toolbars and tabs (`window_tabs.cpp`):** a window's toolbar with items
and its tab bar show in a strip between the title bar and the content view
(the window grows below its top edge; empty, title-bar styling toolbars take
no space). Toolbar items are clicked, validated by -update and laid out by
kind; the size mode, style and centred items are kept, not drawn, and a
toolbar with items over a full-size content view stops explicitly. Window
tabs are real NSWindowTabGroups: tabs share a frame, the selected one is on
screen, the tab bar selects and closes tabs and offers -newWindowForTab:;
preferred tabbing joins windows with the same identifier, automatic tabbing
does not (Windows has no tab preference), and the tab overview and the
toolbar customization palette stop explicitly. -performWindowDragWithEvent:
runs the window server's move loop; NSWindowSharingNone keeps a window out
of screen capture.

**C functions (`c_functions.cpp`):** rectangle lists, tiled rectangles and
the classic bezels (gray edge sequences around a filled middle), nine-part
images, dotted frames, highlights, NSDrawBitmap and NSReadPixel draw and read
through the current context; NSCountWindows / NSWindowList list the
application's windows front to back; the depth helpers use AppKit's
NSWindowDepth encoding; NSRunAlertPanel and its forms run an NSAlert,
NSBeginAlertSheet a sheet; NSDisableScreenUpdates holds window presentation
until updates resume. There is no Services menu (item visibility is kept),
NSShowAnimationEffect draws nothing and tells its delegate at once, and
NSGetAlertPanel, NSCopyBits and NSGetWindowServerMemory stop explicitly.

**Nib keys:** the decoders read the keys Interface Builder archives for the
controls, cells, tables, scroll views, boxes and menus QLab's nibs use; each
decoder names the keys it leaves out and why (design-time values, features
not modelled such as Writing Tools or text completion). The control-level
copies of cell settings are applied to the cell; NSControlContinuous and
NSStepper's range keys are not (archived as the complement of the cell's
flag, and as zeros). Kept but without effect: table styles and automatic
row heights, pop-up arrow positions, button periodic delays (buttons do not
repeat while held), scroll-view magnification bounds (documents draw at
magnification 1; another magnification stops explicitly).
NSUserDefaultsController applies changes immediately even when a nib says
NSAppliesImmediately NO (deferred application is not modelled).

## Moved from darwin/identities/

* `constants_and_shells.cpp`: pasteboard, document and notification constants, NSBitsPerPixelFromDepth, NSStatusBar/NSStackView shells (the support kit).
* `base_classes.cpp`, `input_sources.cpp`, `color_alpha.cpp`, `carbon_events.cpp`, `class_shells.cpp`: headless, every build: the base classes and NSApp, TIS keys, +[NSColor ignoresAlpha], Carbon events (none delivered), and the AppKit class shells.
