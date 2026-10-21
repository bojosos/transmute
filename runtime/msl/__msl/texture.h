// Alchemy metal_stdlib: samplers and textures. Project-authored (MIT) from
// the MSL specification. A texture object is a 32-bit handle and a sampler a
// 64-bit handle (see intrinsics.h); every member function is one intrinsic
// call the lowering turns into an OpImage* instruction.
#pragma once

namespace metal {

// ---- sampler state --------------------------------------------------------------------
// The renamed MSL spelling of read_write (an OpenCL keyword) is
// __msl_id_read_write; the source rewriter renames the user's token.
enum class access { sample, read, write, __msl_id_read_write };
enum class coord { normalized, pixel };
enum class filter { nearest, linear };
enum class mag_filter { nearest, linear };
enum class min_filter { nearest, linear };
enum class mip_filter { none, nearest, linear };
// Values are MTLSamplerAddressMode.
enum class address { clamp_to_edge = 0, mirrored_clamp_to_edge = 1, repeat = 2, mirrored_repeat = 3,
    clamp_to_zero = 4, clamp_to_border = 5 };
enum class s_address { clamp_to_edge = 0, mirrored_clamp_to_edge = 1, repeat = 2, mirrored_repeat = 3,
    clamp_to_zero = 4, clamp_to_border = 5 };
enum class t_address { clamp_to_edge = 0, mirrored_clamp_to_edge = 1, repeat = 2, mirrored_repeat = 3,
    clamp_to_zero = 4, clamp_to_border = 5 };
enum class r_address { clamp_to_edge = 0, mirrored_clamp_to_edge = 1, repeat = 2, mirrored_repeat = 3,
    clamp_to_zero = 4, clamp_to_border = 5 };
// Values are MTLCompareFunction (none = never compare).
enum class compare_func { none = 0, never = 0, less = 1, equal = 2, less_equal = 3, greater = 4,
    not_equal = 5, greater_equal = 6, always = 7 };
// Values are MTLSamplerBorderColor.
enum class border_color { transparent_black = 0, opaque_black = 1, opaque_white = 2 };
enum class component { x, y, z, w };
struct max_anisotropy { int value; constexpr max_anisotropy(int v) : value(v) {} };
struct lod_clamp { float lo, hi; constexpr lod_clamp(float a, float b) : lo(a), hi(b) {} };

// Packed constexpr sampler state (bit 63 set). Bits: 0 min filter, 1 mag
// filter, 2-3 mip filter, 4-6 s, 7-9 t, 10-12 r address, 13-15 compare,
// 16-17 border colour, 18-22 max anisotropy - 1, 23 pixel coordinates,
// 32-47 lod_min and 48-62 lod_max as IEEE half bits.
struct __sampler_state {
    ulong bits;
    constexpr __sampler_state() : bits((1ul << 63) | (0x7bfful << 48)) {}   // lod_max = 65504
    constexpr void set(ulong mask, int shift, ulong value) { bits = (bits & ~(mask << shift)) | ((value & mask) << shift); }
    constexpr void apply(coord c) { set(1, 23, (ulong)c); }
    constexpr void apply(filter f) { set(1, 0, (ulong)f); set(1, 1, (ulong)f); }
    constexpr void apply(mag_filter f) { set(1, 1, (ulong)f); }
    constexpr void apply(min_filter f) { set(1, 0, (ulong)f); }
    constexpr void apply(mip_filter f) { set(3, 2, (ulong)f); }
    constexpr void apply(address a) { set(7, 4, (ulong)a); set(7, 7, (ulong)a); set(7, 10, (ulong)a); }
    constexpr void apply(s_address a) { set(7, 4, (ulong)a); }
    constexpr void apply(t_address a) { set(7, 7, (ulong)a); }
    constexpr void apply(r_address a) { set(7, 10, (ulong)a); }
    constexpr void apply(compare_func f) { set(7, 13, (ulong)f); }
    constexpr void apply(border_color b) { set(3, 16, (ulong)b); }
    constexpr void apply(max_anisotropy m) { set(31, 18, (ulong)(m.value < 1 ? 0 : m.value > 16 ? 15 : m.value - 1)); }
    constexpr void apply(lod_clamp l) {
        set(0xffff, 32, (ulong)__builtin_bit_cast(ushort, (half)l.lo));
        set(0x7fff, 48, (ulong)__builtin_bit_cast(ushort, (half)l.hi));
    }
};

struct sampler {
    ulong __h;
    constexpr sampler() : __h(__sampler_state().bits) {}
    template<typename... A> constexpr sampler(A... a) : __h(__pack(a...)) {}
    template<typename... A> static constexpr ulong __pack(A... a) {
        __sampler_state s;
        (s.apply(a), ...);
        return s.bits;
    }
};

// ---- sample options -------------------------------------------------------------------
struct bias { float value; constexpr bias(float v) : value(v) {} };
struct level { float value; constexpr level(float v) : value(v) {} };
struct min_lod_clamp { float value; constexpr min_lod_clamp(float v) : value(v) {} };
struct gradient2d { float2 dx, dy; gradient2d(float2 a, float2 b) : dx(a), dy(b) {} };
struct gradient3d { float3 dx, dy; gradient3d(float3 a, float3 b) : dx(a), dy(b) {} };
struct gradientcube { float3 dx, dy; gradientcube(float3 a, float3 b) : dx(a), dy(b) {} };

#define __MSL_COMMA ,
// Sample and read results: vec<T, 4> for colour textures, T for depth.
template<typename T> using __texel = __ext<T, 4>;

// The texture types, in the order of TextureType in compiler.hpp. Coordinates
// for read/write use uint vectors (ushort overloads convert).
template<typename T, access A = access::sample> struct texture1d {
    uint __h;
    vec<T, 4> sample(sampler s, float c) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c, 0u, 0u, 0.0f, 0.0f, 0.0f, 0); }
    vec<T, 4> read(uint c, uint lod = 0) const { return __msl_tex_read<__texel<T>>(__h, c, 0u, lod); }
    vec<T, 4> read(ushort c, ushort lod = 0) const { return read(uint(c), uint(lod)); }
    void write(vec<T, 4> v, uint c, uint lod = 0) const { __msl_tex_write(__h, c, 0u, lod, v.v); }
    uint get_width(uint lod = 0) const { return __msl_tex_query(__h, 0, lod); }
    uint get_num_mip_levels() const { return __msl_tex_query(__h, 4, 0); }
    void fence() const { __msl_tex_fence(__h); }
};
template<typename T, access A = access::sample> struct texture1d_array {
    uint __h;
    vec<T, 4> sample(sampler s, float c, uint array) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c, array, 0u, 0.0f, 0.0f, 0.0f, 0); }
    vec<T, 4> read(uint c, uint array, uint lod = 0) const { return __msl_tex_read<__texel<T>>(__h, c, array, lod); }
    void write(vec<T, 4> v, uint c, uint array, uint lod = 0) const { __msl_tex_write(__h, c, array, lod, v.v); }
    uint get_width(uint lod = 0) const { return __msl_tex_query(__h, 0, lod); }
    uint get_array_size() const { return __msl_tex_query(__h, 3, 0); }
    uint get_num_mip_levels() const { return __msl_tex_query(__h, 4, 0); }
    void fence() const { __msl_tex_fence(__h); }
};

template<typename T, access A = access::sample> struct texture2d {
    uint __h;
    vec<T, 4> sample(sampler s, float2 c, int2 offset = int2(0)) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, 0u, 0u, 0.0f, float2(0).v, float2(0).v, offset.v); }
    vec<T, 4> sample(sampler s, float2 c, bias b, int2 offset = int2(0)) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, 0u, 1u, b.value, float2(0).v, float2(0).v, offset.v); }
    vec<T, 4> sample(sampler s, float2 c, level l, int2 offset = int2(0)) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, 0u, 2u, l.value, float2(0).v, float2(0).v, offset.v); }
    vec<T, 4> sample(sampler s, float2 c, gradient2d g, int2 offset = int2(0)) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, 0u, 3u, 0.0f, g.dx.v, g.dy.v, offset.v); }
    vec<T, 4> gather(sampler s, float2 c, int2 offset = int2(0), component k = component::x) const {
        return __msl_tex_gather<__texel<T>>(__h, s.__h, c.v, 0u, offset.v, (uint)k); }
    vec<T, 4> read(uint2 c, uint lod = 0) const { return __msl_tex_read<__texel<T>>(__h, c.v, 0u, lod); }
    vec<T, 4> read(ushort2 c, ushort lod = 0) const { return read(uint2(c), uint(lod)); }
    void write(vec<T, 4> v, uint2 c, uint lod = 0) const { __msl_tex_write(__h, c.v, 0u, lod, v.v); }
    void write(vec<T, 4> v, ushort2 c, ushort lod = 0) const { write(v, uint2(c), uint(lod)); }
    uint get_width(uint lod = 0) const { return __msl_tex_query(__h, 0, lod); }
    uint get_height(uint lod = 0) const { return __msl_tex_query(__h, 1, lod); }
    uint get_num_mip_levels() const { return __msl_tex_query(__h, 4, 0); }
    void fence() const { __msl_tex_fence(__h); }
#define __MSL_TEX_ATOMICS(COORD, CEXPR, LAYER_PARAM, LAYER) \
    vec<T, 4> atomic_fetch_add(COORD c LAYER_PARAM, T v) const { return vec<T, 4>(__msl_tex_atomic(__h, CEXPR, LAYER, 0u, v)); } \
    vec<T, 4> atomic_fetch_sub(COORD c LAYER_PARAM, T v) const { return vec<T, 4>(__msl_tex_atomic(__h, CEXPR, LAYER, 1u, v)); } \
    vec<T, 4> atomic_fetch_and(COORD c LAYER_PARAM, T v) const { return vec<T, 4>(__msl_tex_atomic(__h, CEXPR, LAYER, 2u, v)); } \
    vec<T, 4> atomic_fetch_or(COORD c LAYER_PARAM, T v) const { return vec<T, 4>(__msl_tex_atomic(__h, CEXPR, LAYER, 3u, v)); } \
    vec<T, 4> atomic_fetch_xor(COORD c LAYER_PARAM, T v) const { return vec<T, 4>(__msl_tex_atomic(__h, CEXPR, LAYER, 4u, v)); } \
    vec<T, 4> atomic_fetch_min(COORD c LAYER_PARAM, T v) const { return vec<T, 4>(__msl_tex_atomic(__h, CEXPR, LAYER, 5u, v)); } \
    vec<T, 4> atomic_fetch_max(COORD c LAYER_PARAM, T v) const { return vec<T, 4>(__msl_tex_atomic(__h, CEXPR, LAYER, 6u, v)); } \
    vec<T, 4> atomic_exchange(COORD c LAYER_PARAM, T v) const { return vec<T, 4>(__msl_tex_atomic(__h, CEXPR, LAYER, 7u, v)); }
    __MSL_TEX_ATOMICS(uint2, c.v, , 0u)
};
template<typename T, access A = access::sample> struct texture2d_array {
    uint __h;
    vec<T, 4> sample(sampler s, float2 c, uint array, int2 offset = int2(0)) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, array, 0u, 0.0f, float2(0).v, float2(0).v, offset.v); }
    vec<T, 4> sample(sampler s, float2 c, uint array, bias b, int2 offset = int2(0)) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, array, 1u, b.value, float2(0).v, float2(0).v, offset.v); }
    vec<T, 4> sample(sampler s, float2 c, uint array, level l, int2 offset = int2(0)) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, array, 2u, l.value, float2(0).v, float2(0).v, offset.v); }
    vec<T, 4> sample(sampler s, float2 c, uint array, gradient2d g, int2 offset = int2(0)) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, array, 3u, 0.0f, g.dx.v, g.dy.v, offset.v); }
    vec<T, 4> gather(sampler s, float2 c, uint array, int2 offset = int2(0), component k = component::x) const {
        return __msl_tex_gather<__texel<T>>(__h, s.__h, c.v, array, offset.v, (uint)k); }
    vec<T, 4> read(uint2 c, uint array, uint lod = 0) const { return __msl_tex_read<__texel<T>>(__h, c.v, array, lod); }
    vec<T, 4> read(ushort2 c, ushort array, ushort lod = 0) const { return read(uint2(c), uint(array), uint(lod)); }
    void write(vec<T, 4> v, uint2 c, uint array, uint lod = 0) const { __msl_tex_write(__h, c.v, array, lod, v.v); }
    void write(vec<T, 4> v, ushort2 c, ushort array, ushort lod = 0) const {
        write(v, uint2(c), uint(array), uint(lod)); }
    uint get_width(uint lod = 0) const { return __msl_tex_query(__h, 0, lod); }
    uint get_height(uint lod = 0) const { return __msl_tex_query(__h, 1, lod); }
    uint get_array_size() const { return __msl_tex_query(__h, 3, 0); }
    uint get_num_mip_levels() const { return __msl_tex_query(__h, 4, 0); }
    void fence() const { __msl_tex_fence(__h); }
    __MSL_TEX_ATOMICS(uint2, c.v, __MSL_COMMA uint array, array)
};
template<typename T, access A = access::read> struct texture2d_ms {
    uint __h;
    vec<T, 4> read(uint2 c, uint sample) const { return __msl_tex_read<__texel<T>>(__h, c.v, 0u, sample); }
    uint get_width() const { return __msl_tex_query(__h, 0, 0); }
    uint get_height() const { return __msl_tex_query(__h, 1, 0); }
    uint get_num_samples() const { return __msl_tex_query(__h, 5, 0); }
};
template<typename T, access A = access::read> struct texture2d_ms_array {
    uint __h;
    vec<T, 4> read(uint2 c, uint array, uint sample) const { return __msl_tex_read<__texel<T>>(__h, c.v, array, sample); }
    uint get_width() const { return __msl_tex_query(__h, 0, 0); }
    uint get_height() const { return __msl_tex_query(__h, 1, 0); }
    uint get_array_size() const { return __msl_tex_query(__h, 3, 0); }
    uint get_num_samples() const { return __msl_tex_query(__h, 5, 0); }
};
template<typename T, access A = access::sample> struct texture3d {
    uint __h;
    vec<T, 4> sample(sampler s, float3 c, int3 offset = int3(0)) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, 0u, 0u, 0.0f, float3(0).v, float3(0).v, offset.v); }
    vec<T, 4> sample(sampler s, float3 c, bias b, int3 offset = int3(0)) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, 0u, 1u, b.value, float3(0).v, float3(0).v, offset.v); }
    vec<T, 4> sample(sampler s, float3 c, level l, int3 offset = int3(0)) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, 0u, 2u, l.value, float3(0).v, float3(0).v, offset.v); }
    vec<T, 4> sample(sampler s, float3 c, gradient3d g, int3 offset = int3(0)) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, 0u, 3u, 0.0f, g.dx.v, g.dy.v, offset.v); }
    vec<T, 4> read(uint3 c, uint lod = 0) const { return __msl_tex_read<__texel<T>>(__h, c.v, 0u, lod); }
    vec<T, 4> read(ushort3 c, ushort lod = 0) const { return read(uint3(c), uint(lod)); }
    void write(vec<T, 4> v, uint3 c, uint lod = 0) const { __msl_tex_write(__h, c.v, 0u, lod, v.v); }
    void write(vec<T, 4> v, ushort3 c, ushort lod = 0) const { write(v, uint3(c), uint(lod)); }
    uint get_width(uint lod = 0) const { return __msl_tex_query(__h, 0, lod); }
    uint get_height(uint lod = 0) const { return __msl_tex_query(__h, 1, lod); }
    uint get_depth(uint lod = 0) const { return __msl_tex_query(__h, 2, lod); }
    uint get_num_mip_levels() const { return __msl_tex_query(__h, 4, 0); }
    void fence() const { __msl_tex_fence(__h); }
    __MSL_TEX_ATOMICS(uint3, c.v, , 0u)
};
template<typename T, access A = access::sample> struct texturecube {
    uint __h;
    vec<T, 4> sample(sampler s, float3 c) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, 0u, 0u, 0.0f, float3(0).v, float3(0).v, 0); }
    vec<T, 4> sample(sampler s, float3 c, bias b) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, 0u, 1u, b.value, float3(0).v, float3(0).v, 0); }
    vec<T, 4> sample(sampler s, float3 c, level l) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, 0u, 2u, l.value, float3(0).v, float3(0).v, 0); }
    vec<T, 4> sample(sampler s, float3 c, gradientcube g) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, 0u, 3u, 0.0f, g.dx.v, g.dy.v, 0); }
    vec<T, 4> gather(sampler s, float3 c, component k = component::x) const {
        return __msl_tex_gather<__texel<T>>(__h, s.__h, c.v, 0u, 0, (uint)k); }
    vec<T, 4> read(uint2 c, uint face, uint lod = 0) const { return __msl_tex_read<__texel<T>>(__h, c.v, face, lod); }
    void write(vec<T, 4> v, uint2 c, uint face, uint lod = 0) const { __msl_tex_write(__h, c.v, face, lod, v.v); }
    uint get_width(uint lod = 0) const { return __msl_tex_query(__h, 0, lod); }
    uint get_height(uint lod = 0) const { return __msl_tex_query(__h, 1, lod); }
    uint get_num_mip_levels() const { return __msl_tex_query(__h, 4, 0); }
    void fence() const { __msl_tex_fence(__h); }
};
template<typename T, access A = access::sample> struct texturecube_array {
    uint __h;
    vec<T, 4> sample(sampler s, float3 c, uint array) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, array, 0u, 0.0f, float3(0).v, float3(0).v, 0); }
    vec<T, 4> sample(sampler s, float3 c, uint array, bias b) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, array, 1u, b.value, float3(0).v, float3(0).v, 0); }
    vec<T, 4> sample(sampler s, float3 c, uint array, level l) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, array, 2u, l.value, float3(0).v, float3(0).v, 0); }
    vec<T, 4> sample(sampler s, float3 c, uint array, gradientcube g) const {
        return __msl_tex_sample<__texel<T>>(__h, s.__h, c.v, array, 3u, 0.0f, g.dx.v, g.dy.v, 0); }
    vec<T, 4> gather(sampler s, float3 c, uint array, component k = component::x) const {
        return __msl_tex_gather<__texel<T>>(__h, s.__h, c.v, array, 0, (uint)k); }
    vec<T, 4> read(uint2 c, uint face, uint array, uint lod = 0) const {
        return __msl_tex_read<__texel<T>>(__h, c.v, array * 6u + face, lod); }
    void write(vec<T, 4> v, uint2 c, uint face, uint array, uint lod = 0) const {
        __msl_tex_write(__h, c.v, array * 6u + face, lod, v.v); }
    uint get_width(uint lod = 0) const { return __msl_tex_query(__h, 0, lod); }
    uint get_height(uint lod = 0) const { return __msl_tex_query(__h, 1, lod); }
    uint get_array_size() const { return __msl_tex_query(__h, 3, 0); }
    uint get_num_mip_levels() const { return __msl_tex_query(__h, 4, 0); }
    void fence() const { __msl_tex_fence(__h); }
};
template<typename T, access A = access::read> struct texture_buffer {
    uint __h;
    vec<T, 4> read(uint c) const { return __msl_tex_read<__texel<T>>(__h, c, 0u, 0u); }
    void write(vec<T, 4> v, uint c) const { __msl_tex_write(__h, c, 0u, 0u, v.v); }
    uint get_width() const { return __msl_tex_query(__h, 0, 0); }
    void fence() const { __msl_tex_fence(__h); }
};

// Depth textures: sample/read return T, sample_compare compares in hardware.
#define __MSL_DEPTH_SAMPLE(C, CV, LAYER_PARAM, LAYER, O, OV, OZ) \
    T sample(sampler s, C c LAYER_PARAM O) const { \
        return __msl_tex_sample<__texel<T>>(__h, s.__h, CV, LAYER, 0u, 0.0f, CV, CV, OV).x; } \
    T sample(sampler s, C c LAYER_PARAM, level l O) const { \
        return __msl_tex_sample<__texel<T>>(__h, s.__h, CV, LAYER, 2u, l.value, CV, CV, OV).x; } \
    T sample(sampler s, C c LAYER_PARAM, bias b O) const { \
        return __msl_tex_sample<__texel<T>>(__h, s.__h, CV, LAYER, 1u, b.value, CV, CV, OV).x; } \
    T sample_compare(sampler s, C c LAYER_PARAM, float ref O) const { \
        return (T)__msl_tex_sample_compare<float>(__h, s.__h, CV, LAYER, ref, 0u, 0.0f, OV); } \
    T sample_compare(sampler s, C c LAYER_PARAM, float ref, level l O) const { \
        return (T)__msl_tex_sample_compare<float>(__h, s.__h, CV, LAYER, ref, 2u, l.value, OV); }
template<typename T = float, access A = access::sample> struct depth2d {
    uint __h;
    __MSL_DEPTH_SAMPLE(float2, c.v, , 0u, __MSL_COMMA int2 offset = int2(0), offset.v, int2(0))
    vec<T, 4> gather(sampler s, float2 c, int2 offset = int2(0)) const {
        return __msl_tex_gather<__texel<T>>(__h, s.__h, c.v, 0u, offset.v, 0u); }
    vec<T, 4> gather_compare(sampler s, float2 c, float ref, int2 offset = int2(0)) const {
        return __msl_tex_gather_compare<__texel<T>>(__h, s.__h, c.v, 0u, ref, offset.v); }
    T read(uint2 c, uint lod = 0) const { return __msl_tex_read<__texel<T>>(__h, c.v, 0u, lod).x; }
    void write(T v, uint2 c, uint lod = 0) const { __msl_tex_write(__h, c.v, 0u, lod, vec<T, 4>(v).v); }
    uint get_width(uint lod = 0) const { return __msl_tex_query(__h, 0, lod); }
    uint get_height(uint lod = 0) const { return __msl_tex_query(__h, 1, lod); }
    uint get_num_mip_levels() const { return __msl_tex_query(__h, 4, 0); }
};
template<typename T = float, access A = access::sample> struct depth2d_array {
    uint __h;
    __MSL_DEPTH_SAMPLE(float2, c.v, __MSL_COMMA uint array, array, __MSL_COMMA int2 offset = int2(0), offset.v, int2(0))
    vec<T, 4> gather(sampler s, float2 c, uint array, int2 offset = int2(0)) const {
        return __msl_tex_gather<__texel<T>>(__h, s.__h, c.v, array, offset.v, 0u); }
    vec<T, 4> gather_compare(sampler s, float2 c, uint array, float ref, int2 offset = int2(0)) const {
        return __msl_tex_gather_compare<__texel<T>>(__h, s.__h, c.v, array, ref, offset.v); }
    T read(uint2 c, uint array, uint lod = 0) const { return __msl_tex_read<__texel<T>>(__h, c.v, array, lod).x; }
    uint get_width(uint lod = 0) const { return __msl_tex_query(__h, 0, lod); }
    uint get_height(uint lod = 0) const { return __msl_tex_query(__h, 1, lod); }
    uint get_array_size() const { return __msl_tex_query(__h, 3, 0); }
    uint get_num_mip_levels() const { return __msl_tex_query(__h, 4, 0); }
};
template<typename T = float, access A = access::read> struct depth2d_ms {
    uint __h;
    T read(uint2 c, uint sample) const { return __msl_tex_read<__texel<T>>(__h, c.v, 0u, sample).x; }
    uint get_width() const { return __msl_tex_query(__h, 0, 0); }
    uint get_height() const { return __msl_tex_query(__h, 1, 0); }
    uint get_num_samples() const { return __msl_tex_query(__h, 5, 0); }
};
template<typename T = float, access A = access::read> struct depth2d_ms_array {
    uint __h;
    T read(uint2 c, uint array, uint sample) const { return __msl_tex_read<__texel<T>>(__h, c.v, array, sample).x; }
    uint get_width() const { return __msl_tex_query(__h, 0, 0); }
    uint get_height() const { return __msl_tex_query(__h, 1, 0); }
    uint get_array_size() const { return __msl_tex_query(__h, 3, 0); }
    uint get_num_samples() const { return __msl_tex_query(__h, 5, 0); }
};
template<typename T = float, access A = access::sample> struct depthcube {
    uint __h;
    __MSL_DEPTH_SAMPLE(float3, c.v, , 0u, , 0, 0)
    vec<T, 4> gather(sampler s, float3 c) const { return __msl_tex_gather<__texel<T>>(__h, s.__h, c.v, 0u, 0, 0u); }
    uint get_width(uint lod = 0) const { return __msl_tex_query(__h, 0, lod); }
    uint get_height(uint lod = 0) const { return __msl_tex_query(__h, 1, lod); }
    uint get_num_mip_levels() const { return __msl_tex_query(__h, 4, 0); }
};
template<typename T = float, access A = access::sample> struct depthcube_array {
    uint __h;
    __MSL_DEPTH_SAMPLE(float3, c.v, __MSL_COMMA uint array, array, , 0, 0)
    uint get_width(uint lod = 0) const { return __msl_tex_query(__h, 0, lod); }
    uint get_height(uint lod = 0) const { return __msl_tex_query(__h, 1, lod); }
    uint get_array_size() const { return __msl_tex_query(__h, 3, 0); }
    uint get_num_mip_levels() const { return __msl_tex_query(__h, 4, 0); }
};
#undef __MSL_DEPTH_SAMPLE
#undef __MSL_TEX_ATOMICS
#undef __MSL_COMMA

} // namespace metal
