#pragma once

// linux backend of fx/host/fault.hpp: CpuContext is the x86-64 ucontext_t
// the kernel hands a SA_SIGINFO handler. The accessors use its fixed glibc
// layout; fault.cpp checks the offsets against <ucontext.h> with
// static_asserts, so this header needs no OS header.

#include <cstddef>
#include <cstdint>

namespace transmute::fx::host {

inline constexpr std::size_t kCpuContextBytes = 968;   // sizeof(ucontext_t)

namespace linux_context {
inline constexpr std::uint32_t kGregs = 40;     // uc_mcontext.gregs (REG_R8 .. REG_CR2, 8 bytes each)
inline constexpr std::uint32_t kFpregs = 224;   // uc_mcontext.fpregs (pointer to _libc_fpstate)
inline constexpr std::uint32_t kXmm = 160;      // _libc_fpstate::_xmm
// gregs index of each x64 register number (rax, rcx, rdx, rbx, rsp, rbp, rsi,
// rdi, r8 .. r15): REG_RAX = 13, REG_RCX = 14, ...
inline constexpr std::uint8_t kGregOf[16] = {13, 14, 12, 11, 15, 10, 9, 8, 0, 1, 2, 3, 4, 5, 6, 7};
inline constexpr std::uint32_t kRip = 16;   // REG_RIP
inline constexpr std::uint32_t kEfl = 17;   // REG_EFL
inline constexpr std::uint32_t kErr = 19;   // REG_ERR: the page-fault error code
inline constexpr std::uint32_t kTrapno = 20;

inline std::uint64_t* gregs(CpuContext& c) noexcept {
    return reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uint8_t*>(&c) + kGregs);
}
inline const std::uint64_t* gregs(const CpuContext& c) noexcept {
    return reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const std::uint8_t*>(&c) + kGregs);
}
} // namespace linux_context

inline std::uint64_t& gpr(CpuContext& c, unsigned reg) noexcept {
    return linux_context::gregs(c)[linux_context::kGregOf[reg & 15]];
}
inline std::uint64_t gpr(const CpuContext& c, unsigned reg) noexcept {
    return linux_context::gregs(c)[linux_context::kGregOf[reg & 15]];
}
inline std::uint64_t& rip(CpuContext& c) noexcept { return linux_context::gregs(c)[linux_context::kRip]; }
inline std::uint64_t rip(const CpuContext& c) noexcept { return linux_context::gregs(c)[linux_context::kRip]; }
inline std::uint64_t& rsp(CpuContext& c) noexcept { return gpr(c, 4); }
inline std::uint32_t eflags(const CpuContext& c) noexcept {
    return static_cast<std::uint32_t>(linux_context::gregs(c)[linux_context::kEfl]);
}
inline std::uint8_t* xmm(CpuContext& c, unsigned reg) noexcept {
    auto* fpstate = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(&c) + linux_context::kFpregs);
    return fpstate + linux_context::kXmm + 16 * (reg & 15);
}

} // namespace transmute::fx::host
