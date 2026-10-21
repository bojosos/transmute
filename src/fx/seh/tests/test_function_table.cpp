// Function-table lookup: .pdata binary search over modules, plus dynamic
// tables and callbacks for guest-generated code.

#include "fx/seh/seh.hpp"
#include "fx/seh/tests/seh_test_util.hpp"
#include "fx/testing/test.hpp"

#include <cstring>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::seh;
namespace t = fxseh_test;

namespace {

// Builds a module with N functions each 0x40 bytes long, packed .pdata
// (Flag 2, no prolog/epilog) so function_length_bytes works and the table is
// sorted by BeginAddress.
struct Module {
    std::uint64_t base;
    std::uint64_t pdata;
    std::uint32_t count;
};

Module build_module(unsigned functions, std::uint32_t func_size = 0x40) {
    const std::size_t span = 0x10000;
    const std::uint64_t base = t::alloc_guest(span, 0x10000);
    const std::uint64_t code = base + 0x1000;
    const std::uint64_t pdata = base + 0x8000;
    for (unsigned i = 0; i < functions; ++i) {
        std::uint32_t entry[2];
        entry[0] = static_cast<std::uint32_t>((code + i * func_size) - base);  // BeginAddress
        // packed: Flag=2 (no prolog/epilog), FunctionLength = func_size/4.
        entry[1] = 2u | ((func_size / 4) << 2);
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(pdata + i * 8)), entry, 8);
    }
    GuestModule module{};
    module.base = base;
    module.size = span;
    module.pdata_rva = static_cast<std::uint32_t>(pdata - base);
    module.pdata_size = functions * 8;
    module.text_begin = code;
    module.text_end = code + functions * func_size;
    module.name = L"test.dll";
    t::register_module(module);
    Module m{base, pdata, functions};
    return m;
}

} // namespace

FX_TEST(lookup_in_module_pdata) {
    t::reset_all();
    Module m = build_module(16);
    const std::uint64_t code = m.base + 0x1000;
    for (unsigned i = 0; i < m.count; ++i) {
        std::uint64_t image_base = 0;
        const std::uint64_t pc = code + i * 0x40 + 0x10;  // inside function i
        const std::uint64_t entry = lookup_function_entry(pc, &image_base);
        FX_CHECK_EQ(entry, m.pdata + i * 8);
        FX_CHECK_EQ(image_base, m.base);
    }
    // Address past the last function: no entry.
    std::uint64_t image_base = 0;
    FX_CHECK_EQ(lookup_function_entry(code + m.count * 0x40 + 8, &image_base), 0u);
}

FX_TEST(lookup_binary_search_boundaries) {
    t::reset_all();
    Module m = build_module(64);
    const std::uint64_t code = m.base + 0x1000;
    std::uint64_t image_base = 0;
    // First byte of first function.
    FX_CHECK_EQ(lookup_function_entry(code, &image_base), m.pdata);
    // Last byte of last function.
    const std::uint64_t last = code + 63 * 0x40 + 0x3f;
    FX_CHECK_EQ(lookup_function_entry(last, &image_base), m.pdata + 63 * 8);
    // Below the module: nothing.
    FX_CHECK_EQ(lookup_function_entry(m.base, &image_base), 0u);
}

FX_TEST(dynamic_add_and_delete_function_table) {
    t::reset_all();
    // Code range outside any module.
    const std::uint64_t base = t::alloc_guest(0x10000, 0x10000);
    const std::uint64_t code = base + 0x2000;
    const std::uint64_t table = base + 0x8000;
    const unsigned count = 8;
    for (unsigned i = 0; i < count; ++i) {
        std::uint32_t entry[2];
        entry[0] = static_cast<std::uint32_t>((code + i * 0x40) - base);
        entry[1] = 2u | ((0x40u / 4) << 2);  // packed, 0x40 bytes
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(table + i * 8)), entry, 8);
    }
    FX_CHECK(add_function_table(table, count, base));
    std::uint64_t image_base = 0;
    const std::uint64_t pc = code + 3 * 0x40 + 4;
    FX_CHECK_EQ(lookup_function_entry(pc, &image_base), table + 3 * 8);
    FX_CHECK_EQ(image_base, base);

    FX_CHECK(delete_function_table(table));
    FX_CHECK_EQ(lookup_function_entry(pc, &image_base), 0u);
    FX_CHECK(!delete_function_table(table));  // already gone
}

FX_TEST(install_function_table_callback_lookup) {
    t::reset_all();
    const std::uint64_t base = t::alloc_guest(0x10000, 0x10000);
    const std::uint64_t code = base + 0x2000;
    // A single entry the callback returns for any PC in range.
    const std::uint64_t entry_storage = t::alloc_guest(16, 8);
    std::uint32_t entry[2] = {static_cast<std::uint32_t>(code - base), 2u | ((0x100u / 4) << 2)};
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(entry_storage)), entry, 8);

    const std::uint64_t identifier = base | 3;  // low bits must be 11b
    const std::uint64_t callback = 0x9000'0000;
    bool called = false;
    t::register_host_entry(callback, [&](GuestContext& ctx) {
        called = true;
        ctx.x[0] = entry_storage;  // return the RUNTIME_FUNCTION address
        return GuestResolution::continue_at_caller;
    });
    FX_CHECK(install_function_table_callback(identifier, base, 0x4000, callback, 0x1234));

    std::uint64_t image_base = 0;
    const std::uint64_t result = lookup_function_entry(code + 0x20, &image_base);
    FX_CHECK(called);
    FX_CHECK_EQ(result, entry_storage);
    FX_CHECK_EQ(image_base, base);
}

FX_TEST(growable_function_table) {
    t::reset_all();
    const std::uint64_t base = t::alloc_guest(0x10000, 0x10000);
    const std::uint64_t code = base + 0x2000;
    const std::uint64_t table = base + 0x8000;
    auto write_entry = [&](unsigned i) {
        std::uint32_t entry[2] = {static_cast<std::uint32_t>((code + i * 0x40) - base), 2u | ((0x40u / 4) << 2)};
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(table + i * 8)), entry, 8);
    };
    for (unsigned i = 0; i < 8; ++i) write_entry(i);

    std::uint64_t handle = 0;
    // RVAs in the table are relative to the image base (alloc base), so the
    // growable table's range base must be that base, not the code pointer.
    FX_CHECK_EQ(add_growable_function_table(&handle, table, 2, 8, base, code + 8 * 0x40), 0u);
    std::uint64_t image_base = 0;
    // Entry 5 is not yet live (count = 2).
    FX_CHECK_EQ(lookup_function_entry(code + 5 * 0x40, &image_base), 0u);
    grow_function_table(handle, 8);
    FX_CHECK_EQ(lookup_function_entry(code + 5 * 0x40, &image_base), table + 5 * 8);
    delete_growable_function_table(handle);
    FX_CHECK_EQ(lookup_function_entry(code + 5 * 0x40, &image_base), 0u);
}

FX_TEST(return_to_host_sentinel_has_no_entry) {
    t::reset_all();
    build_module(4);
    std::uint64_t image_base = 0;
    FX_CHECK_EQ(lookup_function_entry(kReturnToHostPc, &image_base), 0u);
    FX_CHECK_EQ(lookup_function_entry(0, &image_base), 0u);
}
