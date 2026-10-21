// Force-included when compiling the MSVC STL/vcruntime sources for ARM64 with
// clang-cl: MSVC ARM64 intrinsics that clang does not provide.
#pragma once

#ifdef __cplusplus
extern "C++" {
#endif

static __inline unsigned __int8 __load_acquire8(const volatile unsigned __int8* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static __inline unsigned __int16 __load_acquire16(const volatile unsigned __int16* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static __inline unsigned __int32 __load_acquire32(const volatile unsigned __int32* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static __inline unsigned __int64 __load_acquire64(const volatile unsigned __int64* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

#ifdef __cplusplus
}
#endif
