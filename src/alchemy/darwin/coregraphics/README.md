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

**Fades, gamma, input monitoring (the media apps):**
* There is no display fade reservation: CGAcquireDisplayFadeReservation
  answers kCGErrorNoneAvailable, so every token is invalid.
* CGGetDisplayTransferByTable reads an identity table, and
  CGSetDisplayTransferByTable cannot complete: the host's gamma is never
  changed.
* Listening to other applications' input is not granted.
* CGDisplayStream* create nothing.
* Font variations: CGFontCopyVariationAxes / CGFontCopyVariations and CTFontCopyVariationAxes / CTFontCopyVariation read a variable font's `fvar` axes, named from its `name` table (English). Faces are at their default instance, so the current variation is each axis's default. A font without `fvar` gives NULL. CGFontCreateCopyWithVariations makes no variant copies (NULL).
* CGFontCopyTableTags probes the known table tags.
* kCGColorSpaceDisplayP3_PQ / _HLG are names only: their transfer curves are
  not modelled, so creating them answers NULL.

**Display modes:** each display reports one mode, its current one (the host
gives no mode list); `CGDisplaySetDisplayMode` accepts only that mode.
`CGDisplayPixelsWide`/`High` answer the screen's pixel size; macOS answers
the current mode's width, which is in points on a scaled mode (unverified).
Reconfiguration callbacks are kept but never called.

**Colour space variants:** CGColorSpaceCreateExtended / Linearized /
ExtendedLinearized / CopyWithStandardRange map the named spaces onto their
variants (NULL where a space has none, as Display P3 has no linear variant
here); CGColorSpaceCopyBaseColorSpace; no space made here has an HDR transfer
function (IsHDR / IsPQBased / IsHLGBased / UsesITUR_2100TF are false);
CGColorSpaceCreateWithColorSyncProfile reads the profile's ICC data.

**PDF reading** (`pdf/pdf_parser.cpp`, `pdf_document.cpp`): CGPDFDocument
reads PDF files written to the PDF specification: cross-reference tables
and streams (with /Prev and hybrid sections), object streams, and files
whose cross-reference data is broken (reconstructed from the object
headers). Stream data decodes FlateDecode (with predictors), LZWDecode,
ASCIIHex, ASCII85 and RunLength; DCT and JPX data stays encoded
(CGPDFStreamCopyData reports the format); CCITT, JBIG2 and Crypt filters
give no data. Pages, inherited boxes and rotation, drawing transforms,
the CGPDF object, dictionary, array, stream and string functions (text
strings and dates included), content streams, resources, operator tables
and scanners are real. The CGPDFObjectType, CGPDFBox and CGPDFDataFormat
values follow their documented order (unverified). Not provided:
decrypting encrypted documents (they stay locked; their structure reads),
the document outline and access permissions.

**Drawing PDF pages** (`pdf/pdf_render.cpp`): CGContextDrawPDFPage
interprets the page's content in the context's current user space, clipped
to the crop box, through the same drawing entry points as everything else
(so a PDF page also draws into a PDF context). Drawn: the graphics state
(including ExtGState line parameters, constant alpha and blend modes),
paths, clipping, Gray / RGB / CMYK / ICCBased / CalGray / CalRGB / Lab /
Indexed / Separation / DeviceN colour (tint transforms through sampled,
exponential, stitching and PostScript functions), axial and radial
shadings and shading patterns, images (1 to 16 bits per component, Decode
arrays, JPEG through ImageIO, stencil masks, soft masks, explicit and
colour-key masks), inline images, form XObjects, and text with embedded
TrueType and OpenType programs, simple-font encodings and /Differences,
composite fonts (Identity CIDs, CIDToGIDMap, /W widths, ToUnicode) and
Type 3 fonts; other fonts (Type 1 and bare CFF programs, the standard 14)
use the system font their name maps to, through Unicode. Not drawn (a
diagnostic says so): tiling patterns, shading types 1 and 4 to 7, soft
masks in ExtGState, JPEG 2000, CCITT and JBIG2 images, annotations.

**Printing** (`appkit_printing.cpp`, AppKit classes over this layer):
NSPrintOperation writes PDF. **Printed pages are images; text is not
selectable**: each page is the view's rectangle drawn at 300 dpi (150 for
very large pages) through -cacheDisplayInRect:toBitmapImageRep: and placed
on a CGPDFContext page (vector printing needs AppKit's own control drawing
to reach non-bitmap contexts, later work). Pagination follows NSPrintInfo:
paper size, margins, scaling, automatic / fit / clip pagination,
centring, views that know their page range (-rectForPage:), and
NSPrintFirstPage / NSPrintLastPage. PDF operations (to data or a file),
-dataWithPDFInsideRect: and NSPrintSaveJob (to NSPrintJobSavingURL) write
the file; there are no printers, so spool jobs, previews and print panels
make -runOperation answer NO with a diagnostic (never a job that did not
happen). NSPrinter lists no printers. NSPDFImageRep (and an NSImage made
from PDF data) draws its pages through the page renderer. EPS output stops
explicitly.

**PDF writing** (`pdf_context.cpp`, `pdf/pdf_writer.cpp`): CGPDFContext
records CGContext drawing as PDF pages: filled and stroked paths (strokes
under the CTM), DeviceRGB colours with alpha and the blend modes PDF has,
path clipping, images (alpha as soft masks, image masks in the fill
colour, tiling), text as glyph outlines (not searchable), gradients as
shadings (alpha as a soft mask), pages with their boxes, the information
dictionary, XMP metadata, URL links and named destinations. Not provided:
encryption and PDF/A (creation fails and says why), shadows (not drawn),
pattern paint and mask clipping (stop explicitly), transparency layers as
groups (drawn directly), output intents and linearization (a diagnostic
says they are not written).

## Moved from darwin/identities/

* `color_space_property_list.cpp`: CGColorSpaceCopyPropertyList answers NULL (the support kit).
* `no_displays.cpp`, `event_source.cpp`: headless, every build: per-display queries for no display, CGEventSourceKeyState (no keyboard).
