# Alchemy Quartz 2D, CoreText and ImageIO

Owner: Q. Sources: `src/alchemy/darwin/coregraphics/` (with `imageio/`) and
`src/alchemy/darwin/coretext/`, built as part of the fx GUI set
(`cmake/darwin/coregraphics.cmake`, `cmake/darwin/coretext.cmake`). Direct2D,
DirectWrite and WIC are behind `host::gpu` (`src/alchemy/host/win32/gui/`).
All of it is project-authored (MIT). It uses only Windows system components:
Direct2D (`d2d1.dll`), DirectWrite (`dwrite.dll`) and WIC. No Apple code, headers or
binaries are involved.

## Scope

- **CoreGraphics drawing** (about 380 `CG*` exports). Covers CGContext and
  CGBitmapContext (RGBA/BGRA 8-bit premultiplied or skip-alpha, gray 8-bit, alpha-only),
  the graphics-state stack, CTM operations, and paths built with move, line, quad and
  cubic curves, arcs, rects, rounded rects and ellipses, plus CGPath and CGMutablePath.
  Fill supports both non-zero and even-odd rules. Stroke supports width, cap, join,
  miter limit and dash, and paths can be replaced by their stroke outline. Clipping works
  to a path, a rect or a mask image. Also included:
  - colors and color spaces: device RGB, sRGB, gray, generic and named, indexed and pattern
  - global alpha and all CGBlendModes, both separable and non-separable
  - shadows, with or without blur
  - transparency layers
  - CGImage from data providers or files: masks, cropping, masking with an image
  - linear and radial CGGradient and CGShading
  - minimal CGPattern
  - CGLayer
  - CGDataProvider and CGDataConsumer
  - antialiasing and interpolation-quality flags

  The CGColorSpace objects are the same ones the existing CoreGraphics provider uses.
- **CoreText** (about 126 `CT*` exports). CTFont comes from names, descriptors or UI font
  types. It provides metrics, glyphs for characters, advances, bounding rects, glyph
  outlines and symbolic traits. Also covered: CTLine and CTRun (typographic and image
  bounds, positions, drawing, truncation), CTTypesetter, CTFramesetter and CTFrame (word
  wrapping in a rectangular path, suggested frame size, line origins), and CTParagraphStyle.
  NSAttributedString and CFAttributedString support the font, color, paragraph-style,
  kern and underline keys.
- **ImageIO**. CGImageSource reads data or URLs in PNG, JPEG, TIFF, BMP, GIF and ICO
  through WIC, and `.icns` through an in-house parser (PNG, ARGB, and RLE RGB with mask
  entries). CGImageDestination writes PNG, JPEG, TIFF, BMP and GIF.
- **AppKit drawing**. NSGraphicsContext covers `currentContext`, `CGContext`,
  `graphicsContextWithCGContext:flipped:`, `graphicsContextWithBitmapImageRep:` and
  save/restore. Also covered:
  - NSImage `lockFocus` and drawing through CG
  - NSBitmapImageRep pixels
  - NSColor `set`, `setFill` and `setStroke`
  - NSBezierPath fill and stroke over CGPath
  - NSString and NSAttributedString `drawAtPoint:`, `drawInRect:` and `sizeWithAttributes:`
    through CoreText

Unsupported behaviour fails explicitly (`unsupported: Alchemy Quartz: ...`). Known cases:
- non-rectangular CTFrame paths
- color-key masking (`CGImageCreateWithMaskingColors`)
- live mutable-string proxies of attributed strings
- 16-bit and float bitmap contexts (these return NULL with a diagnostic, as on macOS
  for invalid combinations)
- JPEG 2000 icns entries

## Backend model

A bitmap context owns a software Direct2D render target over a WIC bitmap in
premultiplied BGRA. The guest's pixel memory is authoritative: each drawing operation
copies the affected guest pixels in, draws, and writes them back. As a result,
`CGBitmapContextGetData` always shows the current pixels, and guest writes between draws
are preserved ("flush semantics", see `d2d.hpp` and `context.cpp`). The factories are
created on first use, never at install time.

Font mapping keeps the Apple names visible to the guest: `CTFontCopyPostScriptName` and
`CopyFamilyName` return the Apple names. The table and its style-suffix rules are in
`src/alchemy/darwin/coretext/fonts.cpp`:

| Apple name(s) | Windows family |
|---|---|
| .AppleSystemUIFont, .SFNS-*, SF Pro *, System Font, Lucida Grande, Avenir (Next), Futura, Gill Sans, Optima | Segoe UI |
| .AppleSystemUIFontMonospaced, SF Mono, Menlo, Monaco, Andale Mono | Consolas |
| Helvetica, Helvetica Neue (unknown names also fall back to Helvetica) | Arial |
| Courier / Times / Geneva | Courier New / Times New Roman / Verdana |
| Apple Color Emoji; Apple Symbols, Zapf Dingbats | Segoe UI Emoji; Segoe UI Symbol |
| PingFang SC/TC/HK, Heiti SC; Hiragino; Apple SD Gothic Neo | Microsoft YaHei/JhengHei; Yu Gothic UI; Malgun Gothic |

Any other name resolves through DirectWrite, by family name first and then by
PostScript or full name.

## Integration point (AppKit drawRect: views)

This layer does not composite windows. The contract is `src/alchemy/darwin/coregraphics/quartz2d.hpp`:

1. `create_bitmap_context(width_px, height_px, scale)` creates the window backing store
   in BGRA with a bottom-left origin, in points.
2. `render_view_hierarchy(content_view, backing, dirty)` makes an NSGraphicsContext
   current for each view. For each view it applies the frame, bounds and flipped
   transform, clips to the view, sends `-drawRect:`, then recurses into the subviews.
   It uses messages only.
3. `context_image(backing)` returns straight RGBA, top row first. The window server then
   presents those pixels to the HWND. That present call belongs to the window-server
   contract and is not part of this layer.

`current_cg_context()` returns `[[NSGraphicsContext currentContext] CGContext]` for host
code.

## Tests

- `alchemy_gui_quartz2d_unit`: offscreen pixel tests covering shapes, strokes and dashes,
  paths, clipping (rect, even-odd, mask), gradients, images, blend modes, alpha, shadows,
  transparency layers, pixel layouts, CoreText metrics, layout and drawing bounds, ImageIO
  PNG/JPEG/icns, and AppKit drawing.
- `alchemy_gui_quartz2d_fixture`: an arm64 guest program run under `alchemy-fx`. It draws
  through CGBitmapContext and CoreText, round-trips the result through PNG with ImageIO,
  and prints pixel probes and a checksum.
