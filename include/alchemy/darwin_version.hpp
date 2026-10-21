#pragma once

#include <cstdint>
#include <string_view>

namespace alchemy {

// The one OS version Alchemy's virtual arm64 Mac reports: macOS 14.0 (Darwin
// 23.0.0), the SDK profile Blender 4.5 is built against. Every interface that
// answers a version uses these values: sysctl kern.osrelease /
// kern.osproductversion / kern.osversion / kern.version, uname, NSProcessInfo
// operatingSystemVersion(String) and isOperatingSystemAtLeastVersion:,
// __isPlatformVersionAtLeast and _availability_version_check (the @available
// and __builtin_available checks). Guests that gate features on @available
// (Blender's Metal backend) see a consistent answer. It is a compatibility
// profile, not a claim of macOS 14 API completeness.
inline constexpr std::int64_t macos_major = 14;
inline constexpr std::int64_t macos_minor = 0;
inline constexpr std::int64_t macos_patch = 0;
inline constexpr std::string_view macos_product_version = "14.0";
inline constexpr std::string_view macos_version_string = "Alchemy macOS 14.0.0 compatibility profile";
inline constexpr std::string_view darwin_release = "23.0.0";
inline constexpr std::string_view darwin_build = "23A344";
inline constexpr std::string_view darwin_kernel_version = "Darwin Kernel Version 23.0.0: Alchemy compatibility profile";
// dyld_build_version_t encoding (major << 16 | minor << 8 | patch).
inline constexpr std::uint32_t macos_packed_version =
    static_cast<std::uint32_t>(macos_major << 16 | macos_minor << 8 | macos_patch);

} // namespace alchemy
