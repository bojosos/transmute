#pragma once

// Signature model for the fx ABI component (owner: abi).
//
// Signatures use the sigdb grammar documented in src/fx/abi/NOTES.md, e.g.
//   "p(pIIpIIp)"      HANDLE CreateFileW(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE)
//   "v(pll@i(pp))"    void qsort(void*, size_t, size_t, int (*)(const void*, const void*))
//   "i(pp.)"          int wsprintfA(LPSTR, LPCSTR, ...)
//   "{d2}({d2})"      _Dcomplex conj(_Dcomplex)
//   "?"               unknown: positional integers (see NOTES.md)
//
// plan_call() turns a signature into register/stack locations for both the
// ARM64 (Windows AAPCS64) and the Win64 sides of a call.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace transmute::fx::abi {

enum class TypeKind : std::uint8_t {
    void_, i8, u8, i16, u16, i32, u32, i64, ptr, f32, f64,
    record,   // integer-class aggregate (size/align)
    hfa,      // homogeneous floating-point aggregate: hfa_count x (float|double)
    fnptr,    // function pointer with a known signature (fn)
};

struct Signature;

struct TypeDesc {
    TypeKind kind{TypeKind::i64};
    std::uint8_t hfa_count{};     // hfa: 1..4
    bool hfa_double{};            // hfa element type
    std::uint8_t align{8};        // record/hfa alignment in bytes (16 when declared so)
    std::uint32_t size{8};        // bytes
    const Signature* fn{};        // fnptr

    [[nodiscard]] bool is_aggregate() const noexcept { return kind == TypeKind::record || kind == TypeKind::hfa; }
    [[nodiscard]] bool is_fp() const noexcept { return kind == TypeKind::f32 || kind == TypeKind::f64; }
    [[nodiscard]] bool is_integer_like() const noexcept { return !is_aggregate() && !is_fp() && kind != TypeKind::void_; }
    [[nodiscard]] unsigned hfa_element_size() const noexcept { return hfa_double ? 8u : 4u; }
};

struct Signature {
    TypeDesc ret;
    std::vector<TypeDesc> params;
    bool variadic{};
    bool unknown{};               // "?"
    std::string text;             // canonical text
};

// Parses `text`. Returns nullptr on a syntax error. Results are interned:
// equal text yields the same pointer for the lifetime of the process.
const Signature* intern_signature(std::string_view text);
// Parses without interning (tests). Throws std::invalid_argument on errors.
Signature parse_signature(std::string_view text);
// The "?" signature.
const Signature* unknown_signature();

// ---- call plans -------------------------------------------------------------

// One 8-byte (or smaller) piece of an argument on the ARM64 side.
struct ArmPiece {
    enum Kind : std::uint8_t { gpr, vreg, stack } kind;
    std::uint8_t index;           // x/v register number (gpr/vreg)
    std::uint8_t bytes;           // 4 or 8 (vreg members), 8 otherwise
    std::uint32_t offset;         // guest-stack byte offset from SP (stack)
};

struct ArgPlan {
    TypeDesc type;
    // ARM64 location: pieces in memory order. For an aggregate passed
    // indirectly (larger than 16 bytes and not an HFA) there is one piece
    // holding the pointer and `arm_indirect` is set.
    std::vector<ArmPiece> arm;
    bool arm_indirect{};
    // Win64 position (0-based, after the hidden return pointer if any).
    std::uint8_t win_pos{};
    bool win_by_reference{};      // pointer to a caller copy
    bool win_fp{};                // xmm register when win_pos < 4
};

enum class ReturnKind : std::uint8_t {
    none,
    integer,        // rax <-> x0
    fp,             // xmm0 <-> v0
    record_regs,    // ARM x0[,x1] ; Win64 rax (1/2/4/8) or hidden buffer
    record_memory,  // ARM x8 buffer ; Win64 hidden buffer (same memory)
    hfa_regs,       // ARM v0..v3 ; Win64 rax (4/8 bytes) or hidden buffer
};

struct CallPlan {
    std::vector<ArgPlan> args;
    ReturnKind ret_kind{ReturnKind::none};
    TypeDesc ret;
    bool win_hidden_return{};     // Win64 passes the result buffer in rcx
    bool variadic{};
    unsigned win_slots{};         // positional slots used by named arguments (incl. hidden)
    unsigned arm_stack_bytes{};   // guest stack bytes used by named arguments
    // Variadic: imaginary-stack slots used by the named arguments (x0..x7
    // are slots 0..7, the guest stack continues at slot 8).
    unsigned arm_named_slots{};
};

CallPlan plan_call(const Signature& signature);

[[nodiscard]] constexpr bool win_by_value_size(std::uint32_t size) noexcept {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

} // namespace transmute::fx::abi
