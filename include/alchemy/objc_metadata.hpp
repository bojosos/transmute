#pragma once

#include "alchemy/process_context.hpp"

#include <string>
#include <vector>

namespace alchemy {
struct ObjcMethodInfo { std::string selector, types; std::uint64_t implementation{}; };
// One protocol_ref_t slot: a protocol-list entry or an __objc_protorefs cell.
struct ObjcProtocolReference { std::uint64_t slot{}, protocol{}; };
// One property_t entry ({name, attributes} C-string pointers); `address`
// is the entry itself, which libobjc hands out as objc_property_t.
struct ObjcPropertyInfo { std::string name, attributes; std::uint64_t address{}; };
struct ObjcProtocolInfo {
    std::uint64_t address{};
    std::string name, demangled_name;
    std::uint32_t size{}, flags{};
    std::vector<ObjcProtocolReference> protocols;
    // Method descriptions; implementation is always zero.
    std::vector<ObjcMethodInfo> instance_methods, class_methods, optional_instance_methods, optional_class_methods;
    // Same order as the four method lists, when the protocol records them.
    std::vector<std::string> extended_types;
    std::vector<ObjcPropertyInfo> properties, class_properties;
    bool listed{}; // Present in this image's __objc_protolist rather than only referenced.
};
struct ObjcIvarInfo { std::string name, types; std::uint64_t offset_address{}; std::uint32_t offset{}, size{}, alignment{}; };
struct ObjcClassInfo {
    std::uint64_t address{}, metaclass{}, superclass{};
    std::string name;
    std::uint32_t instance_start{}, instance_size{};
    bool is_metaclass{};
    // Swift classes: class_t data bits FAST_IS_SWIFT_LEGACY/STABLE. A class
    // whose class_ro_t has RO_HAS_SWIFT_INITIALIZER gets its layout (instance
    // size, ivar offsets and sizes) from that Swift metadata initializer,
    // which ends in _objc_realizeClassFromSwift; until then `ivars` is empty.
    bool swift{};
    std::uint64_t swift_initializer{};
    bool root{};   // RO_ROOT: a root class (no superclass by design)
    std::vector<ObjcMethodInfo> methods;
    std::vector<ObjcIvarInfo> ivars;
    std::vector<ObjcProtocolReference> protocols;
    // class_ro_t baseProperties: a metaclass's are the class properties.
    std::vector<ObjcPropertyInfo> properties;
};
struct ObjcCategoryInfo {
    std::uint64_t address{}, target_class{};
    std::string name;
    std::vector<ObjcMethodInfo> instance_methods, class_methods;
    std::vector<ObjcProtocolReference> protocols;
    std::vector<ObjcPropertyInfo> properties, class_properties;
};
struct ObjcSelectorReference { std::uint64_t slot{}; std::string name; };
struct ObjcImageInfo {
    std::uint64_t module_id{};
    std::vector<ObjcClassInfo> classes;
    std::vector<ObjcCategoryInfo> categories;
    std::vector<ObjcSelectorReference> selectors;
    // Listed protocols first, then protocols reachable only through references.
    std::vector<ObjcProtocolInfo> protocols;
    std::vector<ObjcProtocolReference> protocol_references;
    std::vector<std::uint64_t> nonlazy_classes, nonlazy_categories;
    // __objc_classrefs and __objc_superrefs cells (class pointers used by code).
    std::vector<std::uint64_t> class_references;
};
// Decode one class (or metaclass) built at run time, outside any image: the
// class pairs Swift registers through objc_readClassPair and the layouts it
// publishes through _objc_realizeClassFromSwift. Pointers must reference
// readable memory; method implementations must lie in loaded image code.
[[nodiscard]] ObjcClassInfo parse_objc_runtime_class(const ProcessContext& process,
    const transmute::GuestMemory& memory, std::uint64_t address, bool metaclass);
// Decode the complete batch without mutation. Addresses are already relocated.
[[nodiscard]] std::vector<ObjcImageInfo> parse_objc_images(
    const ProcessContext& process, const transmute::GuestMemory& memory,
    std::span<const ImageRuntimeInfo> images);
} // namespace alchemy
