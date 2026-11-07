# ScreenCaptureKit

`/System/Library/Frameworks/ScreenCaptureKit.framework/Versions/A/ScreenCaptureKit` (alchemy-fx), and the CoreGraphics
window lists (`CGWindowListCopyWindowInfo`, `CGWindowListCreate`, `CGWindowListCreateDescriptionFromArray`,
`CGWindowListCreateImage`, `CGWindowListCreateImageFromArray`).

**Scope: the process's own content, nothing else.**
* Everything listed and everything captured is one of the following:
  * the displays (the screens the window server reports: the virtual display the guest's windows are on);
  * the guest's own windows: its visible NSWindows, except those whose `sharingType` is NSWindowSharingNone;
  * its own application.
* The pixels come from what this process presented in its own windows (`darwin/windowserver/presented_pixels.hpp`). The
  host never hands pixels back, so **the Windows desktop is never captured**: no host window, no other process (other
  Alchemy guests included), no title bar or frame drawn by Windows.
* Listings leave everything else out; they are not errors.
* `CGPreflightScreenCaptureAccess` and `CGRequestScreenCaptureAccess` answer granted
  (`coregraphics/graphics_gaps.cpp`). The grant covers only this scope.

**Real:**
* **SCShareableContent:**
  * `getShareableContent…` (every variant, including the current-process one) completes on the main queue with the
    displays, the own windows (on screen only when asked) and the own SCRunningApplication.
  * `+infoForFilter:` gives the filter's style, scale and content rectangle.
  * SCWindow `frame` is the content area in CG coordinates (the title bar is the host's).
* **SCContentFilter:** the desktop-independent window and the display forms (excluding or including windows,
  including or excluding applications with excepted windows). Other applications have no content.
* **SCScreenshotManager:**
  * `captureImageWithFilter:configuration:completionHandler:` and `captureImageInRect:completionHandler:` make CGImages.
  * A display frame is the own windows composited back to front over opaque black; a window frame is the window's
    content.
  * The configured `width`/`height` (else the area at the display scale) and `sourceRect` are used.
* **SCStream:**
  * After `startCapture…`, each screen output gets a frame on its sample handler queue (the main queue without one)
    every `minimumFrameInterval` (one 60 Hz frame when zero), with at most `queueDepth` (default 8) undelivered.
  * A frame is a CMSampleBuffer of an IOSurface-backed 'BGRA' CVPixelBuffer.
  * Its attachments are SCStreamFrameInfoStatus complete, DisplayTime, ScaleFactor, ContentScale and ContentRect.
  * `updateConfiguration:`, `updateContentFilter:`, `stopCapture…` and `removeStreamOutput:` are provided.
* **The CoreGraphics window lists:** the same windows, as kCGWindow* dictionaries (number, owner PID and name, layer,
  bounds, name, on-screen, alpha, sharing and store type) or CGWindowID arrays. The options (on screen only,
  above/below a window, including a window) are honoured. The images compose the same pixels over transparency.
* **SCContentSharingPicker:** the singleton, with its `active` and `maximumStreamCount` properties and observers. No
  picker window is shown.

**Unavailable, as documented:**
* Audio: adding an audio or microphone output fails with SCStreamErrorUserDeclined, and no audio is captured.
* CGDisplayStream.
* Frames are delivered at the interval whether or not anything changed (always status complete; no idle frames).
* Areas a window shows through CAMetalLayer views (Vulkan presentation) keep what was last presented there in software.
* The cursor is never drawn.

**Host dependencies:** none (the window server's screens and the process's own presented pixels).

**Registration:** `install_screencapturekit`, one row of the installer table after the support kit. It uses the
support kit, and registers the ScreenCaptureKit classes and the CoreGraphics window-list functions. Chrome 155 first
linked the framework.

**Tests:**
* `tests/screencapturekit_tests.cpp` (ctest `alchemy_gui_screencapturekit`): the constants, the picker and the grant.
* `tests/own_content_tests.cpp` (ctest `alchemy_gui_screencapturekit_own_content`) uses an own window, an unshared own
  window and a host window with green pixels, and checks:
  * the listings hold only the own window;
  * the screenshot, the window-list image and a stream frame show the own window and black or transparency elsewhere;
  * no green pixel appears anywhere.
