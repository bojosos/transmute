#include "alchemy/darwin/corefoundation/cf_allocators.hpp"

#include "alchemy/darwin/kit/completion/runtime/common.hpp"
#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/gui/fill_provider.hpp"

#include <array>

// Custom allocators (CFBase.h): CFAllocatorCreate copies a
// CFAllocatorContext { version, info, retain, release, copyDescription,
// allocate, reallocate, deallocate, preferredSize } into an allocator object
// (retaining info, released with the allocator). The allocator functions call
// its callbacks; CF objects created with it keep CoreFoundation's storage.
namespace alchemy::cf_allocators {
namespace {

using namespace gui::completion_kit::rt;
using gui::completion_kit::Support;
namespace completion_kit = gui::completion_kit;
namespace fill = gui::fill;
namespace security = gui::security;
using Address = std::uint64_t;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::uint64_t context_size = 72;

// kCFAllocatorNull's value (0 when not exported).
Address null_allocator(Support& support) {
    const auto found = support.process.resolve(core_foundation, "_kCFAllocatorNull");
    return found ? support.memory().read64(found->address) : 0;
}

} // namespace

std::uint64_t allocate(Support& support, std::uint64_t allocator, std::uint64_t size, std::uint64_t hint) {
    const auto context = find(support.process.objects(), allocator);
    if (!context) completion_kit::unsupported("CFAllocatorAllocate with a custom allocator");
    if (!context->allocate || !size) return 0;
    return call_address(support, context->allocate, {size, hint, context->info});
}

void deallocate(Support& support, std::uint64_t allocator, std::uint64_t pointer) {
    const auto context = find(support.process.objects(), allocator);
    if (!context) completion_kit::unsupported("CFAllocatorDeallocate with a custom allocator");
    if (context->deallocate && pointer) static_cast<void>(call_address(support, context->deallocate, {pointer, context->info}));
}

void install(Support& support) {
    const auto cls = support.declare_class("__NSCFAllocator", "NSObject");
    if (const auto bridge = security::bridge())
        bridge->register_type_id(cls, static_cast<security::TypeId>(fill::TypeId::allocator));
    support.function(core_foundation, "_CFAllocatorGetTypeID", [](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(fill::TypeId::allocator);
    });
    // CFAllocatorRef CFAllocatorCreate(CFAllocatorRef allocator, CFAllocatorContext *context): the
    // allocator object itself lives in CF's storage, whatever `allocator` says.
    support.function(core_foundation, "_CFAllocatorCreate", [&support, cls](CpuState& s, GuestMemory& m) {
        if (!s.x[1] || m.read64(s.x[1]) != 0) { s.x[0] = 0; return; }   // no context, or a version other than 0
        auto context = std::make_shared<Context>();
        const auto at = [&](unsigned index) { return m.read64(s.x[1] + 8 * index); };
        context->info = at(1);
        context->retain = at(2);
        context->release = at(3);
        context->copy_description = at(4);
        context->allocate = at(5);
        context->reallocate = at(6);
        context->deallocate = at(7);
        context->preferred_size = at(8);
        if (context->retain && context->info) context->info = call_address(support, context->retain, {context->info});
        s.x[0] = support.create(cls, context, [&support, context](Address) {
            if (context->release && context->info) static_cast<void>(call_address(support, context->release, {context->info}));
        });
    });
    // void CFAllocatorGetContext(CFAllocatorRef, CFAllocatorContext *): the standard allocators
    // report a context without callbacks.
    support.function(core_foundation, "_CFAllocatorGetContext", [&support](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) return;
        std::array<std::uint64_t, context_size / 8> fields{};
        if (const auto context = find(support.process.objects(), s.x[0]))
            fields = {0, context->info, context->retain, context->release, context->copy_description,
                      context->allocate, context->reallocate, context->deallocate, context->preferred_size};
        for (std::size_t index = 0; index < fields.size(); ++index) m.write64(s.x[1] + 8 * index, fields[index]);
    });
    // CFIndex CFAllocatorGetPreferredSizeForSize(CFAllocatorRef, CFIndex size, CFOptionFlags hint)
    support.function(core_foundation, "_CFAllocatorGetPreferredSizeForSize", [&support](CpuState& s, GuestMemory&) {
        const auto context = find(support.process.objects(), s.x[0]);
        if (context && context->preferred_size) s.x[0] = call_address(support, context->preferred_size, {s.x[1], s.x[2], context->info});
        else s.x[0] = s.x[1];
    });
    // void *CFAllocatorReallocate(CFAllocatorRef, void *ptr, CFIndex newsize, CFOptionFlags hint):
    // NULL ptr allocates, newsize 0 deallocates (NULL), otherwise the reallocate callback (NULL
    // without one); the default allocator is malloc's, kCFAllocatorNull allocates nothing.
    support.function(core_foundation, "_CFAllocatorReallocate", [&support](CpuState& s, GuestMemory&) {
        const auto allocator = s.x[0], pointer = s.x[1], size = s.x[2], hint = s.x[3];
        if (allocator && allocator == null_allocator(support)) { s.x[0] = 0; return; }
        const auto context = find(support.process.objects(), allocator);
        if (allocator && !context) completion_kit::unsupported("CFAllocatorReallocate with an allocator CFAllocatorCreate did not make");
        if (!pointer) {
            s.x[0] = !size ? 0 : context ? allocate(support, allocator, size, hint) : support.call(libsystem, "_malloc", {size});
        } else if (!size) {
            if (context) deallocate(support, allocator, pointer);
            else static_cast<void>(support.call(libsystem, "_free", {pointer}));
            s.x[0] = 0;
        } else if (context) {
            s.x[0] = context->reallocate ? call_address(support, context->reallocate, {pointer, size, hint, context->info}) : 0;
        } else {
            s.x[0] = support.call(libsystem, "_realloc", {pointer, size});
        }
    });
    // kCFAllocatorDefault is the system default (NULL here); CFAllocatorSetDefault is not provided.
    support.function(core_foundation, "_CFAllocatorGetDefault", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
}

} // namespace alchemy::cf_allocators
