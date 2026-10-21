#pragma once

// Register image of one host->guest call under the Apple ARM64 calling
// convention (AAPCS64 with Apple's stack-argument rules).
//
// Shared contract (wave 0). Used by FxEngine::invoke_callback_regs,
// EngineAdapter::call_regs and ObjcRuntime::send. Pure data: no engine,
// process or Objective-C dependency, so every layer can include it.
//
// Argument classification is the caller's job:
// - Integer and pointer arguments go in x[0..7] in order, then on the stack.
// - float/double and homogeneous floating-point aggregates (NSPoint, NSSize,
//   NSRect, CGAffineTransform halves, simd vectors up to 16 bytes) go in
//   v[0..7]; each member takes its own register (NSRect = d0..d3). An HFA
//   that does not fit in the remaining v registers goes entirely on the stack.
// - Composite arguments larger than 16 bytes that are not HFAs are passed by
//   reference: the caller copies them to guest memory and passes the pointer.
// - Apple ARM64 packs non-variadic stack arguments at their natural size and
//   alignment (a char takes one byte), unlike AAPCS64's 8-byte slots. Variadic
//   arguments always take 8-byte slots. push_stack implements the packed rule;
//   push_variadic the slot rule.
// - Results larger than 16 bytes that are not HFAs (NSOperatingSystemVersion)
//   are written through the indirect-result register x8: set
//   `indirect_result` to a guest buffer. A 16-byte NSRange returns in x0/x1.
//
// Ownership: plain value types. The stack bytes are copied onto the guest
// stack below the current guest SP for the duration of the call only.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace alchemy {

// One 128-bit SIMD/FP register, {low 64 bits, high 64 bits}. A float uses the
// low 32 bits, a double the low 64 bits; the rest is zero.
using VectorRegister = std::array<std::uint64_t, 2>;

struct CallRegs {
    std::array<std::uint64_t, 8> x{};        // x0..x7
    std::array<VectorRegister, 8> v{};       // q0..q7
    std::uint64_t indirect_result{};         // x8 (indirect result location), 0 when unused
    std::vector<std::uint8_t> stack;         // bytes at [sp, sp + size) on entry, 16-byte aligned by the engine

    void set_s(unsigned index, float value) { v.at(index) = {std::bit_cast<std::uint32_t>(value), 0}; }
    void set_d(unsigned index, double value) { v.at(index) = {std::bit_cast<std::uint64_t>(value), 0}; }
    // Apple ARM64 named (non-variadic) stack argument: natural alignment.
    template <class T>
    void push_stack(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "stack arguments are raw bytes");
        push_bytes(&value, sizeof(T), alignof(T) < 8 ? alignof(T) : 8);
    }
    // Variadic argument: one 8-byte slot (16 bytes for long double/vectors).
    template <class T>
    void push_variadic(const T& value) {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= 16, "variadic arguments are register sized");
        push_bytes(&value, sizeof(T), 8, sizeof(T) <= 8 ? 8 : 16);
    }
    void push_bytes(const void* data, std::size_t size, std::size_t alignment, std::size_t slot = 0) {
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) throw std::invalid_argument("invalid stack alignment");
        const auto offset = (stack.size() + alignment - 1) & ~(alignment - 1);
        stack.resize(offset + std::max(size, slot), 0);
        std::memcpy(stack.data() + offset, data, size);
    }
    [[nodiscard]] bool uses_vectors() const noexcept {
        return std::any_of(v.begin(), v.end(), [](const VectorRegister& value) { return value[0] || value[1]; });
    }
};

// Return registers after the call. HFA results come back in v0..v3 (NSRect:
// d0..d3 as v[0..3][0]); 9-16 byte integer composites in x0/x1.
struct CallResult {
    std::uint64_t x0{}, x1{};
    std::array<VectorRegister, 4> v{};       // q0..q3
    // False when the engine could only report x0 (the reference engine has no
    // register-image call); x1 and v are then zero and must not be used.
    bool complete{};

    [[nodiscard]] float s(unsigned index) const { return std::bit_cast<float>(static_cast<std::uint32_t>(v.at(index)[0])); }
    [[nodiscard]] double d(unsigned index) const { return std::bit_cast<double>(v.at(index)[0]); }
};

} // namespace alchemy
