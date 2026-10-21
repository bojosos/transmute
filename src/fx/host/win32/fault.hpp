#pragma once

// win32 backend of fx/host/fault.hpp: CpuContext is the x64 CONTEXT. The
// accessors use its fixed offsets (winnt.h); fault.cpp checks them against
// windows.h with static_asserts.

#include <cstddef>
#include <cstdint>

namespace transmute::fx::host {

inline constexpr std::size_t kCpuContextBytes = 0x4d0;   // sizeof(CONTEXT)

namespace win32_context {
inline constexpr std::uint32_t kEFlags = 0x44;
inline constexpr std::uint32_t kRax = 0x78;     // Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8 .. R15
inline constexpr std::uint32_t kRsp = 0x98;
inline constexpr std::uint32_t kRip = 0xf8;
inline constexpr std::uint32_t kXmm0 = 0x1a0;   // Xmm0 .. Xmm15, 16 bytes each
} // namespace win32_context

inline std::uint64_t& gpr(CpuContext& c, unsigned reg) noexcept {
    return reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uint8_t*>(&c) + win32_context::kRax)[reg & 15];
}
inline std::uint64_t gpr(const CpuContext& c, unsigned reg) noexcept {
    return reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const std::uint8_t*>(&c) + win32_context::kRax)[reg & 15];
}
inline std::uint64_t& rip(CpuContext& c) noexcept {
    return *reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uint8_t*>(&c) + win32_context::kRip);
}
inline std::uint64_t rip(const CpuContext& c) noexcept {
    return *reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const std::uint8_t*>(&c) + win32_context::kRip);
}
inline std::uint64_t& rsp(CpuContext& c) noexcept {
    return *reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uint8_t*>(&c) + win32_context::kRsp);
}
inline std::uint32_t eflags(const CpuContext& c) noexcept {
    return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(&c) + win32_context::kEFlags);
}
inline std::uint8_t* xmm(CpuContext& c, unsigned reg) noexcept {
    return reinterpret_cast<std::uint8_t*>(&c) + win32_context::kXmm0 + 16 * (reg & 15);
}

} // namespace transmute::fx::host
