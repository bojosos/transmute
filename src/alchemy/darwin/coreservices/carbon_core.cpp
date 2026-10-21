#include "alchemy/darwin/coreservices/install.hpp"

#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/host/files.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>

// CarbonCore pieces: the fp.h aliases (nan, nanf, fegetenv, fesetenv, ...) of
// libSystem's functions, and FSGetVolumeInfo/FSGetVolumeParms over the host's
// fixed volumes. a renderer plug-in's images first imported them (moved from
// darwin/identities/framework_gaps.cpp).
namespace alchemy::gui {
namespace {

using namespace alchemy::export_fill;

constexpr std::string_view core_services = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices";
// MacErrors.h
constexpr std::int64_t no_err = 0, nsv_err = -35, param_err = -50;

struct Volume {
    std::string root;             // the host root, "C:\"
    std::int16_t reference{};     // FSVolumeRefNum
    std::string device;           // BSD device name reported by FSGetVolumeParms
};

// The host's fixed volumes, the one the host OS runs from first.
std::vector<Volume> fixed_volumes() {
    std::vector<Volume> result;
    const auto roots = host::files::volume_roots();
    if (!roots) return result;
    std::vector<host::files::Volume> fixed;
    for (const auto& root : *roots)
        if (const auto volume = host::files::volume(root); volume && volume->kind == host::files::VolumeKind::fixed) fixed.push_back(*volume);
    std::stable_partition(fixed.begin(), fixed.end(), [](const host::files::Volume& volume) { return volume.system; });
    for (std::size_t index = 0; index < fixed.size(); ++index)
        result.push_back({fixed[index].root, static_cast<std::int16_t>(-100 - static_cast<int>(index)), "disk" + std::to_string(index + 1) + "s1"});
    return result;
}

// UTCDateTime: seconds since 1904-01-01 UTC as 16+32 bits plus a 1/65536 fraction.
void write_utc(GuestMemory& memory, std::uint64_t address, std::int64_t unix_ns) {
    constexpr std::int64_t epoch_1904 = 20828448000000000LL;   // 1904->1970 in 100 ns units
    const auto since_1904 = unix_ns / 100 + epoch_1904;
    const auto ticks = static_cast<std::uint64_t>(since_1904 > 0 ? since_1904 : 0);
    const auto seconds = ticks / 10'000'000ULL;
    const auto fraction = (ticks % 10'000'000ULL) * 65536ULL / 10'000'000ULL;
    memory.write8(address, static_cast<std::uint8_t>(seconds >> 32));
    memory.write8(address + 1, static_cast<std::uint8_t>(seconds >> 40));
    memory.write32(address + 2, static_cast<std::uint32_t>(seconds));
    memory.write8(address + 6, static_cast<std::uint8_t>(fraction));
    memory.write8(address + 7, static_cast<std::uint8_t>(fraction >> 8));
}

} // namespace

void install_coreservices_carbon_core(InstallContext& context) {
    auto& process = context.process;
    Provider p(process);
    const auto alias = [&](const char* name, transmute::GuestCallHandler fallback) {
        if (provided(process, core_services, name)) return;
        if (const auto found = process.resolve(libsystem_library, name)) process.install_export(std::string(core_services), name, *found);
        else static_cast<void>(process.install_function(name, std::move(fallback), std::string(core_services)));
    };
    alias("_nan", [](CpuState& s, GuestMemory&) { s.v[0] = {std::bit_cast<std::uint64_t>(std::numeric_limits<double>::quiet_NaN()), 0}; });
    alias("_nanf", [](CpuState& s, GuestMemory&) { s.v[0] = {std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN()), 0}; });
    for (const auto* name : {"_fegetenv", "_fesetenv", "_feholdexcept", "_feupdateenv", "_fegetround", "_fesetround"}) {
        if (process.resolve(core_services, name)) continue;
        if (const auto found = process.resolve(libsystem_library, name)) process.install_export(std::string(core_services), name, *found);
    }

    auto volumes = std::make_shared<std::pair<std::once_flag, std::vector<Volume>>>();
    auto devices = std::make_shared<std::pair<std::mutex, std::map<std::int16_t, std::uint64_t>>>();
    const auto volume_list = [volumes]() -> const std::vector<Volume>& {
        std::call_once(volumes->first, [&] { volumes->second = fixed_volumes(); });
        return volumes->second;
    };
    // OSErr FSGetVolumeInfo(FSVolumeRefNum volume, ItemCount volumeIndex, FSVolumeRefNum* actualVolume,
    //                       FSVolumeInfoBitmap whichInfo, FSVolumeInfo* info, HFSUniStr255* volumeName, FSRef* rootDirectory)
    p.function(core_services, "_FSGetVolumeInfo", [volume_list](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto& list = volume_list();
        const auto reference = static_cast<std::int16_t>(s.x[0]);
        const Volume* volume = nullptr;
        if (reference != 0) {
            for (const auto& item : list) if (item.reference == reference) volume = &item;
        } else if (s.x[1] >= 1 && s.x[1] <= list.size()) volume = &list[static_cast<std::size_t>(s.x[1] - 1)];
        if (!volume) return static_cast<std::uint64_t>(static_cast<std::int64_t>(nsv_err));
        if (s.x[2]) write16(m, s.x[2], static_cast<std::uint16_t>(volume->reference));
        const auto facts = host::files::volume(volume->root);
        if (const auto info = s.x[4]) {
            m.fill_bytes(info, 0, 126);
            if (const auto times = host::files::times(volume->root)) {
                write_utc(m, info, times->created_ns);
                write_utc(m, info + 8, times->modified_ns);
                write_utc(m, info + 24, times->modified_ns);
            }
            const auto total = facts ? facts->total : 0, free_bytes = facts ? facts->free : 0;
            const auto block = facts ? facts->block_size : 0;
            m.write64(info + 40, total);
            m.write64(info + 48, free_bytes);
            m.write32(info + 56, block);
            m.write32(info + 60, block ? static_cast<std::uint32_t>(std::min<std::uint64_t>(total / block, UINT32_MAX)) : 0);
            m.write32(info + 64, block ? static_cast<std::uint32_t>(std::min<std::uint64_t>(free_bytes / block, UINT32_MAX)) : 0);
            write16(m, info + 120, 0x482b);   // 'H+': the signature macOS also reports for APFS volumes
        }
        if (const auto name = s.x[5]) {
            // HFSUniStr255: the label (or the root's first character) in UTF-16.
            const auto label = facts && !facts->name.empty() ? facts->name : volume->root.substr(0, 1);
            auto text = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(label.data()), label.size())).u16string();
            text.resize(std::min<std::size_t>(text.size(), 255));
            write16(m, name, static_cast<std::uint16_t>(text.size()));
            for (std::size_t index = 0; index < text.size(); ++index) write16(m, name + 2 + index * 2, static_cast<std::uint16_t>(text[index]));
        }
        if (const auto root = s.x[6]) {
            m.fill_bytes(root, 0, 80);
            write16(m, root, static_cast<std::uint16_t>(volume->reference));
        }
        return no_err;
    });
    // OSStatus FSGetVolumeParms(FSVolumeRefNum, GetVolParmsInfoBuffer*, ByteCount)
    p.function(core_services, "_FSGetVolumeParms", [volume_list, devices, &process](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto reference = static_cast<std::int16_t>(s.x[0]);
        const Volume* volume = nullptr;
        for (const auto& item : volume_list()) if (item.reference == reference) volume = &item;
        if (!volume) return static_cast<std::uint64_t>(static_cast<std::int64_t>(nsv_err));
        if (!s.x[1]) return static_cast<std::uint64_t>(static_cast<std::int64_t>(param_err));
        std::uint64_t device{};
        {
            std::lock_guard lock(devices->first);
            auto& cell = devices->second[reference];
            if (!cell) { cell = process.allocate_guest(volume->device.size() + 1); put_string(process.main_engine(), cell, volume->device); }
            device = cell;
        }
        // GetVolParmsInfoBuffer (2-byte packing), version 4: 44 bytes.
        std::array<std::uint8_t, 44> buffer{};
        const auto put16 = [&](std::size_t at, std::uint16_t v) { std::memcpy(buffer.data() + at, &v, 2); };
        const auto put64 = [&](std::size_t at, std::uint64_t v) { std::memcpy(buffer.data() + at, &v, 8); };
        put16(0, 4);
        put64(28, device);
        put64(36, 255);
        const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(s.x[2], buffer.size()));
        m.write_bytes(s.x[1], std::span(buffer.data(), size));
        return no_err;
    });
}

} // namespace alchemy::gui
