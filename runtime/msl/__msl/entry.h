// Alchemy metal_stdlib: helpers for the entry point wrappers the source
// rewriter generates (src/alchemy/msl/rewriter.cpp). Project-authored (MIT).
// Interface values cross the wrapper as scalars or ext vectors so the IR
// carries their exact types; bool interface values travel as uint.
#pragma once

namespace metal {

template<typename T> struct __io {
    using raw = conditional_t<is_same_v<T, bool>, uint, T>;
    static T wrap(raw x) { return T(x); }
    static raw unwrap(T x) { return raw(x); }
};
template<typename T, int N> struct __io<vec<T, N>> {
    using raw = conditional_t<is_same_v<T, bool>, __ext<uint, N>, __ext<T, N>>;
    static vec<T, N> wrap(raw x) {
        if constexpr (is_same_v<T, bool>) return vec<T, N>(vec<uint, N>(x));
        else return vec<T, N>(x);
    }
    static raw unwrap(vec<T, N> x) {
        if constexpr (is_same_v<T, bool>) return vec<uint, N>(x).v;
        else return x.v;
    }
};
template<typename T, int N> struct __io<packed_vec<T, N>> {
    using raw = __ext<T, N>;
    static packed_vec<T, N> wrap(raw x) { return packed_vec<T, N>(vec<T, N>(x)); }
    static raw unwrap(packed_vec<T, N> x) { return vec<T, N>(x).v; }
};

// Signedness of an interface value's declared type, resolved by clang through
// any alias (`using ivec2 = int2;`): the IR carries only the bit width, but
// Vulkan matches signed/unsigned integer vertex formats, varyings and colour
// attachments against the SPIR-V variable's signedness.
template<typename T> constexpr uint __io_sign_of() {
    if constexpr (is_integral_v<T> && !is_same_v<remove_cv_t<T>, bool>) return (T)(-1) < (T)(0) ? 1u : 0u;
    else return 0u;
}
template<typename T> struct __io_sign { static constexpr uint value = __io_sign_of<T>(); };
template<typename T, int N> struct __io_sign<vec<T, N>> : __io_sign<T> {};
template<typename T, int N> struct __io_sign<packed_vec<T, N>> : __io_sign<T> {};

template<typename T> inline T __msl_in(uint location, uint interpolation) {
    using IO = __io<remove_cv_t<T>>;
    return IO::wrap(__msl_stage_input<typename IO::raw>(location, interpolation, __io_sign<remove_cv_t<T>>::value));
}
template<typename T> inline void __msl_out(uint location, T value) {
    using IO = __io<remove_cv_t<T>>;
    __msl_stage_output<typename IO::raw>(location, __io_sign<remove_cv_t<T>>::value, IO::unwrap(value));
}
// Framebuffer fetch: a fragment [[color(n)]] input.
template<typename T> inline T __msl_color_in(uint color) {
    using IO = __io<remove_cv_t<T>>;
    return IO::wrap(__msl_color_input<typename IO::raw>(color, __io_sign<remove_cv_t<T>>::value));
}
template<typename T> inline void __msl_color(uint color, uint index, T value) {
    using IO = __io<remove_cv_t<T>>;
    __msl_color_output<typename IO::raw>(color, index, __io_sign<remove_cv_t<T>>::value, IO::unwrap(value));
}
template<typename T> inline void __msl_builtin_out(uint builtin, uint element, T value) {
    using IO = __io<remove_cv_t<T>>;
    __msl_builtin_output<typename IO::raw>(builtin, element, IO::unwrap(value));
}
// An array builtin output held in an array object (metal::array<float, N>,
// Tint's tint_array<float, N>): element i of the object is element i of the
// builtin (the loop unrolls to constant indices); a scalar is element 0.
// `neutral` instead of the values when the output is absent.
// (Overloads, not a type trait: a local scalar is `__private float` here.)
inline void __msl_builtin_out_elements(uint builtin, float value) { __msl_builtin_out(builtin, 0u, value); }
template<typename T> inline void __msl_builtin_out_elements(uint builtin, const T& value) {
    for (uint i = 0; i < sizeof(T) / sizeof(value[0]); ++i) __msl_builtin_out(builtin, i, value[i]);
}
inline void __msl_builtin_out_neutral(uint builtin, float, float neutral) { __msl_builtin_out(builtin, 0u, neutral); }
template<typename T> inline void __msl_builtin_out_neutral(uint builtin, const T& value, float neutral) {
    for (uint i = 0; i < sizeof(T) / sizeof(value[0]); ++i) __msl_builtin_out(builtin, i, neutral);
}

// Builtin inputs are read with their natural Vulkan type (S, a scalar or an
// ext vector) and converted to the parameter's declared type: ushort ids,
// uint2 grid positions of a 2D dispatch and so on.
template<typename T, typename S> inline T __msl_builtin_in(uint builtin) {
    using P = remove_cv_t<T>;
    const S raw = __msl_builtin_input<S>(builtin);
    if constexpr (is_vector_v<P>) {
        P r{};
        for (int i = 0; i < vec_elements<P>::value; ++i) {
            if constexpr (is_arithmetic_v<S>) r[i] = i == 0 ? __scalar_t<P>(raw) : __scalar_t<P>(0);
            else r[i] = __scalar_t<P>(raw[i]);
        }
        return r;
    } else if constexpr (is_arithmetic_v<S>) {
        return P(raw);
    } else {
        return P(raw[0]);
    }
}

// Conditional operands: the rewriter turns `c ? a : b` into
// `c ? (__msl_rv{}, (a)) : (__msl_rv{}, (b))` so both are prvalues without an
// address space (clang's OpenCL C++ rejects a class prvalue against a
// `__private` lvalue of the same class). Void operands use the built-in comma.
struct __msl_rv {};
template<typename T> constexpr T operator,(__msl_rv, T value) { return value; }
// A packed vector operand is its vector (`c ? packed_member : float3(0)`
// would otherwise be ambiguous: each converts to the other).
template<typename T, int N> constexpr vec<T, N> operator,(__msl_rv, packed_vec<T, N> value) { return value; }

template<typename T> inline T __msl_texture(uint binding) { T t; t.__h = binding << 16; return t; }
template<typename T, size_t N> inline array<T, N> __msl_texture_array(uint binding) {
    array<T, N> r;
    for (size_t i = 0; i < N; ++i) r.__elems[i].__h = (binding << 16) | uint(i);
    return r;
}
inline sampler __msl_sampler(uint binding) { sampler s; s.__h = binding; return s; }

} // namespace metal
