#pragma once

// Objective-C type encodings and the Apple arm64 calling convention for
// NSMethodSignature / NSInvocation (host-only core, unit tested in
// tests/runtime_tests.cpp).
//
// Types: c C s S i I l L q Q f d D(=double) B v * # : @ @? ^type, structs
// {name=...}, unions (...), arrays [Ntype], and the qualifiers r n N o O R V A
// (skipped). Bitfields (b), vectors and long double other than D fail.
//
// Assignment (AAPCS64 with Apple's rules): integers, pointers, objects and
// selectors take x registers; float/double take v registers; homogeneous
// floating-point aggregates of 1-4 float or double members take consecutive
// v registers (or the stack when they do not all fit, after which no v
// register is used); other composites up to 16 bytes take one or two x
// registers (or the stack when they do not fit, after which no x register is
// used); larger composites are copied by the caller and passed by
// reference. Stack arguments are packed at their natural alignment (Apple),
// composites at 8. Results: integers in x0, floating point in v0, HFAs in
// v0-v3, composites up to 16 bytes in x0/x1, larger ones through x8.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::completion_kit::invocation {

enum class Kind { void_type, integer, floating, composite };

struct Type {
    std::string encoding;     // without qualifiers and frame offsets
    Kind kind{Kind::integer};
    std::size_t size{}, alignment{1};
    bool is_signed{};         // integers
    bool object{};            // @ (and @?: blocks)
    bool block{};             // @?
    bool c_string{};          // *
    unsigned hfa_count{};     // composite: 1-4 when homogeneous floating point, else 0
    std::size_t hfa_member{}; // 4 (float) or 8 (double)
};

// Parses one type at the front of `cursor` and advances past it (and past
// any frame offset digits that follow).
[[nodiscard]] Type parse_type(std::string_view& cursor);

struct Signature {
    std::string types;        // the encoding as given
    Type result;
    std::vector<Type> arguments;   // including self (@) and _cmd (:)
    std::size_t frame_length{};    // bytes of the argument frame (registers and stack, as Foundation reports)
};

// Parses a method type encoding ("v24@0:8@16"). Fails on malformed input.
[[nodiscard]] Signature parse_signature(std::string_view types);

// Where one argument lives at the call.
struct Location {
    enum class Where { x, v, stack, reference } where{Where::x};
    unsigned first{};         // x or v register index (reference: the x register holding the pointer)
    unsigned count{};         // registers used (x: 1-2, v: HFA members)
    std::size_t offset{};     // stack: byte offset from the stack pointer at entry (reference with a stack pointer too)
    bool stack_pointer{};     // reference: the pointer itself is on the stack at `offset`
};

struct Layout {
    std::vector<Location> arguments;
    std::size_t stack_size{};  // bytes of stack arguments
    bool indirect_result{};    // result through x8
};

[[nodiscard]] Layout assign(const Signature& signature);

} // namespace alchemy::gui::completion_kit::invocation
