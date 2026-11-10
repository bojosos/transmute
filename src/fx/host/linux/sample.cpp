// linux backend of fx/host/sample.hpp and fx/host/diagnostics.hpp.
//
// Thread sampling (the profiler, the image-audit tool and the arena's reclaim
// scan): suspend sends the thread a spare real-time signal (SIGRTMIN + 5;
// Alchemy's process parking is SIGRTMIN + 4) with tgkill. Its handler copies
// the ucontext_t it is given into the thread's record, says so, and parks on a
// futex until resume. While it is parked the copy's fpregs pointer (into the
// handler's frame) stays valid, so the vector registers can be read too.
// Stacks are read with process_vm_readv on this process, which fails instead
// of faulting, inside the mapping that /proc/self/maps lists for the stack
// pointer (read without allocating: the stopped thread may hold the heap's
// lock). Unwinding walks frame pointers over the copy. Symbols come from
// dladdr and, for names the dynamic table lacks, the module's own ELF symbol
// table. Core-type statistics have no Linux source.
#include "fx/host/diagnostics.hpp"
#include "fx/host/sample.hpp"
#include "fx/host/time.hpp"

#include <cxxabi.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <linux/futex.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace transmute::fx::host {

namespace sample {

namespace {

static_assert(kCpuContextBytes == sizeof(ucontext_t));

// A sampled thread's record. States: running, requested (signal sent), stopped (handler parked),
// abandoned (the sampler gave up waiting; the handler returns at once if it still comes).
enum : std::uint32_t { running = 0, requested = 1, stopped = 2, abandoned = 3 };
struct Record {
    pid_t tid{};
    std::atomic<std::uint32_t> state{running};
    std::atomic<std::uint32_t> resumed{0};   // the futex the handler waits on
    alignas(16) unsigned char context[sizeof(ucontext_t)];
};

// Records live for the process (sampler handles stay open); the handler finds its own by tid in this
// fixed table without locking or allocating.
constexpr std::size_t kRecords = 1024;
std::atomic<Record*> g_records[kRecords];
std::once_flag g_installed;
int g_signal = 0;

long futex(std::atomic<std::uint32_t>* word, int op, std::uint32_t value, const timespec* timeout = nullptr) noexcept {
    return syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(word), op, value, timeout, nullptr, 0);
}

void on_sample(int, siginfo_t*, void* raw) noexcept {
    const int saved = errno;
    const auto self = static_cast<pid_t>(syscall(SYS_gettid));
    Record* record = nullptr;
    for (auto& slot : g_records) {
        Record* candidate = slot.load(std::memory_order_acquire);
        if (candidate && candidate->tid == self) { record = candidate; break; }
    }
    if (record) {
        std::memcpy(record->context, raw, sizeof(ucontext_t));
        std::uint32_t expected = requested;
        // The sampler may have given up (abandoned): then this signal is stale and does nothing.
        if (record->state.compare_exchange_strong(expected, stopped, std::memory_order_acq_rel)) {
            futex(&record->state, FUTEX_WAKE_PRIVATE, 1);
            while (record->resumed.load(std::memory_order_acquire) == 0) futex(&record->resumed, FUTEX_WAIT_PRIVATE, 0);
            record->resumed.store(0, std::memory_order_relaxed);
            record->state.store(running, std::memory_order_release);
        }
    }
    errno = saved;
}

bool install() noexcept {
    std::call_once(g_installed, [] {
        const int signal = SIGRTMIN + 5;
        struct sigaction action {};
        action.sa_sigaction = &on_sample;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigfillset(&action.sa_mask);
        if (sigaction(signal, &action, nullptr) == 0) g_signal = signal;
    });
    return g_signal != 0;
}

Record* record_of(pid_t tid) noexcept {
    for (auto& slot : g_records) {
        Record* value = slot.load(std::memory_order_acquire);
        if (value && value->tid == tid) return value;
    }
    auto* fresh = new (std::nothrow) Record;
    if (!fresh) return nullptr;
    fresh->tid = tid;
    for (auto& slot : g_records) {
        Record* empty = nullptr;
        if (slot.compare_exchange_strong(empty, fresh, std::memory_order_acq_rel)) return fresh;
    }
    delete fresh;
    return nullptr;
}

// The end of the mapping holding `address`, from /proc/self/maps, read with system calls into a stack
// buffer: no allocation, no lock. 0 when no mapping holds it.
std::uint64_t mapping_end(std::uint64_t address) noexcept {
    const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char buffer[4096];
    std::size_t kept = 0;
    std::uint64_t end = 0;
    for (;;) {
        const auto got = read(fd, buffer + kept, sizeof(buffer) - kept);
        if (got <= 0) break;
        kept += static_cast<std::size_t>(got);
        std::size_t line = 0;
        for (std::size_t at = 0; at < kept; ++at) {
            if (buffer[at] != '\n') continue;
            // "begin-end perms ..."
            std::uint64_t begin = 0, limit = 0;
            std::size_t p = line;
            const auto hex = [&](std::uint64_t& value) {
                for (; p < at; ++p) {
                    const char c = buffer[p];
                    const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                    if (digit < 0) break;
                    value = value * 16 + static_cast<std::uint64_t>(digit);
                }
            };
            hex(begin);
            if (p < at && buffer[p] == '-') { ++p; hex(limit); }
            if (address >= begin && address < limit) { end = limit; break; }
            line = at + 1;
        }
        if (end) break;
        std::memmove(buffer, buffer + line, kept - line);
        kept -= line;
        if (kept == sizeof(buffer)) kept = 0;   // an over-long line: skip it
    }
    close(fd);
    return end;
}

// Reads `size` bytes at `address` of this process without faulting; the bytes read.
std::size_t read_safely(std::uint64_t address, void* out, std::size_t size) noexcept {
    iovec local{out, size};
    iovec remote{reinterpret_cast<void*>(address), size};
    const auto got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return got < 0 ? 0 : static_cast<std::size_t>(got);
}

} // namespace

Thread open_thread(std::uint32_t id) noexcept {
    if (!id || !install()) return nullptr;
    return record_of(static_cast<pid_t>(id));
}
Thread open_current_thread() noexcept { return open_thread(static_cast<std::uint32_t>(syscall(SYS_gettid))); }

bool suspend(Thread thread) noexcept {
    auto* record = static_cast<Record*>(thread);
    if (!record || record->tid == static_cast<pid_t>(syscall(SYS_gettid))) return false;
    std::uint32_t expected = running;
    if (!record->state.compare_exchange_strong(expected, requested, std::memory_order_acq_rel)) return false;
    record->resumed.store(0, std::memory_order_relaxed);
    if (syscall(SYS_tgkill, getpid(), record->tid, g_signal) != 0) {
        record->state.store(running, std::memory_order_release);
        return false;
    }
    // A thread that cannot take the signal within a second (blocked with it masked, or gone) is not sampled.
    for (int wait = 0; wait < 100; ++wait) {
        if (record->state.load(std::memory_order_acquire) == stopped) return true;
        timespec timeout{0, 10'000'000};
        futex(&record->state, FUTEX_WAIT_PRIVATE, requested, &timeout);
    }
    expected = requested;
    if (record->state.compare_exchange_strong(expected, abandoned, std::memory_order_acq_rel)) {
        // The handler, if it still runs, sees abandoned and returns; the record is usable again.
        record->state.store(running, std::memory_order_release);
        return false;
    }
    return record->state.load(std::memory_order_acquire) == stopped;
}

void resume(Thread thread) noexcept {
    auto* record = static_cast<Record*>(thread);
    if (!record || record->state.load(std::memory_order_acquire) != stopped) return;
    record->resumed.store(1, std::memory_order_release);
    futex(&record->resumed, FUTEX_WAKE_PRIVATE, 1);
    // Running again once the handler leaves (it sets the state itself), so the next suspend can start.
    for (int spin = 0; spin < 100000 && record->state.load(std::memory_order_acquire) == stopped; ++spin) sched_yield();
}

bool capture(Thread thread, Registers& registers, bool) noexcept {
    auto* record = static_cast<Record*>(thread);
    if (!record || record->state.load(std::memory_order_acquire) != stopped) return false;
    std::memcpy(registers.storage, record->context, sizeof(ucontext_t));
    return true;
}

std::size_t copy_stack(std::uint64_t sp, std::uint8_t* out, std::size_t size) noexcept {
    const auto end = mapping_end(sp);
    if (!end) return 0;
    return read_safely(sp, out, static_cast<std::size_t>(std::min<std::uint64_t>(size, end - sp)));
}

void close_thread(Thread) noexcept {}   // records live for the process (a tid's record is reused)

std::size_t capture_words(Thread thread, std::uint64_t* out, std::size_t max, std::uint64_t& sp) noexcept {
    auto* record = static_cast<Record*>(thread);
    if (!record || record->state.load(std::memory_order_acquire) != stopped) return 0;
    auto& context = *reinterpret_cast<CpuContext*>(record->context);
    sp = rsp(context);
    std::size_t n = 0;
    for (unsigned reg = 0; reg < 16; ++reg)
        if (n < max) out[n++] = gpr(context, reg);
    if (n < max) out[n++] = rip(context);
    // fpregs points into the parked handler's frame: valid while the thread is stopped.
    const auto* ucontext = reinterpret_cast<const ucontext_t*>(record->context);
    if (ucontext->uc_mcontext.fpregs) {
        for (unsigned reg = 0; reg < 16; ++reg) {
            std::uint64_t halves[2]{};
            std::memcpy(halves, xmm(context, reg), sizeof(halves));
            if (n < max) out[n++] = halves[0];
            if (n < max) out[n++] = halves[1];
        }
    }
    return n;
}

bool scan_stack(std::uint64_t sp, void (*word)(void* user, std::uint64_t value), void* user) noexcept {
    sp &= ~std::uint64_t{7};
    const auto end = mapping_end(sp);
    if (!end) return false;
    std::uint64_t chunk[512];
    for (std::uint64_t at = sp; at < end;) {
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(chunk), end - at));
        const auto got = read_safely(at, chunk, want);
        if (got != want) return false;
        for (std::size_t index = 0; index < got / 8; ++index) word(user, chunk[index]);
        at += got;
    }
    return true;
}

// Frame pointers over the copy: [rbp] is the caller's rbp, [rbp + 8] the return address. Code built
// without them ends the walk early (the profiler still has the pc).
unsigned unwind_copy(const Registers& registers, const std::uint8_t* copy, std::size_t size, std::uint64_t* frames,
                     unsigned max, bool (*stop)(std::uint64_t pc)) noexcept {
    const auto& context = registers.context();
    const std::uint64_t real = gpr(context, 4);   // rsp
    std::uint64_t pc = rip(context), frame = gpr(context, 5);   // rbp
    unsigned n = 0;
    while (n < max && pc != 0 && !(stop && stop(pc))) {
        frames[n++] = pc;
        if (frame < real || frame + 16 > real + size || frame % 8) break;
        std::uint64_t next_frame = 0, return_address = 0;
        std::memcpy(&next_frame, copy + (frame - real), 8);
        std::memcpy(&return_address, copy + (frame - real) + 8, 8);
        if (next_frame <= frame) {
            if (n < max && return_address && !(stop && stop(return_address))) frames[n++] = return_address;
            break;
        }
        frame = next_frame;
        pc = return_address;
    }
    return n;
}

namespace {

// A module's function symbols from its ELF .symtab (sorted by address, relative to the load bias).
struct Function {
    std::uint64_t begin{}, end{};
    std::string name;
};
struct ModuleSymbols {
    std::vector<Function> functions;
};

ModuleSymbols read_symbols(const char* path) {
    ModuleSymbols result;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return result;
    struct stat info {};
    if (fstat(fd, &info) != 0 || info.st_size < static_cast<off_t>(sizeof(Elf64_Ehdr))) { close(fd); return result; }
    void* mapped = mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED) return result;
    const auto* base = static_cast<const std::uint8_t*>(mapped);
    const auto size = static_cast<std::size_t>(info.st_size);
    const auto* header = reinterpret_cast<const Elf64_Ehdr*>(base);
    const bool ok = std::memcmp(header->e_ident, ELFMAG, SELFMAG) == 0 && header->e_ident[EI_CLASS] == ELFCLASS64 &&
                    header->e_shoff + static_cast<std::uint64_t>(header->e_shnum) * sizeof(Elf64_Shdr) <= size;
    if (ok) {
        const auto* sections = reinterpret_cast<const Elf64_Shdr*>(base + header->e_shoff);
        for (unsigned index = 0; index < header->e_shnum; ++index) {
            const auto& table = sections[index];
            if (table.sh_type != SHT_SYMTAB || table.sh_link >= header->e_shnum) continue;
            const auto& strings = sections[table.sh_link];
            if (table.sh_offset + table.sh_size > size || strings.sh_offset + strings.sh_size > size) continue;
            const auto* symbols = reinterpret_cast<const Elf64_Sym*>(base + table.sh_offset);
            const auto count = table.sh_size / sizeof(Elf64_Sym);
            for (std::size_t at = 0; at < count; ++at) {
                const auto& symbol = symbols[at];
                if (ELF64_ST_TYPE(symbol.st_info) != STT_FUNC || !symbol.st_value || symbol.st_name >= strings.sh_size) continue;
                const char* name = reinterpret_cast<const char*>(base + strings.sh_offset + symbol.st_name);
                result.functions.push_back({symbol.st_value, symbol.st_value + std::max<std::uint64_t>(symbol.st_size, 1), name});
            }
        }
    }
    munmap(mapped, size);
    std::sort(result.functions.begin(), result.functions.end(), [](const Function& a, const Function& b) { return a.begin < b.begin; });
    return result;
}

std::mutex g_symbols_lock;
std::map<std::string, std::shared_ptr<const ModuleSymbols>> g_symbols;

// The symbol name at `pc`: the dynamic table's (dladdr), else the module's .symtab's; demangled.
std::string name_at(std::uint64_t pc, const Dl_info& info) {
    std::string raw = info.dli_sname ? info.dli_sname : "";
    if (raw.empty() && info.dli_fname) {
        std::shared_ptr<const ModuleSymbols> module;
        {
            std::lock_guard lock(g_symbols_lock);
            auto& slot = g_symbols[info.dli_fname];
            if (!slot) slot = std::make_shared<const ModuleSymbols>(read_symbols(info.dli_fname));
            module = slot;
        }
        // .symtab values are link-time addresses: subtract the module's load bias.
        Dl_info detail{};
        link_map* map = nullptr;
        std::uint64_t bias = reinterpret_cast<std::uint64_t>(info.dli_fbase);
        if (dladdr1(reinterpret_cast<void*>(pc), &detail, reinterpret_cast<void**>(&map), RTLD_DL_LINKMAP) && map) bias = map->l_addr;
        const auto relative = pc - bias;
        const auto& functions = module->functions;
        auto found = std::upper_bound(functions.begin(), functions.end(), relative,
                                      [](std::uint64_t value, const Function& function) { return value < function.begin; });
        if (found != functions.begin()) {
            --found;
            if (relative < found->end) raw = found->name;
        }
    }
    if (raw.empty()) return {};
    int status = 0;
    char* demangled = abi::__cxa_demangle(raw.c_str(), nullptr, nullptr, &status);
    if (status == 0 && demangled) {
        std::string value = demangled;
        std::free(demangled);
        return value;
    }
    return raw;
}

} // namespace

bool load_symbols() noexcept { return true; }   // read on demand, per module

std::string describe(std::uint64_t pc, bool symbols) {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(pc), &info) || !info.dli_fname) return "?";
    std::string module = info.dli_fname;
    if (const auto slash = module.rfind('/'); slash != std::string::npos) module.erase(0, slash + 1);
    if (symbols) {
        if (auto name = name_at(pc, info); !name.empty()) return module + "!" + name;
    }
    char offset[32];
    std::snprintf(offset, sizeof(offset), "+0x%llx",
                  static_cast<unsigned long long>(pc - reinterpret_cast<std::uint64_t>(info.dli_fbase)));
    return module + offset;
}

std::string symbol(std::uint64_t pc) {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(pc), &info)) return {};
    return name_at(pc, info);
}

bool source_line(std::uint64_t, std::string&, unsigned&) { return false; }   // no DWARF line tables yet

// Blocked threads wait in libc (futex, poll, read and the rest are its system call stubs).
bool wait_module(std::uint64_t& begin, std::uint64_t& end) noexcept {
    struct Range {
        std::uint64_t probe, begin, end;
    } range{reinterpret_cast<std::uint64_t>(&::syscall), 0, 0};
    dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* data) -> int {
        auto& wanted = *static_cast<Range*>(data);
        std::uint64_t low = UINT64_MAX, high = 0;
        for (unsigned index = 0; index < info->dlpi_phnum; ++index) {
            const auto& segment = info->dlpi_phdr[index];
            if (segment.p_type != PT_LOAD) continue;
            low = std::min<std::uint64_t>(low, info->dlpi_addr + segment.p_vaddr);
            high = std::max<std::uint64_t>(high, info->dlpi_addr + segment.p_vaddr + segment.p_memsz);
        }
        if (wanted.probe >= low && wanted.probe < high) { wanted.begin = low; wanted.end = high; return 1; }
        return 0;
    }, &range);
    if (!range.end) return false;
    begin = range.begin;
    end = range.end;
    return true;
}

// clock_nanosleep is the high-resolution wait; the timer only marks that it exists.
Timer create_timer() noexcept { return reinterpret_cast<Timer>(std::uintptr_t{1}); }
void wait(Timer timer, std::uint32_t microseconds) noexcept {
    if (!timer) { sleep_ms(1); return; }
    timespec duration{static_cast<time_t>(microseconds / 1'000'000), static_cast<long>(microseconds % 1'000'000) * 1000};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &duration, &duration) == EINTR) {}
}

} // namespace sample

void corestats_start_if_requested(void (*)(std::vector<std::uint32_t>&)) {}
void corestats_flush() {}

} // namespace transmute::fx::host
