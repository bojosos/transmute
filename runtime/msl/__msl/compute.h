// Alchemy metal_stdlib: atomics, barriers, SIMD-group and quad-group
// functions, derivatives and fragment discard. Project-authored (MIT).
#pragma once

namespace metal {

// ---- atomics ---------------------------------------------------------------------------
enum memory_order { memory_order_relaxed = __ATOMIC_RELAXED, memory_order_acquire = __ATOMIC_ACQUIRE,
    memory_order_release = __ATOMIC_RELEASE, memory_order_acq_rel = __ATOMIC_ACQ_REL,
    memory_order_seq_cst = __ATOMIC_SEQ_CST };
enum class thread_scope { thread, simdgroup, threadgroup, device };

template<typename T> struct _atomic { T __v; };
template<typename T> using atomic = _atomic<T>;
// atomic_int etc. are OpenCL builtin typedefs: the rewriter renames the
// application's spellings to these.
typedef _atomic<int> __msl_id_atomic_int;
typedef _atomic<uint> __msl_id_atomic_uint;
typedef _atomic<bool> __msl_id_atomic_bool;
typedef _atomic<float> __msl_id_atomic_float;
typedef _atomic<ulong> __msl_id_atomic_ulong;
typedef _atomic<long> __msl_id_atomic_long;

// The memory order is lowered as relaxed (all Metal supports for most
// operations); the address space follows the pointer.
#define __MSL_ATOMICS(AS) \
    template<typename T> inline T atomic_load_explicit(const volatile AS _atomic<T>* o, memory_order) { \
        return __atomic_load_n(&o->__v, __ATOMIC_RELAXED); } \
    template<typename T> inline void atomic_store_explicit(volatile AS _atomic<T>* o, type_identity_t<T> v, memory_order) { \
        __atomic_store_n(&o->__v, v, __ATOMIC_RELAXED); } \
    template<typename T> inline T atomic_exchange_explicit(volatile AS _atomic<T>* o, type_identity_t<T> v, memory_order) { \
        return __atomic_exchange_n(&o->__v, v, __ATOMIC_RELAXED); } \
    template<typename T> inline bool atomic_compare_exchange_weak_explicit(volatile AS _atomic<T>* o, \
        type_identity_t<T>* expected, type_identity_t<T> desired, memory_order, memory_order) { \
        return __atomic_compare_exchange_n(&o->__v, expected, desired, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED); } \
    template<typename T> inline bool atomic_compare_exchange_strong_explicit(volatile AS _atomic<T>* o, \
        type_identity_t<T>* expected, type_identity_t<T> desired, memory_order, memory_order) { \
        return __atomic_compare_exchange_n(&o->__v, expected, desired, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED); } \
    template<typename T> inline T atomic_fetch_add_explicit(volatile AS _atomic<T>* o, type_identity_t<T> v, memory_order) { \
        return __atomic_fetch_add(&o->__v, v, __ATOMIC_RELAXED); } \
    template<typename T> inline T atomic_fetch_sub_explicit(volatile AS _atomic<T>* o, type_identity_t<T> v, memory_order) { \
        return __atomic_fetch_sub(&o->__v, v, __ATOMIC_RELAXED); } \
    template<typename T> inline T atomic_fetch_and_explicit(volatile AS _atomic<T>* o, type_identity_t<T> v, memory_order) { \
        return __atomic_fetch_and(&o->__v, v, __ATOMIC_RELAXED); } \
    template<typename T> inline T atomic_fetch_or_explicit(volatile AS _atomic<T>* o, type_identity_t<T> v, memory_order) { \
        return __atomic_fetch_or(&o->__v, v, __ATOMIC_RELAXED); } \
    template<typename T> inline T atomic_fetch_xor_explicit(volatile AS _atomic<T>* o, type_identity_t<T> v, memory_order) { \
        return __atomic_fetch_xor(&o->__v, v, __ATOMIC_RELAXED); } \
    template<typename T> inline T atomic_fetch_min_explicit(volatile AS _atomic<T>* o, type_identity_t<T> v, memory_order) { \
        return __atomic_fetch_min(&o->__v, v, __ATOMIC_RELAXED); } \
    template<typename T> inline T atomic_fetch_max_explicit(volatile AS _atomic<T>* o, type_identity_t<T> v, memory_order) { \
        return __atomic_fetch_max(&o->__v, v, __ATOMIC_RELAXED); } \
    template<typename T> inline T atomic_max_explicit(volatile AS _atomic<T>* o, type_identity_t<T> v, memory_order) { \
        return __atomic_fetch_max(&o->__v, v, __ATOMIC_RELAXED); } \
    template<typename T> inline T atomic_min_explicit(volatile AS _atomic<T>* o, type_identity_t<T> v, memory_order) { \
        return __atomic_fetch_min(&o->__v, v, __ATOMIC_RELAXED); }
__MSL_ATOMICS(__global)
__MSL_ATOMICS(__local)
#undef __MSL_ATOMICS

// ---- barriers ----------------------------------------------------------------------
enum class mem_flags : uint { mem_none = 0, mem_device = 1, mem_threadgroup = 2, mem_texture = 4,
    mem_threadgroup_imageblock = 8, mem_object_data = 16 };
constexpr mem_flags operator|(mem_flags a, mem_flags b) { return mem_flags(uint(a) | uint(b)); }
constexpr mem_flags operator&(mem_flags a, mem_flags b) { return mem_flags(uint(a) & uint(b)); }
// Scope numbers are SPIR-V's (2 workgroup, 3 subgroup).
inline void threadgroup_barrier(mem_flags f) { __msl_barrier(2u, uint(f)); }
inline void simdgroup_barrier(mem_flags f) { __msl_barrier(3u, uint(f)); }
inline void atomic_thread_fence(mem_flags f, memory_order, thread_scope s = thread_scope::device) {
    __msl_barrier(s == thread_scope::threadgroup ? 0x102u : s == thread_scope::simdgroup ? 0x103u : 0x101u, uint(f)); }

// ---- SIMD-group (subgroup) functions --------------------------------------------------
// OpGroupNonUniform* opcode numbers.
enum : uint {
    __SPV_GNU_Elect = 333, __SPV_GNU_All = 334, __SPV_GNU_Any = 335, __SPV_GNU_AllEqual = 336,
    __SPV_GNU_Broadcast = 337, __SPV_GNU_BroadcastFirst = 338, __SPV_GNU_Ballot = 339,
    __SPV_GNU_Shuffle = 345, __SPV_GNU_ShuffleXor = 346, __SPV_GNU_ShuffleUp = 347, __SPV_GNU_ShuffleDown = 348,
    __SPV_GNU_IAdd = 349, __SPV_GNU_FAdd = 350, __SPV_GNU_IMul = 351, __SPV_GNU_FMul = 352,
    __SPV_GNU_SMin = 353, __SPV_GNU_UMin = 354, __SPV_GNU_FMin = 355, __SPV_GNU_SMax = 356,
    __SPV_GNU_UMax = 357, __SPV_GNU_FMax = 358, __SPV_GNU_BitwiseAnd = 359, __SPV_GNU_BitwiseOr = 360,
    __SPV_GNU_BitwiseXor = 361, __SPV_GNU_QuadBroadcast = 365, __SPV_GNU_QuadSwap = 366,
};
struct simd_vote {
    ulong __m;
    constexpr explicit simd_vote(ulong m = 0) : __m(m) {}
    constexpr bool all() const { return __m == ~0ul; }
    constexpr bool any() const { return __m != 0; }
    constexpr explicit operator ulong() const { return __m; }
    constexpr explicit operator bool() const { return __m != 0; }
};
template<typename T> inline T __gop(uint f, uint s, uint u, uint group, T x) {
    return T(__msl_group1<decltype(__raw(x))>(__pick<T>(f, s, u), group, __raw(x)));
}
#define __MSL_SIMD_ARITH(NAME, GROUP, F, S, U) \
    template<typename T> inline T NAME(T x) { return __gop(F, S, U, GROUP, x); }
__MSL_SIMD_ARITH(simd_sum, 0u, __SPV_GNU_FAdd, __SPV_GNU_IAdd, __SPV_GNU_IAdd)
__MSL_SIMD_ARITH(simd_product, 0u, __SPV_GNU_FMul, __SPV_GNU_IMul, __SPV_GNU_IMul)
__MSL_SIMD_ARITH(simd_min, 0u, __SPV_GNU_FMin, __SPV_GNU_SMin, __SPV_GNU_UMin)
__MSL_SIMD_ARITH(simd_max, 0u, __SPV_GNU_FMax, __SPV_GNU_SMax, __SPV_GNU_UMax)
__MSL_SIMD_ARITH(simd_and, 0u, __SPV_GNU_BitwiseAnd, __SPV_GNU_BitwiseAnd, __SPV_GNU_BitwiseAnd)
__MSL_SIMD_ARITH(simd_or, 0u, __SPV_GNU_BitwiseOr, __SPV_GNU_BitwiseOr, __SPV_GNU_BitwiseOr)
__MSL_SIMD_ARITH(simd_xor, 0u, __SPV_GNU_BitwiseXor, __SPV_GNU_BitwiseXor, __SPV_GNU_BitwiseXor)
__MSL_SIMD_ARITH(simd_prefix_inclusive_sum, 1u, __SPV_GNU_FAdd, __SPV_GNU_IAdd, __SPV_GNU_IAdd)
__MSL_SIMD_ARITH(simd_prefix_exclusive_sum, 2u, __SPV_GNU_FAdd, __SPV_GNU_IAdd, __SPV_GNU_IAdd)
__MSL_SIMD_ARITH(simd_prefix_inclusive_product, 1u, __SPV_GNU_FMul, __SPV_GNU_IMul, __SPV_GNU_IMul)
__MSL_SIMD_ARITH(simd_prefix_exclusive_product, 2u, __SPV_GNU_FMul, __SPV_GNU_IMul, __SPV_GNU_IMul)
#undef __MSL_SIMD_ARITH
#define __MSL_SIMD_LANE(NAME, OP) \
    template<typename T> inline T NAME(T x, ushort lane) { \
        return T(__msl_group2<decltype(__raw(x))>(OP, __raw(x), uint(lane))); }
__MSL_SIMD_LANE(simd_shuffle, __SPV_GNU_Shuffle)
__MSL_SIMD_LANE(simd_shuffle_xor, __SPV_GNU_ShuffleXor)
__MSL_SIMD_LANE(simd_shuffle_up, __SPV_GNU_ShuffleUp)
__MSL_SIMD_LANE(simd_shuffle_down, __SPV_GNU_ShuffleDown)
__MSL_SIMD_LANE(simd_broadcast, __SPV_GNU_Broadcast)
__MSL_SIMD_LANE(quad_broadcast, __SPV_GNU_QuadBroadcast)
#undef __MSL_SIMD_LANE
template<typename T> inline T simd_broadcast_first(T x) {
    return T(__msl_group1<decltype(__raw(x))>(__SPV_GNU_BroadcastFirst, 0u, __raw(x))); }
inline bool simd_all(bool x) { return __msl_group1<bool>(__SPV_GNU_All, 0u, x); }
inline bool simd_any(bool x) { return __msl_group1<bool>(__SPV_GNU_Any, 0u, x); }
inline bool simd_is_first() { return __msl_group0<bool>(__SPV_GNU_Elect); }
inline simd_vote simd_ballot(bool x) { return simd_vote(__msl_group1<ulong>(__SPV_GNU_Ballot, 0u, x)); }
inline simd_vote simd_active_threads_mask() { return simd_ballot(true); }
template<typename T> inline T quad_shuffle_xor(T x, ushort mask) {
    return T(__msl_group2<decltype(__raw(x))>(__SPV_GNU_QuadSwap, __raw(x), uint(mask))); }
template<typename T> inline T quad_shuffle(T x, ushort lane) { return quad_broadcast(x, lane); }

// ---- fragment functions ----------------------------------------------------------------
template<typename T, enable_if_t<!__is_packed<T>::value, int> = 0> inline T dfdx(T x) { return T(__msl_deriv(__SPV_DPdx, __raw(x))); }
template<typename T, enable_if_t<!__is_packed<T>::value, int> = 0> inline T dfdy(T x) { return T(__msl_deriv(__SPV_DPdy, __raw(x))); }
template<typename T, enable_if_t<!__is_packed<T>::value, int> = 0> inline T fwidth(T x) { return T(__msl_deriv(__SPV_Fwidth, __raw(x))); }
inline void discard_fragment() { __msl_discard(); }

} // namespace metal
