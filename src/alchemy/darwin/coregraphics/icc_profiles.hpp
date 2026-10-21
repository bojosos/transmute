#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// ICC profiles of CoreGraphics' named color spaces (CGColorSpaceCopyICCData).
// Project-authored ICC v4 display profiles generated from the public
// standards' parameters (icc_profiles.cpp); no Apple profile bytes.
namespace alchemy::gui::quartz2d {
// The profile of a named RGB or gray space (CGColorSpace.h name), built once
// and never modified; nullptr for spaces without one (CMYK, Lab, XYZ, unknown).
[[nodiscard]] const std::vector<std::uint8_t>* named_icc_profile(std::string_view name);
// The named space a generated profile belongs to when `bytes` is exactly one
// of them (spaces sharing a profile answer the first, non-extended name); ""
// for any other profile.
[[nodiscard]] std::string_view named_icc_profile_owner(std::span<const std::uint8_t> bytes);
} // namespace alchemy::gui::quartz2d
