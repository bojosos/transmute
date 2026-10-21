#include "alchemy/objc_metadata.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace alchemy {
namespace {
constexpr std::uint64_t maximum_items = 65536;
constexpr std::uint64_t maximum_string = 65536;
constexpr std::uint32_t maximum_instance = 16U * 1024U * 1024U;

std::string hex(std::uint32_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    do { text.insert(text.begin(), digits[value & 15U]); value >>= 4; } while (value);
    return text;
}
std::string hex64(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    do { text.insert(text.begin(), digits[value & 15U]); value >>= 4; } while (value);
    return text;
}
[[noreturn]] void malformed(std::string_view detail) {
    throw std::runtime_error("Objective-C metadata: " + std::string(detail));
}
std::uint64_t add(std::uint64_t address, std::uint64_t size) {
    if (size > UINT64_MAX - address) malformed("address arithmetic overflow");
    return address + size;
}
std::uint64_t relative(std::uint64_t field, std::uint32_t encoded) {
    const auto displacement = std::bit_cast<std::int32_t>(encoded);
    if (displacement >= 0) return add(field, static_cast<std::uint32_t>(displacement));
    const auto magnitude = static_cast<std::uint64_t>(-static_cast<std::int64_t>(displacement));
    if (magnitude > field) malformed("relative pointer underflow");
    return field - magnitude;
}

class Decoder {
public:
    Decoder(const ProcessContext& process, const transmute::GuestMemory& memory,
            std::span<const ImageRuntimeInfo> images, bool runtime = false)
        : memory_(memory), images_(process.images()), runtime_(runtime) {
        images_.insert(images_.end(), images.begin(), images.end());
        for (const auto& image : images_) {
            if (image.end < image.begin) malformed("invalid image extent");
            for (const auto& section : image.sections) {
                const auto end = add(section.address, section.size);
                if (section.address < image.begin || end > image.end)
                    malformed("section outside image extent");
            }
            for (const auto& code : image.code_ranges)
                if (code.end < code.begin || code.begin < image.begin || code.end > image.end)
                    malformed("code range outside image extent");
        }
    }

    ObjcImageInfo image(const ImageRuntimeInfo& image) {
        ObjcImageInfo result;
        result.module_id = image.module_id;
        bool class_properties = false;
        for (const auto& section : image.sections) {
            if (section.name == "__objc_imageinfo") {
                if (section.size != 8) malformed("invalid image-info size");
                range(section.address, 8);
                if (memory_.read32(section.address) != 0) malformed("unsupported image-info version");
                const auto flags = memory_.read32(add(section.address, 4));
                // Bits 8-15 are the Swift ABI version and 16-31 the Swift
                // language version of Swift images; they carry no layout.
                if ((flags & 0xffU & ~0x58U) != 0)
                    malformed("unsupported image-info flags (GC or preoptimized metadata)");
                class_properties = (flags & 0x40U) != 0;
            }
            if (section.size && section.name == "__objc_catlist2")
                malformed("unsupported Swift stub categories (__objc_catlist2)");
        }
        std::set<std::uint64_t> classes, categories, listed;
        std::vector<std::uint64_t> pending;
        for (const auto& section : image.sections) {
            if (section.name == "__objc_classlist") {
                pointers(section, [&](auto address) {
                    if (!classes.insert(address).second) malformed("duplicate class-list entry");
                    auto ordinary = klass(address, false);
                    auto meta = klass(ordinary.metaclass, true);
                    if (ordinary.name != meta.name) malformed("class and metaclass names differ");
                    result.classes.push_back(std::move(ordinary));
                    result.classes.push_back(std::move(meta));
                });
            } else if (section.name == "__objc_catlist") {
                pointers(section, [&](auto address) {
                    if (!categories.insert(address).second) malformed("duplicate category-list entry");
                    result.categories.push_back(category(address, class_properties));
                });
            } else if (section.name == "__objc_selrefs") {
                if (section.size % 8 != 0) malformed("selector-reference section is not pointer sized");
                if (section.size / 8 > maximum_items) malformed("selector-reference count exceeds limit");
                range(section.address, section.size);
                for (std::uint64_t offset = 0; offset < section.size; offset += 8) {
                    const auto slot = add(section.address, offset);
                    result.selectors.push_back({slot, string(memory_.read64(slot))});
                }
            } else if (section.name == "__objc_protolist") {
                pointers(section, [&](auto address) {
                    if (!listed.insert(address).second) malformed("duplicate protocol-list entry");
                    pending.push_back(address);
                });
            } else if (section.name == "__objc_protorefs") {
                if (section.size % 8 != 0) malformed("protocol-reference section is not pointer sized");
                if (section.size / 8 > maximum_items) malformed("protocol-reference count exceeds limit");
                range(section.address, section.size);
                for (std::uint64_t offset = 0; offset < section.size; offset += 8) {
                    const auto slot = add(section.address, offset);
                    result.protocol_references.push_back({slot, reference(memory_.read64(slot))});
                }
            } else if (section.name == "__objc_classrefs" || section.name == "__objc_superrefs") {
                if (section.size % 8 != 0) malformed("class-reference section is not pointer sized");
                if (section.size / 8 > maximum_items) malformed("class-reference count exceeds limit");
                range(section.address, section.size);
                for (std::uint64_t offset = 0; offset < section.size; offset += 8) result.class_references.push_back(add(section.address, offset));
            } else if (section.name == "__objc_nlclslist") {
                pointers(section, [&](auto address) { result.nonlazy_classes.push_back(address); });
            } else if (section.name == "__objc_nlcatlist") {
                pointers(section, [&](auto address) { result.nonlazy_categories.push_back(address); });
            }
        }
        for (const auto address : result.nonlazy_classes)
            if (!classes.contains(address)) malformed("nonlazy class absent from class list");
        for (const auto address : result.nonlazy_categories)
            if (!categories.contains(address)) malformed("nonlazy category absent from category list");
        const auto queue = [&](const std::vector<ObjcProtocolReference>& references) {
            for (const auto& entry : references) pending.push_back(entry.protocol);
        };
        for (const auto& info : result.classes) queue(info.protocols);
        for (const auto& info : result.categories) queue(info.protocols);
        queue(result.protocol_references);
        // Decode every protocol reachable from this image, listed protocols first.
        std::map<std::uint64_t, std::size_t> decoded;
        for (std::size_t next = 0; next < pending.size(); ++next) {
            if (decoded.contains(pending[next])) continue;
            if (decoded.size() >= maximum_items) malformed("protocol graph exceeds count limit");
            auto info = protocol(pending[next]);
            info.listed = listed.contains(info.address);
            queue(info.protocols);
            decoded.emplace(info.address, result.protocols.size());
            result.protocols.push_back(std::move(info));
        }
        // Protocol inheritance must be acyclic: conformance queries recurse through it.
        std::map<std::uint64_t, int> state;
        const auto visit = [&](auto&& self, std::uint64_t address) -> void {
            if (state[address] == 2) return;
            if (state[address] == 1) malformed("cyclic protocol inheritance");
            state[address] = 1;
            for (const auto& entry : result.protocols[decoded.at(address)].protocols) self(self, entry.protocol);
            state[address] = 2;
        };
        for (const auto& [address, index] : decoded) { static_cast<void>(index); visit(visit, address); }
        return result;
    }

    ObjcClassInfo runtime_class(std::uint64_t address, bool meta) const { return klass(address, meta); }

private:
    const transmute::GuestMemory& memory_;
    std::vector<ImageRuntimeInfo> images_;
    bool runtime_{}; // Run-time metadata: any readable memory, not only images.

    void range(std::uint64_t address, std::uint64_t size) const {
        const auto end = add(address, size);
        if (!address || (!runtime_ && !std::any_of(images_.begin(), images_.end(), [&](const auto& image) {
            return address >= image.begin && end <= image.end;
        }))) malformed(address ? "pointer 0x" + hex64(address) + " outside loaded image metadata" : std::string("null metadata pointer"));
        auto current = address;
        while (current < end) {
            const auto region = memory_.query_region(current);
            if (!region || !region->readable || current < region->base ||
                current - region->base >= region->size) malformed("metadata points to unreadable memory");
            current = std::min(end, add(region->base, region->size));
        }
    }
    std::string string(std::uint64_t address, bool allow_empty = false) const {
        std::string value;
        for (std::uint64_t offset = 0; offset < maximum_string; ++offset) {
            const auto at = add(address, offset);
            range(at, 1);
            const auto byte = memory_.read8(at);
            if (!byte) {
                if (value.empty() && !allow_empty) malformed("empty name or type string");
                return value;
            }
            value.push_back(static_cast<char>(byte));
        }
        malformed("unterminated metadata string exceeds limit");
    }
    template<class Consumer> void pointers(const ImageRuntimeInfo::Section& section, Consumer consume) {
        if (section.address % 8 || section.size % 8) malformed("metadata list is not pointer aligned");
        if (section.size / 8 > maximum_items) malformed("metadata list exceeds count limit");
        range(section.address, section.size);
        for (std::uint64_t offset = 0; offset < section.size; offset += 8)
            consume(memory_.read64(add(section.address, offset)));
    }
    void implementation(std::uint64_t address) const {
        if (address % 4 || !std::any_of(images_.begin(), images_.end(), [&](const auto& image) {
            return std::any_of(image.code_ranges.begin(), image.code_ranges.end(), [&](const auto& code) {
                return address >= code.begin && address < code.end && code.end - address >= 4;
            });
        })) malformed("method implementation outside executable image code");
        const auto region = memory_.query_region(address);
        // Loader preparation precedes final permissions. The published code ranges
        // carry the checked executable segment permissions; current mappings are writable.
        if (!region || !region->readable)
            malformed("method implementation is not readable mapped code");
    }
    // Protocol method lists carry descriptions only: no implementation is decoded.
    std::vector<ObjcMethodInfo> methods(std::uint64_t address, bool description = false) const {
        if (!address) return {};
        range(address, 8);
        const auto flags = memory_.read32(address);
        const bool small = (flags & 0x80000000U) != 0;
        const bool direct = (flags & 0x40000000U) != 0;
        if ((flags & 0x3fff0000U) || (direct && !small)) malformed("unsupported method-list flags");
        const auto stride = flags & 0xfffcU;
        const auto count = memory_.read32(add(address, 4));
        if (stride != (small ? 12U : 24U)) malformed("unsupported method-list entry size");
        if (count > maximum_items) malformed("method-list count exceeds limit");
        range(address, add(8, static_cast<std::uint64_t>(count) * stride));
        std::vector<ObjcMethodInfo> result;
        std::set<std::string> selectors;
        result.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto entry = add(address, 8 + static_cast<std::uint64_t>(index) * stride);
            std::uint64_t name{}, types{}, imp{};
            if (small) {
                name = relative(entry, memory_.read32(entry));
                if (!direct) { range(name, 8); name = memory_.read64(name); }
                types = relative(add(entry, 4), memory_.read32(add(entry, 4)));
                if (!description) imp = relative(add(entry, 8), memory_.read32(add(entry, 8)));
            } else {
                name = memory_.read64(entry);
                types = memory_.read64(add(entry, 8));
                if (!description) imp = memory_.read64(add(entry, 16));
            }
            if (!description) implementation(imp);
            auto selector = string(name);
            // A list naming a selector twice resolves to its first entry, as
            // objc4's linear and sorted method-list searches do.
            if (!selectors.insert(selector).second) continue;
            auto type_string = string(types);
            // Block parameters ("@?") are object pointers at the call ABI;
            // the Blocks runtime (_Block_copy/_Block_release) handles them.
            result.push_back({std::move(selector), std::move(type_string), imp});
        }
        return result;
    }
    std::vector<ObjcPropertyInfo> properties(std::uint64_t address) const {
        if (!address) return {};
        range(address, 8);
        const auto stride = memory_.read32(address), count = memory_.read32(add(address, 4));
        if (stride != 16 || count > maximum_items) malformed("invalid property-list size or count");
        range(address, add(8, static_cast<std::uint64_t>(stride) * count));
        std::vector<ObjcPropertyInfo> result;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto entry = add(address, 8 + static_cast<std::uint64_t>(stride) * index);
            auto name = string(memory_.read64(entry));
            auto attributes = string(memory_.read64(add(entry, 8)));
            // Weak properties (",W") have compiled accessors over objc_loadWeakRetained/objc_storeWeak.
            result.push_back({std::move(name), std::move(attributes), entry});
        }
        return result;
    }
    static std::uint64_t reference(std::uint64_t address) {
        if (!address || address % 8) malformed("invalid protocol reference");
        return address;
    }
    // protocol_list_t: uintptr_t count, then count protocol_t pointers.
    std::vector<ObjcProtocolReference> protocol_list(std::uint64_t address) const {
        if (!address) return {};
        if (address % 8) malformed("unaligned protocol list");
        range(address, 8);
        const auto count = memory_.read64(address);
        if (count > maximum_items) malformed("protocol-list count exceeds limit");
        range(address, add(8, count * 8));
        std::vector<ObjcProtocolReference> result;
        for (std::uint64_t index = 0; index < count; ++index) {
            const auto slot = add(address, 8 + index * 8);
            result.push_back({slot, reference(memory_.read64(slot))});
        }
        return result;
    }
    // protocol_t: isa, mangledName, protocols, instanceMethods, classMethods,
    // optionalInstanceMethods, optionalClassMethods, instanceProperties, size, flags,
    // then, when size covers them, extendedMethodTypes, demangledName, classProperties.
    ObjcProtocolInfo protocol(std::uint64_t address) const {
        if (address % 8) malformed("unaligned protocol pointer");
        range(address, 72);
        ObjcProtocolInfo result;
        result.address = address;
        result.size = memory_.read32(add(address, 64));
        result.flags = memory_.read32(add(address, 68));
        if (result.size < 72 || result.size > 96 || result.size % 8) malformed("unsupported protocol structure size");
        // Compilers emit zero; fixed-up and canonical bits (high half) only
        // exist in preoptimized caches. Bits 0-15 are reserved for Swift.
        if (result.flags & 0xffff0000U) malformed("unsupported protocol flags (preoptimized or runtime-owned metadata)");
        range(address, result.size);
        result.name = string(memory_.read64(add(address, 8)));
        result.protocols = protocol_list(memory_.read64(add(address, 16)));
        result.instance_methods = methods(memory_.read64(add(address, 24)), true);
        result.class_methods = methods(memory_.read64(add(address, 32)), true);
        result.optional_instance_methods = methods(memory_.read64(add(address, 40)), true);
        result.optional_class_methods = methods(memory_.read64(add(address, 48)), true);
        result.properties = properties(memory_.read64(add(address, 56)));
        if (result.size >= 80) {
            if (const auto types = memory_.read64(add(address, 72))) {
                const auto count = static_cast<std::uint64_t>(result.instance_methods.size()) + result.class_methods.size() +
                    result.optional_instance_methods.size() + result.optional_class_methods.size();
                if (types % 8) malformed("unaligned protocol extended method types");
                if (count) range(types, count * 8);
                for (std::uint64_t index = 0; index < count; ++index)
                    result.extended_types.push_back(string(memory_.read64(add(types, index * 8))));
            }
        }
        if (result.size >= 88)
            if (const auto demangled = memory_.read64(add(address, 80))) result.demangled_name = string(demangled);
        if (result.size >= 96) result.class_properties = properties(memory_.read64(add(address, 88)));
        return result;
    }
    ObjcClassInfo klass(std::uint64_t address, bool meta) const {
        if (address % 8) malformed("unaligned class pointer");
        range(address, 40);
        const auto data = memory_.read64(add(address, 32));
        // FAST_IS_SWIFT_LEGACY (1) and FAST_IS_SWIFT_STABLE (2): Swift class metadata.
        const bool swift = (data & 3) != 0;
        const auto ro = data & ~7ULL;
        range(ro, 72);
        const auto flags = memory_.read32(ro);
        // RO_HAS_SWIFT_INITIALIZER (0x40) is meaningful for Swift classes only.
        // RO_HAS_WEAK_WITHOUT_ARC (0x200): manual-retain-release code with
        // __weak ivars, which it manages through objc_storeWeak/objc_loadWeak.
        if (flags & ~(swift ? 0x3f7U : 0x3b7U)) malformed("unsupported class flags 0x" + hex(flags) + " (obsolete ivar releasers)");
        if (((flags & 1U) != 0) != meta) malformed("class/metaclass flag mismatch");
        // ro+56 is the weak ivar layout. ARC code manages __weak ivars itself
        // through objc_initWeak/objc_storeWeak/objc_destroyWeak (provided),
        // so the layout is descriptive only.
        ObjcClassInfo result;
        result.address = address;
        result.metaclass = memory_.read64(address);
        result.superclass = memory_.read64(add(address, 8));
        result.is_metaclass = meta;
        result.swift = swift;
        if (swift && (flags & 0x40U) && !meta) {
            // class_ro_t's trailing _swiftMetadataInitializer_NEVER_USE[0].
            range(ro, 80);
            result.swift_initializer = memory_.read64(add(ro, 72));
            implementation(result.swift_initializer);
        }
        result.root = (flags & 2U) != 0;
        result.instance_start = memory_.read32(add(ro, 4));
        result.instance_size = memory_.read32(add(ro, 8));
        if (result.instance_start > result.instance_size || result.instance_size > maximum_instance)
            malformed("invalid class instance extent");
        // Swift generic and prespecialized classes have no static name; the
        // Swift runtime's lazy class namer supplies it (runtime decoding only).
        // (Their metaclasses carry no Swift data bits.)
        if (const auto name = memory_.read64(add(ro, 24)); name || !runtime_) result.name = string(name);
        result.methods = methods(memory_.read64(add(ro, 32)));
        result.protocols = protocol_list(memory_.read64(add(ro, 40)));
        result.properties = properties(memory_.read64(add(ro, 64)));
        const auto layout = memory_.read64(add(ro, 16));
        if (layout) {
            bool terminated = false;
            for (std::uint64_t offset = 0; offset < 4096; ++offset) {
                const auto at = add(layout, offset);
                range(at, 1);
                if (!memory_.read8(at)) { terminated = true; break; }
            }
            if (!terminated) malformed("unterminated strong ivar layout");
        }
        const auto ivars = memory_.read64(add(ro, 48));
        // The Swift initializer publishes the real ivar offsets and sizes.
        if (!ivars || result.swift_initializer) return result;
        if (meta) malformed("metaclass has instance variables");
        range(ivars, 8);
        const auto stride = memory_.read32(ivars), count = memory_.read32(add(ivars, 4));
        if (stride != 32 || count > maximum_items) malformed("invalid ivar-list size or count");
        range(ivars, add(8, static_cast<std::uint64_t>(stride) * count));
        std::set<std::string> names;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto entry = add(ivars, 8 + static_cast<std::uint64_t>(stride) * index);
            ObjcIvarInfo ivar;
            ivar.offset_address = memory_.read64(entry);
            // Generic Swift classes keep field offsets in their metadata's
            // field offset vector; their ivar entries have no offset global.
            if (!ivar.offset_address && swift) continue;
            range(ivar.offset_address, 4);
            ivar.offset = memory_.read32(ivar.offset_address);
            ivar.name = string(memory_.read64(add(entry, 8)));
            // Swift stored properties without an Objective-C type have an
            // empty (or absent) type encoding.
            const auto types = memory_.read64(add(entry, 16));
            if (types) ivar.types = string(types, true);
            const auto exponent = memory_.read32(add(entry, 24));
            ivar.size = memory_.read32(add(entry, 28));
            if (exponent != UINT32_MAX && exponent > 20) malformed("invalid ivar alignment exponent");
            ivar.alignment = exponent == UINT32_MAX ? 8 : (1U << exponent);
            if (ivar.offset < result.instance_start || ivar.offset > result.instance_size ||
                ivar.size > result.instance_size - ivar.offset || !ivar.size)
                malformed("ivar outside class instance extent");
            if (!names.insert(ivar.name).second) malformed("duplicate ivar name");
            // Block ivars ("@?") hold object pointers that compiled code copies and releases.
            result.ivars.push_back(std::move(ivar));
        }
        return result;
    }
    ObjcCategoryInfo category(std::uint64_t address, bool class_properties) const {
        if (address % 8) malformed("unaligned category pointer");
        range(address, class_properties ? 56 : 48);
        ObjcCategoryInfo result;
        result.address = address;
        result.name = string(memory_.read64(address));
        result.target_class = memory_.read64(add(address, 8));
        if (!result.target_class) malformed("category has no target class");
        result.instance_methods = methods(memory_.read64(add(address, 16)));
        result.class_methods = methods(memory_.read64(add(address, 24)));
        result.protocols = protocol_list(memory_.read64(add(address, 32)));
        result.properties = properties(memory_.read64(add(address, 40)));
        if (class_properties) result.class_properties = properties(memory_.read64(add(address, 48)));
        return result;
    }
};
} // namespace

ObjcClassInfo parse_objc_runtime_class(const ProcessContext& process,
    const transmute::GuestMemory& memory, std::uint64_t address, bool metaclass) {
    Decoder decoder(process, memory, {}, true);
    return decoder.runtime_class(address, metaclass);
}

std::vector<ObjcImageInfo> parse_objc_images(const ProcessContext& process,
    const transmute::GuestMemory& memory, std::span<const ImageRuntimeInfo> images) {
    Decoder decoder(process, memory, images);
    std::vector<ObjcImageInfo> result;
    result.reserve(images.size());
    std::set<std::uint64_t> ids;
    for (const auto& image : images) {
        if (!ids.insert(image.module_id).second) malformed("duplicate image identity in batch");
        result.push_back(decoder.image(image));
    }
    return result;
}
} // namespace alchemy
