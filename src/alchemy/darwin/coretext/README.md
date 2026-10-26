# CoreText

**Covers:** fonts and font descriptors over DirectWrite, attributed strings,
lines, frames and the typesetter. NSFont, NSFontManager and AppKit's text
attribute keys are CoreText's (`font_exports.cpp`), as is NSAttributedString
in alchemy-fx (`attributed.cpp`).

**Headless:** `coretext_headless.cpp` and `attributed_string_headless.cpp`
(alchemy.exe only), empty `coretext_fx.cpp` and `attributed_string_fx.cpp`
in alchemy-fx (`darwin/shells.hpp`, `darwin/foundation/headless_shells.hpp`).

**Host dependencies:** `host::gpu` fonts and text layout (DirectWrite on
win32, dwrite.dll loaded at run time); drawing goes through CoreGraphics.

**Glyph names** come from the 'post' table (formats 1.0, 2.0 and 2.5): a
face without them (format 3.0, CFF fonts) names no glyphs.

**Font manager:** fonts registered by URL stay registered for the life of
the process (the host cannot remove them): unregistering fails with
kCTFontManagerErrorInUse, or kCTFontManagerErrorNotRegistered for a file
never registered. Registration errors are CFErrors of
kCTFontManagerErrorDomain. Font descriptors from a URL name the file's
first face; making a font from one registers the file. There are no
downloadable font assets (kCTFontManagerErrorAssetNotFound), and
auto-activation settings are kept per process only.

**Not registered:** CTRunDelegate and CTGlyphInfo (the host's line layout
has no inline objects or glyph substitutions), CTRubyAnnotation, font
features (CTFontCopyFeatures, CTFontDescriptorCreateCopyWithFeature) and
CTFontManagerCopyAvailableFontURLs and CTFontManagerCreateFontDescriptorsFromData.

**Unsupported (explicit failures):** for example CTFramesetterCreateFrame
with a non-rectangular path, decimal tab stops, and live mutable string
proxies (-mutableString, CFAttributedStringGetMutableString).

## Moved from darwin/identities/

* `font_data.cpp`: kCTFontLanguagesAttribute and CTFontManagerCreateFontDescriptorFromData (the support kit).
