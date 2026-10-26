// Shared guest memory (map_shared_guest_memory / alias_guest_memory /
// unmap_shared_guest_memory): aliasing, backing kept by an alias, overwrite
// of whole views, placeholder reuse, and code invalidation through an alias;
// file section views, shared and copy-on-write.

#include "fx/host/vm.hpp"
#include "fx/runtime/tests/test_support.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>

#include <stdexcept>

using namespace rtt;

namespace {

constexpr std::uint32_t kRetWord = 0xD65F03C0;
std::uint32_t movz_x0(std::uint32_t imm) { return 0xD2800000u | (imm << 5); }

// A free, 64 KiB aligned address range (reserved and released at once).
std::uint64_t free_range(std::uint64_t size) {
    void* p = VirtualAlloc(nullptr, static_cast<SIZE_T>(size), MEM_RESERVE, PAGE_NOACCESS);
    VirtualFree(p, 0, MEM_RELEASE);
    return reinterpret_cast<std::uint64_t>(p);
}

volatile std::uint32_t& word(std::uint64_t address) { return *reinterpret_cast<volatile std::uint32_t*>(address); }

bool throws(void (*fn)(std::uint64_t, std::uint64_t), std::uint64_t a, std::uint64_t b) {
    try {
        fn(a, b);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

} // namespace

FX_TEST(shared_memory_alias_sees_the_same_pages) {
    rt::ensure_initialized();
    const std::uint64_t base = free_range(0x40000);
    const std::uint64_t a = base + 0x4000, b = base + 0x20000;   // 16 KiB pages inside 64 KiB granules
    map_shared_guest_memory(a, 0x8000, 0x4000, PAGE_READWRITE);
    FX_CHECK(is_shared_guest_memory(a) && is_shared_guest_memory(a + 0x7fff));
    FX_CHECK(!is_shared_guest_memory(a + 0x8000));
    FX_CHECK_EQ(word(a), 0u);   // zero-filled
    word(a + 0x10) = 0x1234;
    alias_guest_memory(b, a, 0x8000, PAGE_READWRITE);
    FX_CHECK_EQ(word(b + 0x10), 0x1234u);
    word(b + 0x4020) = 0x5678;
    FX_CHECK_EQ(word(a + 0x4020), 0x5678u);
    // A partial alias of the second 16 KiB at another place.
    const std::uint64_t c = base + 0x30000;
    alias_guest_memory(c, a + 0x4000, 0x4000, PAGE_READONLY);
    FX_CHECK_EQ(word(c + 0x20), 0x5678u);
    // The original goes away; the aliases keep the pages.
    unmap_shared_guest_memory(a, 0x8000);
    FX_CHECK(!is_shared_guest_memory(a));
    FX_CHECK_EQ(word(b + 0x10), 0x1234u);
    FX_CHECK_EQ(word(c + 0x20), 0x5678u);
    // Unmapped space is fx placeholder space again: map there once more.
    map_shared_guest_memory(a, 0x4000, 0, PAGE_READWRITE);
    FX_CHECK_EQ(word(a + 0x10), 0u);
    // Overwrite: an alias replaces whole views at the destination.
    alias_guest_memory(a, b, 0x4000, PAGE_READWRITE);
    FX_CHECK_EQ(word(a + 0x10), 0x1234u);
    // Partial views cannot be unmapped; overlapping aliases are refused.
    FX_CHECK(throws([](std::uint64_t x, std::uint64_t) { unmap_shared_guest_memory(x + 0x1000, 0x1000); }, b, 0));
    FX_CHECK(throws([](std::uint64_t x, std::uint64_t) { alias_guest_memory(x + 0x1000, x, 0x4000, PAGE_READWRITE); }, b, 0));
    unmap_shared_guest_memory(a, 0x4000);
    unmap_shared_guest_memory(b, 0x8000);
    unmap_shared_guest_memory(c, 0x4000);
    // Everything is released: the address space is free again.
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(reinterpret_cast<void*>(base + 0x20000), &info, sizeof(info));
    FX_CHECK_EQ(info.State, static_cast<DWORD>(MEM_FREE));
}

FX_TEST(shared_memory_alias_writes_invalidate_translations) {
    rt::ensure_initialized();
    set_mini_translator(true);
    const std::uint64_t base = free_range(0x40000);
    const std::uint64_t code = base, data = base + 0x20000;
    map_shared_guest_memory(data, 0x4000, 0, PAGE_READWRITE);   // the writable view
    word(data) = movz_x0(1);
    word(data + 4) = kRetWord;
    alias_guest_memory(code, data, 0x4000, PAGE_READONLY);       // the code view (RX for the guest)
    register_guest_code_memory(code, 0x4000);
    FX_CHECK_EQ(call_guest(code, {}), 1u);
    const auto before = runtime_stats();
    FX_CHECK_EQ(call_guest(code, {}), 1u);
    FX_CHECK_EQ(runtime_stats().translations, before.translations);   // translated once
    // The code view is read-only already; the writable alias was protected
    // with it, so this store faults, invalidates the code view's translation
    // and retries.
    word(data) = movz_x0(2);
    const auto after = runtime_stats();
    FX_CHECK_EQ(after.smc_write_faults, before.smc_write_faults + 1);
    FX_CHECK(after.invalidated_regions > before.invalidated_regions);
    FX_CHECK_EQ(word(code), movz_x0(2));
    FX_CHECK_EQ(call_guest(code, {}), 2u);
    unregister_guest_code_memory(code, 0x4000);
    unmap_shared_guest_memory(code, 0x4000);
    unmap_shared_guest_memory(data, 0x4000);
    set_mini_translator(false);
}

// map_section_guest_memory: a file section viewed at a guest address (Darwin
// mmap(MAP_SHARED)): two views of one file see each other's stores, a view
// may end inside its last page (the file's end), the pages past it stay
// inaccessible placeholder space, and unmapping releases everything.
FX_TEST(shared_memory_file_section_views) {
    rt::ensure_initialized();
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    GetTempPathW(MAX_PATH, directory);
    GetTempFileNameW(directory, L"fxs", 0, path);
    const HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    FX_CHECK(file != INVALID_HANDLE_VALUE);
    // 0x2008 bytes: two whole pages and 8 bytes of a third.
    FILE_END_OF_FILE_INFO end{};
    end.EndOfFile.QuadPart = 0x2008;
    FX_CHECK(SetFileInformationByHandle(file, FileEndOfFileInfo, &end, sizeof(end)) != 0);
    const HANDLE section = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    FX_CHECK(section != nullptr);
    // A host-chosen view of the whole file in a 16 KiB region, and a second
    // view from offset 0x1000 at a chosen address.
    const std::uint64_t a = map_section_guest_memory(0, 0x4000, section, 0, 0x2008, true, PAGE_READWRITE);
    FX_CHECK(a != 0 && (a & 0xffff) == 0);
    FX_CHECK(is_shared_guest_memory(a) && is_shared_guest_memory(a + 0x2fff));
    const std::uint64_t place = free_range(0x10000);
    const std::uint64_t b = map_section_guest_memory(place, 0x2000, section, 0x1000, 0x1008, true, PAGE_READONLY);
    FX_CHECK_EQ(b, place);
    CloseHandle(section);   // the views keep it
    word(a + 0x1010) = 0xabcd;
    FX_CHECK_EQ(word(b + 0x10), 0xabcdu);
    word(a + 0x2004) = 0x77;   // inside the last, partial page
    FX_CHECK_EQ(word(b + 0x1004), 0x77u);
    // The store reached the file.
    std::uint32_t value = 0;
    DWORD read = 0;
    LARGE_INTEGER at{};
    at.QuadPart = 0x1010;
    SetFilePointerEx(file, at, nullptr, FILE_BEGIN);
    FX_CHECK(ReadFile(file, &value, sizeof(value), &read, nullptr) && read == 4 && value == 0xabcd);
    // Past the file's last page: an fx placeholder, not memory.
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(reinterpret_cast<void*>(a + 0x3000), &info, sizeof(info));
    FX_CHECK(info.State == MEM_RESERVE);
    // A read-only section refuses a writable view.
    const HANDLE readonly = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    bool refused = false;
    try {
        map_section_guest_memory(0, 0x4000, readonly, 0, 0x2008, false, PAGE_READWRITE);
    } catch (const std::exception&) {
        refused = true;
    }
    FX_CHECK(refused);
    CloseHandle(readonly);
    unmap_shared_guest_memory(b, 0x2000);
    unmap_shared_guest_memory(a, 0x4000);
    VirtualQuery(reinterpret_cast<void*>(a), &info, sizeof(info));
    FX_CHECK_EQ(info.State, static_cast<DWORD>(MEM_FREE));
    VirtualQuery(reinterpret_cast<void*>(place), &info, sizeof(info));
    FX_CHECK_EQ(info.State, static_cast<DWORD>(MEM_FREE));
    CloseHandle(file);
}

// map_file_copy_guest_memory: a read-only file viewed copy-on-write (Mach-O
// segments): file bytes at a 4 KiB (not 64 KiB) aligned offset, the rest of
// the last file page and the pages after it zero, writes private to this view
// (the file and a second view keep the file's bytes), writable protection
// accepted again after a read-only one (host vm::protect maps it to
// write-copy), and none of the shared-memory operations that would expose the
// file's pages instead of the written ones.
FX_TEST(shared_memory_file_copy_views) {
    rt::ensure_initialized();
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    GetTempPathW(MAX_PATH, directory);
    GetTempFileNameW(directory, L"fxc", 0, path);
    const HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    FX_CHECK(file != INVALID_HANDLE_VALUE);
    // 0x6000 bytes: word i of the file holds 0x10000000 + i.
    std::vector<std::uint32_t> contents(0x6000 / 4);
    for (std::size_t i = 0; i < contents.size(); ++i) contents[i] = 0x10000000u + static_cast<std::uint32_t>(i);
    DWORD written = 0;
    FX_CHECK(WriteFile(file, contents.data(), 0x6000, &written, nullptr) && written == 0x6000);
    const HANDLE section = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    FX_CHECK(section != nullptr);
    // 0x1808 file bytes from offset 0x3000 at base + 0x4000, in a 0x4000 region:
    // file page 0x3000, 0x808 bytes of page 0x4000 (its rest zero), two zero pages.
    const std::uint64_t base = free_range(0x20000);
    const std::uint64_t a = base + 0x4000;
    map_file_copy_guest_memory(a, 0x4000, section, 0x3000, 0x1808);
    FX_CHECK_EQ(word(a), 0x10000000u + 0x3000 / 4);
    FX_CHECK_EQ(word(a + 0x1804), 0x10000000u + 0x4804 / 4);
    FX_CHECK_EQ(word(a + 0x1808), 0u);   // past the file bytes, inside their last page
    FX_CHECK_EQ(word(a + 0x1ffc), 0u);
    FX_CHECK_EQ(word(a + 0x2000), 0u);   // zero pages
    FX_CHECK_EQ(word(a + 0x3ffc), 0u);
    // A second view of the same file bytes elsewhere.
    const std::uint64_t b = base + 0x10000;
    map_file_copy_guest_memory(b, 0x2000, section, 0x3000, 0x2000);
    CloseHandle(section);   // the views keep it
    // Writes stay private: the other view and the file keep the file's bytes.
    word(a + 0x10) = 0xabcd;
    word(a + 0x2010) = 0x1234;
    FX_CHECK_EQ(word(a + 0x10), 0xabcdu);
    FX_CHECK_EQ(word(b + 0x10), 0x10000000u + 0x3010 / 4);
    std::uint32_t value = 0;
    DWORD read = 0;
    LARGE_INTEGER at{};
    at.QuadPart = 0x3010;
    SetFilePointerEx(file, at, nullptr, FILE_BEGIN);
    FX_CHECK(ReadFile(file, &value, sizeof(value), &read, nullptr) && read == 4 && value == 0x10000000u + 0x3010 / 4);
    // Read-only, then writable again (a written page and an unwritten one).
    FX_CHECK(host::vm::protect(reinterpret_cast<void*>(a), 0x2000, PAGE_READONLY));
    FX_CHECK(host::vm::protect(reinterpret_cast<void*>(a), 0x2000, PAGE_READWRITE));
    word(a + 0x1000) = 0x5678;
    FX_CHECK_EQ(word(a + 0x1000), 0x5678u);
    FX_CHECK_EQ(word(a + 0x10), 0xabcdu);
    // Not shared memory for aliases, duplicates and partial unmaps.
    FX_CHECK(throws([](std::uint64_t x, std::uint64_t y) { alias_guest_memory(y, x, 0x2000, PAGE_READWRITE); }, a, base + 0x18000));
    FX_CHECK(throws([](std::uint64_t x, std::uint64_t) { unmap_shared_guest_range(x, 0x1000); }, a, 0));
    FX_CHECK(duplicate_shared_guest_section(a, nullptr, nullptr, nullptr) == nullptr);
    FX_CHECK_EQ(word(a + 0x10), 0xabcdu);
    unmap_shared_guest_memory(a, 0x4000);
    unmap_shared_guest_memory(b, 0x2000);
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(reinterpret_cast<void*>(base + 0x4000), &info, sizeof(info));
    FX_CHECK_EQ(info.State, static_cast<DWORD>(MEM_FREE));
    CloseHandle(file);
}

// Guest space (reserve/provide/reclaim/release_guest_space): a Darwin mmap
// reservation held as placeholders. Provided parts take commits; shared
// memory is aliased into an untouched part (vm_remap over mmap'd pages, as
// Chrome maps a GPU ring buffer into V8's sandbox); a provided part goes back
// to placeholder only while nothing in it is committed; unmapping the alias
// leaves a placeholder that can be provided again; release frees it all.
FX_TEST(shared_memory_guest_space_takes_an_alias) {
    rt::ensure_initialized();
    constexpr std::uint64_t kSize = 0x100000;
    const std::uint64_t space = reserve_guest_space_above(std::uint64_t{1} << 36, kSize);
    FX_REQUIRE(space != 0);
    FX_CHECK(!reserve_guest_space(space, 0x10000));                 // already reserved
    FX_CHECK(provide_guest_space(space, space + 0x40000));          // [0, 256 KiB): the guest's heap
    FX_CHECK(host::vm::commit(reinterpret_cast<void*>(space + 0x4000), 0x4000, PAGE_READWRITE) != nullptr);
    word(space + 0x4000) = 0xabcd;
    // Shared memory elsewhere, aliased into the untouched middle.
    const std::uint64_t source = free_range(0x10000);
    map_shared_guest_memory(source, 0x10000, 0, PAGE_READWRITE);
    word(source + 8) = 0x5a5a0001;
    const std::uint64_t target = space + 0x80000;
    FX_CHECK(reclaim_guest_space(target, target + 0x10000));       // placeholder already: nothing to do
    alias_guest_memory(target, source, 0x10000, PAGE_READWRITE);
    FX_CHECK_EQ(word(target + 8), 0x5a5a0001u);
    word(target + 16) = 0x7a26e7;
    FX_CHECK_EQ(word(source + 16), 0x7a26e7u);
    FX_CHECK_EQ(word(space + 0x4000), 0xabcdu);                     // the provided part kept its page
    FX_CHECK(!release_guest_space(space, kSize));                   // a view is still mapped in it
    // A provided part with a committed page cannot go back; an uncommitted one can,
    // also when its piece reaches past the range (the rest stays provided).
    FX_CHECK(!reclaim_guest_space(space, space + 0x10000));
    FX_CHECK(provide_guest_space(space + 0xc0000, space + 0x100000));
    FX_CHECK(reclaim_guest_space(space + 0xc4000, space + 0xd0000));
    FX_CHECK(host::vm::commit(reinterpret_cast<void*>(space + 0xc0000), 0x4000, PAGE_READWRITE) != nullptr);
    FX_CHECK(host::vm::commit(reinterpret_cast<void*>(space + 0xd0000), 0x4000, PAGE_READWRITE) != nullptr);
    word(space + 0xd0000) = 1;
    alias_guest_memory(space + 0xc4000, source, 0xc000, PAGE_READONLY);
    FX_CHECK_EQ(word(space + 0xc4000 + 16), 0x7a26e7u);
    // Unmapped, the alias leaves placeholders that can be provided again.
    unmap_shared_guest_memory(target, 0x10000);
    unmap_shared_guest_memory(space + 0xc4000, 0xc000);
    FX_CHECK(!is_shared_guest_memory(target));
    FX_CHECK(provide_guest_space(target, target + 0x10000));
    FX_CHECK(host::vm::commit(reinterpret_cast<void*>(target), 0x4000, PAGE_READWRITE) != nullptr);
    FX_CHECK_EQ(word(target + 8), 0u);                              // fresh memory, not the section's
    unmap_shared_guest_memory(source, 0x10000);
    FX_CHECK(release_guest_space(space, kSize));
    FX_CHECK(!provide_guest_space(space, space + 0x10000));         // gone
    void* again = VirtualAlloc(reinterpret_cast<void*>(space), kSize, MEM_RESERVE, PAGE_NOACCESS);
    FX_CHECK(reinterpret_cast<std::uint64_t>(again) == space);      // the address space is free again
    if (again) VirtualFree(again, 0, MEM_RELEASE);
}

// A store through an alias inside guest space invalidates translations of
// the other view (SMC through either address).
FX_TEST(shared_memory_guest_space_alias_smc) {
    rt::ensure_initialized();
    set_mini_translator(true);
    const std::uint64_t space = reserve_guest_space_above(std::uint64_t{1} << 36, 0x40000);
    FX_REQUIRE(space != 0);
    const std::uint64_t code = free_range(0x10000);
    map_shared_guest_memory(code, 0x4000, 0, PAGE_READWRITE);
    word(code) = movz_x0(3);
    word(code + 4) = kRetWord;
    const std::uint64_t data = space + 0x10000;
    alias_guest_memory(data, code, 0x4000, PAGE_READWRITE);
    register_guest_code_memory(code, 0x4000);
    FX_CHECK_EQ(call_guest(code, {}), 3u);
    const auto before = runtime_stats();
    word(data) = movz_x0(4);   // through the alias in guest space: faults, invalidates
    FX_CHECK_EQ(runtime_stats().smc_write_faults, before.smc_write_faults + 1);
    FX_CHECK_EQ(call_guest(code, {}), 4u);
    unregister_guest_code_memory(code, 0x4000);
    unmap_shared_guest_memory(data, 0x4000);
    unmap_shared_guest_memory(code, 0x4000);
    FX_CHECK(release_guest_space(space, 0x40000));
    set_mini_translator(false);
}
