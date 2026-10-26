# QuickLookThumbnailing

`/System/Library/Frameworks/QuickLookThumbnailing.framework/Versions/A/QuickLookThumbnailing` (alchemy-fx).

* **Real:** `QLThumbnailGenerator` makes thumbnails of image files with ImageIO
  (`CGImageSourceCreateThumbnailAtIndex`, scaled to the request's size times
  its scale) and, when the request allows icons, answers other files with their
  icon (`-[NSWorkspace iconForFile:]`). `QLThumbnailGenerationRequest` and
  `QLThumbnailRepresentation` are real value objects; the best representation
  can be saved to a file (`CGImageDestination`).
* **Unavailable, as documented:** files that are neither images nor allowed an
  icon fail with `QLThumbnailErrorDomain` code 0 (generation failed; the code
  value is unverified). There are no Quick Look generator plug-ins, so document
  previews (PDF pages, movies) are not thumbnailed. Requests complete before
  the generating call returns, on the calling thread.
* **Host dependencies:** none (ImageIO and AppKit providers).
* **Registration:** `install_quicklookthumbnailing`, one row of the installer
  table after QuickLookUI. VS Code's Electron framework first linked it (weakly).
* **Tests:** `tests/quicklookthumbnailing_tests.cpp` (ctest
  `alchemy_gui_quicklookthumbnailing`).
