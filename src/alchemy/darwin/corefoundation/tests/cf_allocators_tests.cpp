// Custom CFAllocators (cf_allocators.cpp): CFAllocatorCreate's context
// callbacks serve the allocator functions and NoCopy deallocators, and CF
// objects created with the allocator keep CoreFoundation's storage.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr Address info = 0x5eed;
constexpr Address utf8 = 0x08000100;

alchemy::ProcessContext* process_of_test{};
int retains{}, releases{}, allocations{}, reallocations{}, deallocations{};
Address last_freed{}, info_seen{};

const Case corefoundation_allocators_case("corefoundation_allocators", [](Fixture& f) {
    using transmute::CpuState;
    using transmute::GuestMemory;
    process_of_test = &f.process;
    retains = releases = allocations = reallocations = deallocations = 0;
    last_freed = info_seen = 0;
    // The context's callbacks (host functions at guest addresses).
    const auto retain = f.process.install_function("_cf_allocators_test_retain", [](CpuState&, GuestMemory&) { ++retains; });   // returns info (x0)
    const auto release = f.process.install_function("_cf_allocators_test_release", [](CpuState&, GuestMemory&) { ++releases; });
    const auto allocate = f.process.install_function("_cf_allocators_test_allocate", [](CpuState& s, GuestMemory&) {
        ++allocations;
        info_seen = s.x[2];
        s.x[0] = process_of_test->allocate_guest(s.x[0], 16);
    });
    const auto reallocate = f.process.install_function("_cf_allocators_test_reallocate", [](CpuState& s, GuestMemory&) {
        ++reallocations;
        info_seen = s.x[3];
        s.x[0] = process_of_test->allocate_guest(s.x[1], 16);
    });
    const auto deallocate = f.process.install_function("_cf_allocators_test_deallocate", [](CpuState& s, GuestMemory&) {
        ++deallocations;
        last_freed = s.x[0];
        info_seen = s.x[1];
    });
    const auto preferred = f.process.install_function("_cf_allocators_test_preferred", [](CpuState& s, GuestMemory&) { s.x[0] *= 2; });
    // { version, info, retain, release, copyDescription, allocate, reallocate, deallocate, preferredSize }
    const auto context = f.block(72);
    const Address fields[] = {0, info, retain, release, 0, allocate, reallocate, deallocate, preferred};
    for (std::size_t index = 0; index < std::size(fields); ++index) f.engine.write64(context + 8 * index, fields[index]);

    const auto allocator = f.call(cf, "_CFAllocatorCreate", {0, context});
    require(allocator && retains == 1, "CFAllocatorCreate retains the info");
    require(f.call(cf, "_CFGetTypeID", {allocator}) == f.call(cf, "_CFAllocatorGetTypeID"), "CFGetTypeID of an allocator");
    const auto copy = f.block(72, 0xff);
    f.call(cf, "_CFAllocatorGetContext", {allocator, copy});
    require(f.engine.read64(copy) == 0 && f.engine.read64(copy + 8) == info && f.engine.read64(copy + 40) == allocate &&
            f.engine.read64(copy + 56) == deallocate && f.engine.read64(copy + 64) == preferred, "CFAllocatorGetContext");
    require(f.call(cf, "_CFAllocatorGetPreferredSizeForSize", {allocator, 24, 0}) == 48, "CFAllocatorGetPreferredSizeForSize");

    // The allocator functions call the callbacks with the info.
    const auto block = f.call(cf, "_CFAllocatorAllocate", {allocator, 32, 0});
    require(block && allocations == 1 && info_seen == info, "CFAllocatorAllocate");
    const auto grown = f.call(cf, "_CFAllocatorReallocate", {allocator, block, 64, 0});
    require(grown && reallocations == 1 && info_seen == info, "CFAllocatorReallocate");
    f.call(cf, "_CFAllocatorDeallocate", {allocator, grown});
    require(deallocations == 1 && last_freed == grown && info_seen == info, "CFAllocatorDeallocate");
    require(f.call(cf, "_CFAllocatorReallocate", {allocator, block, 0, 0}) == 0 && deallocations == 2 && last_freed == block,
            "CFAllocatorReallocate to no size deallocates");
    const auto fresh = f.call(cf, "_CFAllocatorReallocate", {allocator, 0, 8, 0});
    require(fresh && allocations == 2, "CFAllocatorReallocate of NULL allocates");

    // NoCopy creators hand their bytes to the deallocator's deallocate callback.
    const auto bytes = f.call(cf, "_CFAllocatorAllocate", {allocator, 5, 0});
    f.engine.write_bytes(bytes, std::vector<std::uint8_t>{'h', 'e', 'l', 'l', 'o'});
    const auto data = f.call(cf, "_CFDataCreateWithBytesNoCopy", {0, bytes, 5, allocator});
    require(data && f.call(cf, "_CFDataGetLength", {data}) == 5 && deallocations == 3 && last_freed == bytes,
            "CFDataCreateWithBytesNoCopy gives the bytes to the deallocator");
    const auto characters = f.call(cf, "_CFAllocatorAllocate", {allocator, 3, 0});
    f.engine.write_bytes(characters, std::vector<std::uint8_t>{'a', 'b', 'c'});
    const auto string = f.call(cf, "_CFStringCreateWithBytesNoCopy", {0, characters, 3, utf8, 0, allocator});
    require(f.cf.utf8(string) == "abc" && deallocations == 4 && last_freed == characters,
            "CFStringCreateWithBytesNoCopy gives the bytes to the deallocator");

    // Objects created with the allocator keep CoreFoundation's storage.
    const auto made = f.call(cf, "_CFStringCreateWithCString", {allocator, f.text("allocated"), utf8});
    const auto array = f.call(cf, "_CFArrayCreateMutable", {allocator, 0, 0});
    const auto mutable_string = f.call(cf, "_CFStringCreateMutable", {allocator, 0});
    require(f.cf.utf8(made) == "allocated" && array && mutable_string && allocations == 4, "creators accept the allocator");
    for (const auto object : {data, string, made, array, mutable_string}) f.call(cf, "_CFRelease", {object});
    f.call(cf, "_CFRelease", {allocator});
    require(releases == 1, "the last release of the allocator releases its info");

    // The default allocator: CFAllocatorReallocate is malloc's.
    const auto plain = f.call(cf, "_CFAllocatorReallocate", {0, 0, 16, 0});
    const auto bigger = plain ? f.call(cf, "_CFAllocatorReallocate", {0, plain, 64, 0}) : 0;
    require(plain && bigger && f.call(cf, "_CFAllocatorReallocate", {0, bigger, 0, 0}) == 0, "the default allocator reallocates");
    require(f.call(cf, "_CFAllocatorGetDefault") == 0 && f.call(cf, "_CFAllocatorGetPreferredSizeForSize", {0, 24, 0}) == 24,
            "the default allocator");
    // CFAllocatorCreate needs a context of version 0.
    f.engine.write64(context, 1);
    require(f.call(cf, "_CFAllocatorCreate", {0, context}) == 0 && f.call(cf, "_CFAllocatorCreate", {0, 0}) == 0 && retains == 1,
            "CFAllocatorCreate without a version 0 context");
});

} // namespace
