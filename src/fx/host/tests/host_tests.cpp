// fx host seams: behaviour every backend must have (no OS headers here).

#include "fx/host/env.hpp"
#include "fx/host/fault.hpp"
#include "fx/host/file.hpp"
#include "fx/host/guard.hpp"
#include "fx/host/process.hpp"
#include "fx/host/sample.hpp"
#include "fx/host/sync.hpp"
#include "fx/host/text.hpp"
#include "fx/host/thread.hpp"
#include "fx/host/time.hpp"
#include "fx/host/unwind.hpp"
#include "fx/host/vm.hpp"
#include "fx/testing/test.hpp"

#include <atomic>
#include <cstring>
#include <string>

namespace transmute::fx {
namespace {

namespace vm = host::vm;
constexpr std::size_t kPage = 0x1000;
constexpr std::size_t kGranule = 0x10000;

host::RwLock g_constant_lock;   // constant-initialized: usable before any constructor runs

FX_TEST(host_rwlock_exclusive_and_shared) {
    host::RwLock lock;
    FX_CHECK(lock.try_lock());
    FX_CHECK(!lock.try_lock_shared());
    lock.unlock();
    FX_CHECK(lock.try_lock_shared());
    FX_CHECK(lock.try_lock_shared());   // readers share
    FX_CHECK(!lock.try_lock());
    lock.unlock_shared();
    lock.unlock_shared();
    {
        host::ExclusiveLock guard(lock);
        FX_CHECK(!lock.try_lock_shared());
    }
    {
        host::SharedLock guard(g_constant_lock);
        FX_CHECK(!g_constant_lock.try_lock());
    }
    FX_CHECK(g_constant_lock.try_lock());
    g_constant_lock.unlock();
}

FX_TEST(host_condvar_times_out_and_wakes) {
    host::RwLock lock;
    host::CondVar condition;
    lock.lock();
    const auto start = host::monotonic_ms();
    FX_CHECK(!condition.wait_for(lock, 20));   // nobody notifies: timeout, lock held again
    FX_CHECK(host::monotonic_ms() - start >= 10);
    FX_CHECK(!lock.try_lock_shared());
    lock.unlock();

    struct Shared {
        host::RwLock lock;
        host::CondVar condition;
        bool ready = false;
    } shared;
    auto* thread = host::create_thread(
        [](void* argument) -> std::uint32_t {
            auto& s = *static_cast<Shared*>(argument);
            host::sleep_ms(5);
            host::ExclusiveLock guard(s.lock);
            s.ready = true;
            s.condition.notify_all();
            return 0;
        },
        &shared, 0, 0, nullptr);
    FX_CHECK(thread != nullptr);
    {
        host::ExclusiveLock guard(shared.lock);
        while (!shared.ready) FX_CHECK(shared.condition.wait_for(shared.lock, 10'000));
    }
    host::close_thread(thread);
}

FX_TEST(host_clocks) {
    FX_CHECK(host::tick_frequency() >= 1'000'000u);
    const auto a = host::ticks();
    const auto b = host::ticks();
    FX_CHECK(b >= a);
    // 2026 in FILETIME units (100 ns since 1601).
    FX_CHECK(host::wall_time() > 133'000'000'000'000'000ull);
    host::ProcessTimes times;
    FX_CHECK(host::process_times(times));
    FX_CHECK(times.created != 0 && times.created <= host::wall_time());
}

FX_TEST(host_threads_and_error_code) {
    std::uint32_t id = 0;
    std::atomic<std::uint32_t> seen{0};
    auto* thread = host::create_thread(
        [](void* argument) -> std::uint32_t {
            static_cast<std::atomic<std::uint32_t>*>(argument)->store(host::current_thread_id());
            return 7;
        },
        &seen, 64 * 1024, host::kThreadStackReservation, &id);
    FX_CHECK(thread != nullptr);
    FX_CHECK(id != 0);
    while (seen.load() == 0) host::sleep_ms(1);
    FX_CHECK_EQ(seen.load(), id);
    host::close_thread(thread);
    FX_CHECK(host::current_thread_id() != id);
    host::set_last_error(1234);
    FX_CHECK_EQ(host::last_error(), 1234u);
    FX_CHECK(!host::process_shutting_down());
    host::set_thread_priority(host::current_thread(), host::ThreadPriority::normal);
}

FX_TEST(host_thread_block_and_stack) {
    int local = 0;
    const auto here = reinterpret_cast<std::uint64_t>(&local);
    FX_CHECK(host::thread_stack_base() > here);
    FX_CHECK(host::thread_stack_base() - here < (64u << 20));
    void* teb = host::thread_block();
    FX_CHECK(teb != nullptr);
    std::uint8_t copy[0x2000]{};
    const auto bytes = host::copy_thread_block(copy, sizeof(copy));
    FX_CHECK(bytes >= 0x1000 && bytes <= sizeof(copy));
    FX_CHECK(std::memcmp(copy, teb, 0x40) == 0);
}

FX_TEST(host_thread_pool_and_process) {
    std::atomic<int> ran{0};
    FX_CHECK(host::submit_to_thread_pool([](void* context) { static_cast<std::atomic<int>*>(context)->store(1); }, &ran));
    for (int i = 0; i < 5000 && ran.load() == 0; ++i) host::sleep_ms(1);
    FX_CHECK_EQ(ran.load(), 1);
    FX_CHECK(host::process_id() != 0);
}

FX_TEST(host_text) {
    char out[16]{};
    FX_CHECK_EQ(host::to_utf8(L"aé", out, sizeof(out)), 4u);   // 'a', two bytes, zero
    FX_CHECK(std::strcmp(out, "a\xc3\xa9") == 0);
    FX_CHECK_EQ(host::to_utf8(L"abcdef", out, 3), 0u);              // does not fit
    FX_CHECK(host::to_utf8(std::wstring_view(L"xyé")) == "xy\xc3\xa9");
    FX_CHECK(host::from_utf8("p\xc3\xa9") == L"pé");
    FX_CHECK(host::from_utf8("").empty());
}

} // namespace

// Defined in host_win32_tests.cpp (the seam has no remove-directory call).
void remove_directory_for_tests(const std::wstring& path);

namespace {

FX_TEST(host_files) {
    namespace file = host::file;
    const std::wstring dir = file::temp_directory() + L"fx-host-test-" + std::to_wstring(host::process_id());
    FX_CHECK(file::create_directory(dir) == file::Directory::created);
    FX_CHECK(file::create_directory(dir) == file::Directory::exists);
    FX_CHECK(file::create_directory(dir + L"\\missing\\child") == file::Directory::parent_missing);
    const std::wstring path = dir + L"\\data.bin";
    file::File out = file::open(path, file::kWrite, file::kRead | file::kDelete, file::Create::create_new,
                                file::Flags::normal);
    FX_CHECK(out != nullptr);
    FX_CHECK(file::open(path, file::kWrite, file::kShareNone, file::Create::create_new, file::Flags::normal) == nullptr);
    const char text[] = "0123456789";
    FX_CHECK(file::write_all(out, text, 10));
    FX_CHECK(file::seek(out, 2));
    FX_CHECK(file::write_all(out, "AB", 2));
    FX_CHECK(file::seek_end(out));
    FX_CHECK(file::write_all(out, "!", 1));
    file::close(out);
    std::uint64_t bytes = 0, written = 0;
    FX_CHECK(file::attributes(path, &bytes, &written));
    FX_CHECK_EQ(bytes, 11u);
    FX_CHECK(written != 0 && written <= host::wall_time() + 10'000'000);
    file::File in = file::open(path, file::kRead, file::kRead | file::kDelete, file::Create::open_existing,
                               file::Flags::sequential);
    FX_CHECK(in != nullptr);
    FX_CHECK(file::size(in, bytes) && bytes == 11);
    char back[11]{};
    FX_CHECK(file::read_all(in, back, 11));
    FX_CHECK(std::memcmp(back, "01AB456789!", 11) == 0);
    const std::uint8_t* view = file::map_read_only(in);
    FX_CHECK(view != nullptr && view[2] == 'A');
    file::close(in);
    FX_CHECK(view[10] == '!');   // the view outlives the handle
    file::unmap(view);
    const auto entries = file::list(dir + L"\\*.bin");
    FX_CHECK_EQ(entries.size(), 1u);
    if (!entries.empty()) {
        FX_CHECK(entries[0].name == L"data.bin");
        FX_CHECK(!entries[0].directory);
        FX_CHECK_EQ(entries[0].size, 11u);
    }
    // Lock files: exclusive while held, gone once closed.
    const std::wstring lock_path = dir + L"\\x.lock";
    file::File lock = file::open(lock_path, file::kRead | file::kWrite, file::kShareNone, file::Create::open_always,
                                 file::Flags::lock_file);
    FX_CHECK(lock != nullptr);
    FX_CHECK(file::open(lock_path, file::kRead | file::kWrite, file::kShareNone, file::Create::open_always,
                        file::Flags::lock_file) == nullptr);
    file::close(lock);
    FX_CHECK(!file::attributes(lock_path, nullptr, nullptr));
    // Rename over an existing file; delete while mapped.
    const std::wstring other = dir + L"\\other.bin";
    out = file::open(other, file::kWrite, file::kShareNone, file::Create::create_always, file::Flags::normal);
    FX_CHECK(out != nullptr && file::write_all(out, "x", 1));
    file::close(out);
    FX_CHECK(file::rename_replace(other, path));
    FX_CHECK(file::attributes(path, &bytes, nullptr) && bytes == 1);
    in = file::open(path, file::kRead | file::kDelete, file::kRead | file::kDelete, file::Create::open_existing,
                    file::Flags::normal);
    view = file::map_read_only(in);
    FX_CHECK(view != nullptr);
    file::mark_deleted(in);
    file::close(in);
    FX_CHECK(!file::attributes(path, nullptr, nullptr));   // the name is gone
    FX_CHECK(view[0] == 'x');                               // the mapping stays
    file::unmap(view);
    FX_CHECK(!file::remove(path));
    FX_CHECK(file::list(dir + L"\\*.bin").empty());
    remove_directory_for_tests(dir);
}

FX_TEST(host_auto_reset_event) {
    host::AutoResetEvent event;
    FX_CHECK(!event.wait_for(1));
    event.set();
    FX_CHECK(event.wait_for(0));
    FX_CHECK(!event.wait_for(0));   // auto-reset: one wait consumed it
}

FX_TEST(host_process_queries) {
    FX_CHECK(host::process_age_ms() > 0);
    const std::wstring self = host::executable_path();
    FX_CHECK(self.size() > 4 && self.substr(self.size() - 4) == L".exe");
    const std::wstring full = host::file::full_path(L"relative.txt");
    FX_CHECK(full.size() > 12 && full.find(L"relative.txt") == full.size() - 12);
    host::set_thread_affinity(host::current_thread(), ~std::uint64_t{0} >> 1);   // keeps every processor
}

FX_TEST(host_file_replace_atomically) {
    namespace file = host::file;
    const std::wstring dir = file::temp_directory() + L"fx-host-replace-" + std::to_wstring(host::process_id());
    FX_CHECK(file::create_directory(dir) == file::Directory::created);
    const std::wstring target = dir + L"\\report.txt", temp = dir + L"\\report.tmp";
    for (const char* text : {"one", "two!"}) {
        file::File out = file::open(temp, file::kWrite, file::kShareNone, file::Create::create_always, file::Flags::normal);
        FX_CHECK(out && file::write_all(out, text, std::strlen(text)));
        file::close(out);
        FX_CHECK(file::replace_atomically(temp, target));
        std::uint64_t bytes = 0;
        FX_CHECK(file::attributes(target, &bytes, nullptr) && bytes == std::strlen(text));
        FX_CHECK(!file::attributes(temp, nullptr, nullptr));
    }
    FX_CHECK(!file::replace_atomically(temp, target));   // nothing to move
    file::remove(target);
    remove_directory_for_tests(dir);
}

volatile bool g_spin = true;
void spin_until_stopped() {
    while (g_spin) {}
}

FX_TEST(host_sample_thread) {
    namespace sample = host::sample;
    std::atomic<std::uint32_t> id{0};
    auto* thread = host::create_thread(
        [](void* argument) -> std::uint32_t {
            static_cast<std::atomic<std::uint32_t>*>(argument)->store(host::current_thread_id());
            spin_until_stopped();
            return 0;
        },
        &id, 0, 0, nullptr);
    FX_REQUIRE(thread != nullptr);
    while (id.load() == 0) host::sleep_ms(1);
    const sample::Thread handle = sample::open_thread(id.load());
    FX_REQUIRE(handle != nullptr);
    FX_CHECK(sample::suspend(handle));
    sample::Registers registers;
    FX_CHECK(sample::capture(handle, registers, true));
    const std::uint64_t pc = host::rip(registers.context());
    const std::uint64_t sp = host::rsp(registers.context());
    static std::uint8_t copy[64 * 1024];
    const std::size_t copied = sample::copy_stack(sp, copy, sizeof(copy));
    sample::resume(handle);
    FX_CHECK(pc != 0 && sp != 0);
    FX_CHECK(copied > 0);
    std::uint64_t frames[16]{};
    const unsigned depth = sample::unwind_copy(registers, copy, copied, frames, 16, nullptr);
    FX_CHECK(depth >= 1 && frames[0] == pc);
    FX_CHECK(sample::load_symbols());
    const std::string where = sample::describe(pc, true);
    FX_CHECK(where.find("fx_host_tests") != std::string::npos);
    FX_CHECK(sample::symbol(reinterpret_cast<std::uint64_t>(&spin_until_stopped)).find("spin_until_stopped") !=
             std::string::npos);
    std::uint64_t begin = 0, end = 0;
    FX_CHECK(sample::wait_module(begin, end) && begin < end);
    g_spin = false;
    host::close_thread(thread);
    const sample::Timer timer = sample::create_timer();
    const auto start = host::ticks();
    sample::wait(timer, 500);
    FX_CHECK(host::ticks() > start);
    const sample::Thread self = sample::open_current_thread();
    FX_CHECK(self != nullptr);
}

FX_TEST(host_env) {
    char value[16]{};
    FX_CHECK_EQ(host::get_env("TRANSMUTE_FX_HOST_TEST_UNSET_VARIABLE", value, sizeof(value)), 0u);
    FX_CHECK(host::get_env("PATH", nullptr, 0) > 1);   // the size needed, with the zero
    wchar_t wide[4]{};
    FX_CHECK(host::get_env(L"PATH", wide, 4) > 4);
}

FX_TEST(host_vm_reserve_commit_protect) {
    auto* base = static_cast<std::uint8_t*>(vm::reserve(nullptr, 4 * kGranule, vm::kReadWrite));
    FX_CHECK(base != nullptr);
    vm::RegionInfo info;
    FX_CHECK(vm::query(base, info));
    FX_CHECK(info.state == vm::State::reserved);
    FX_CHECK_EQ(info.allocation_base, reinterpret_cast<std::uint64_t>(base));
    FX_CHECK_EQ(info.protect, 0u);
    FX_CHECK(vm::commit(base + kPage, kPage, vm::kReadWrite) == base + kPage);
    FX_CHECK(vm::query(base + kPage, info));
    FX_CHECK(info.committed());
    FX_CHECK(info.type == vm::Type::private_memory);
    FX_CHECK_EQ(info.protect, vm::kReadWrite);
    FX_CHECK_EQ(info.size, kPage);
    base[kPage] = 42;
    vm::Protection old = 0;
    FX_CHECK(vm::protect(base + kPage, kPage, vm::kReadOnly, &old));
    FX_CHECK_EQ(old, vm::kReadWrite);
    FX_CHECK(vm::query(base + kPage, info));
    FX_CHECK_EQ(info.protect, vm::kReadOnly);
    std::uint8_t byte = 0;
    FX_CHECK(!host::guarded_move(base + kPage, &byte, 1));   // read-only: the write faults
    FX_CHECK(host::guarded_move(&byte, base + kPage, 1));
    FX_CHECK_EQ(byte, 42);
    FX_CHECK(vm::protect(base + kPage, kPage, vm::kReadWrite | vm::kGuard));
    FX_CHECK(!host::guarded_move(&byte, base + kPage, 1));   // guard page: one fault, then the guard is gone
    FX_CHECK(host::guarded_move(&byte, base + kPage, 1));
    FX_CHECK(vm::decommit(base + kPage, kPage));
    FX_CHECK(vm::query(base + kPage, info));
    FX_CHECK(info.state == vm::State::reserved);
    FX_CHECK(vm::release(base));
    FX_CHECK(vm::query(base, info));
    FX_CHECK(info.state == vm::State::free);

    auto* memory = static_cast<std::uint8_t*>(vm::allocate(nullptr, kPage, vm::kReadWrite));
    FX_CHECK(memory != nullptr && memory[0] == 0);
    memory[0] = 1;
    vm::discard(memory, kPage);
    vm::flush_instruction_cache(memory, kPage);
    FX_CHECK(vm::release(memory));

    auto* high = vm::reserve_above(std::uint64_t{1} << 40, kGranule);
    FX_CHECK(reinterpret_cast<std::uint64_t>(high) >= (std::uint64_t{1} << 40));
    FX_CHECK(vm::release(high));
    auto* top = vm::reserve(nullptr, kGranule, vm::kNoAccess, true);
    FX_CHECK(top != nullptr);
    FX_CHECK(vm::release(top));
}

FX_TEST(host_vm_placeholders_and_sections) {
    // One placeholder, split into two pages; a section mapped into both
    // pieces shares its contents; unmapping leaves placeholders behind.
    auto* base = static_cast<std::uint8_t*>(vm::reserve_placeholder(nullptr, kGranule));
    FX_CHECK(base != nullptr);
    FX_CHECK(vm::split_placeholder(base, kPage));
    FX_CHECK(vm::split_placeholder(base + kPage, kPage));
    vm::Section section = vm::create_section(kPage);
    FX_CHECK(section != nullptr);
    vm::Section copy = vm::duplicate_section(section);
    FX_CHECK(copy != nullptr);
    vm::close_section(section);   // the duplicate keeps it
    FX_CHECK(vm::map_view_into_placeholder(copy, base, 0, kPage, vm::kReadWrite) == base);
    FX_CHECK(vm::map_view_into_placeholder(copy, base + kPage, 0, kPage, vm::kReadOnly) == base + kPage);
    vm::RegionInfo info;
    FX_CHECK(vm::query(base, info));
    FX_CHECK(info.type == vm::Type::mapped);
    base[16] = 0x5a;
    FX_CHECK_EQ(base[kPage + 16], 0x5a);   // one section, two views
    FX_CHECK(vm::unmap_view_to_placeholder(base + kPage));
    FX_CHECK(vm::unmap_view_to_placeholder(base));
    FX_CHECK(vm::query(base, info));
    FX_CHECK(info.state == vm::State::reserved);
    FX_CHECK(vm::coalesce_placeholders(base, kGranule));
    FX_CHECK(vm::release(base));
    vm::close_section(copy);
}

FX_TEST(host_vm_heap) {
    vm::Heap heap = vm::create_heap();
    FX_CHECK(heap != nullptr);
    auto* block = static_cast<std::uint8_t*>(vm::heap_allocate(heap, 100));
    FX_CHECK(block != nullptr);
    FX_CHECK((reinterpret_cast<std::uint64_t>(block) & 15) == 0);
    FX_CHECK(block[0] == 0 && block[99] == 0);
    FX_CHECK(vm::heap_free(heap, block));
}

FX_TEST(host_guarded_access) {
    auto* reserved = static_cast<std::uint8_t*>(vm::reserve(nullptr, kGranule, vm::kReadWrite));
    std::uint32_t value = 99;
    FX_CHECK(!host::try_read32(reinterpret_cast<std::uint64_t>(reserved), value));
    FX_CHECK_EQ(value, 99u);
    const std::uint32_t word = 0x12345678;
    FX_CHECK(host::try_read32(reinterpret_cast<std::uint64_t>(&word), value));
    FX_CHECK_EQ(value, 0x12345678u);
    std::uint8_t pair[8]{};
    FX_CHECK(!host::try_copy(pair, reserved, sizeof(pair)));
    FX_CHECK(host::try_copy(pair, &word, sizeof(word)));
    FX_CHECK(!host::guarded_fill(reserved, 0, 16));
    FX_CHECK_EQ(host::guarded_length(reserved, 1, 16), -1);
    const char text[] = "abc";
    FX_CHECK_EQ(host::guarded_length(reinterpret_cast<const std::uint8_t*>(text), 1, 16), 3);
    const char16_t wide[] = u"abcd";
    FX_CHECK_EQ(host::guarded_length(reinterpret_cast<const std::uint8_t*>(wide), 2, 16), 4);
    FX_CHECK_EQ(host::guarded_length(reinterpret_cast<const std::uint8_t*>(wide), 2, 2), 2);

    host::FaultRecord fault{};
    volatile std::uint8_t* target = reserved + 0x123;
    FX_CHECK(!host::call_guarded([&] { *target = 1; }, fault));
    FX_CHECK_EQ(fault.code, host::kFaultAccessViolation);
    FX_CHECK(fault.parameter_count >= 2);
    FX_CHECK_EQ(fault.parameters[0], host::kFaultWrite);
    FX_CHECK_EQ(fault.parameters[1], reinterpret_cast<std::uint64_t>(reserved + 0x123));
    int ran = 0;
    FX_CHECK(host::call_guarded([&] { ran = 1; }, fault));
    FX_CHECK_EQ(ran, 1);
    FX_CHECK(!host::call_guarded_memory([&] { static_cast<void>(*target); }));
    FX_CHECK(vm::release(reserved));
}

// The fault handler: a write to a read-only page makes it writable and resumes.
std::atomic<std::uint64_t> g_fault_page{0};
std::atomic<unsigned> g_faults{0};
std::atomic<std::uint64_t> g_fault_rip{0};

host::FaultAction test_handler(host::FaultRecord& record, host::CpuContext& context) {
    const auto page = g_fault_page.load();
    if (record.code != host::kFaultAccessViolation || record.parameter_count < 2 ||
        record.parameters[0] != host::kFaultWrite || (record.parameters[1] & ~std::uint64_t{0xfff}) != page || page == 0)
        return host::FaultAction::pass;
    g_faults.fetch_add(1);
    g_fault_rip.store(host::rip(context));
    FX_CHECK(host::rsp(context) != 0);
    vm::protect(reinterpret_cast<void*>(page), kPage, vm::kReadWrite);
    return host::FaultAction::resume;
}

FX_TEST(host_fault_handler_resumes) {
    FX_CHECK(host::install_fault_handler(&test_handler));
    auto* page = static_cast<volatile std::uint8_t*>(vm::allocate(nullptr, kPage, vm::kReadOnly));
    g_fault_page.store(reinterpret_cast<std::uint64_t>(page));
    page[8] = 5;   // faults once; the handler makes the page writable
    FX_CHECK_EQ(g_faults.load(), 1u);
    FX_CHECK_EQ(static_cast<int>(page[8]), 5);
    FX_CHECK(g_fault_rip.load() != 0);
    g_fault_page.store(0);
    vm::release(const_cast<std::uint8_t*>(page));
}

// The unwinder finds an entry through the registered lookup.
host::UnwindEntry g_unwind_entry{0x10, 0x20, 0x100};
std::uint64_t g_unwind_base = 0;
host::UnwindEntry* test_lookup(std::uint64_t pc) {
    return pc - g_unwind_base >= 0x10 && pc - g_unwind_base < 0x20 ? &g_unwind_entry : nullptr;
}

} // namespace

// Defined in host_win32_tests.cpp (needs the host's unwinder API).
const host::UnwindEntry* lookup_function_entry_for_tests(std::uint64_t pc, std::uint64_t* image_base);

namespace {

FX_TEST(host_unwind_lookup_registration) {
    auto* code = static_cast<std::uint8_t*>(vm::allocate(nullptr, kGranule, vm::kExecuteReadWrite));
    g_unwind_base = reinterpret_cast<std::uint64_t>(code);
    FX_CHECK(host::register_unwind_lookup(g_unwind_base, kGranule, &test_lookup));
    std::uint64_t image_base = 0;
    FX_CHECK(lookup_function_entry_for_tests(g_unwind_base + 0x18, &image_base) == &g_unwind_entry);
    FX_CHECK_EQ(image_base, g_unwind_base);
    FX_CHECK(lookup_function_entry_for_tests(g_unwind_base + 0x30, &image_base) == nullptr);
}

} // namespace
} // namespace transmute::fx
