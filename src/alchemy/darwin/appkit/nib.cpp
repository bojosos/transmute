#include "alchemy/darwin/appkit/controls_internal.hpp"
#include "alchemy/darwin/appkit/nib_archive.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

// Nib loading: NSNib, -[NSBundle loadNibNamed:owner:topLevelObjects:],
// NSApplicationMain (principal class + NSMainNibFile), and the nib coder.
//
// A compiled nib (NIBArchive, nib_archive.hpp) is instantiated object by
// object: values (strings, numbers, collections, colors, fonts) become
// Foundation/AppKit values directly; placeholders resolve to the File's
// Owner, NSApp or nil (First Responder); NSWindowTemplate becomes an
// NSWindow; NSCustomView / NSCustomObject / NSClassSwapper name their
// (guest) class; every other object is allocated and sent -initWithCoder:
// with the nib coder (_AlchemyNibCoder, an NSCoder answering keyed
// decoding from the archive), so guest subclasses decode their own keys and
// reach AppKit's -initWithCoder: through super. Then outlets (setter, KVC
// or the instance variable) and actions (target/action) are connected,
// -awakeFromNib is sent, and the windows marked visible are ordered front.
// A class the archive needs that does not exist, or an AppKit class whose
// coding is not provided here (no -initWithCoder:), fails explicitly.
//
// Ownership follows +[NSBundle loadNibNamed:owner:]: top-level objects keep
// the reference the load made (the caller owns them; the topLevelObjects:
// array holds them too); other objects are owned by their parents.
namespace alchemy::gui::appkit {
namespace {
struct NibLoad {
    nib::Archive archive;
    nib::Graph graph;
    std::map<std::uint32_t, Address> made;    // +1 (except placeholders)
    std::set<std::uint32_t> placeholders;
    std::vector<std::uint32_t> order;         // objects sent -awakeFromNib
    std::vector<Address> buffers;             // decodeBytesForKey: memory (released with the load)
    Address owner{};
};
struct CoderState final : ObjectPayload {
    std::shared_ptr<NibLoad> load;
    std::vector<std::uint32_t> stack;         // the object being decoded is the back
};
struct NibState final : ObjectPayload {
    std::vector<std::uint8_t> bytes;
    Address bundle{};                         // +1
};

const nib::Value* value_for(const CoderState& coder, std::string_view key) {
    if (coder.stack.empty()) fail("nib coder used outside -initWithCoder:");
    return coder.load->archive.find(coder.stack.back(), key);
}
// "Name" of ".../Name.nib".
std::string base_name(std::string path) {
    if (const auto slash = path.find_last_of('/'); slash != std::string::npos) path = path.substr(slash + 1);
    if (path.ends_with(".nib")) path.resize(path.size() - 4);
    return path;
}
std::string capitalized(std::string text) {
    if (!text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}
Rect rect_from(const std::vector<double>& n) {
    if (n.size() == 4) return {{n[0], n[1]}, {n[2], n[3]}};
    if (n.size() == 2) return {{0, 0}, {n[0], n[1]}};
    fail("malformed rectangle string in a nib");
}
} // namespace

namespace {
// The instantiation engine (free functions over AppKit's public members).
Address instantiate(AppKit& kit, const std::shared_ptr<NibLoad>& load, std::uint32_t index, Address coder);

Address decode_reference(AppKit& kit, const std::shared_ptr<NibLoad>& load, const nib::Value* value, Address coder) {
    if (!value || value->type == nib::ValueType::nil) return 0;
    if (!value->is_object()) fail("nib value is not an object reference");
    return instantiate(kit, load, static_cast<std::uint32_t>(value->integer), coder);
}

// A color's components; NSCustomColorSpace (the color's NSColorSpace object)
// is not read: the components are taken in the NSColorSpace model below.
drawing::Color color_from(AppKit& kit, const nib::Archive& archive, std::uint32_t index) {
    const auto space = archive.find(index, "NSColorSpace");
    const auto space_id = space ? space->integer : 1;
    const auto components = [&](std::string_view key) {
        const auto* bytes = archive.find(index, key);
        if (!bytes || bytes->type != nib::ValueType::data) return std::vector<double>{};
        std::string text(bytes->data.begin(), bytes->data.end());
        while (!text.empty() && text.back() == '\0') text.pop_back();
        std::vector<double> numbers;
        std::size_t at = 0;
        while (at < text.size()) {
            while (at < text.size() && text[at] == ' ') ++at;
            std::size_t end = at;
            while (end < text.size() && text[end] != ' ') ++end;
            if (end > at) try { numbers.push_back(std::stod(text.substr(at, end - at))); } catch (...) {}
            at = end;
        }
        return numbers;
    };
    switch (space_id) {
    case 1: case 2: {   // calibrated / device RGB
        const auto c = components("NSRGB");
        if (c.size() >= 3) return {c[0], c[1], c[2], c.size() > 3 ? c[3] : 1};
        break;
    }
    case 3: case 4: {   // calibrated / device white
        const auto c = components("NSWhite");
        if (!c.empty()) return drawing::Color::gray(c[0], c.size() > 1 ? c[1] : 1);
        break;
    }
    case 5: {           // CMYK
        const auto c = components("NSCMYK");
        if (c.size() >= 4) return {(1 - c[0]) * (1 - c[3]), (1 - c[1]) * (1 - c[3]), (1 - c[2]) * (1 - c[3]), c.size() > 4 ? c[4] : 1};
        break;
    }
    case 6: {           // catalog color: the system color of that name, else its fallback
        const auto name = archive.string_at(index, "NSColorName");
        if (name && kit.responds(kit.color, *name)) return kit.color_value(kit.send(kit.color, *name).x0);
        if (const auto fallback = archive.object_at(index, "NSColor")) return color_from(kit, archive, *fallback);
        break;
    }
    default: break;
    }
    unsupported("NSColorSpace " + std::to_string(space_id) + " of an archived color (no -[NSColor initWithCoder:] provider decodes it)");
}

Address make_value(AppKit& kit, const std::shared_ptr<NibLoad>& load, std::uint32_t index, Address coder, bool& handled) {
    handled = true;
    const auto& archive = load->archive;
    const auto& name = archive.class_name(index);
    if (const auto text = archive.string_value(index)) {
        // NSLocalizableString: the base-language text; NSKey (its key in a
        // <language>.lproj/<nib>.strings table) and NSDev (the comment) only
        // matter when such a table exists, which read_nib_file refuses.
        if (name == "NSLocalizableString") {
            static_cast<void>(archive.find(index, "NSKey"));
            static_cast<void>(archive.find(index, "NSDev"));
        }
        return kit.cf.string(*text);
    }
    if (name == "NSNumber") {
        if (const auto* v = archive.find(index, "NS.intval")) return kit.cf.integer(v->integer);
        if (const auto* v = archive.find(index, "NS.dblval")) return kit.cf.real(v->is_real() ? v->real : static_cast<double>(v->integer));
        if (const auto* v = archive.find(index, "NS.boolval")) return kit.retain(kit.cf.boolean(v->integer != 0));
        return kit.cf.integer(0);
    }
    if (name == "NSArray" || name == "NSMutableArray" || name == "NSSet" || name == "NSMutableSet" || name == "NSOrderedSet" ||
        name == "NSMutableOrderedSet") {
        std::vector<Address> elements;
        for (const auto element : archive.elements(index)) elements.push_back(instantiate(kit, load, element, coder));
        elements.erase(std::remove(elements.begin(), elements.end(), Address{}), elements.end());
        const bool mutable_collection = name.starts_with("NSMutable");
        if (name.find("Set") != std::string::npos) {
            const auto set_class = kit.objc.find_class(mutable_collection ? "NSMutableSet" : "NSSet");
            if (set_class && kit.responds(set_class, "setWithArray:")) {
                const auto array = kit.cf.array(elements);
                const auto set = kit.retain(kit.send_id(set_class, "setWithArray:", {array}));
                kit.release(array);
                return set;
            }
        }
        return kit.cf.array(elements, mutable_collection);
    }
    if (name == "NSDictionary" || name == "NSMutableDictionary") {
        const auto elements = archive.elements(index);
        if (elements.size() % 2) fail("nib dictionary with an odd number of elements");
        std::vector<std::pair<Address, Address>> entries;
        for (std::size_t i = 0; i < elements.size(); i += 2)
            entries.emplace_back(instantiate(kit, load, elements[i], coder), instantiate(kit, load, elements[i + 1], coder));
        return kit.cf.dictionary(entries, name == "NSMutableDictionary");
    }
    if (name == "NSData" || name == "NSMutableData") {
        const auto* bytes = archive.find(index, "NS.bytes");
        return kit.retain(kit.make_data(bytes ? std::span<const std::uint8_t>(bytes->data) : std::span<const std::uint8_t>{}));
    }
    if (name == "NSColor") {
        // A provider of -[NSColor initWithCoder:] decodes every color space
        // (component, custom and pattern colors): the generic path below
        // sends it the nib coder. Otherwise the calibrated / device RGB,
        // white, CMYK and catalog spaces are converted here.
        const auto decoder = kit.guest_call(objc_library, "_class_getInstanceMethod", {kit.color, kit.sel("initWithCoder:")});
        if (decoder) { handled = false; return 0; }
        // Catalog (named) colors stay the shared semantic color, which
        // follows the appearance it is drawn in.
        if (const auto* space = archive.find(index, "NSColorSpace"); space && space->integer == 6) {
            const auto catalog = archive.string_at(index, "NSCatalogName").value_or("System");
            if (const auto color_name = archive.string_at(index, "NSColorName")) {
                if (kit.responds(kit.color, "colorWithCatalogName:colorName:"))
                    if (const auto named = kit.send_id(kit.color, "colorWithCatalogName:colorName:", {kit.auto_string(catalog), kit.auto_string(*color_name)}))
                        return kit.retain(named);
                if (catalog == "System" && kit.responds(kit.color, *color_name))
                    if (const auto named = kit.send(kit.color, *color_name).x0) return kit.retain(named);
            }
        }
        const auto c = color_from(kit, archive, index);
        return kit.retain(kit.color_object(c.r, c.g, c.b, c.a));
    }
    if (name == "NSFont") {
        drawing::FontSpec spec;
        spec.name = archive.string_at(index, "NSName").value_or(".AppleSystemUIFont");
        if (const auto* size = archive.find(index, "NSSize")) spec.size = size->is_real() ? size->real : static_cast<double>(size->integer);
        const auto lower = [&] { std::string s; for (const auto c : spec.name) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }();
        spec.bold = lower.find("bold") != std::string::npos;
        // Not read: NSfFlags, NSHasWidth, NSFontUsesAppearanceFontSize and
        // NSTextStyleDescriptor (a text style's descriptor, design-time).
        return kit.retain(kit.font_object(spec));
    }
    if (name == "NSAppearance") {
        // A named appearance (NSAppearanceTintColor, a custom accent, is
        // not applied: the theme's accent stays).
        const auto appearance_name = archive.string_at(index, "NSAppearanceName");
        if (!appearance_name) fail("nib NSAppearance without NSAppearanceName");
        static_cast<void>(archive.find(index, "NSAppearanceTintColor"));
        return kit.retain(kit.appearance_named(*appearance_name));
    }
    if (name == "NSCustomResource") {
        // A named image or color (NSClassName) from the bundle: +imageNamed: /
        // +colorNamed: with NSResourceName. IBNamespaceID, IBDesignSize and
        // IBDesignImageConfiguration are Interface Builder's design-time
        // preview data.
        const auto resource_class = archive.string_at(index, "NSClassName").value_or("NSImage");
        const auto resource = archive.string_at(index, "NSResourceName");
        if (!resource) fail("nib NSCustomResource without NSResourceName");
        static_cast<void>(archive.find(index, "IBNamespaceID"));
        static_cast<void>(archive.find(index, "IBDesignSize"));
        static_cast<void>(archive.find(index, "IBDesignImageConfiguration"));
        if (resource_class == "NSImage") return kit.retain(kit.send_id(kit.image, "imageNamed:", {kit.auto_string(*resource)}));
        if (resource_class == "NSColor") return kit.retain(kit.send_id(kit.color, "colorNamed:", {kit.auto_string(*resource)}));
        unsupported("an archived NSCustomResource of class " + resource_class);
    }
    if (name == "IBClassReference") {
        // Interface Builder's class reference (IBClassName in IBModuleName,
        // from IBModuleProvider): the class object.
        const auto class_name = archive.string_at(index, "IBClassName");
        auto module = archive.string_at(index, "IBModuleName");
        static_cast<void>(archive.find(index, "IBModuleProvider"));
        if (!class_name) fail("nib IBClassReference without IBClassName");
        const auto cls = kit.objc.find_class(module && !module->empty() ? *module + "." + *class_name : *class_name);
        return cls ? cls : kit.objc.find_class(*class_name);
    }
    if (name == "NSColorSpace") {
        // Colors are converted to sRGB when decoded (color_from), so the
        // color space object (NSID / NSModel / NSICC profile) is the generic
        // sRGB one.
        static_cast<void>(archive.find(index, "NSID"));
        static_cast<void>(archive.find(index, "NSModel"));
        static_cast<void>(archive.find(index, "NSICC"));
        static_cast<void>(archive.find(index, "NSSpaceID"));
        const auto space_class = kit.objc.find_class("NSColorSpace");
        return space_class && kit.responds(space_class, "sRGBColorSpace") ? kit.retain(kit.send(space_class, "sRGBColorSpace").x0) : 0;
    }
    if (name == "NSParagraphStyle") {
        // Decoded by -[NSParagraphStyle initWithCoder:] where the text
        // system provides it; else text draws with the default style.
        const auto cls = kit.objc.find_class(name);
        if (cls && kit.guest_call(objc_library, "_class_getInstanceMethod", {cls, kit.sel("initWithCoder:")})) { handled = false; return 0; }
        return 0;
    }
    if (name == "NSButtonImageSource" || name == "NSNibTouchBar" || name == "NSFontDescriptor" || name == "NSValue") {
        // Values AppKit draws without: button image sources (decoded with
        // their button cell), Touch Bars, design-time values: NSValue is
        // the IBDesignSize of an NSCustomResource (NS.special 2 with an
        // NS.sizeval string), NSFontDescriptor an NSFont's
        // NSTextStyleDescriptor (NSFontDescriptorAttributes,
        // NSFontDescriptorOptions); the font is its NSName at NSSize.
        return 0;
    }
    handled = false;
    return 0;
}

// The window attributes of an NSWindowTemplate besides its frame, style,
// title, content view and size limits.
void decode_window_template(AppKit& kit, const std::shared_ptr<NibLoad>& load, std::uint32_t index, Address window_object, Address coder) {
    const auto& archive = load->archive;
    const auto value = [&](std::string_view key) { return archive.find(index, key); };
    const auto integer = [&](std::string_view key, std::int64_t fallback) {
        const auto* v = value(key);
        return v && (v->is_integer() || v->is_bool()) ? v->integer : fallback;
    };
    // NSWTFlags: bit 31 hides on deactivate, bit 30 not released when
    // closed; bits 29 (deferred) and 28 (one shot) concern backing stores
    // made on demand here anyway, bit 27 (visible at launch) is
    // NSVisibleWindows, bits 24-19 the auto-position mask relative to
    // NSScreenRect (the design-time screen), which is not applied: windows
    // open at their archived frame.
    if (const auto* flags = value("NSWTFlags")) {
        const auto bits = static_cast<std::uint32_t>(flags->integer);
        if (bits & 0x80000000u) kit.send_id(window_object, "setHidesOnDeactivate:", {1});
        kit.send_id(window_object, "setReleasedWhenClosed:", {(bits & 0x40000000u) ? 0u : 1u});
    }
    static_cast<void>(archive.string_at(index, "NSScreenRect"));
    if (const auto* v = value("NSWindowCollectionBehavior")) kit.send_id(window_object, "setCollectionBehavior:", {static_cast<Address>(v->integer)});
    if (const auto* v = value("NSWindowAnimationBehavior")) kit.send_id(window_object, "setAnimationBehavior:", {static_cast<Address>(v->integer)});
    if (value("NSTitlebarAppearsTransparent"))
        kit.send_id(window_object, "setTitlebarAppearsTransparent:", {static_cast<Address>(integer("NSTitlebarAppearsTransparent", 0) != 0)});
    if (const auto* v = value("NSWindowTitleVisibility")) kit.send_id(window_object, "setTitleVisibility:", {static_cast<Address>(v->integer)});
    if (value("NSWindowIsRestorable"))
        kit.send_id(window_object, "setRestorable:", {static_cast<Address>(integer("NSWindowIsRestorable", 1) != 0)});
    if (const auto subtitle = archive.string_at(index, "NSWindowSubtitle"); subtitle && !subtitle->empty())
        kit.send_id(window_object, "setSubtitle:", {kit.auto_string(*subtitle)});
    if (const auto identifier = archive.string_at(index, "NSUserInterfaceItemIdentifier"))
        kit.send_id(window_object, "setIdentifier:", {kit.auto_string(*identifier)});
    if (const auto autosave = archive.string_at(index, "NSFrameAutosaveName"); autosave && !autosave->empty())
        kit.send_id(window_object, "setFrameAutosaveName:", {kit.auto_string(*autosave)});
    if (const auto* v = value("NSContentBorderThicknessMinY")) {
        CallRegs registers;
        registers.set_d(0, v->is_real() ? v->real : static_cast<double>(v->integer));
        registers.x[2] = 1;   // NSRectEdgeMinY
        static_cast<void>(kit.send(window_object, "setContentBorderThickness:forEdge:", registers));
    }
    if (value("NSAutorecalculatesContentBorderThicknessMinY")) {
        CallRegs registers;
        registers.x[2] = integer("NSAutorecalculatesContentBorderThicknessMinY", 0) != 0;
        registers.x[3] = 1;
        static_cast<void>(kit.send(window_object, "setAutorecalculatesContentBorderThickness:forEdge:", registers));
    }
    // NSViewClass: the content view's class when Interface Builder made it
    // a custom one (the content view object carries the class already).
    if (const auto view_class = archive.string_at(index, "NSViewClass"); view_class && !kit.objc.find_class(*view_class))
        fail("nib window content view class " + *view_class + " does not exist");
    static_cast<void>(coder);
}

Address instantiate(AppKit& kit, const std::shared_ptr<NibLoad>& load, std::uint32_t index, Address coder) {
    if (const auto found = load->made.find(index); found != load->made.end()) return found->second;
    auto& archive = load->archive;
    const auto name = archive.class_name(index);
    bool handled{};
    if (const auto value = make_value(kit, load, index, coder, handled); handled) {
        load->made.emplace(index, value);
        if (!value) load->placeholders.insert(index);
        return value;
    }
    // Placeholders.
    if (name == "NSCustomObject") {
        const auto cls_name = archive.string_at(index, "NSClassName").value_or("NSObject");
        if (index == load->graph.root) { load->placeholders.insert(index); load->made.emplace(index, load->owner); return load->owner; }
        if (cls_name == "FirstResponder") { load->placeholders.insert(index); load->made.emplace(index, 0); return 0; }
        if (cls_name == "NSApplication") {
            load->placeholders.insert(index);
            const auto shared = kit.app ? kit.app : kit.shared_application(kit.application);
            load->made.emplace(index, shared);
            return shared;
        }
        const auto cls = kit.objc.find_class(cls_name);
        if (!cls) fail("nib object class " + cls_name + " does not exist");
        const auto object = kit.alloc_init(cls);
        load->made.emplace(index, object);
        load->order.push_back(index);
        return object;
    }
    if (name == "NSWindowTemplate") {
        const auto cls_name = archive.string_at(index, "NSWindowClass").value_or("NSWindow");
        const auto cls = kit.objc.find_class(cls_name);
        if (!cls) fail("nib window class " + cls_name + " does not exist");
        const auto numbers = nib::parse_numbers(archive.string_at(index, "NSWindowRect").value_or("{{0, 0}, {480, 270}}"));
        if (!numbers) fail("malformed NSWindowRect");
        const auto* mask = archive.find(index, "NSWindowStyleMask");
        CallRegs init;
        put_rect(init, 0, rect_from(*numbers));
        init.x[2] = mask ? static_cast<Address>(mask->integer) : 0;
        init.x[3] = 2;   // NSBackingStoreBuffered
        init.x[4] = 1;   // defer
        const auto window_object = kit.send(kit.send(cls, "alloc").x0, "initWithContentRect:styleMask:backing:defer:", init).x0;
        load->made.emplace(index, window_object);
        load->order.push_back(index);
        if (const auto title = archive.string_at(index, "NSWindowTitle")) kit.send_id(window_object, "setTitle:", {kit.auto_string(*title)});
        if (const auto content = archive.object_at(index, "NSWindowView"))
            if (const auto made = instantiate(kit, load, *content, coder)) kit.send_id(window_object, "setContentView:", {made});
        decode_window_template(kit, load, index, window_object, coder);
        for (const auto& [key, selector] : {std::pair<const char*, const char*>{"NSWindowContentMinSize", "setContentMinSize:"},
                                            {"NSWindowContentMaxSize", "setContentMaxSize:"}, {"NSMinSize", "setMinSize:"},
                                            {"NSMaxSize", "setMaxSize:"}, {"NSMinFullScreenContentSize", "setMinFullScreenContentSize:"},
                                            {"NSMaxFullScreenContentSize", "setMaxFullScreenContentSize:"}})
            if (const auto text = archive.string_at(index, key))
                if (const auto size = nib::parse_numbers(*text); size && size->size() == 2 && kit.responds(window_object, selector)) {
                    CallRegs registers;
                    put_size(registers, 0, {(*size)[0], (*size)[1]});
                    static_cast<void>(kit.send(window_object, selector, registers));
                }
        return window_object;
    }
    // Objects decoded by -initWithCoder: (NSCustomView and NSClassSwapper name the class).
    std::string cls_name = name;
    bool custom_view = false;
    if (name == "NSClassSwapper") {
        cls_name = archive.string_at(index, "NSClassName").value_or("");
        if (!kit.objc.find_class(cls_name)) cls_name = archive.string_at(index, "NSOriginalClassName").value_or(cls_name);
    } else if (name == "NSCustomView") {
        cls_name = archive.string_at(index, "NSClassName").value_or("NSView");
        custom_view = true;
    }
    const auto cls = kit.objc.find_class(cls_name);
    if (!cls) fail("nib object class " + cls_name + " does not exist");
    const auto object = kit.send(cls, "alloc").x0;
    load->made.emplace(index, object);   // cycles (superview <-> subviews) see the allocated object
    auto& coder_state = *kit.objc.host_state<CoderState>(coder);
    coder_state.stack.push_back(index);
    Address result{};
    try {
        if (custom_view) {
            // Custom views are made with -initWithFrame:, then get their subviews.
            const auto frame_text = archive.string_at(index, "NSFrame");
            const auto size_text = archive.string_at(index, "NSFrameSize");
            const auto numbers = nib::parse_numbers(frame_text ? *frame_text : size_text.value_or("{0, 0}"));
            CallRegs registers;
            put_rect(registers, 0, numbers ? rect_from(*numbers) : Rect{});
            result = kit.send(object, "initWithFrame:", registers).x0;
            if (result) {
                kit.send_id(result, "_alchemyDecodeViewExtras:", {coder});
            }
        } else {
            if (!kit.responds(object, "initWithCoder:")) unsupported("nib decoding of " + cls_name + " (no -initWithCoder:)");
            result = kit.send_id(object, "initWithCoder:", {coder});
        }
    } catch (...) {
        coder_state.stack.pop_back();
        throw;
    }
    coder_state.stack.pop_back();
    if (result != object) load->made[index] = result;
    load->order.push_back(index);
    return result;
}

// One outlet: the setter, KVC, or the instance variable.
void connect_outlet(AppKit& kit, Address source, Address destination, const std::string& label) {
    if (!source) return;
    const auto setter = "set" + capitalized(label) + ":";
    if (kit.responds(source, setter)) { kit.send_id(source, setter, {destination}); return; }
    if (kit.responds(source, "setValue:forKey:")) {
        kit.send_id(source, "setValue:forKey:", {destination, kit.auto_string(label)});
        return;
    }
    // The instance variable (label or _label), through the Objective-C runtime.
    const auto cls = kit.objc.object_class(source);
    for (const auto& name : {label, "_" + label}) {
        const auto block = kit.process.allocate_guest(name.size() + 1);
        kit.memory().write_bytes(block, std::span(reinterpret_cast<const std::uint8_t*>(name.c_str()), name.size() + 1));
        Address ivar{};
        try { ivar = kit.guest_call(objc_library, "_class_getInstanceVariable", {cls, block}); }
        catch (...) { kit.process.release_guest(block); throw; }
        kit.process.release_guest(block);
        if (!ivar) continue;
        const auto offset = kit.guest_call(objc_library, "_ivar_getOffset", {ivar});
        kit.memory().write64(source + offset, destination);   // outlets are not retained (weak/assign)
        return;
    }
    std::string class_name = "?";
    if (const auto pointer = kit.guest_call(objc_library, "_class_getName", {cls})) {
        bool terminated{};
        const auto bytes = kit.memory().read_string(pointer, 1, 4096, &terminated);
        if (terminated) class_name.assign(bytes.begin(), bytes.end());
    }
    fail("nib outlet '" + label + "' has no setter or instance variable on its source (" + class_name + ")");
}

// Connectors other than outlets and actions, as their -establishConnection
// does in AppKit.
void establish_connector(AppKit& kit, const std::shared_ptr<NibLoad>& load, const nib::Connection& connection, Address coder) {
    const auto& archive = load->archive;
    const auto& name = connection.connector_class;
    const auto object = [&](std::string_view key) -> Address {
        const auto index = archive.object_at(connection.object, key);
        return index ? instantiate(kit, load, *index, coder) : 0;
    };
    const auto text_at = [&](std::string_view key) { return archive.string_at(connection.object, key); };
    if (name == "NSNibAXAttributeConnector") {
        // An accessibility attribute override (AXDescription, AXHelp,
        // AXIdentifier...): -accessibilitySetOverrideValue:forAttribute:.
        const auto destination = object("AXDestinationArchiveKey");
        const auto attribute = object("AXAttributeTypeArchiveKey");
        const auto value = object("AXAttributeValueArchiveKey");
        if (!attribute) fail("NSNibAXAttributeConnector without an attribute");
        if (destination) kit.send_id(destination, "accessibilitySetOverrideValue:forAttribute:", {value, attribute});
        return;
    }
    if (name == "NSIBHelpConnector") {
        // Interface Builder archives tool tips as help connections to the
        // NSToolTipHelpKey "file"; other files are Help Book anchors.
        const auto destination = object("NSDestination");
        const auto file = text_at("NSFile").value_or("");
        if (file != "NSToolTipHelpKey") unsupported("nib help connector to the Help Book file " + file + " (NSHelpManager context help)");
        if (destination) kit.send_id(destination, "setToolTip:", {object("NSMarker")});
        return;
    }
    if (name == "NSNibAuxiliaryActionConnector") {
        // A second action of a control (its NSTrigger property, such as
        // doubleAction) sent to the destination.
        const auto source = object("NSSource");
        const auto trigger = text_at("NSTrigger").value_or("");
        if (trigger.empty()) fail("NSNibAuxiliaryActionConnector without NSTrigger");
        if (!source) return;
        kit.send_id(source, "setTarget:", {object("NSDestination")});
        kit.send_id(source, "set" + capitalized(trigger) + ":", {kit.sel(connection.label)});
        return;
    }
    if (name == "NSNibConnector") {
        // The base connector establishes nothing; Interface Builder uses it
        // to make its source decode before the other connections.
        static_cast<void>(object("NSSource"));
        static_cast<void>(object("NSDestination"));
        return;
    }
    if (name == "NSNibBindingConnector") {
        // Cocoa bindings: -bind:toObject:withKeyPath:options: (Foundation's
        // key-value binding provider implements it). NSPreviousConnector
        // (Interface Builder's connector order) is not read.
        const auto source = object("NSSource");
        const auto destination = object("NSDestination");
        const auto binding = text_at("NSBinding");
        const auto key_path = text_at("NSKeyPath");
        if (!binding || !key_path) fail("NSNibBindingConnector without NSBinding or NSKeyPath");
        const auto version = archive.find(connection.object, "NSNibBindingConnectorVersion");
        if (version && version->integer != 2) unsupported("nib binding connector version " + std::to_string(version->integer));
        if (source) kit.send_id(source, "bind:toObject:withKeyPath:options:",
                                {kit.auto_string(*binding), destination, kit.auto_string(*key_path), object("NSOptions")});
        return;
    }
    unsupported("nib connector " + name);
}

std::vector<Address> run_load(AppKit& kit, const std::shared_ptr<NibLoad>& load) {
    const auto coder = kit.objc.allocate(kit.nib_coder);
    kit.objc.host_state<CoderState>(coder)->load = load;
    std::vector<Address> top_level;
    try {
        load->graph = nib::read_graph(load->archive);
        // Every object the graph lists, then whatever they reference.
        for (const auto& [child, parent] : load->graph.parents) {
            static_cast<void>(instantiate(kit, load, child, coder));
            static_cast<void>(parent);
        }
        for (const auto window_index : load->graph.visible_windows) static_cast<void>(instantiate(kit, load, window_index, coder));
        // Connections.
        for (const auto& connection : load->graph.connections) {
            if (connection.kind == nib::Connection::Kind::other) {
                // User-defined runtime attributes: -setValue:forKeyPath: on the
                // object for each key path, before -awakeFromNib (as AppKit's
                // connector does when the connection is established).
                if (connection.connector_class == "NSIBUserDefinedRuntimeAttributesConnector") {
                    const auto& archive = load->archive;
                    const auto target_index = archive.object_at(connection.object, "NSObject");
                    const auto paths_index = archive.object_at(connection.object, "NSKeyPaths");
                    const auto values_index = archive.object_at(connection.object, "NSValues");
                    if (!target_index || !paths_index || !values_index) fail("runtime attributes connector without NSObject/NSKeyPaths/NSValues");
                    const auto target = instantiate(kit, load, *target_index, coder);
                    const auto paths = kit.cf.array_values(instantiate(kit, load, *paths_index, coder));
                    const auto values = kit.cf.array_values(instantiate(kit, load, *values_index, coder));
                    if (paths.size() != values.size()) fail("runtime attributes: key paths and values differ in count");
                    const auto trace = host_environment("ALCHEMY_NIB_TRACE");
                    if (target)
                        for (std::size_t i = 0; i < paths.size(); ++i) {
                            if (!trace.empty() && trace != "0")
                                std::fprintf(stderr, "[alchemy nib] runtime attribute %s = 0x%llx (%s)\n", kit.text(paths[i]).c_str(),
                                             static_cast<unsigned long long>(values[i]), values[i] ? kit.text(kit.send_id(kit.send_id(values[i], "class"), "description")).c_str() : "nil");
                            kit.send_id(target, "setValue:forKeyPath:", {values[i], paths[i]});
                        }
                    continue;
                }
                establish_connector(kit, load, connection, coder);
                continue;
            }
            const auto source = connection.source ? instantiate(kit, load, *connection.source, coder) : 0;
            const auto destination = connection.destination ? instantiate(kit, load, *connection.destination, coder) : 0;
            if (connection.kind == nib::Connection::Kind::outlet) {
                // Storyboard-style outlets that create a child controller
                // through a selector are not nib connections AppKit makes
                // from compiled nibs; none is expected here.
                if (const auto creation = load->archive.string_at(connection.object, "NSChildControllerCreationSelectorName"))
                    unsupported("nib outlet '" + connection.label + "' with NSChildControllerCreationSelectorName " + *creation);
                connect_outlet(kit, source, destination, connection.label);
            } else if (source) {
                kit.send_id(source, "setTarget:", {destination});
                kit.send_id(source, "setAction:", {kit.sel(connection.label)});
            }
        }
        for (const auto& connection : load->graph.accessibility_connections) establish_connector(kit, load, connection, coder);
        for (const auto index : load->graph.top_level)
            if (const auto found = load->made.find(index); found != load->made.end() && found->second) top_level.push_back(found->second);
        // -awakeFromNib, in instantiation order, then the File's Owner.
        for (const auto index : load->order) {
            const auto found = load->made.find(index);
            if (found == load->made.end() || !found->second) continue;
            static_cast<void>(kit.send_optional(found->second, "awakeFromNib"));
        }
        if (load->owner) static_cast<void>(kit.send_optional(load->owner, "awakeFromNib"));
        for (const auto window_index : load->graph.visible_windows)
            if (const auto found = load->made.find(window_index); found != load->made.end() && found->second)
                kit.send_id(found->second, "makeKeyAndOrderFront:", {0});
    } catch (...) {
        kit.release(coder);
        for (const auto buffer : load->buffers) kit.process.release_guest(buffer);
        throw;
    }
    kit.release(coder);
    for (const auto buffer : load->buffers) kit.process.release_guest(buffer);
    load->buffers.clear();
    // Ownership: top-level objects keep the load's reference; the rest
    // belong to their parents now.
    std::set<Address> keep(top_level.begin(), top_level.end());
    for (const auto& [index, object] : load->made)
        if (object && !keep.contains(object) && !load->placeholders.contains(index)) {
            if (const auto trace = host_environment("ALCHEMY_NIB_TRACE"); trace == "2")
                std::fprintf(stderr, "[alchemy nib] release #%u %s 0x%llx\n", index, load->archive.class_name(index).c_str(),
                             static_cast<unsigned long long>(object));
            kit.release(object);
        }
    return top_level;
}
} // namespace

std::vector<Address> AppKit::load_nib(std::span<const std::uint8_t> bytes, Address owner) {
    auto load = std::make_shared<NibLoad>();
    load->archive = nib::Archive::parse_any(bytes);   // NIBArchive or keyed archive (bplist)
    load->owner = owner;
    return run_load(*this, load);
}

std::optional<std::vector<std::uint8_t>> AppKit::read_nib_file(Address bundle, std::string_view name) {
    if (!bundle) bundle = send(objc.find_class("NSBundle"), "mainBundle").x0;
    if (!bundle) return std::nullopt;
    std::string base(name);
    if (base.ends_with(".nib")) base.resize(base.size() - 4);
    const auto path_object = send_id(bundle, "pathForResource:ofType:", {auto_string(base), auto_string("nib")});
    if (!path_object) return std::nullopt;
    const auto path = text(path_object);
    // Diagnostic: ALCHEMY_NIB_TRACE=1 names every nib file as it is read.
    if (const auto trace = host_environment("ALCHEMY_NIB_TRACE"); !trace.empty() && trace != "0")
        std::fprintf(stderr, "[alchemy nib] %s\n", path.c_str());
    // Base-internationalized nibs are localized by a strings table per
    // language; this loader shows the base text only.
    if (const auto base = path.find("/Base.lproj/"); base != std::string::npos) {
        const auto languages = responds(bundle, "preferredLocalizations") ? send(bundle, "preferredLocalizations").x0 : 0;
        const auto language = languages && send(languages, "count").x0 ? text(send_id(languages, "objectAtIndex:", {0})) : std::string();
        if (!language.empty() && language != "Base") {
            const auto strings = path.substr(0, base) + "/" + language + ".lproj/" + base_name(path) + ".strings";
            if (read_file(strings)) unsupported("localized Base nib strings table " + strings);
        }
    }
    if (auto bytes = read_file(path)) return bytes;
    // A nib directory (localized or versioned compiled nibs).
    for (const auto* inner : {"/keyedobjects.nib", "/keyedobjects-110000.nib", "/keyedobjects-101300.nib", "/runtime.nib"})
        if (auto bytes = read_file(path + inner)) return bytes;
    return std::nullopt;
}

void AppKit::install_nib() {
    nib = declare_class("NSNib", "NSObject");
    // NSObject (NSNibAwaking): the base -awakeFromNib that subclasses reach
    // through [super awakeFromNib] does nothing.
    if (const auto object_class = objc.find_class("NSObject"))
        method(object_class, "awakeFromNib", [](CpuState&, GuestMemory&) {}, "v@:");
    if (!objc.find_class("NSCoder")) {
        const auto coder_class = objc.register_native_class("NSCoder", objc.find_class("NSObject"), 8);
        objc.publish_class("NSCoder", coder_class, foundation_library);
    }
    nib_coder = objc.register_native_class("_AlchemyNibCoder", objc.find_class("NSCoder"), 8);
    string_constant("_NSNibOwner", "NSNibOwner");
    string_constant("_NSNibTopLevelObjects", "NSNibTopLevelObjects");

    // ---- the coder ----
    const auto k = nib_coder;
    const auto state_of = [this](Address coder) {
        const auto state = objc.find_host_state<CoderState>(coder);
        if (!state || !state->load) fail("nib coder without a nib");
        return state;
    };
    method(k, "allowsKeyedCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(k, "requiresSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(k, "decodingFailurePolicy", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");
    method(k, "error", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(k, "systemVersion", [](CpuState& s, GuestMemory&) { s.x[0] = 1000; }, "I@:");
    method(k, "containsValueForKey:", [this, state_of](CpuState& s, GuestMemory&) {
        return_bool(s, value_for(*state_of(s.x[0]), text(s.x[2])) != nullptr);
    }, "B@:@");
    const auto decode_object = [this, state_of](Address coder, Address key) {
        const auto state = state_of(coder);
        return decode_reference(*this, state->load, value_for(*state, text(key)), coder);
    };
    method(k, "decodeObjectForKey:", [decode_object](CpuState& s, GuestMemory&) { s.x[0] = decode_object(s.x[0], s.x[2]); });
    method(k, "decodeObjectOfClass:forKey:", [decode_object](CpuState& s, GuestMemory&) { s.x[0] = decode_object(s.x[0], s.x[3]); });
    method(k, "decodeObjectOfClasses:forKey:", [decode_object](CpuState& s, GuestMemory&) { s.x[0] = decode_object(s.x[0], s.x[3]); });
    method(k, "decodeTopLevelObjectForKey:error:", [decode_object](CpuState& s, GuestMemory&) { s.x[0] = decode_object(s.x[0], s.x[2]); });
    method(k, "decodeArrayOfObjectsOfClass:forKey:", [decode_object](CpuState& s, GuestMemory&) { s.x[0] = decode_object(s.x[0], s.x[3]); });
    const auto number = [this, state_of](Address coder, Address key) -> double {
        const auto* value = value_for(*state_of(coder), text(key));
        if (!value) return 0;
        if (value->is_real()) return value->real;
        if (value->is_integer() || value->is_bool()) return static_cast<double>(value->integer);
        return 0;
    };
    const auto integer = [this, state_of](Address coder, Address key) -> std::int64_t {
        const auto* value = value_for(*state_of(coder), text(key));
        if (!value) return 0;
        if (value->is_real()) return static_cast<std::int64_t>(value->real);
        if (value->is_integer() || value->is_bool()) return value->integer;
        return 0;
    };
    method(k, "decodeBoolForKey:", [integer](CpuState& s, GuestMemory&) { s.x[0] = integer(s.x[0], s.x[2]) != 0; }, "B@:@");
    for (const auto* name : {"decodeIntForKey:", "decodeInt32ForKey:"})
        method(k, name, [integer](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(integer(s.x[0], s.x[2]))); }, "i@:@");
    for (const auto* name : {"decodeInt64ForKey:", "decodeIntegerForKey:"})
        method(k, name, [integer](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(integer(s.x[0], s.x[2])); }, "q@:@");
    method(k, "decodeDoubleForKey:", [number](CpuState& s, GuestMemory&) { set_d(s, 0, number(s.x[0], s.x[2])); }, "d@:@");
    method(k, "decodeFloatForKey:", [number](CpuState& s, GuestMemory&) { set_f(s, 0, static_cast<float>(number(s.x[0], s.x[2]))); }, "f@:@");
    method(k, "decodeBytesForKey:returnedLength:", [this, state_of](CpuState& s, GuestMemory& m) {
        const auto state = state_of(s.x[0]);
        const auto* value = value_for(*state, text(s.x[2]));
        if (!value || value->type != nib::ValueType::data) { if (s.x[3]) m.write64(s.x[3], 0); s.x[0] = 0; return; }
        const auto block = process.allocate_guest(std::max<Address>(1, value->data.size()));
        m.write_bytes(block, value->data);
        state->load->buffers.push_back(block);
        if (s.x[3]) m.write64(s.x[3], value->data.size());
        s.x[0] = block;
    }, "r*@:@^Q");
    const auto geometry = [this, state_of](Address coder, Address key) -> std::vector<double> {
        const auto state = state_of(coder);
        const auto* value = value_for(*state, text(key));
        if (!value || !value->is_object()) return {};
        const auto string_text = state->load->archive.string_value(static_cast<std::uint32_t>(value->integer));
        if (!string_text) return {};
        return nib::parse_numbers(*string_text).value_or(std::vector<double>{});
    };
    method(k, "decodePointForKey:", [geometry](CpuState& s, GuestMemory&) {
        const auto n = geometry(s.x[0], s.x[2]);
        return_point(s, n.size() >= 2 ? Point{n[0], n[1]} : Point{});
    }, "{CGPoint=dd}@:@");
    method(k, "decodeSizeForKey:", [geometry](CpuState& s, GuestMemory&) {
        const auto n = geometry(s.x[0], s.x[2]);
        return_size(s, n.size() >= 2 ? Size{n[0], n[1]} : Size{});
    }, "{CGSize=dd}@:@");
    method(k, "decodeRectForKey:", [geometry](CpuState& s, GuestMemory&) {
        const auto n = geometry(s.x[0], s.x[2]);
        return_rect(s, n.size() == 4 ? Rect{{n[0], n[1]}, {n[2], n[3]}} : Rect{});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:@");
    for (const auto* name : {"decodeObject", "decodeValueOfObjCType:at:", "decodeDataObject"})
        method(k, name, [name](CpuState&, GuestMemory&) { unsupported(std::string("-[NSCoder ") + name + "] (unkeyed nib coding)"); });

    // ---- NSNib ----
    const auto n = nib;
    const auto nib_of = [this](Address value) { return objc.host_state<NibState>(value); };
    method(n, "initWithNibNamed:bundle:", [this, nib_of](CpuState& s, GuestMemory&) {
        auto bytes = read_nib_file(s.x[3], text(s.x[2]));
        if (!bytes) { release(s.x[0]); s.x[0] = 0; return; }
        const auto state = nib_of(s.x[0]);
        state->bytes = std::move(*bytes);
        state->bundle = retain(s.x[3]);
    });
    method(n, "initWithNibData:bundle:", [this, nib_of](CpuState& s, GuestMemory&) {
        const auto state = nib_of(s.x[0]);
        state->bytes = data_bytes(s.x[2]);
        state->bundle = retain(s.x[3]);
    });
    for (const auto* name : {"instantiateWithOwner:topLevelObjects:", "instantiateNibWithOwner:topLevelObjects:"})
        method(n, name, [this, nib_of](CpuState& s, GuestMemory& m) {
            const auto objects = load_nib(nib_of(s.x[0])->bytes, s.x[2]);
            if (s.x[3]) m.write64(s.x[3], auto_array(objects));
            s.x[0] = 1;
        }, "B@:@^@");
    method(n, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<NibState>(s.x[0])) release(std::exchange(state->bundle, 0));
        static_cast<void>(objc.send_super(s.x[0], nib, sel("dealloc")));
    }, "v@:");

    // ---- NSBundle (AppKit additions) ----
    if (const auto bundle_class = objc.find_class("NSBundle")) {
        method(bundle_class, "loadNibNamed:owner:topLevelObjects:", [this](CpuState& s, GuestMemory& m) {
            const auto bytes = read_nib_file(s.x[0], text(s.x[2]));
            if (!bytes) { s.x[0] = 0; return; }
            const auto objects = load_nib(*bytes, s.x[3]);
            if (s.x[4]) m.write64(s.x[4], auto_array(objects));
            s.x[0] = 1;
        }, "B@:@@^@");
        class_method(bundle_class, "loadNibNamed:owner:", [this](CpuState& s, GuestMemory&) {
            const auto bytes = read_nib_file(0, text(s.x[2]));
            if (!bytes) { s.x[0] = 0; return; }
            static_cast<void>(load_nib(*bytes, s.x[3]));
            s.x[0] = 1;
        }, "B@:@@");
    }

    // ---- NSApplicationMain ----
    for (const auto library : {appkit_library, cocoa_library})
        function(library, "_NSApplicationMain", [this](CpuState& s, GuestMemory&) {
            const auto bundle = send(objc.find_class("NSBundle"), "mainBundle").x0;
            Address principal = application;
            if (bundle) {
                const auto name = send_id(bundle, "objectForInfoDictionaryKey:", {auto_string("NSPrincipalClass")});
                if (name) {
                    const auto found = objc.find_class(text(name));
                    if (!found) fail("NSPrincipalClass " + text(name) + " does not exist");
                    principal = found;
                }
            }
            const auto shared = send(principal, "sharedApplication").x0;
            if (bundle) {
                const auto main_nib = send_id(bundle, "objectForInfoDictionaryKey:", {auto_string("NSMainNibFile")});
                if (main_nib) {
                    const auto bytes = read_nib_file(bundle, text(main_nib));
                    if (!bytes) fail("the main nib " + text(main_nib) + " is missing from the bundle");
                    static_cast<void>(load_nib(*bytes, shared));
                }
            }
            static_cast<void>(send(shared, "run"));
            s.x[0] = 0;
        });

    install_nib_coding();
}

// ---- -initWithCoder: of AppKit classes -------------------------------------------------------
void AppKit::install_nib_coding() {
    struct Access {
        AppKit& kit;
        std::shared_ptr<CoderState> state;
        Address coder;
        const nib::Archive& archive() const { return state->load->archive; }
        std::uint32_t current() const { return state->stack.back(); }
        const nib::Value* value(std::string_view key) const { return archive().find(current(), key); }
        std::int64_t integer(std::string_view key, std::int64_t fallback = 0) const {
            const auto* v = value(key);
            return v && (v->is_integer() || v->is_bool()) ? v->integer : fallback;
        }
        double real(std::string_view key, double fallback = 0) const {
            const auto* v = value(key);
            if (!v) return fallback;
            return v->is_real() ? v->real : static_cast<double>(v->integer);
        }
        bool flag(std::string_view key, bool fallback = false) const { return integer(key, fallback ? 1 : 0) != 0; }
        Address object(std::string_view key) const { return decode_reference(kit, state->load, value(key), coder); }
        std::optional<std::string> string(std::string_view key) const { return archive().string_at(current(), key); }
        std::optional<Rect> rect(std::string_view key) const {
            const auto text = string(key);
            if (!text) return std::nullopt;
            const auto n = nib::parse_numbers(*text);
            if (!n || (n->size() != 4 && n->size() != 2)) return std::nullopt;
            return rect_from(*n);
        }
    };
    const auto access = [this](Address coder) {
        const auto state = objc.find_host_state<CoderState>(coder);
        if (!state || !state->load || state->stack.empty()) unsupported("-initWithCoder: with a coder other than the nib coder");
        return Access{*this, state, coder};
    };
    const auto super_init = [this](Address self, Address cls, Address coder) {
        CallRegs registers;
        registers.x[2] = coder;
        return objc.send_super(self, cls, sel("initWithCoder:"), registers).x0;
    };

    // NSNib inside an archive (table cell view prototypes): its nib bytes,
    // compiled nibs both keyed or not (the loader reads either);
    // NSNibFileUseParentBundle has it load resources from the outer nib's
    // bundle, which is the main bundle here; nibs with their own image or
    // sound files are not supported.
    method(nib, "initWithCoder:", [this, access](CpuState& s, GuestMemory&) {
        const auto a = access(s.x[2]);
        const auto data = a.object("NSNibFileData");
        if (!data) fail("archived NSNib without NSNibFileData");
        static_cast<void>(a.flag("NSNibFileIsKeyed"));
        static_cast<void>(a.flag("NSNibFileUseParentBundle"));
        for (const auto* key : {"NSNibFileImages", "NSNibFileSounds"})
            if (const auto list = a.object(key); list && send(list, "count").x0)
                unsupported(std::string("archived NSNib with ") + key);
        s.x[0] = send_id(s.x[0], "initWithNibData:bundle:", {data, 0});
    });
    // NSTabView: its items and the selected one.
    method(tab_view, "initWithCoder:", [this, access, super_init](CpuState& s, GuestMemory&) {
        const auto self = super_init(s.x[0], tab_view, s.x[2]);
        s.x[0] = self;
        const auto a = access(s.x[2]);
        if (const auto list = a.object("NSTabViewItems"))
            for (const auto item : items(list)) if (item) static_cast<void>(send_id(self, "addTabViewItem:", {item}));
        if (a.value("NSAllowTruncatedLabels")) static_cast<void>(send_id(self, "setAllowsTruncatedLabels:", {a.flag("NSAllowTruncatedLabels") ? 1u : 0u}));
        if (const auto selected = a.object("NSSelectedTabViewItem")) static_cast<void>(send_id(self, "selectTabViewItem:", {selected}));
    });
    method(tab_view_item, "initWithCoder:", [this, access](CpuState& s, GuestMemory&) {
        const auto a = access(s.x[2]);
        const auto self = send_id(s.x[0], "initWithIdentifier:", {a.object("NSIdentifier")});
        s.x[0] = self;
        if (const auto label = a.object("NSLabel")) static_cast<void>(send_id(self, "setLabel:", {label}));
        if (const auto item_view = a.object("NSView")) static_cast<void>(send_id(self, "setView:", {item_view}));
        if (const auto color_object = a.object("NSColor"); color_object && responds(self, "setColor:"))
            static_cast<void>(send_id(self, "setColor:", {color_object}));
        static_cast<void>(a.object("NSTabView"));   // the owning tab view adds the item
    });
    // NSCursor: archived cursors are the system ones (NSCursorType 0 the
    // arrow, 13 the pointing hand of link attributes), at NSHotSpot.
    method(cursor, "initWithCoder:", [this, access](CpuState& s, GuestMemory&) {
        const auto a = access(s.x[2]);
        const auto type = a.integer("NSCursorType", 0);
        static_cast<void>(a.string("NSHotSpot"));
        SystemCursor kind{};
        switch (type) {
        case 0: kind = SystemCursor::arrow; break;
        case 13: kind = SystemCursor::pointing_hand; break;
        default: unsupported("NSCursorType " + std::to_string(type) + " in an archived cursor");
        }
        release(s.x[0]);
        s.x[0] = retain(make_cursor(kind));
    });
    // NSObject-level default: plain -init (placeholders' guest classes).
    method(responder, "initWithCoder:", [this](CpuState& s, GuestMemory&) { s.x[0] = send(s.x[0], "init").x0; });
    // NSView: frame, flags, subviews.
    method(view, "initWithCoder:", [this, access](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto a = access(s.x[2]);
        const auto state = view_state(self);
        state->frame = a.rect("NSFrame").value_or(a.rect("NSFrameSize").value_or(Rect{}));
        const auto flags = static_cast<std::uint64_t>(a.integer("NSvFlags", 0x100));
        state->autoresizing_mask = flags & 0x3f;
        state->autoresizes_subviews = (flags & 0x100) != 0;
        state->hidden = (flags & 0x80000000ULL) != 0;
        if (const auto tag = a.value("NSTag")) state->tag = tag->integer;
        static_cast<void>(send_id(self, "_alchemyDecodeViewExtras:", {s.x[2]}));
    });
    method(view, "_alchemyDecodeViewExtras:", [this, access](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto a = access(s.x[2]);
        if (const auto subviews = a.object("NSSubviews"))
            for (const auto child : items(subviews))
                if (child && view_state(child)->superview != self) static_cast<void>(send_id(self, "addSubview:", {child}));
        if (const auto appearance_object = a.object("NSAppearance")) assign(view_state(self)->appearance, appearance_object);
        // Table cell view prototypes carry their reuse identifier.
        if (const auto identifier = a.object("NSReuseIdentifierKey")) static_cast<void>(send_id(self, "setIdentifier:", {identifier}));
        const auto state = view_state(self);
        // Auto Layout (kit/completion/windows/auto_layout.cpp): the
        // constraints this view holds, whether its frame becomes
        // constraints, its content priorities ("{horizontal, vertical}").
        if (a.value("NSDoNotTranslateAutoresizingMask")) state->translates_autoresizing = !a.flag("NSDoNotTranslateAutoresizingMask");
        for (const auto& [key, slot] : {std::pair<const char*, std::array<float, 2>*>{"NSHuggingPriority", &state->hugging},
                                        {"NSAntiCompressionPriority", &state->compression}})
            if (const auto text = a.string(key))
                if (const auto numbers = nib::parse_numbers(*text); numbers && numbers->size() == 2)
                    *slot = {static_cast<float>((*numbers)[0]), static_cast<float>((*numbers)[1])};
        if (const auto constraints = a.object("NSViewConstraints"))
            static_cast<void>(send_id(self, "addConstraints:", {constraints}));
        // Clipping (NSViewClipsToBoundsKeyName, or Interface Builder's
        // IBNSClipsToBounds), layer hosting, the OpenGL surface request.
        if (a.value("NSViewClipsToBoundsKeyName")) state->clips_to_bounds = a.flag("NSViewClipsToBoundsKeyName");
        else if (a.value("IBNSClipsToBounds")) state->clips_to_bounds = a.flag("IBNSClipsToBounds");
        if (a.flag("NSViewIsLayerTreeHost")) state->wants_layer = true;
        if (a.value("NSViewWantsBestResolutionOpenGLSurface")) state->best_resolution_gl = a.flag("NSViewWantsBestResolutionOpenGLSurface");
        if (const auto next = a.object("NSNextKeyView")) static_cast<void>(send_id(self, "setNextKeyView:", {next}));
        if (const auto shadow = a.object("NSViewShadow")) assign(state->shadow, shadow);
        if (const auto recognizers = a.object("NSGestureRecognizers"))
            for (const auto recognizer : items(recognizers))
                if (recognizer) static_cast<void>(send_id(self, "addGestureRecognizer:", {recognizer}));
        if (const auto types = a.object("NSDragTypes")) {
            const auto list = responds(types, "allObjects") ? send(types, "allObjects").x0 : types;
            static_cast<void>(send_id(self, "registerForDraggedTypes:", {list}));
        }
        // Interface Builder archives its layout-margins and safe-area guides
        // as nil; a guide object would be an NSLayoutGuide constraints refer to.
        for (const auto* key : {"IBNSLayoutMarginsGuide", "IBNSSafeAreaLayoutGuide"})
            if (a.object(key)) unsupported(std::string("an archived view with an ") + key + " layout guide");
        // Not read: NSNextResponder and NSSuperview (the responder chain and
        // the superview follow -addSubview:), NSNibTouchBar (no Touch Bar).
    }, "v@:@");
    // NSControl: the cell.
    method(control, "initWithCoder:", [this, access, super_init](CpuState& s, GuestMemory&) {
        const auto self = super_init(s.x[0], control, s.x[2]);
        s.x[0] = self;
        const auto a = access(s.x[2]);
        if (const auto made = a.object("NSCell")) static_cast<void>(send_id(self, "setCell:", {made}));
        if (const auto target = control_cell(*this, self)) {
            const auto cs = cell_state(*this, target);
            if (a.value("NSEnabled")) cs->enabled = a.flag("NSEnabled", true);
            // Interface Builder repeats the cell's settings on the control.
            if (a.value("NSControlSendActionMask")) cs->send_action_mask = static_cast<std::uint64_t>(a.integer("NSControlSendActionMask"));
            if (a.value("NSControlLineBreakMode")) cs->line_break = a.integer("NSControlLineBreakMode");
            if (a.value("NSControlRefusesFirstResponder")) cs->refuses_first_responder = a.flag("NSControlRefusesFirstResponder");
            if (a.value("NSControlUsesSingleLineMode")) cs->single_line = a.flag("NSControlUsesSingleLineMode");
            if (a.value("NSControlWritingDirection")) cs->writing_direction = a.integer("NSControlWritingDirection");
        } else {
            // Controls without a cell (NSTableView, NSScroller) keep target and action themselves.
            const auto state = extension<ControlState>(self);
            if (const auto target = a.object("NSControlTarget")) state->target = target;
            if (const auto action_name = a.string("NSControlAction")) state->action = sel(*action_name);
            if (a.value("NSControlRefusesFirstResponder")) state->refuses_first_responder = a.flag("NSControlRefusesFirstResponder");
        }
        // Not read: NSControlContinuous (archived as the complement of the
        // cell's continuous flag, NSCellFlags 0x40000, which is read),
        // NSControlTextAlignment (the cell's NSCellFlags2 alignment),
        // NSAllowsLogicalLayoutDirection (left and right are not swapped
        // in right-to-left layouts), NSControlAllowsExpansionToolTips (no
        // expansion tool tips), NSStepperMinValue, NSStepperMaxValue,
        // NSStepperIncrement, NSStepperWraps and NSStepperAutorepeat (an
        // NSStepper archives them as zeros; its NSStepperCell's NSMinValue,
        // NSMaxValue, NSIncrement, NSValueWraps and NSAutorepeat are read),
        // IBNSShadowedSymbolConfiguration and NSControlContents (SF Symbol
        // configurations and NSSwitch's design-time state).
        // NSImageView's NSImageViewPlaceholderPrecedence is not read (no placeholder images).
        if (is_kind(self, text_field)) {
            if (const auto target = control_cell(*this, self); target && a.value("NSPreferredMaxLayoutWidth"))
                cell_state(*this, target)->preferred_max_width = a.real("NSPreferredMaxLayoutWidth");
            // Not read: NSControlAutosetMaxLayoutWidth (the width is not set
            // automatically), NSAllowsWritingTools and
            // NSTextFieldAllowsWritingToolsAffordance (no Writing Tools),
            // NSTextFieldAlignmentRectInsetsVersion (alignment rects are frames).
        }
    });
    // NSCell: contents, font and the packed flags.
    method(cell, "initWithCoder:", [this, access](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto a = access(s.x[2]);
        const auto state = cell_state(*this, self);
        const auto flags = static_cast<std::uint32_t>(a.integer("NSCellFlags"));
        const auto flags2 = static_cast<std::uint32_t>(a.integer("NSCellFlags2"));
        state->kind = static_cast<CellState::Kind>(std::min<std::uint32_t>((flags >> 26) & 3, 2));
        if (!a.value("NSCellFlags")) state->kind = CellState::Kind::text;
        state->wraps = (flags & 0x40) == 0 && a.value("NSCellFlags");
        state->scrollable = (flags & 0x100000) != 0;
        state->selectable = (flags & 0x200000) != 0;
        state->bezeled = (flags & 0x400000) != 0;
        state->bordered = (flags & 0x800000) != 0;
        state->editable = (flags & 0x10000000) != 0;
        state->enabled = (flags & 0x20000000) == 0;
        state->highlighted = (flags & 0x40000000) != 0;
        state->state = (flags & 0x80000000) ? state_on : state_off;
        state->continuous = (flags & 0x40000) != 0;
        state->allows_mixed = (flags2 & 0x1000000) != 0;
        state->alignment = static_cast<std::int64_t>((flags2 >> 26) & 7);
        state->control_size = a.integer("NSControlSize2", a.integer("NSControlSize"));
        if (const auto contents = a.object("NSContents")) {
            if (is_kind(contents, image)) { state->kind = CellState::Kind::image; assign(state->image, contents); }
            else release(std::exchange(state->contents, retain(contents)));
        }
        if (const auto support = a.object("NSSupport")) {
            if (is_kind(support, font)) assign(state->font, support);
            else if (is_kind(support, image)) assign(state->image, support);
        }
        if (const auto value = a.object("NSTextColor")) assign(state->text_color, value);
        if (const auto value = a.object("NSBackgroundColor")) assign(state->background_color, value);
        if (const auto value = a.object("NSPlaceholderString")) release(std::exchange(state->placeholder, cf.copy(value)));
        if (const auto tag = a.value("NSTag")) state->tag = tag->integer;
    });
    // NSButtonCell: title, bezel, key equivalent, checkbox / radio images.
    method(button_cell, "initWithCoder:", [this, access, super_init](CpuState& s, GuestMemory&) {
        const auto self = super_init(s.x[0], button_cell, s.x[2]);
        s.x[0] = self;
        const auto a = access(s.x[2]);
        const auto state = cell_state(*this, self);
        assign(state->title, state->contents ? state->contents : auto_string(""));
        release(std::exchange(state->contents, 0));
        state->kind = CellState::Kind::text;
        state->bezel_style = a.integer("NSBezelStyle", bezel_style::rounded);
        const auto flags = static_cast<std::uint32_t>(a.integer("NSButtonFlags"));
        state->bordered = (flags & 0x800000) != 0;
        state->transparent = (flags & 0x8000) != 0;
        state->button_type = button_type::momentary_push_in;
        if (const auto normal = a.archive().object_at(a.current(), "NSNormalImage")) {
            if (a.archive().class_name(*normal) == "NSButtonImageSource") {
                const auto image_name = a.archive().string_at(*normal, "NSImageName").value_or("");
                if (image_name == "NSSwitch") state->button_type = button_type::switch_;
                if (image_name == "NSRadioButton") state->button_type = button_type::radio;
                state->bordered = false;
            } else if (const auto picture = a.object("NSNormalImage")) {
                assign(state->image, picture);
                state->image_position = 2;   // NSImageLeft
            }
        }
        if (const auto alternate = a.object("NSAlternateContents")) release(std::exchange(state->alternate_title, cf.copy(alternate)));
        if (const auto key = a.object("NSKeyEquivalent")) release(std::exchange(state->key_equivalent, cf.copy(key)));
        state->key_modifier_mask = static_cast<std::uint64_t>(a.integer("NSKeyEquivModMask"));
        // NSAuxButtonType is the button type; the highlight and state
        // masks follow it as -setButtonType: sets them.
        if (a.value("NSAuxButtonType")) static_cast<void>(send_id(self, "setButtonType:", {static_cast<Address>(a.integer("NSAuxButtonType"))}));
        if (const auto alternate = a.archive().object_at(a.current(), "NSAlternateImage");
            alternate && a.archive().class_name(*alternate) != "NSButtonImageSource")
            if (const auto picture = a.object("NSAlternateImage")) assign(state->alternate_image, picture);
        // Milliseconds in the archive.
        if (a.value("NSPeriodicDelay")) state->periodic_delay = a.real("NSPeriodicDelay") / 1000;
        if (a.value("NSPeriodicInterval")) state->periodic_interval = a.real("NSPeriodicInterval") / 1000;
        // Not read: NSButtonFlags2 (its bezel and key-equivalent bits repeat
        // NSBezelStyle and NSKeyEquivModMask), NSAccessibilityOverriddenAttributes
        // (no assistive-technology host).
        if (state->button_type == button_type::switch_ || state->button_type == button_type::radio) state->alignment = 4;
        else if (state->alignment == 4) state->alignment = 2;
    });
    // NSTextFieldCell: background drawing.
    method(text_field_cell, "initWithCoder:", [this, access, super_init](CpuState& s, GuestMemory&) {
        const auto self = super_init(s.x[0], text_field_cell, s.x[2]);
        s.x[0] = self;
        const auto a = access(s.x[2]);
        const auto state = cell_state(*this, self);
        state->draws_background = a.flag("NSDrawsBackground", false);
        // Not read: NSAutomaticTextCompletionDisabled (no text completion),
        // NSCellIdentifier (a cell-based column's design-time identifier).
        if (!state->contents) state->contents = string("");
    });
    // NSPopUpButtonCell: its menu and selection.
    method(popup_button_cell, "initWithCoder:", [this, access, super_init](CpuState& s, GuestMemory&) {
        const auto self = super_init(s.x[0], popup_button_cell, s.x[2]);
        s.x[0] = self;
        const auto a = access(s.x[2]);
        const auto state = cell_state(*this, self);
        state->pulls_down = a.flag("NSPullDown");
        if (const auto made = a.object("NSMenu")) static_cast<void>(send_id(self, "setMenu:", {made}));
        if (const auto chosen = a.object("NSMenuItem"); chosen && state->menu) {
            const auto list = objc.host_state<MenuState>(state->menu)->items;
            for (std::size_t i = 0; i < list.size(); ++i) if (list[i] == chosen) state->selected_item = static_cast<std::int64_t>(i);
        }
        if (a.value("NSSelectedIndex")) state->selected_item = a.integer("NSSelectedIndex");
        if (a.value("NSPreferredEdge")) state->preferred_edge = static_cast<std::uint64_t>(a.integer("NSPreferredEdge"));
        if (a.value("NSArrowPosition")) state->arrow_position = static_cast<std::uint64_t>(a.integer("NSArrowPosition"));
        if (a.value("NSAltersState")) state->alters_state = a.flag("NSAltersState");
        if (a.value("NSUsesItemFromMenu")) state->uses_item_from_menu = a.flag("NSUsesItemFromMenu");
        // Not read: NSMenuItemRespectAlignment (item titles are not aligned to the cell's text).
    });
    // NSImageCell: alignment, scaling and frame style.
    method(image_cell, "initWithCoder:", [this, access, super_init](CpuState& s, GuestMemory&) {
        const auto self = super_init(s.x[0], image_cell, s.x[2]);
        s.x[0] = self;
        const auto a = access(s.x[2]);
        const auto state = cell_state(*this, self);
        state->kind = CellState::Kind::image;
        if (a.value("NSAlign")) state->image_alignment = static_cast<std::uint64_t>(a.integer("NSAlign"));
        if (a.value("NSScale")) state->image_scaling = a.integer("NSScale");
        if (a.value("NSStyle")) state->image_frame = a.integer("NSStyle") != 0;
        // Not read: NSImageAnimation (animated images show their first frame),
        // NSContentTintColor (the cell's tint, kept on NSImageView).
    });
    // NSSliderCell / NSStepperCell: the numeric range and value.
    method(slider_cell, "initWithCoder:", [this, access, super_init](CpuState& s, GuestMemory&) {
        const auto self = super_init(s.x[0], slider_cell, s.x[2]);
        s.x[0] = self;
        const auto a = access(s.x[2]);
        const auto state = cell_state(*this, self);
        state->min_value = a.real("NSMinValue", 0);
        state->max_value = a.real("NSMaxValue", 1);
        state->alt_increment = a.real("NSAltIncValue", 0);
        state->tick_marks = a.integer("NSNumberOfTickMarks", 0);
        state->tick_position = a.integer("NSTickMarkPosition", 0);
        state->ticks_only = a.flag("NSAllowsTickMarkValuesOnly");
        if (a.value("NSVertical")) state->vertical = a.flag("NSVertical") ? 1 : 0;
        if (a.integer("NSSliderType", 0) != 0) unsupported("nib NSSliderTypeCircular (circular sliders)");
        state->continuous = true;
        release(std::exchange(state->contents, cf.real(std::clamp(a.real("NSValue", state->min_value),
            std::min(state->min_value, state->max_value), std::max(state->min_value, state->max_value)))));
    });
    method(stepper_cell, "initWithCoder:", [this, access, super_init](CpuState& s, GuestMemory&) {
        const auto self = super_init(s.x[0], stepper_cell, s.x[2]);
        s.x[0] = self;
        const auto a = access(s.x[2]);
        const auto state = cell_state(*this, self);
        state->min_value = a.real("NSMinValue", 0);
        state->max_value = a.real("NSMaxValue", 59);
        state->increment = a.real("NSIncrement", 1);
        state->value_wraps = a.flag("NSValueWraps", false);
        state->autorepeat = a.flag("NSAutorepeat", true);
        release(std::exchange(state->contents, cf.real(std::clamp(a.real("NSValue", state->min_value),
            std::min(state->min_value, state->max_value), std::max(state->min_value, state->max_value)))));
    });
    // NSComboBoxCell: the internal item list (or the data source).
    method(combo_box_cell, "initWithCoder:", [this, access, super_init](CpuState& s, GuestMemory&) {
        const auto self = super_init(s.x[0], combo_box_cell, s.x[2]);
        s.x[0] = self;
        const auto a = access(s.x[2]);
        const auto state = cell_state(*this, self);
        state->trailing_inset = 20;
        state->visible_items = a.integer("NSVisibleItemCount", 5);
        state->uses_data_source = a.flag("NSUsesDataSource");
        state->completes = a.flag("NSCompletes");
        state->has_vertical_scroller = a.flag("NSHasVerticalScroller", true);
        // Not read: NSAutomaticTextCompletionStored (no text completion).
        if (const auto source = a.object("NSDataSource")) state->data_source = source;
        if (const auto list = a.object("NSPopUpListData"))
            for (const auto value : items(list)) if (value) state->combo_items.push_back(retain(value));
    });
    // NSMenu / NSMenuItem.
    method(menu, "initWithCoder:", [this, access](CpuState& s, GuestMemory&) {
        const auto a = access(s.x[2]);
        const auto self = send_id(s.x[0], "initWithTitle:", {auto_string(a.string("NSTitle").value_or(""))});
        s.x[0] = self;
        if (const auto list = a.object("NSMenuItems"))
            for (const auto item : items(list)) if (item) static_cast<void>(send_id(self, "addItem:", {item}));
        if (a.value("NSNoAutoenable")) static_cast<void>(send_id(self, "setAutoenablesItems:", {a.flag("NSNoAutoenable") ? 0u : 1u}));
        if (a.value("NSMenuExcludeMarkColumn"))
            static_cast<void>(send_id(self, "setShowsStateColumn:", {a.flag("NSMenuExcludeMarkColumn") ? 0u : 1u}));
    });
    method(menu_item, "initWithCoder:", [this, access](CpuState& s, GuestMemory&) {
        const auto a = access(s.x[2]);
        if (a.flag("NSIsSeparator")) {
            const auto separator = send(menu_item, "separatorItem").x0;
            release(s.x[0]);
            s.x[0] = retain(separator);
            return;
        }
        const auto action_name = a.string("NSAction");
        const auto self = send_id(s.x[0], "initWithTitle:action:keyEquivalent:",
                                  {auto_string(a.string("NSTitle").value_or("")), action_name ? sel(*action_name) : 0,
                                   auto_string(a.string("NSKeyEquiv").value_or(""))});
        s.x[0] = self;
        if (a.value("NSKeyEquivModMask")) static_cast<void>(send_id(self, "setKeyEquivalentModifierMask:", {static_cast<Address>(a.integer("NSKeyEquivModMask"))}));
        if (a.value("NSTag")) static_cast<void>(send_id(self, "setTag:", {static_cast<Address>(a.integer("NSTag"))}));
        if (a.value("NSState")) static_cast<void>(send_id(self, "setState:", {static_cast<Address>(a.integer("NSState"))}));
        if (a.flag("NSIsDisabled")) static_cast<void>(send_id(self, "setEnabled:", {0}));
        if (a.flag("NSIsHidden")) static_cast<void>(send_id(self, "setHidden:", {1}));
        if (const auto target = a.object("NSTarget")) static_cast<void>(send_id(self, "setTarget:", {target}));
        if (const auto submenu = a.object("NSSubmenu")) static_cast<void>(send_id(self, "setSubmenu:", {submenu}));
        // The identifier and state images are the value kit's NSMenuItem
        // properties (darwin/appkit/menus.cpp), installed in every alchemy-fx process.
        for (const auto& [key, setter] : {std::pair{"NSMenuItemIdentifier", "setIdentifier:"}, std::pair{"NSOnImage", "setOnStateImage:"},
                                          std::pair{"NSMixedImage", "setMixedStateImage:"}})
            if (const auto value = a.object(key)) {
                CallRegs registers;
                registers.x[2] = value;
                static_cast<void>(send_optional(self, setter, registers));
            }
        // Not read: NSMnemonicLoc (no mnemonics), NSHiddenInRepresentation
        // (no alternate representations), NSAllowsKeyEquivalentLocalization
        // and NSAllowsKeyEquivalentMirroring (key equivalents are not
        // localized or mirrored).
    });
    // NSProgressIndicator.
    method(progress_indicator, "initWithCoder:", [this, access, super_init](CpuState& s, GuestMemory&) {
        const auto self = super_init(s.x[0], progress_indicator, s.x[2]);
        s.x[0] = self;
        const auto a = access(s.x[2]);
        const auto state = extension<ControlState>(self);
        const auto flags = static_cast<std::uint64_t>(a.integer("NSpiFlags"));
        state->indeterminate = (flags & 0x2) != 0;
        state->bar_style = (flags & 0x1000) == 0;
        state->min_value = a.real("NSMinValue", 0);
        state->max_value = a.real("NSMaxValue", 100);
        state->value = a.real("NSProgressIndicatorDoubleValue", a.real("NSValue", state->min_value));
        // Not read: NSViewContentFilters (Core Image content filters are not applied).
    });
    // NSBox.
    method(box, "initWithCoder:", [this, access, super_init](CpuState& s, GuestMemory&) {
        const auto self = super_init(s.x[0], box, s.x[2]);
        s.x[0] = self;
        const auto a = access(s.x[2]);
        const auto state = extension<ControlState>(self);
        state->box_type = a.integer("NSBoxType", 0);
        state->border_type = a.integer("NSBorderType", 3);
        state->title_position = a.integer("NSTitlePosition", 2);
        if (const auto title_cell = a.object("NSTitleCell"))
            release(std::exchange(state->box_title, retain(send(title_cell, "stringValue").x0)));
        if (const auto content = a.object("NSContentView")) state->content_view = content;
        if (const auto fill_color = a.object("NSFillColor2")) assign(state->fill_color, fill_color);
        if (const auto border_color = a.object("NSBorderColor2")) assign(state->border_color, border_color);
        if (a.value("NSCornerRadius2")) state->corner_radius = a.real("NSCornerRadius2", 5);
        state->box_transparent = a.flag("NSTransparent");
        if (const auto offsets = a.string("NSOffsets"))   // "{width, height}"
            if (const auto n = nib::parse_numbers(*offsets); n && n->size() == 2) state->box_margins = {(*n)[0], (*n)[1]};
        // Not read: IBNSBoxIsUsingDocumentContentView (Interface Builder's editing state).
    });
}

} // namespace alchemy::gui::appkit
