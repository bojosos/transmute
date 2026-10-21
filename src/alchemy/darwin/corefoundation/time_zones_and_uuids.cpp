#include "alchemy/darwin/corefoundation/install.hpp"

#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/object_store.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>

// CFTimeZone (the default zone by IANA name), CFUUID, CFStringHasPrefix and
// CFStringHasSuffix, and kCFAllocatorNull, each installed only when missing.
// a renderer plug-in's images first imported them (moved from
// darwin/identities/framework_gaps.cpp).
namespace alchemy::gui {

using namespace alchemy::export_fill;

void install_corefoundation_time_zones_and_uuids(InstallContext& context) {
    auto& process = context.process;
    auto& objc = context.objc;
    auto& cf = context.cf;
    const auto cf_function = [&](const char* name, transmute::GuestCallHandler handler) {
        if (provided(process, core_foundation_library, name)) return;
        static_cast<void>(process.install_function(name, std::move(handler), std::string(core_foundation_library)));
    };
    const auto make_zone = [&process, &cf, &objc](const std::string& name) { return make_time_zone(process, cf, objc, name); };
    const auto zone = [&process](std::uint64_t object) {
        const auto record = object ? process.objects().find(object) : nullptr;
        const auto payload = record ? std::dynamic_pointer_cast<CfTimeZone>(record->payload) : nullptr;
        if (!payload) throw std::runtime_error("Alchemy CoreFoundation: object is not a CFTimeZone");
        return payload;
    };
    for (const auto* name : {"_CFTimeZoneCopyDefault", "_CFTimeZoneCopySystem"})
        cf_function(name, [make_zone](CpuState& s, GuestMemory&) { s.x[0] = make_zone(default_time_zone()); });
    cf_function("_CFTimeZoneGetName", [zone](CpuState& s, GuestMemory&) { s.x[0] = zone(s.x[0])->name; });
    cf_function("_CFStringHasPrefix", [&cf](CpuState& s, GuestMemory&) {
        const auto text = cf.units(s.x[0]), prefix = cf.units(s.x[1]);
        s.x[0] = text.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), text.begin()) ? 1 : 0;
    });
    cf_function("_CFStringHasSuffix", [&cf](CpuState& s, GuestMemory&) {
        const auto text = cf.units(s.x[0]), suffix = cf.units(s.x[1]);
        s.x[0] = text.size() >= suffix.size() && std::equal(suffix.rbegin(), suffix.rend(), text.rbegin()) ? 1 : 0;
    });
    // CFUUIDCreate: a random (version 4) identifier.
    const auto uuid_class = cf_uuid_class(objc);
    cf_function("_CFUUIDCreate", [&process, uuid_class](CpuState& s, GuestMemory&) {
        auto payload = std::make_shared<CfUuid>();
        std::random_device device;
        for (auto& byte : payload->bytes) byte = static_cast<std::uint8_t>(device());
        payload->bytes[6] = static_cast<std::uint8_t>((payload->bytes[6] & 0x0f) | 0x40);
        payload->bytes[8] = static_cast<std::uint8_t>((payload->bytes[8] & 0x3f) | 0x80);
        s.x[0] = process.objects().create(16, uuid_class, ObjectKind::instance, std::move(payload));
    });
    cf_function("_CFUUIDCreateString", [&process, &cf](CpuState& s, GuestMemory&) {
        const auto record = s.x[1] ? process.objects().find(s.x[1]) : nullptr;
        const auto payload = record ? std::dynamic_pointer_cast<CfUuid>(record->payload) : nullptr;
        if (!payload) throw std::runtime_error("Alchemy CoreFoundation: object is not a CFUUID");
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string text;
        for (std::size_t index = 0; index < 16; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) text.push_back('-');
            text.push_back(digits[payload->bytes[index] >> 4]);
            text.push_back(digits[payload->bytes[index] & 15]);
        }
        s.x[0] = cf.string(text);
    });
    // const CFAllocatorRef kCFAllocatorNull: the allocator that never frees.
    // Alchemy's CF functions copy their input, so it is a distinct non-null
    // marker (CFStringCreateWithCStringNoCopy & co. are not provided).
    if (!process.resolve(core_foundation_library, "_kCFAllocatorNull")) {
        const auto marker = process.allocate_guest(16);
        process.main_engine().fill_bytes(marker, 0, 16);
        const auto cell = process.allocate_guest(8);
        process.main_engine().write64(cell, marker);
        process.install_export(std::string(core_foundation_library), "_kCFAllocatorNull", {cell, ExportKind::data});
    }
}

} // namespace alchemy::gui
