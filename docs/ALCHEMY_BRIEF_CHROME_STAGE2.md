# Brief: Chrome stage 2, windowed Chrome with CPU rendering

The rules for running this wave are in
[ALCHEMY_WAVE_RULES.md](ALCHEMY_WAVE_RULES.md). For background, see
[ALCHEMY_CHROME_AUDIT.md](ALCHEMY_CHROME_AUDIT.md): "Wave 1 result" and the
AppKit section.

* **When:** after the provider refactor
  ([ALCHEMY_BRIEF_REFACTOR.md](ALCHEMY_BRIEF_REFACTOR.md)) has landed. The
  paths below use the post-refactor layout (`src/alchemy/darwin/<framework>/`,
  host calls through `include/alchemy/host/`).
* **New OS work goes through the host layer**, so it carries over to a
  Linux backend. Window, input and display work goes behind `host::ui`.

## Goal

Run Chrome 155 without `--headless`, using the wave's usual flags plus
`--disable-gpu --use-mock-keychain --js-flags=--jitless --no-sandbox`, with
`ALCHEMY_NETWORK=loopback`, cloaked, and no injected input.

* It opens one browser window showing a local `file://` page.
* The page's pixels are captured through Alchemy's cloaked capture
  (`ALCHEMY_CONTENTS_CAPTURE`). The capture shows the page text and the
  browser frame (tab strip, toolbar), rendered by Chrome's software
  compositor.
* Then it quits cleanly with exit code 0, for example through
  `--remote-debugging-pipe` or a timed `window.close()` in the page.
  Don't inject input.
* `--single-process` is optional. Renderers are child processes either
  way, and wave 1's cross-process Mach IPC carries them.

## What is known (from wave 1)

* **The browser builds a window when headless.** Headless strict runs
  already construct `BrowserCrApplication` and a browser window, and run
  the main-thread run loop. They also build the menus, including NSMenu
  copying, and the titlebar accessory controllers, which are stored but
  not drawn.
* **Chrome subclasses these AppKit classes:** NSApplication, NSWindow,
  NSWindowController, NSView, NSViewController, NSCursor, NSTrackingArea,
  NSAnimation, NSAccessibilityElement and
  NSTitlebarAccessoryViewController.
  * Its frame view is `BrowserNativeWidgetWindow` on `NSThemeFrame`
    *(design)*.
  * `NSThemeFrame`/`NSNextStepFrame` are registered, subclassable classes,
    but they don't yet draw or lay out a frame.
* **The GPU process runs in-process with `--disable-gpu`**, using the
  software compositor (viz). Output reaches the window through IOSurfaces
  and CALayers *(design: `ui::CALayerTreeCoordinator` and
  `DisplayCALayerTree`)*.
  * `CAContext`/`CALayerHost` are not registered, so Chrome takes the
    IOSurface-contents path. That is the intended route.
* **The pieces already exist from QLab, Blender and Qt:**
  * the Win32 window server with cloaking;
  * AppKit windows, views and events;
  * QuartzCore's presenter for `CALayer.contents`;
  * IOSurface;
  * CVDisplayLink;
  * cloaked capture.

## Groups (suggested)

* **w1 windows:** NSWindow/NSView subclass behaviour, `NSThemeFrame` and
  `NSNextStepFrame` frame views, full-size content and titlebar layout,
  window levels and ordering, fullscreen, and NSScreen. Owns
  `darwin/appkit/` window and view files.
* **w2 events:** the NSEvent pipeline from the host window server, mouse
  and keyboard and IME (`NSTextInputClient`, marked text), tracking areas,
  cursors (NSCursor subclassing), tool tips, menus (the main menu and
  context menus) and drag and drop. Owns the `darwin/appkit/` event, menu
  and cursor files, and `host::ui` input.
* **w3 compositing:** IOSurface-backed `CALayer.contents` from viz,
  CALayer trees, `CATransaction` and the commit/present timing, display
  link and vsync, colour spaces and the HDR flags Chrome queries, and
  capture. Owns `darwin/quartzcore/`, IOSurface and `host::ui` display
  output.
* **w4 accessibility and misc:** NSAccessibility constants and the AX
  element tree that Chrome builds, which must not stop the run. Also
  NSWorkspace, notifications and the services menu.

## Done when

* The goal's capture is checked in as a test artifact description, not
  as image bytes; images stay out of git under `artifacts/`. The capture
  file is reported.
* Chrome exits with 0 in 3 of 3 runs.
* No new stops appear in headless `--dump-dom`.
* There's a unit test for every AppKit and QuartzCore behaviour added.
* The regression check before landing passes: Blender's and QLab's
  windowed runs are unchanged.

## Not in scope

* GPU rasterization (stage 4), the JIT (stage 3) and the sandbox.
