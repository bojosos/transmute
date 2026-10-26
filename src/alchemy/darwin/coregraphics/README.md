# CoreGraphics

**Covers:** Quartz 2D drawing over Direct2D (contexts, paths, colors, color
spaces, gradients, images, geometry), ImageIO over WIC (`imageio/`), and
AppKit's drawing classes on top of it (`appkit_drawing.cpp`:
NSGraphicsContext, NSBezierPath, NSString / NSImage drawing). Quartz
Display Services over the window server's screens (`displays.cpp`: display
lists, bounds, sizes, modes, reconfiguration callbacks). Also the
CoreGraphics additions Qt's Cocoa plugin binds (`cg_extra.cpp`: event taps)
and the Qt WebEngine graphics gaps (`graphics_gaps.cpp`). The named RGB and
gray color spaces' ICC data (`CGColorSpaceCopyICCData`) are project-authored
ICC v4 profiles built from the standards' parameters (`icc_profiles.cpp`);
CMYK, Lab and XYZ have none.

**Headless:** `coregraphics_headless.cpp` (alchemy.exe only) holds the
headless answers for the names this provider owns in alchemy-fx: named color
spaces, CGColor values, geometry, the display queries (no display), and the
explicit failures of drawing and image codecs. `coregraphics_fx.cpp` is its empty alchemy-fx twin
(`darwin/shells.hpp`).

**Host dependencies:** `host::gpu`: the 2D surface, geometry and brushes
(`surface.cpp` wraps them; Direct2D on win32, d2d1.dll loaded at run time),
image frames for ImageIO (WIC), fonts for glyph drawing; the window
server's screens; `host::ui` (input idle time, keyboard kind).

**Events (`graphics_gaps.cpp`):** CGEvent creation (keyboard, mouse,
scroll), copies, every field getter and setter, flags, timestamps and
Unicode strings, and CGEventSource state and user data are real. Posting
reaches only this process (CGEventPostToPid / PostToPSN for its own pid go
to `-[NSApplication postEvent:atStart:]`); other targets fail explicitly.

**Displays:** display modes report their IOKit flags; capturing displays
returns kCGErrorCannotComplete (the window server shares every display) and
releasing succeeds.

**Unsupported:** JPEG 2000 images (ImageIO reports them as undecodable).

**Display modes:** each display reports one mode, its current one (the host
gives no mode list); `CGDisplaySetDisplayMode` accepts only that mode.
`CGDisplayPixelsWide`/`High` answer the screen's pixel size; macOS answers
the current mode's width, which is in points on a scaled mode (unverified).
Reconfiguration callbacks are kept but never called.

## Moved from darwin/identities/

* `color_space_property_list.cpp`: CGColorSpaceCopyPropertyList answers NULL (the support kit).
* `no_displays.cpp`, `event_source.cpp`: headless, every build: per-display queries for no display, CGEventSourceKeyState (no keyboard).
