#pragma once

// Host area "guard": host memory accesses that may fault and must not crash
// (docs/FX_HOST_SEAMS.md, "guarded access"): reads of guest memory that may
// be unmapped, and host code (the interpreter, providers) touching guest
// memory on the guest's behalf. Structured exception handling (__try) on
// win32; a per-thread recovery point the fault handler longjmps to on Linux.
//
// The memory-fault filter catches access violations, in-page errors and
// guard-page faults and lets every other exception continue; the "any"
// variants catch every host exception, as safe_read32 always did.

#include "fx/host/fault.hpp"

#include <cstddef>
#include <cstdint>

namespace transmute::fx::host {

// 4 or 8 bytes at `address` (any alignment); false (value unchanged) on any
// host exception.
bool try_read32(std::uint64_t address, std::uint32_t& value) noexcept;
bool try_read64(std::uint64_t address, std::uint64_t& value) noexcept;
// memcpy under the "any" filter; false if it faulted (`destination` may be
// partly written).
bool try_copy(void* destination, const void* source, std::size_t size) noexcept;

// memmove / memset under the memory-fault filter; false if they faulted.
bool guarded_move(void* destination, const void* source, std::size_t size) noexcept;
bool guarded_fill(void* destination, int value, std::size_t size) noexcept;
// Elements (`width` 1 or 2 bytes) before the first zero, up to `maximum`; -1
// on a memory fault.
long long guarded_length(const std::uint8_t* address, unsigned width, std::size_t maximum) noexcept;

// inline (backend):
//   template <typename F> bool call_guarded(F&& f, FaultRecord& fault) noexcept
//     runs f() (which must not need C++ unwinding across a fault); false with
//     `fault` filled if any host exception left it.
//   template <typename F> bool call_guarded_memory(F&& f) noexcept
//     the same under the memory-fault filter, without the record.
//   template <typename F> bool call_guarded_access(F&& f) noexcept
//     the same for access violations and in-page errors only (a guard-page
//     fault continues to the next handler).

} // namespace transmute::fx::host

#if defined(TRANSMUTE_FX_HOST_WIN32)
#include "fx/host/win32/guard.hpp"
#endif
