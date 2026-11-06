#include "alchemy/darwin/appkit/controls/g3.hpp"
#include "alchemy/darwin/security/crypto_core.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <utility>
#include <string>
#include <vector>

// NSAdaptiveImageGlyph (macOS 15): an image glyph (Genmoji) whose content is
// a multi-resolution image. It keeps its content (-imageContent, a copy) and
// provides images for CoreText (CTAdaptiveImageProviding:
// -imageForProposedSize:scaleFactor:imageOffset:imageSize:, used by
// CTFontDrawImageFromAdaptiveImageProviderAtPoint, font_exports.cpp): the
// content is decoded through ImageIO and the smallest image at least the
// proposed size in pixels is chosen (else the largest), fitted to the
// proposed size with its aspect kept. Content ImageIO cannot decode (HEIC
// without the host's HEIF codec) gives NULL.
// * -contentIdentifier is the SHA-256 of the content in hexadecimal: unique
//   per content, as documented, but not the identifier stored inside the
//   image (Alchemy does not read that metadata). -contentDescription is
//   empty for the same reason.
// * +contentType is UTType public.heic (nil without UniformTypeIdentifiers).
// * NSSecureCoding keeps the content under "NSImageContent" (an Alchemy key);
//   copies are the glyph itself (it is immutable).
// * NSAdaptiveImageGlyphAttributeName names it in attributed strings; the
//   text system keeps the attribute but does not lay the glyph out.
namespace alchemy::gui::completion_kit::controls {

void install_adaptive_image_glyph(AppKit& kit) {
    using namespace appkit;
    auto* const k = &kit;
    constexpr std::string_view image_io = "/System/Library/Frameworks/ImageIO.framework/Versions/A/ImageIO";
    constexpr std::string_view core_graphics = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
    constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
    if (!kit.process.resolve(appkit_library, "_NSAdaptiveImageGlyphAttributeName"))
        kit.string_constant("_NSAdaptiveImageGlyphAttributeName", "CTAdaptiveImageProvider");   // (unverified: kCTAdaptiveImageProviderAttributeName's)
    const auto cls = kit.declare_class("NSAdaptiveImageGlyph", "NSObject");
    struct GlyphState final : ObjectPayload { Address content{}, identifier{}; };   // +1 each
    const auto state_of = [k](Address self) { return k->objc.host_state<GlyphState>(self); };

    const auto initialize = [k, state_of](Address self, Address data) -> Address {
        if (!self || !data) return 0;
        const auto state = state_of(self);
        state->content = k->send_id(data, "copy");
        const auto length = k->send(state->content, "length").x0;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        if (length) k->memory().read_bytes(k->send(state->content, "bytes").x0, bytes);
        const auto digest = crypto::sha256(bytes);
        std::string hex;
        for (const auto byte : digest) {
            char pair[3];
            std::snprintf(pair, sizeof pair, "%02x", byte);
            hex += pair;
        }
        state->identifier = k->string(hex);
        return self;
    };
    kit.method(cls, "initWithImageContent:", [initialize](CpuState& s, GuestMemory&) { s.x[0] = initialize(s.x[0], s.x[2]); });
    kit.method(cls, "dealloc", [k, cls, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        k->release(std::exchange(state->content, 0));
        k->release(std::exchange(state->identifier, 0));
        static_cast<void>(k->objc.send_super(s.x[0], cls, k->sel("dealloc")));
    }, "v@:");
    kit.method(cls, "imageContent", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->content; });
    kit.method(cls, "contentIdentifier", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->identifier; });
    kit.method(cls, "contentDescription", [k](CpuState& s, GuestMemory&) { s.x[0] = k->auto_string(""); });
    kit.class_method(cls, "contentType", [k](CpuState& s, GuestMemory&) {
        const auto type_class = k->objc.find_class("UTType");
        s.x[0] = type_class && k->responds(type_class, "typeWithIdentifier:") ? k->send_id(type_class, "typeWithIdentifier:", {k->auto_string("public.heic")}) : 0;
    });
    kit.method(cls, "copyWithZone:", [k](CpuState& s, GuestMemory&) { s.x[0] = k->retain(s.x[0]); }, "@@:^v");
    kit.method(cls, "isEqual:", [k, cls, state_of](CpuState& s, GuestMemory&) {
        const auto other = s.x[2];
        return_bool(s, s.x[0] == other || (other && k->is_kind(other, cls) && k->cf.equal(state_of(s.x[0])->identifier, state_of(other)->identifier)));
    }, "B@:@");
    kit.method(cls, "hash", [k, state_of](CpuState& s, GuestMemory&) { s.x[0] = k->cf.hash(state_of(s.x[0])->identifier); }, "Q@:");
    // NSSecureCoding.
    kit.class_method(cls, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { return_bool(s, true); }, "B@:");
    kit.method(cls, "encodeWithCoder:", [k, state_of](CpuState& s, GuestMemory&) {
        static_cast<void>(k->send_id(s.x[2], "encodeObject:forKey:", {state_of(s.x[0])->content, k->auto_string("NSImageContent")}));
    }, "v@:@");
    kit.method(cls, "initWithCoder:", [k, initialize](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], coder = s.x[2];
        const auto data_class = k->objc.find_class("NSData");
        const auto data = k->send_id(coder, "decodeObjectOfClass:forKey:", {data_class, k->auto_string("NSImageContent")});
        if (!data) { k->release(self); s.x[0] = 0; return; }
        s.x[0] = initialize(self, data);
    });

    // CTAdaptiveImageProviding: (proposedSize d0 d1, scaleFactor d2, imageOffset *x2, imageSize *x3) -> CGImageRef (not retained).
    kit.method(cls, "imageForProposedSize:scaleFactor:imageOffset:imageSize:", [k, state_of, image_io, core_graphics, core_foundation](CpuState& s, GuestMemory& m) {
        const auto self = s.x[0];
        const Size proposed{d(s, 0), d(s, 1)};
        const double scale = d(s, 2) > 0 ? d(s, 2) : 1;
        const auto offset_out = s.x[2], size_out = s.x[3];
        const auto write_pair = [&m](Address where, double a, double b) {
            if (!where) return;
            m.write64(where, std::bit_cast<std::uint64_t>(a));
            m.write64(where + 8, std::bit_cast<std::uint64_t>(b));
        };
        write_pair(offset_out, 0, 0);
        write_pair(size_out, 0, 0);
        s.x[0] = 0;
        const auto content = state_of(self)->content;
        if (!content || !(proposed.width > 0) || !(proposed.height > 0)) return;
        const auto source = k->guest_call(image_io, "_CGImageSourceCreateWithData", {content, 0});
        if (!source) return;
        const auto count = k->guest_call(image_io, "_CGImageSourceGetCount", {source});
        const double wanted = proposed.height * scale;
        Address chosen{};
        double chosen_height{};
        for (Address index = 0; index < count; ++index) {
            const auto image = k->guest_call(image_io, "_CGImageSourceCreateImageAtIndex", {source, index, 0});
            if (!image) continue;
            const auto height = static_cast<double>(k->guest_call(core_graphics, "_CGImageGetHeight", {image}));
            const bool better = !chosen || (chosen_height < wanted ? height > chosen_height : height >= wanted && height < chosen_height);
            if (better) { k->release(chosen); chosen = image; chosen_height = height; }
            else k->release(image);
        }
        k->release(source);
        if (!chosen) return;
        const auto width = static_cast<double>(k->guest_call(core_graphics, "_CGImageGetWidth", {chosen}));
        const double fit = std::min(proposed.width / width, proposed.height / chosen_height);
        const Size drawn{width * fit, chosen_height * fit};
        write_pair(offset_out, (proposed.width - drawn.width) / 2, (proposed.height - drawn.height) / 2);
        write_pair(size_out, drawn.width, drawn.height);
        s.x[0] = k->guest_call(core_foundation, "_CFAutorelease", {chosen});
    }, "^{CGImage=}@:{CGSize=dd}d^{CGPoint=dd}^{CGSize=dd}");
}

} // namespace alchemy::gui::completion_kit::controls
