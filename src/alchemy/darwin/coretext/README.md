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

**Unsupported (explicit failures):** for example CTFramesetterCreateFrame
with a non-rectangular path, decimal tab stops, and live mutable string
proxies (-mutableString, CFAttributedStringGetMutableString).

## Moved from darwin/identities/

* `font_data.cpp`: kCTFontLanguagesAttribute and CTFontManagerCreateFontDescriptorFromData (the support kit).
