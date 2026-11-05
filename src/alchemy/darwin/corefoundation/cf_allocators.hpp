#pragma once

// Custom CFAllocators (CFAllocatorCreate, cf_allocators.cpp). An allocator
// object carries the CFAllocatorContext it was created with; its callbacks
// serve CFAllocatorAllocate, Reallocate, Deallocate and
// GetPreferredSizeForSize, and the bytes handed to a NoCopy creator with it as
// the deallocator. CF objects created with one keep CoreFoundation's own
// storage (CFGetAllocator reports the default allocator), so the creators
// accept it as they accept NULL.

#include "alchemy/object_store.hpp"

#include <cstdint>
#include <memory>

namespace alchemy {
namespace gui::support_kit { struct Support; }

namespace cf_allocators {

// The CFAllocatorContext (CFBase.h) as CFAllocatorCreate copied it; `info`
// is the retained info.
struct Context final : ObjectPayload {
    std::uint64_t info{}, retain{}, release{}, copy_description{};
    std::uint64_t allocate{}, reallocate{}, deallocate{}, preferred_size{};
};

// The context of a CFAllocatorCreate allocator; nullptr for anything else.
[[nodiscard]] inline std::shared_ptr<Context> find(const ObjectStore& objects, std::uint64_t allocator) {
    if (!allocator) return nullptr;
    const auto record = objects.find(allocator);
    return record ? std::dynamic_pointer_cast<Context>(record->payload) : nullptr;
}
// An allocator a creator accepts for an object's storage: the default (NULL)
// or a CFAllocatorCreate allocator.
[[nodiscard]] inline bool accepted(const ObjectStore& objects, std::uint64_t allocator) {
    return !allocator || find(objects, allocator) != nullptr;
}

// CFAllocatorAllocate / CFAllocatorDeallocate with a CFAllocatorCreate
// allocator: its allocate (NULL without one) and deallocate callbacks, guest
// code called from inside those functions. Any other allocator stops with
// system_c.cpp's own unsupported() text (libSystem's system_c.cpp handles the
// standard ones first and calls these for the rest).
[[nodiscard]] std::uint64_t allocate(gui::support_kit::Support& support, std::uint64_t allocator, std::uint64_t size, std::uint64_t hint);
void deallocate(gui::support_kit::Support& support, std::uint64_t allocator, std::uint64_t pointer);

// Installs CFAllocatorCreate, GetContext, Reallocate, GetPreferredSizeForSize,
// GetTypeID and GetDefault.
void install(gui::support_kit::Support& support);

} // namespace cf_allocators
} // namespace alchemy
