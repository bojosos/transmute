#pragma once

// Code requirements (the code signing requirement language), host only: text
// is compiled to the binary form a signature carries (a 0xfade0c00
// Requirement blob of big-endian expression opcodes), rendered back to
// text, and evaluated against static code (code_signature.hpp).
//   Evaluated: identifier, cdhash, platform, info[key] and entitlement[key]
//   matches, anchor apple / anchor apple generic (Apple Root CA by its
//   fingerprint; Apple's own code by its "Software Signing" leaf),
//   certificate N = H"sha1", certificate N[subject.X], [field.OID] and
//   [policy.OID] matches, and / or / !.
//   Always false: certificate N trusted (no user trust settings),
//   notarized (stapled tickets are not checked), legacy, named anchors and
//   code, certificate dates.

#include "alchemy/darwin/security/code_signature.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace alchemy::gui::security::codesign {

// Requirement types in a requirement set (CSCommon.h).
inline constexpr std::uint32_t requirement_host = 1, requirement_guest = 2, requirement_designated = 3, requirement_library = 4,
    requirement_plugin = 5;

// Text to a Requirement blob; nullopt with `error` set when it does not parse.
[[nodiscard]] std::optional<Bytes> compile(std::string_view text, std::string& error);
// A Requirement blob to text; nullopt when it is malformed.
[[nodiscard]] std::optional<std::string> requirement_text(std::span<const std::uint8_t> blob);
// The requirements of a requirement set blob (0xfade0c01) by type.
[[nodiscard]] std::map<std::uint32_t, Bytes> requirement_set(std::span<const std::uint8_t> blob);
// A requirement set as text ("designated => ...", one line per requirement).
[[nodiscard]] std::string requirement_set_text(std::span<const std::uint8_t> blob);

// Evaluates a Requirement blob against signed static code: 0 when satisfied, errSecCSReqFailed when not,
// errSecCSReqInvalid for a malformed blob, errSecCSUnsigned for unsigned code.
[[nodiscard]] std::int32_t evaluate(std::span<const std::uint8_t> blob, const StaticCode& code, std::string* detail = nullptr);

// The designated requirement: the signature's own, else the implicit one Apple derives (cdhash for ad-hoc code,
// the Developer ID form, Apple's own code, or the root certificate's hash). nullopt for unsigned code.
[[nodiscard]] std::optional<Bytes> designated_requirement(const StaticCode& code, bool* implicit = nullptr);

} // namespace alchemy::gui::security::codesign
