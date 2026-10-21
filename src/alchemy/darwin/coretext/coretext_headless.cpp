#include "alchemy/darwin/shell_kit.hpp"
#include "alchemy/darwin/shells.hpp"

#include "alchemy/call_regs.hpp"

// alchemy.exe only (darwin/shells.hpp): CoreText's headless answers, from the
// framework identities (darwin/headless_additions.cpp): the
// attribute keys are real constants, CFAttributedStringCreate makes the
// Foundation provider's NSAttributedString, and text layout fails explicitly.
namespace alchemy::darwin_shells {
namespace {
constexpr std::string_view appkit = "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit";
constexpr std::string_view cocoa = "/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa";
constexpr std::string_view core_text = "/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText";
} // namespace

void install_coretext_shell(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    ShellKit kit{context, objc, cf};
    // Toll-free bridged: the Foundation provider's NSAttributedString.
    kit.function(core_foundation_library, "_CFAttributedStringCreate", [&objc](CpuState& s, GuestMemory&) {
        if (s.x[0]) throw std::runtime_error("Alchemy CoreFoundation: custom allocators are unsupported");
        const auto cls = objc.find_class("NSAttributedString");
        if (!cls) throw std::runtime_error("Alchemy installer frameworks: missing class NSAttributedString");
        const auto object = objc.send(cls, "alloc", CallRegs{}).x0;
        CallRegs registers;
        registers.x[2] = s.x[1];
        registers.x[3] = s.x[2];
        s.x[0] = objc.send(object, "initWithString:attributes:", std::move(registers)).x0;
    });
    kit.unsupported(core_text, {"_CTFontCopyAttribute", "_CTFontCopyFontDescriptor", "_CTFontCopyGraphicsFont",
        "_CTFontCreateCopyWithAttributes", "_CTFontCreateUIFontForLanguage", "_CTFontCreateWithFontDescriptor",
        "_CTFontCreateWithName", "_CTFontDescriptorCopyAttribute", "_CTFontDescriptorCreateCopyWithAttributes",
        "_CTFontDescriptorCreateCopyWithSymbolicTraits", "_CTFontDescriptorCreateWithAttributes", "_CTFontGetAscent",
        "_CTFontGetStringEncoding", "_CTFontGetSymbolicTraits", "_CTFontGetXHeight",
        "_CTFontManagerCopyAvailableFontFamilyNames", "_CTLineCreateWithAttributedString", "_CTLineDraw",
        "_CTLineGetOffsetForStringIndex", "_CTLineGetTypographicBounds"}, "text layout needs the CoreText provider");
    for (const auto& [symbol, value] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"_kCTFontAttributeName", "NSFont"}, {"_kCTFontFamilyNameAttribute", "NSFontFamilyAttribute"},
             {"_kCTFontNameAttribute", "NSFontNameAttribute"}, {"_kCTFontSizeAttribute", "NSFontSizeAttribute"},
             {"_kCTFontSlantTrait", "NSCTFontSlantTrait"}, {"_kCTFontSymbolicTrait", "NSCTFontSymbolicTrait"},
             {"_kCTFontTraitsAttribute", "NSCTFontTraitsAttribute"}, {"_kCTFontWeightTrait", "NSCTFontWeightTrait"},
             {"_kCTFontWidthTrait", "NSCTFontProportionTrait"}, {"_kCTForegroundColorAttributeName", "CTForegroundColor"},
             {"_kCTForegroundColorFromContextAttributeName", "CTForegroundColorFromContext"},
             {"_kCTStrokeColorAttributeName", "CTStrokeColor"}, {"_kCTStrokeWidthAttributeName", "CTStrokeWidth"}})
        kit.string_symbol(core_text, std::string(symbol), value);
    // AppKit's text attribute keys (NSAttributedString drawing), also under
    // Cocoa; NSUnderlineColorAttributeName under AppKit only.
    for (const auto& [symbol, value] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"_NSFontAttributeName", "NSFont"}, {"_NSForegroundColorAttributeName", "NSColor"},
             {"_NSBackgroundColorAttributeName", "NSBackgroundColor"}, {"_NSParagraphStyleAttributeName", "NSParagraphStyle"},
             {"_NSUnderlineStyleAttributeName", "NSUnderline"}, {"_NSStrikethroughStyleAttributeName", "NSStrikethrough"}}) {
        for (const auto library : {appkit, cocoa}) kit.string_symbol(library, std::string(symbol), value);
    }
    kit.string_symbol(appkit, "_NSUnderlineColorAttributeName", "NSUnderlineColor");
}

} // namespace alchemy::darwin_shells
