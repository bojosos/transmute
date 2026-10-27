#pragma once
// The private interface of DarwinServices (include/alchemy/darwin_services.hpp):
// helpers and DarwinServices::Impl, shared by the libSystem translation units
// that define Impl's installers (services.cpp, fs/files.cpp,
// process/identity.cpp, process/process_model.cpp, net/sockets.cpp).
// Include it first: it sets the CRT macros.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "alchemy/darwin_services.hpp"
#include "alchemy/alchemy_root.hpp"
#include "alchemy/darwin_version.hpp"
#include "alchemy/darwin_locale.hpp"
#include "alchemy/darwin_libsystem_extra.hpp"
#include "alchemy/darwin/libsystem/malloc/malloc_zones.hpp"
#include "alchemy/darwin/libsystem/fs/symlink_records.hpp"
#include "alchemy/darwin/libsystem/errno.hpp"
#include "alchemy/host/crypto.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/host/net.hpp"
#include "alchemy/host/network_policy.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sections.hpp"
#include "alchemy/host/system.hpp"
#include "alchemy/host/sync.hpp"
#include "alchemy/host/time.hpp"
#include "alchemy/process_tree.hpp"
#include "alchemy/record_locks.hpp"
#include "alchemy/darwin_process.hpp"
#include "alchemy/darwin_support.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/darwin_vm_space.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/mach_port_events.hpp"
#include "alchemy/path.hpp"
#include "alchemy/hash_map.hpp"
#include "alchemy/perf.hpp"

#include <functional>

#include <algorithm>
#include <array>
#include <atomic>
#include <tuple>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>   // __cpuid: the host CPU's brand and hypervisor bit (sysctl)
#endif

namespace alchemy {
namespace services_detail {   // Shared by the DarwinServices translation units.

// Darwin's usleep/nanosleep wake within microseconds; a timer-resolution
// sleep (15.6 ms by default on Windows) made Blender's idle loop usleep(5000)
// take 14-16 ms. host::time::sleep_ns sleeps the requested time.
inline void precise_sleep_for(std::chrono::steady_clock::duration duration) {
    if (duration <= std::chrono::steady_clock::duration::zero()) return;
    host::time::sleep_ns(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()));
}

// Guest malloc bookkeeping: address -> block, sharded by address so
// malloc/free on different threads rarely share a lock and never take the
// services mutex (file operations hold it). Each shard is an open-addressing
// alchemy::hash_map (values only, no references kept across inserts).
class MallocTable {
public:
    struct Block {
        std::uint64_t size{};   // requested size (malloc_size)
        std::uint64_t base{};   // engine heap block to release (0: a mapped guest region)
    };
    void insert(std::uint64_t address, Block block) {
        auto& shard = shard_of(address);
        {
            std::lock_guard lock(shard.mutex);
            shard.blocks.insert_or_assign(address, block);
        }
        count_.fetch_add(1, std::memory_order_relaxed);
        const auto bytes = bytes_.fetch_add(block.size, std::memory_order_relaxed) + block.size;
        auto peak = peak_.load(std::memory_order_relaxed);
        while (bytes > peak && !peak_.compare_exchange_weak(peak, bytes, std::memory_order_relaxed)) {}
    }
    std::optional<Block> take(std::uint64_t address) {
        auto& shard = shard_of(address);
        Block block;
        {
            std::lock_guard lock(shard.mutex);
            const auto found = shard.blocks.find(address);
            if (found == shard.blocks.end()) return std::nullopt;
            block = found->second;
            shard.blocks.erase(found);
        }
        count_.fetch_sub(1, std::memory_order_relaxed);
        bytes_.fetch_sub(block.size, std::memory_order_relaxed);
        return block;
    }
    std::optional<Block> find(std::uint64_t address) const {
        const auto& shard = shard_of(address);
        std::lock_guard lock(shard.mutex);
        const auto found = shard.blocks.find(address);
        if (found == shard.blocks.end()) return std::nullopt;
        return found->second;
    }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t peak() const noexcept { return peak_.load(std::memory_order_relaxed); }

private:
    struct Shard {
        mutable std::mutex mutex;
        hash_map<std::uint64_t, Block, address_hash> blocks;
    };
    static constexpr std::size_t shard_count = 32;
    Shard& shard_of(std::uint64_t address) { return shards_[((address >> 4) * 0x9e3779b97f4a7c15ULL) >> 59]; }
    const Shard& shard_of(std::uint64_t address) const { return shards_[((address >> 4) * 0x9e3779b97f4a7c15ULL) >> 59]; }
    std::array<Shard, shard_count> shards_;
    std::atomic<std::uint64_t> count_{}, bytes_{}, peak_{};
};

inline constexpr std::uint32_t eperm = 1, enoent = 2, eio = 5, ebadf = 9, enomem = 12,
    eacces = 13, efault = 14, eexist = 17, enotdir = 20, eisdir = 21, einval = 22,
    emfile = 24, enospc = 28, erange = 34, enosys = 78, enotempty = 66, eoverflow = 84;

inline constexpr std::size_t string_limit = 1024 * 1024;
inline constexpr std::uint64_t transfer_limit = 256ULL * 1024 * 1024;
// One malloc block. Larger than transfer_limit: installers read multi-GiB
// payload files into memory (an installer reads its 1.9 GB executable).
inline constexpr std::uint64_t allocation_limit = 4ULL * 1024 * 1024 * 1024;
inline constexpr std::uint32_t carry = 1U << 29;
inline constexpr std::string_view system_library = "/usr/lib/libSystem.B.dylib";

struct ServiceError { std::uint32_t value; };

// The host socket stack, started once; resolver threads count against it.
struct NetworkSession {
    std::atomic<unsigned> queries{};
    NetworkSession() {
        if (const auto started = host::net::initialize(); !started)
            throw std::runtime_error("Alchemy socket initialization failed: " + std::string(host::name(started.error())));
    }
};
struct NetworkSocket;
// A pseudo-terminal, /dev/ttys<number> (fs/terminal.cpp): a connected stream
// pair whose ends are the master and the slave. Its attributes are in the
// named section "alchemy-pty-<number>", which every process that has an end
// maps, so a child's tcsetattr is what the master's writes see.
struct TerminalShared {
    std::uint64_t iflag, oflag, cflag, lflag;   // struct termios (72 bytes)
    std::uint8_t cc[20], reserved[4];
    std::uint64_t ispeed, ospeed;
    std::uint16_t rows, columns, xpixel, ypixel;   // struct winsize
    std::uint32_t foreground;   // tcsetpgrp (0: none)
    std::uint32_t unlocked;     // unlockpt
};
struct Terminal {
    std::uint32_t number{};
    host::Handle section;
    TerminalShared* shared{};                  // the section's view, or `local`
    std::unique_ptr<TerminalShared> local;     // when the section is gone
    std::mutex gate;
    // Master side: the slave end until this process first opens /dev/ttys<number>.
    std::shared_ptr<NetworkSocket> unopened;
    ~Terminal();
};

struct NetworkSocket {
    std::shared_ptr<NetworkSession> session;
    // A pseudo-terminal end: the master (posix_openpt) or a slave.
    std::shared_ptr<Terminal> terminal;
    bool terminal_master{};
    std::mutex gate;
    host::Handle handle;   // a host::net socket
    std::atomic<unsigned> aliases{1};
    std::atomic<bool> closed{};
    std::atomic<std::uint32_t> status_flags{2};
    int domain{}, type{}, protocol{};
    bool listening{}, connecting{}, no_sigpipe{};
    std::atomic<bool> send_shut{};   // shutdown(SHUT_WR or SHUT_RDWR) on this socket
    // AF_UNIX: the guest pathnames of the bound (or listening) and the connected-to socket.
    std::string unix_local, unix_peer;
    std::chrono::microseconds receive_timeout{}, send_timeout{};
    std::uint64_t identity{};
    void close_handle() noexcept {
        if (!handle) return;
        host::net::close_socket(handle);
        handle = {};
    }
    void release_alias() noexcept {
        if (aliases.fetch_sub(1) != 1) return;
        closed = true;
        std::lock_guard lock(gate); close_handle();
    }
    ~NetworkSocket() { close_handle(); }
};

// The Darwin errno of a failed host call, by the table its call site used
// before the host layer (darwin/libsystem/errno.hpp): path and handle calls,
// or descriptor I/O that went through the C runtime.
inline std::uint32_t file_failure(host::Error error) { return libsystem::darwin_file_errno(error); }
inline std::uint32_t crt_failure(host::Error error) { return libsystem::darwin_crt_errno(error); }

inline std::uint32_t host_error(int error) {
    switch (error) {
    case 0: return 0;
    case EPERM: return eperm;
    case ENOENT: return enoent;
    case EACCES: return eacces;
    case EBADF: return ebadf;
    case ENOMEM: return enomem;
    case EEXIST: return eexist;
    case ENOTDIR: return enotdir;
    case EINVAL: return einval;
    case EMFILE: return emfile;
    case ENOSPC: return enospc;
    case ERANGE: return erange;
    case ENOTEMPTY: return enotempty;
    case EPIPE: return 32;
    default: return eio;
    }
}

// std::filesystem errors: host system errors by the path and handle table,
// generic ones as errno values.
inline std::uint32_t host_error(const std::error_code& error) {
    if (error.category() == std::system_category()) return file_failure(host::files::error(error));
    return host_error(error.default_error_condition().value());
}

// rename(2) of host paths: a file replaces an existing one even while that
// one is open elsewhere (the host's POSIX-semantics rename; Windows refuses
// MoveFileEx then with access denied). Directories and the rest keep
// std::filesystem::rename.
inline void rename_path(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    std::filesystem::rename(from, to, error);
    if (!error) return;
    const auto failure = host_error(error);
    std::error_code ignored;
    if (failure == 13 && !std::filesystem::is_directory(from, ignored)) {   // EACCES
        const auto bytes = from.u8string(), target = to.u8string();
        if (host::files::rename(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                                std::string(reinterpret_cast<const char*>(target.data()), target.size()), true)) return;
    }
    throw ServiceError{failure};
}

inline bool is_random_device(const std::filesystem::path& path) {
    return path == std::filesystem::path("/dev/urandom") || path == std::filesystem::path("/dev/random");
}

// /dev/null opens the host null device (NUL); the path never maps to C:\dev\null.
inline bool is_null_device(const std::filesystem::path& path) {
    return path == std::filesystem::path("/dev/null");
}

// Character devices of the guest devfs that stat, lstat and access know
// (macOS modes, owners and device numbers). /dev/console belongs to the
// console user: this process's real user. /dev/console and /dev/tty have no
// host counterpart to open (ENXIO, as for a process without a controlling
// terminal). The random devices stay read-only (writes are refused).
struct DeviceNode { std::string_view path; std::uint16_t mode; bool console; std::uint32_t rdev, inode; };
inline constexpr std::array<DeviceNode, 5> device_nodes{{
    {"/dev/null", 020666, false, (3U << 24) | 2U, 0x4e554c4c},
    {"/dev/random", 020444, false, (14U << 24) | 0U, 0x524e4430},
    {"/dev/urandom", 020444, false, (14U << 24) | 1U, 0x55524e44},
    {"/dev/console", 020600, true, 0U, 0x434f4e53},
    {"/dev/tty", 020666, false, 2U << 24, 0x54545920},
}};

inline const DeviceNode* device_node(const std::filesystem::path& path) {
    if (!path.has_root_directory() || path.has_root_name()) return nullptr;
    const auto text = path.generic_u8string();
    for (const auto& node : device_nodes)
        if (std::string_view(reinterpret_cast<const char*>(text.data()), text.size()) == node.path) return &node;
    return nullptr;
}

// /dev/ptmx and /dev/ttys<n>, the pseudo-terminal devices (fs/terminal.cpp).
inline bool is_pseudo_terminal_path(const std::filesystem::path& path) {
    if (!path.has_root_directory() || path.has_root_name()) return false;
    const auto text = path.generic_u8string();
    const std::string_view value(reinterpret_cast<const char*>(text.data()), text.size());
    constexpr std::string_view prefix = "/dev/ttys";
    if (value == "/dev/ptmx") return true;
    if (!value.starts_with(prefix) || value.size() == prefix.size() || value.size() > prefix.size() + 6) return false;
    return value.substr(prefix.size()).find_first_not_of("0123456789") == std::string_view::npos;
}

inline bool is_terminal_device(const std::filesystem::path& path) {
    const auto* node = device_node(path);
    return node && (node->path == "/dev/console" || node->path == "/dev/tty");
}


inline void cryptographic_random(std::span<std::uint8_t> bytes) {
    if (bytes.empty()) return;
    if (!host::crypto::random(std::as_writable_bytes(bytes))) throw ServiceError{eio};
}

inline std::uint64_t add(std::uint64_t address, std::uint64_t size) {
    if (size > UINT64_MAX - address) throw ServiceError{efault};
    return address + size;
}

inline void validate(const transmute::GuestMemory& memory, std::uint64_t address,
              std::uint64_t size, bool write) {
    if (size > UINT64_MAX - address) throw ServiceError{efault};
    while (size) {
        const auto region = memory.query_region(address);
        if (!region || (write ? !region->writable : !region->readable) ||
            address < region->base || address - region->base >= region->size)
            throw ServiceError{efault};
        const auto count = std::min(size, region->size - (address - region->base));
        address += count;
        size -= count;
    }
}

inline void random_guest_bytes(transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t size) {
    validate(memory, address, size, true);
    std::array<std::uint8_t, 65536> bytes{};
    std::uint64_t written = 0;
    while (written < size) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size - written, bytes.size()));
        cryptographic_random(std::span(bytes.data(), count));
        memory.write_bytes(address + written, std::span(bytes.data(), count)); written += count;
    }
}

inline std::uint64_t device_stat(transmute::GuestMemory& memory, std::uint64_t result, const DeviceNode& node, std::uint32_t console_user) {
    validate(memory, result, 144, true); memory.fill_bytes(result, 0, 144);
    // A stable, process-independent identity in the Alchemy virtual device
    // namespace; no Windows handle or address appears in it.
    memory.write32(result, 0x414c43);                                  // st_dev
    memory.write32(result + 4, node.mode | (1U << 16));             // st_mode, st_nlink
    memory.write64(result + 8, 0x414c434400000000ULL | node.inode);   // st_ino
    memory.write32(result + 16, node.console ? console_user : 0);     // st_uid: root, or the console user
    memory.write32(result + 20, node.console ? 4U : 0U);              // st_gid: wheel, or tty
    memory.write32(result + 24, node.rdev);                           // st_rdev
    memory.write32(result + 112, 4096);                               // st_blksize
    return 0;
}

// /dev/random or /dev/urandom (one identity per node for every open).
inline std::uint64_t random_device_stat(transmute::GuestMemory& memory, std::uint64_t result, const std::filesystem::path& name) {
    const auto* node = device_node(name);
    return device_stat(memory, result, node ? *node : device_nodes[2], 0);
}

inline std::string temporary_suffix() {
    constexpr std::string_view alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string value;
    while (value.size() < 6) {
        std::array<std::uint8_t, 16> bytes{}; cryptographic_random(bytes);
        for (const auto byte : bytes) {
            if (byte < 248) value += alphabet[byte % 62];
            if (value.size() == 6) break;
        }
    }
    return value;
}

// Bytes to read at `first` and `second` in one chunk: at most `remaining`
// and `maximum`, and never past the end of either address's 4 KiB page
// (a byte-wise loop reads those pages anyway, so no new fault is possible).
inline std::size_t page_chunk(std::uint64_t first, std::uint64_t second, std::uint64_t remaining, std::size_t maximum) {
    const auto room = std::min<std::uint64_t>(4096 - (first & 4095), 4096 - (second & 4095));
    return static_cast<std::size_t>(std::min<std::uint64_t>({remaining, room, maximum}));
}

// A chunked read that may extend past the bytes a byte-wise loop would
// read: false when any of it is unmapped (the reference engine maps exact
// sizes), and the caller then repeats the chunk byte by byte.
inline bool chunk_read(const transmute::GuestMemory& memory, std::uint64_t address, std::span<std::uint8_t> bytes) {
    try { memory.read_bytes(address, bytes); return true; }
    catch (const std::exception&) { return false; }
}

inline std::string text(const transmute::GuestMemory& memory, std::uint64_t address,
                 std::size_t limit = string_limit) {
    bool terminated = false;
    std::vector<std::uint8_t> bytes;
    try { bytes = memory.read_string(address, 1, limit, &terminated); }
    catch (const std::runtime_error&) { throw ServiceError{efault}; }
    if (!terminated) throw std::runtime_error("Darwin string exceeds its checked length limit");
    return std::string(bytes.begin(), bytes.end());
}

inline void put_text(transmute::GuestMemory& memory, std::uint64_t address,
              std::string_view value) {
    memory.write_bytes(address, std::span(
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    memory.write8(add(address, value.size()), 0);
}

inline std::string path_text(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

inline std::size_t bounded_size(std::uint64_t size) {
    if (size > transfer_limit) throw ServiceError{einval};
    return static_cast<std::size_t>(size);
}

inline std::uint64_t page_round(std::uint64_t size) {
    constexpr auto mask = darwin_arm64_page_size - 1;
    if (size > UINT64_MAX - mask) throw ServiceError{einval};
    return (size + mask) & ~mask;
}

// Darwin sysctl values of Alchemy's virtual arm64 Mac (darwin_version.hpp:
// macOS 14.0, kernel 23.0.0).
// Integers keep their Darwin width; strings include the terminating NUL.
struct SysctlValue { std::vector<std::uint8_t> bytes; bool integer{}; };
inline SysctlValue sysctl_integer(std::uint64_t value, unsigned width) {
    SysctlValue result; result.integer = true;
    for (unsigned index = 0; index < width; ++index) result.bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
    return result;
}
inline SysctlValue sysctl_string(std::string_view value) {
    SysctlValue result; result.bytes.assign(value.begin(), value.end()); result.bytes.push_back(0);
    return result;
}
inline std::uint64_t physical_memory() {
    if (const auto memory = host::system::memory()) return memory->physical;
    return 8ULL << 30;
}
inline std::string host_cpu_brand() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    std::array<int, 4> registers{};
    __cpuid(registers.data(), static_cast<int>(0x80000000U));
    if (static_cast<unsigned>(registers[0]) >= 0x80000004U) {
        std::string brand;
        for (unsigned leaf = 0x80000002U; leaf <= 0x80000004U; ++leaf) {
            __cpuid(registers.data(), static_cast<int>(leaf));
            brand.append(reinterpret_cast<const char*>(registers.data()), 16);
        }
        brand.erase(std::find(brand.begin(), brand.end(), '\0'), brand.end());
        while (!brand.empty() && brand.back() == ' ') brand.pop_back();
        while (!brand.empty() && brand.front() == ' ') brand.erase(brand.begin());
        if (!brand.empty()) return brand;
    }
#endif
    return "Alchemy virtual arm64 processor";
}
inline std::optional<SysctlValue> sysctl_named(std::string_view name) {
    const auto cpus = static_cast<std::uint64_t>(std::max(1U, std::thread::hardware_concurrency()));
    if (name == "hw.pagesize" || name == "vm.pagesize") return sysctl_integer(darwin_arm64_page_size, name == "vm.pagesize" ? 4 : 8);
    if (name == "hw.cachelinesize") return sysctl_integer(128, 8);
    if (name == "hw.l1icachesize") return sysctl_integer(131072, 8);
    if (name == "hw.l1dcachesize") return sysctl_integer(65536, 8);
    if (name == "hw.l2cachesize") return sysctl_integer(4ULL << 20, 8);
    if (name == "hw.memsize" || name == "hw.memsize_usable") return sysctl_integer(physical_memory(), 8);
    if (name == "hw.ncpu" || name == "hw.activecpu" || name == "hw.physicalcpu" || name == "hw.physicalcpu_max" ||
        name == "hw.logicalcpu" || name == "hw.logicalcpu_max" || name == "hw.perflevel0.physicalcpu" ||
        name == "hw.perflevel0.physicalcpu_max" || name == "hw.perflevel0.logicalcpu" ||
        name == "hw.perflevel0.logicalcpu_max" || name == "machdep.cpu.core_count" || name == "machdep.cpu.thread_count")
        return sysctl_integer(cpus, 4);
    if (name == "hw.nperflevels" || name == "hw.packages") return sysctl_integer(1, 4);
    if (name == "hw.perflevel0.name") return sysctl_string("Performance");
    if (name == "hw.byteorder") return sysctl_integer(1234, 4);
    if (name == "hw.cputype") return sysctl_integer(0x0100000c, 4); // CPU_TYPE_ARM64
    if (name == "hw.cpusubtype") return sysctl_integer(0, 4);       // CPU_SUBTYPE_ARM64_ALL
    if (name == "hw.cpu64bit_capable") return sysctl_integer(1, 4);
    if (name == "hw.machine") return sysctl_string("arm64");
    if (name == "hw.model") return sysctl_string("AlchemyVirtual1,1");
    if (name == "machdep.cpu.brand_string") return sysctl_string(host_cpu_brand());
    if (name == "hw.optional.neon" || name == "hw.optional.AdvSIMD" || name == "hw.optional.floatingpoint" ||
        name == "hw.optional.arm64") return sysctl_integer(1, 4);
    // FEAT_CRC32 (CRC32{B,H,W,X}, CRC32C*) is part of every Apple arm64 CPU and
    // both engines execute it; Qt Core refuses to start without it.
    if (name == "hw.optional.armv8_crc32" || name == "hw.optional.arm.FEAT_CRC32") return sysctl_integer(1, 4);
    // Other optional ISA features are reported absent so guests keep baseline code paths.
    if (name.starts_with("hw.optional.")) return sysctl_integer(0, 4);
    if (name == "kern.ostype") return sysctl_string("Darwin");
    if (name == "kern.osrelease") return sysctl_string(darwin_release);
    if (name == "kern.osversion") return sysctl_string(darwin_build);
    if (name == "kern.osproductversion") return sysctl_string(macos_product_version);
    if (name == "kern.version") return sysctl_string(darwin_kernel_version);
    if (name == "kern.hostname") return sysctl_string("alchemy");
    if (name == "kern.argmax") return sysctl_integer(1024 * 1024, 4);
    if (name == "kern.osrevision") return sysctl_integer(199506, 4);
    if (name == "kern.maxfilesperproc" || name == "kern.maxfiles") return sysctl_integer(10240, 4);
    if (name == "sysctl.proc_translated") return sysctl_integer(0, 4);
    // kern.hv_vmm_present: 1 when the kernel runs under a hypervisor. The guest
    // runs on the host's kernel, so this is the host CPU's hypervisor-present bit
    // (CPUID leaf 1, ECX bit 31); 0 where it cannot be read.
    if (name == "kern.hv_vmm_present") {
        std::uint64_t present = 0;
#if defined(_WIN32) && (defined(_M_X64) || defined(_M_IX86))
        std::array<int, 4> registers{};
        __cpuid(registers.data(), 1);
        present = (static_cast<std::uint32_t>(registers[2]) >> 31) & 1U;
#endif
        return sysctl_integer(present, 4);
    }
    return std::nullopt;
}
// Fixed Darwin MIB numbers; int widths follow the numeric interface.
struct SysctlMib { int top, second; std::string_view name; unsigned width; };
inline constexpr std::array<SysctlMib, 17> sysctl_mibs{{
    {6, 1, "hw.machine", 0}, {6, 2, "hw.model", 0}, {6, 3, "hw.ncpu", 4}, {6, 4, "hw.byteorder", 4},
    {6, 7, "hw.pagesize", 4}, {6, 16, "hw.cachelinesize", 4}, {6, 17, "hw.l1icachesize", 4},
    {6, 18, "hw.l1dcachesize", 4}, {6, 20, "hw.l2cachesize", 4}, {6, 24, "hw.memsize", 8},
    {6, 25, "hw.activecpu", 4}, {1, 1, "kern.ostype", 0}, {1, 2, "kern.osrelease", 0},
    {1, 4, "kern.version", 0}, {1, 8, "kern.argmax", 4}, {1, 10, "kern.hostname", 0}, {1, 65, "kern.osversion", 0}}};
// Names without fixed numbers get private MIBs {sysctl_private, index}.
inline constexpr int sysctl_private = 0x414c;
inline constexpr std::array<std::string_view, 27> sysctl_private_names{
    "hw.physicalcpu", "hw.physicalcpu_max", "hw.logicalcpu", "hw.logicalcpu_max", "hw.perflevel0.physicalcpu",
    "hw.perflevel0.logicalcpu", "hw.nperflevels", "hw.cputype", "hw.cpusubtype", "hw.optional.neon",
    "hw.optional.AdvSIMD", "hw.optional.floatingpoint", "hw.optional.arm64", "hw.optional.armv8_crc32", "machdep.cpu.brand_string",
    "machdep.cpu.core_count", "machdep.cpu.thread_count", "kern.osproductversion", "hw.memsize_usable",
    "hw.packages", "hw.cpu64bit_capable", "kern.maxfilesperproc", "kern.maxfiles", "sysctl.proc_translated", "kern.osrevision",
    "hw.optional.arm.FEAT_CRC32", "kern.hv_vmm_present"};

inline std::uint64_t sysctl_output(transmute::GuestMemory& memory, const SysctlValue& value, std::uint64_t output,
    std::uint64_t length, std::uint64_t replacement, std::uint64_t replacement_size) {
    if (replacement || replacement_size) throw ServiceError{eperm};
    if (!length) { if (output) throw ServiceError{einval}; return 0; }
    validate(memory, length, 8, true);
    const auto capacity = memory.read64(length);
    const auto size = static_cast<std::uint64_t>(value.bytes.size());
    if (!output) { memory.write64(length, size); return 0; }
    // A 4-byte buffer receives a 64-bit integer that fits, as Darwin's integer handlers allow.
    std::uint64_t number{};
    for (std::size_t index = value.bytes.size(); index-- > 0;) number = (number << 8) | value.bytes[index];
    if (value.integer && size == 8 && capacity == 4 && number <= UINT32_MAX) {
        validate(memory, output, 4, true); memory.write32(output, static_cast<std::uint32_t>(number)); memory.write64(length, 4); return 0;
    }
    const auto count = std::min(capacity, size);
    validate(memory, output, count, true);
    memory.write_bytes(output, std::span(value.bytes.data(), static_cast<std::size_t>(count)));
    memory.write64(length, size);
    if (capacity < size) throw ServiceError{enomem};
    return 0;
}

// struct kinfo_proc (648 bytes, arm64): extern_proc kp_proc at 0, eproc
// kp_eproc at 296.
inline std::vector<std::uint8_t> kinfo_proc(const process_tree::Record& record) {
    std::vector<std::uint8_t> bytes(648, 0);
    const auto put = [&](std::size_t offset, auto value) { std::memcpy(bytes.data() + offset, &value, sizeof value); };
    put(0, record.start_seconds);                         // p_un.__p_starttime.tv_sec
    put(8, static_cast<std::int32_t>(record.start_microseconds));   // tv_usec
    put(32, std::int32_t{0x4});                           // p_flag: P_LP64
    bytes[36] = 2;                                        // p_stat: SRUN
    put(40, static_cast<std::int32_t>(record.pid));       // p_pid
    put(44, static_cast<std::int32_t>(record.ppid));      // p_oppid
    std::memcpy(bytes.data() + 243, record.name.data(), std::min<std::size_t>(record.name.size(), 16));   // p_comm[17]
    put(296 + 16 + 80, record.uid);                       // e_pcred.p_ruid
    put(296 + 16 + 84, record.uid);                       // p_svuid
    put(296 + 16 + 88, record.gid);                       // p_rgid
    put(296 + 16 + 92, record.gid);                       // p_svgid
    put(296 + 120 + 4, record.uid);                       // e_ucred.cr_uid
    put(296 + 120 + 8, std::int16_t{1});                  // cr_ngroups
    put(296 + 120 + 12, record.gid);                      // cr_groups[0]
    put(560, static_cast<std::int32_t>(record.ppid));     // e_ppid
    put(564, static_cast<std::int32_t>(record.pgid));     // e_pgid
    put(296 + 276, std::int32_t{-1});                     // e_tdev: NODEV
    return bytes;
}

inline std::uint64_t numeric_sysctl(transmute::CpuState& state, transmute::GuestMemory& memory) {
    const auto count = state.x[1];
    if (count < 2 || count > 12) throw ServiceError{einval};
    validate(memory, state.x[0], count * 4, false);
    std::array<std::int32_t, 12> mib{};
    for (std::uint64_t index = 0; index < count; ++index) mib[index] = static_cast<std::int32_t>(memory.read32(state.x[0] + index * 4));
    if (count == 4 && mib[0] == 1 && mib[1] == 14 && mib[2] == 1) {
        // KERN_PROC_PID: the kinfo_proc of a process of the Alchemy tree
        // (process_tree.hpp), never traced; nothing (length 0) for another pid.
        SysctlValue value;
        if (const auto record = process_tree::lookup(static_cast<std::uint32_t>(mib[3]))) value.bytes = kinfo_proc(*record);
        return sysctl_output(memory, value, state.x[2], state.x[3], state.x[4], state.x[5]);
    }
    if (count != 2) throw ServiceError{enoent};
    if (mib[0] == sysctl_private && mib[1] >= 0 && static_cast<std::size_t>(mib[1]) < sysctl_private_names.size())
        return sysctl_output(memory, *sysctl_named(sysctl_private_names[static_cast<std::size_t>(mib[1])]), state.x[2], state.x[3], state.x[4], state.x[5]);
    for (const auto& entry : sysctl_mibs) {
        if (entry.top != mib[0] || entry.second != mib[1]) continue;
        auto value = *sysctl_named(entry.name);
        if (entry.width && value.integer) {
            std::uint64_t number{};
            for (std::size_t index = value.bytes.size(); index-- > 0;) number = (number << 8) | value.bytes[index];
            value = sysctl_integer(number, entry.width);
        }
        return sysctl_output(memory, value, state.x[2], state.x[3], state.x[4], state.x[5]);
    }
    throw ServiceError{enoent};
}

inline std::uint64_t sysctl_name_to_mib(transmute::GuestMemory& memory, std::string_view name, std::uint64_t output, std::uint64_t length) {
    validate(memory, length, 8, true);
    const auto capacity = memory.read64(length);
    std::array<std::int32_t, 2> mib{};
    bool found{};
    for (const auto& entry : sysctl_mibs) if (entry.name == name) { mib = {entry.top, entry.second}; found = true; break; }
    for (std::size_t index = 0; !found && index < sysctl_private_names.size(); ++index)
        if (sysctl_private_names[index] == name) { mib = {sysctl_private, static_cast<std::int32_t>(index)}; found = true; }
    if (!found) throw ServiceError{enoent};
    if (capacity < 2) throw ServiceError{enomem};
    validate(memory, output, 8, true);
    memory.write32(output, static_cast<std::uint32_t>(mib[0])); memory.write32(output + 4, static_cast<std::uint32_t>(mib[1]));
    memory.write64(length, 2);
    return 0;
}

inline std::uint64_t clock_nanoseconds(std::uint64_t clock) {
    if (clock == 0) return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if (clock != 4 && clock != 6 && clock != 8) throw ServiceError{einval};
    // Darwin continuous clocks include suspension; uptime-raw excludes it.
    return clock == 8 ? host::time::monotonic_ns() : host::time::continuous_ns();
}

// An open file's size; `failure` is the call site's table (descriptor I/O by default).
inline std::uint64_t file_size(host::Handle file, std::uint32_t (*failure)(host::Error) = crt_failure) {
    const auto size = host::files::size(file);
    if (!size) throw ServiceError{failure(size.error())};
    return *size;
}

struct HostMetadata {
    std::uint64_t inode{}, size{}, allocated{}, links{};
    std::uint32_t device{};
    bool directory{}, symlink{}, private_owner{}, read_only{};
    std::array<std::array<std::uint64_t, 2>, 4> times{};   // access, modification, change, birth: {seconds, nanoseconds}
};
// Unix-epoch nanoseconds as a Darwin timespec (seconds rounded down).
inline std::array<std::uint64_t, 2> darwin_time(std::int64_t nanoseconds) {
    auto seconds = nanoseconds / 1'000'000'000, remainder = nanoseconds % 1'000'000'000;
    if (remainder < 0) { --seconds; remainder += 1'000'000'000; }
    return {static_cast<std::uint64_t>(seconds), static_cast<std::uint64_t>(remainder)};
}
inline HostMetadata host_metadata(const host::Result<host::files::Metadata>& found) {
    if (!found) throw ServiceError{file_failure(found.error())};
    const auto& value = *found;
    HostMetadata result;
    result.inode = value.inode; result.size = value.size; result.allocated = value.allocated; result.links = value.links;
    result.device = static_cast<std::uint32_t>(value.device);
    result.directory = value.directory; result.symlink = value.symlink; result.private_owner = value.owner_only; result.read_only = value.read_only;
    result.times = {darwin_time(value.accessed_ns), darwin_time(value.modified_ns), darwin_time(value.changed_ns), darwin_time(value.created_ns)};
    return result;
}
// A path's metadata (nofollow: a final symbolic link's own), or an open file's.
inline HostMetadata host_metadata(const std::filesystem::path& path, bool nofollow = false) {
    return host_metadata(host::files::metadata(path_text(path), !nofollow));
}
inline HostMetadata host_metadata(host::Handle file) { return host_metadata(host::files::metadata(file)); }

// Parse the guest format and read Apple ARM64 stack slots ourselves. Only local
// host values enter snprintf; a guest pointer or va_list is never forwarded.
inline std::string format_text(const transmute::GuestMemory& memory, std::string_view input, std::uint64_t& arguments) {
    std::string output;
    const auto next = [&]() {
        const auto value = memory.read64(arguments);
        arguments = add(arguments, 8);
        return value;
    };
    const auto append = [&](std::string_view value) {
        if (value.size() > string_limit - output.size())
            throw std::runtime_error("Darwin formatted output exceeds 1 MiB");
        output.append(value);
    };
    for (std::size_t at = 0; at < input.size();) {
        if (input[at] != '%') { append(std::string_view(input.data() + at++, 1)); continue; }
        ++at;
        if (at == input.size()) throw std::runtime_error("Darwin printf has an incomplete conversion");
        if (input[at] == '%') { append("%"); ++at; continue; }
        std::string spec = "%";
        std::optional<std::size_t> string_precision;
        while (at < input.size() && std::string_view("-+ #0").find(input[at]) != std::string_view::npos)
            spec += input[at++];
        if (at < input.size() && input[at] == '*') {
            const auto width = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(next()));
            if (width < -static_cast<int>(string_limit) || width > static_cast<int>(string_limit))
                throw std::runtime_error("Darwin printf width exceeds limit");
            if (width < 0) spec += '-';
            spec += std::to_string(width < 0 ? -width : width);
            ++at;
        } else {
            while (at < input.size() && input[at] >= '0' && input[at] <= '9') spec += input[at++];
        }
        if (at < input.size() && input[at] == '.') {
            ++at;
            if (at < input.size() && input[at] == '*') {
                const auto precision = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(next()));
                if (precision > static_cast<int>(string_limit))
                    throw std::runtime_error("Darwin printf precision exceeds limit");
                if (precision >= 0) { spec += '.' + std::to_string(precision); string_precision = static_cast<std::size_t>(precision); }
                ++at;
            } else {
                spec += '.';
                const auto precision_begin = at;
                while (at < input.size() && input[at] >= '0' && input[at] <= '9') spec += input[at++];
                std::size_t precision = 0;
                if (at != precision_begin) {
                    const auto converted = std::from_chars(input.data() + precision_begin, input.data() + at, precision);
                    if (converted.ec != std::errc{} || precision > string_limit)
                        throw std::runtime_error("Darwin printf precision exceeds limit");
                }
                string_precision = precision;
            }
        }
        std::string length;
        if (at < input.size() && std::string_view("hlztjL").find(input[at]) != std::string_view::npos) {
            length += input[at++];
            if (at < input.size() && (length == "h" || length == "l") && input[at] == length[0])
                length += input[at++];
        }
        if (at == input.size()) throw std::runtime_error("Darwin printf has an incomplete conversion");
        const auto conversion = input[at++];
        if ((length == "L" && std::string_view("eEfFgGaA").find(conversion) == std::string_view::npos) ||
            ((conversion == 's' || conversion == 'c') && !length.empty()) ||
            (std::string_view("eEfFgGaA").find(conversion) != std::string_view::npos && !length.empty() && length != "l" && length != "L") ||
            std::string_view("diuoxXcspeEfFgGaA").find(conversion) == std::string_view::npos)
            throw std::runtime_error(std::string("unsupported Darwin printf conversion %") + conversion);
        // A small stack buffer; a larger result is formatted again into a
        // buffer of its exact size (a 1 MiB zero-filled buffer per conversion
        // made every printf cost about 0.3-0.8 ms).
        char inline_buffer[512];
        std::string large;
        int count = 0;
        const char* buffer = inline_buffer;
        const auto render = [&](auto&& print) {
            count = print(inline_buffer, sizeof(inline_buffer));
            if (count >= 0 && static_cast<std::size_t>(count) >= sizeof(inline_buffer) && static_cast<std::size_t>(count) <= string_limit) {
                large.assign(static_cast<std::size_t>(count) + 1, '\0');
                count = print(large.data(), large.size());
                buffer = large.data();
            }
        };
        const auto value = next();
        if (conversion == 's') {
            std::string string;
            if (value == 0) string = "(null)";
            else if (string_precision) {
                const auto bytes = memory.read_string(value, 1, *string_precision);
                string.assign(bytes.begin(), bytes.end());
            } else string = text(memory, value);
            spec += 's';
            render([&](char* out, std::size_t size) { return std::snprintf(out, size, spec.c_str(), string.c_str()); });
        } else if (conversion == 'c') {
            spec += 'c';
            render([&](char* out, std::size_t size) { return std::snprintf(out, size, spec.c_str(), static_cast<int>(value & 255)); });
        } else if (conversion == 'p') {
            if (spec != "%" || !length.empty())
                throw std::runtime_error("unsupported Darwin printf pointer modifiers");
            render([&](char* out, std::size_t size) { return std::snprintf(out, size, "0x%llx", static_cast<unsigned long long>(value)); });
        } else if (std::string_view("eEfFgGaA").find(conversion) != std::string_view::npos) {
            spec += conversion;
            render([&](char* out, std::size_t size) { return std::snprintf(out, size, spec.c_str(), std::bit_cast<double>(value)); });
        } else {
            const auto wide = length == "l" || length == "ll" || length == "z" || length == "t" || length == "j";
            spec += "ll";
            spec += conversion;
            std::uint64_t integer = wide ? value : static_cast<std::uint32_t>(value);
            if (length == "hh") integer = static_cast<std::uint8_t>(value);
            if (length == "h") integer = static_cast<std::uint16_t>(value);
            if (conversion == 'd' || conversion == 'i') {
                std::int64_t signed_value = wide ? std::bit_cast<std::int64_t>(integer) :
                    std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(integer));
                if (length == "hh") signed_value = std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(integer));
                if (length == "h") signed_value = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(integer));
                render([&](char* out, std::size_t size) { return std::snprintf(out, size, spec.c_str(), static_cast<long long>(signed_value)); });
            } else render([&](char* out, std::size_t size) { return std::snprintf(out, size, spec.c_str(), static_cast<unsigned long long>(integer)); });
        }
        if (count < 0 || static_cast<std::size_t>(count) > string_limit)
            throw std::runtime_error("Darwin formatted output exceeds 1 MiB");
        append(std::string_view(buffer, static_cast<std::size_t>(count)));
    }
    return output;
}

inline std::string format(const transmute::GuestMemory& memory, std::uint64_t format_address,
                   std::uint64_t arguments) {
    const auto input = text(memory, format_address);
    return format_text(memory, input, arguments);
}

// UTF-8 -> code points (invalid sequences become U+FFFD).
inline std::u32string utf8_to_utf32(std::string_view text) {
    std::u32string out;
    for (std::size_t at = 0; at < text.size();) {
        const auto byte = static_cast<unsigned char>(text[at]);
        char32_t value = 0xFFFD;
        std::size_t length = 1;
        if (byte < 0x80) value = byte;
        else if ((byte >> 5) == 6) { length = 2; value = byte & 0x1F; }
        else if ((byte >> 4) == 14) { length = 3; value = byte & 0x0F; }
        else if ((byte >> 3) == 30) { length = 4; value = byte & 0x07; }
        else { out.push_back(0xFFFD); ++at; continue; }
        if (at + length > text.size()) { out.push_back(0xFFFD); break; }
        for (std::size_t k = 1; k < length; ++k) {
            const auto following = static_cast<unsigned char>(text[at + k]);
            if ((following >> 6) != 2) { value = 0xFFFD; length = k; break; }
            value = (value << 6) | (following & 0x3F);
        }
        out.push_back(value);
        at += length;
    }
    return out;
}

inline std::string utf32_to_utf8(std::u32string_view text) {
    std::string out;
    for (const auto c : text) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// Wide printf (swprintf, vswprintf, fwprintf) over a UTF-32 guest format
// (Darwin wchar_t is 32-bit). Literal text may be any Unicode. %ls/%S and
// %lc/%C take wchar_t strings/characters; %s takes a char string, decoded as
// UTF-8 (the guest's multibyte encoding); %c a char. Width and precision of
// string conversions count characters. Every other conversion is the narrow
// printf's (format_text) with the same Apple ARM64 stack-slot arguments.
inline std::u32string wide_format_text(const transmute::GuestMemory& memory, std::uint64_t format_address, std::uint64_t arguments) {
    std::u32string input;
    for (std::uint64_t at = 0;; ++at) {
        if (at == string_limit) throw std::runtime_error("Darwin wide printf format exceeds limit");
        const auto value = memory.read32(add(format_address, at * 4));
        if (!value) break;
        input.push_back(static_cast<char32_t>(value));
    }
    std::u32string output;
    const auto append = [&](std::u32string_view value) {
        if (value.size() > string_limit - output.size()) throw std::runtime_error("Darwin formatted output exceeds 1 MiB");
        output.append(value);
    };
    const auto next = [&]() { const auto value = memory.read64(arguments); arguments = add(arguments, 8); return value; };
    for (std::size_t at = 0; at < input.size();) {
        if (input[at] != U'%') { append(std::u32string_view(input.data() + at++, 1)); continue; }
        ++at;
        if (at == input.size()) throw std::runtime_error("Darwin wide printf has an incomplete conversion");
        if (input[at] == U'%') { append(U"%"); ++at; continue; }
        // The specification up to the conversion character (ASCII only).
        std::string spec = "%";
        bool left = false;
        std::optional<std::int64_t> width, precision;
        std::uint64_t peek = arguments;   // '*' values (format_text reads them again for narrow conversions)
        const auto read_star = [&]() {
            const auto v = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.read64(peek)));
            peek = add(peek, 8);
            return v;
        };
        while (at < input.size() && std::u32string_view(U"-+ #0").find(input[at]) != std::u32string_view::npos) {
            if (input[at] == U'-') left = true;
            spec += static_cast<char>(input[at++]);
        }
        if (at < input.size() && input[at] == U'*') {
            const auto w = read_star();
            if (w < 0) { left = true; width = -static_cast<std::int64_t>(w); } else width = w;
            spec += static_cast<char>(input[at++]);
        } else {
            std::int64_t w = -1;
            while (at < input.size() && input[at] >= U'0' && input[at] <= U'9') {
                w = (w < 0 ? 0 : w) * 10 + (input[at] - U'0');
                if (w > static_cast<std::int64_t>(string_limit)) throw std::runtime_error("Darwin printf width exceeds limit");
                spec += static_cast<char>(input[at++]);
            }
            if (w >= 0) width = w;
        }
        if (at < input.size() && input[at] == U'.') {
            spec += static_cast<char>(input[at++]);
            if (at < input.size() && input[at] == U'*') {
                const auto p = read_star();
                if (p >= 0) precision = p;
                spec += static_cast<char>(input[at++]);
            } else {
                std::int64_t p = 0;
                while (at < input.size() && input[at] >= U'0' && input[at] <= U'9') {
                    p = p * 10 + (input[at] - U'0');
                    if (p > static_cast<std::int64_t>(string_limit)) throw std::runtime_error("Darwin printf precision exceeds limit");
                    spec += static_cast<char>(input[at++]);
                }
                precision = p;
            }
        }
        std::string length;
        while (at < input.size() && std::u32string_view(U"hlztjLq").find(input[at]) != std::u32string_view::npos)
            length += static_cast<char>(input[at++]);
        if (at == input.size()) throw std::runtime_error("Darwin wide printf has an incomplete conversion");
        const auto conversion = input[at++];
        const bool wide_string = (conversion == U's' && length == "l") || conversion == U'S';
        const bool wide_char = (conversion == U'c' && length == "l") || conversion == U'C';
        if (wide_string || wide_char || conversion == U's' || conversion == U'c') {
            if (!wide_string && !wide_char && !length.empty())
                throw std::runtime_error("unsupported Darwin wide printf conversion length " + length);
            arguments = peek;   // past the '*' slots
            const auto value = next();
            std::u32string text_value;
            if (wide_string) {
                if (!value) {
                    text_value = U"(null)";
                } else {
                    for (std::uint64_t i = 0;; ++i) {
                        if (precision && static_cast<std::int64_t>(i) >= *precision) break;
                        if (i == string_limit) throw std::runtime_error("Darwin wide printf string exceeds limit");
                        const auto c = memory.read32(add(value, i * 4));
                        if (!c) break;
                        text_value.push_back(static_cast<char32_t>(c));
                    }
                }
            } else if (wide_char) {
                text_value.push_back(static_cast<char32_t>(static_cast<std::uint32_t>(value)));
            } else if (conversion == U's') {
                text_value = value ? utf8_to_utf32(text(memory, value)) : std::u32string(U"(null)");
                if (precision && static_cast<std::int64_t>(text_value.size()) > *precision)
                    text_value.resize(static_cast<std::size_t>(*precision));
            } else {
                text_value.push_back(static_cast<char32_t>(value & 255));
            }
            const auto pad = width && *width > static_cast<std::int64_t>(text_value.size())
                                 ? static_cast<std::size_t>(*width - static_cast<std::int64_t>(text_value.size())) : 0;
            if (pad > string_limit) throw std::runtime_error("Darwin printf width exceeds limit");
            if (!left) append(std::u32string(pad, U' '));
            append(text_value);
            if (left) append(std::u32string(pad, U' '));
            continue;
        }
        if (conversion > 127) throw std::runtime_error("unsupported Darwin wide printf conversion");
        if (conversion == U'n') throw std::runtime_error("unsupported Darwin printf conversion %n");
        spec += length;
        spec += static_cast<char>(conversion);
        const auto narrow = format_text(memory, spec, arguments);
        std::u32string converted;
        for (const auto c : narrow) converted.push_back(static_cast<unsigned char>(c));
        append(converted);
    }
    return output;
}

inline bool space(char character) { return character == ' ' || (character >= '\t' && character <= '\r'); }

inline std::uint64_t scan(transmute::GuestMemory& memory, std::uint64_t input_address,
                   std::uint64_t format_address, std::uint64_t arguments) {
    const auto input = text(memory, input_address), specification = text(memory, format_address);
    std::size_t position = 0, assigned = 0;
    const auto store_integer = [&](std::uint64_t address, std::uint64_t value, std::string_view length) {
        if (length == "hh") memory.write8(address, static_cast<std::uint8_t>(value));
        else if (length == "h") { memory.write8(address, static_cast<std::uint8_t>(value)); memory.write8(address + 1, static_cast<std::uint8_t>(value >> 8)); }
        else if (length == "l" || length == "ll" || length == "j" || length == "z" || length == "t") memory.write64(address, value);
        else if (length.empty()) memory.write32(address, static_cast<std::uint32_t>(value));
        else throw std::runtime_error("unsupported Darwin scanf integer length");
    };
    for (std::size_t at = 0; at < specification.size();) {
        if (space(specification[at])) { while (at < specification.size() && space(specification[at])) ++at; while (position < input.size() && space(input[position])) ++position; continue; }
        if (specification[at] != '%') { if (position == input.size() || specification[at] != input[position]) break; ++at; ++position; continue; }
        ++at;
        if (at == specification.size()) throw std::runtime_error("incomplete Darwin scanf conversion");
        if (specification[at] == '%') { if (position == input.size() || input[position] != '%') break; ++at; ++position; continue; }
        const bool suppress = specification[at] == '*';
        if (suppress) ++at;
        const auto width_begin = at;
        while (at < specification.size() && specification[at] >= '0' && specification[at] <= '9') ++at;
        std::size_t width = string_limit;
        if (at != width_begin) {
            const auto value = std::from_chars(specification.data() + width_begin, specification.data() + at, width);
            if (value.ec != std::errc{} || width == 0 || width > string_limit) throw std::runtime_error("Darwin scanf field width exceeds limit");
        }
        std::string length;
        if (at < specification.size() && std::string_view("hljztL").find(specification[at]) != std::string_view::npos) {
            length += specification[at++];
            if (at < specification.size() && (length == "h" || length == "l") && length[0] == specification[at]) length += specification[at++];
        }
        if (at == specification.size()) throw std::runtime_error("incomplete Darwin scanf conversion");
        const auto conversion = specification[at++];
        if (conversion == '[') {
            // Scan set: [chars], [^chars], with ']' first taken literally and
            // a-b ranges (FreeBSD scanf semantics).
            std::array<bool, 256> member{};
            const bool negate = at < specification.size() && specification[at] == '^';
            if (negate) ++at;
            const auto set_begin = at;
            for (; at < specification.size() && (specification[at] != ']' || at == set_begin); ++at) {
                const auto first = static_cast<unsigned char>(specification[at]);
                if (at + 2 < specification.size() && specification[at + 1] == '-' && specification[at + 2] != ']' &&
                    static_cast<unsigned char>(specification[at + 2]) >= first) {
                    for (unsigned value = first; value <= static_cast<unsigned char>(specification[at + 2]); ++value) member[value] = true;
                    at += 2;
                } else member[first] = true;
            }
            if (at == specification.size()) throw std::runtime_error("incomplete Darwin scanf scan set");
            ++at; // ']'
            if (!length.empty()) throw std::runtime_error("unsupported Darwin scanf wide conversion");
            std::uint64_t destination = 0;
            if (!suppress) { destination = memory.read64(arguments); arguments = add(arguments, 8); }
            if (position == input.size()) return assigned == 0 ? UINT32_MAX : assigned;
            std::size_t count = 0;
            while (count < width && position + count < input.size() &&
                   member[static_cast<unsigned char>(input[position + count])] != negate) ++count;
            if (!count) break;
            if (!suppress) {
                memory.write_bytes(destination, std::span(reinterpret_cast<const std::uint8_t*>(input.data() + position), count));
                memory.write8(add(destination, count), 0);
                ++assigned;
            }
            position += count;
            continue;
        }
        if (std::string_view("diuoxXfFeEgGaAcsn").find(conversion) == std::string_view::npos)
            throw std::runtime_error(std::string("unsupported Darwin scanf conversion %") + conversion);
        std::uint64_t destination = 0;
        if (!suppress) { destination = memory.read64(arguments); arguments = add(arguments, 8); }
        if (conversion == 'n') { if (!suppress) store_integer(destination, position, length); continue; }
        if (conversion != 'c') while (position < input.size() && space(input[position])) ++position;
        if (position == input.size()) return assigned == 0 ? UINT32_MAX : assigned;
        if (conversion == 's' || conversion == 'c') {
            if (!length.empty()) throw std::runtime_error("unsupported Darwin scanf wide conversion");
            const auto maximum = conversion == 'c' && at == width_begin + 1 ? 1 : width;
            std::size_t count = 0;
            while (count < maximum && position + count < input.size() && (conversion == 'c' || !space(input[position + count]))) ++count;
            if (!count || (conversion == 'c' && count != maximum)) break;
            if (!suppress) { memory.write_bytes(destination, std::span(reinterpret_cast<const std::uint8_t*>(input.data() + position), count)); if (conversion == 's') memory.write8(add(destination, count), 0); ++assigned; }
            position += count;
            continue;
        }
        const auto token = input.substr(position, std::min(width, input.size() - position));
        char* end = nullptr;
        if (std::string_view("fFeEgGaA").find(conversion) != std::string_view::npos) {
            if (!length.empty() && length != "l" && length != "L") throw std::runtime_error("unsupported Darwin scanf float length");
            const auto number = std::strtod(token.c_str(), &end);
            if (end == token.c_str()) break;
            if (!suppress) { if (length.empty()) memory.write32(destination, std::bit_cast<std::uint32_t>(static_cast<float>(number))); else memory.write64(destination, std::bit_cast<std::uint64_t>(number)); ++assigned; }
        } else {
            const auto base = conversion == 'i' ? 0 : (conversion == 'o' ? 8 : (conversion == 'x' || conversion == 'X' ? 16 : 10));
            const auto number = conversion == 'd' || conversion == 'i' ? static_cast<unsigned long long>(std::strtoll(token.c_str(), &end, base)) : std::strtoull(token.c_str(), &end, base);
            if (end == token.c_str()) break;
            if (!suppress) { store_integer(destination, number, length); ++assigned; }
        }
        position += static_cast<std::size_t>(end - token.c_str());
    }
    return assigned;
}

// The Darwin errno of a failed host socket call (net/sockets.cpp,
// process/identity.cpp and process/process_model.cpp).
inline std::uint32_t network_error(host::Error error) { return libsystem::darwin_socket_errno(error); }

// A FILE not backed by a descriptor (fs/memory_streams.cpp: fmemopen, open_memstream,
// open_wmemstream, funopen). Each call moves bytes at a guest address and returns the
// count (short at the end), or throws ServiceError.
struct StreamBackend {
    virtual ~StreamBackend() = default;
    virtual std::uint64_t read(transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t size) = 0;
    virtual std::uint64_t write(transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t size) = 0;
    virtual std::uint64_t seek(transmute::GuestMemory& memory, std::int64_t offset, int whence) = 0;
    virtual void flush(transmute::GuestMemory&) {}
    virtual std::uint64_t close(transmute::GuestMemory& memory) = 0;
};
} // namespace services_detail
using namespace services_detail;
void remove_unix_socket_file(const std::filesystem::path& host);   // net/sockets.cpp

class DarwinServices::Impl {
public:
    explicit Impl(ProcessContext& value) : context(value), root(
        value.working_directory.empty() ? std::filesystem::current_path().root_path() : value.working_directory.root_path()) {}
    ~Impl() { context.map_guest_path = {}; context.shared_location = {}; context.child_exited = {}; context.export_descriptor = {}; context.import_descriptor = {}; }
    struct FlockOwner {
        std::mutex gate;
        std::atomic<unsigned> aliases{1};
        unsigned mode{};
        host::Handle handle;   // the rendezvous file whose first byte carries the lock
        void unlock() {
            if (mode && handle) {
                if (const auto done = host::files::unlock(handle, 0, 1); !done) throw ServiceError{file_failure(done.error())};
            }
            mode = 0;
        }
        void release_alias() noexcept {
            if (aliases.fetch_sub(1) != 1) return;
            std::lock_guard lock(gate);
            try { unlock(); } catch (...) { /* Closing the owned handle releases its locks. */ }
            if (handle) host::close(handle);
            handle = {};
        }
        ~FlockOwner() {
            if (handle) host::close(handle);
        }
    };
    struct NetworkQueue;
    struct File {
        explicit File(std::shared_ptr<std::mutex> gate = std::make_shared<std::mutex>())
            : io_gate(std::move(gate)), mutex(*io_gate) {}
        host::Handle native;   // a host::files handle (empty: none)
        int standard{-1};
        bool readable{}, writable{}, directory{}, cloexec{}, random_device{}, pipe{}, null_device{};
        // Pipe status flags (O_NONBLOCK), shared by every descriptor of the open pipe end.
        std::shared_ptr<std::atomic<std::uint32_t>> pipe_status;
        std::atomic<bool> closed{};
        std::uint64_t flags{};
        std::shared_ptr<std::uint64_t> random_flags;
        std::shared_ptr<FlockOwner> flock_owner;
        std::shared_ptr<NetworkSocket> socket;
        std::shared_ptr<NetworkQueue> queue;
        bool socket_alias_released{};
        std::filesystem::path path;
        std::shared_ptr<std::mutex> io_gate;
        std::mutex& mutex;
        void release_flock() noexcept {
            auto owner = std::move(flock_owner);
            if (owner) owner->release_alias();
        }
        void release_socket() noexcept {
            if (socket && !socket_alias_released) { socket_alias_released = true; socket->release_alias(); }
        }
        ~File() {
            release_socket();
            release_flock();
            if (native) host::close(native);
        }
    };
    struct VnodeState;   // EVFILT_VNODE (net/sockets.cpp)
    struct NetworkQueue {
        struct Watch {
            std::shared_ptr<File> record;
            std::uint64_t ident{}, udata{}, serial{};
            std::int16_t filter{};
            std::uint16_t flags{};
            bool enabled{true};
            std::uint32_t fflags{};        // EVFILT_MACHPORT receive options; EVFILT_TIMER units
            std::uint64_t ext0{}, ext1{};  // EVFILT_MACHPORT receive buffer (kevent64)
            // EVFILT_TIMER: next expiry (mach absolute time, nanoseconds) and period (0: absolute/one-shot).
            std::uint64_t due{}, period{};
            // EV_CLEAR: the state last delivered (data, or the newest Mach
            // message stamp); a new event needs a change from it.
            bool cleared{};
            std::uint64_t cleared_data{};
            std::shared_ptr<VnodeState> vnode;   // EVFILT_VNODE: the watched file's state
        };
        std::mutex gate;
        std::map<std::pair<std::uint64_t, std::int16_t>, Watch> watches;
        std::uint64_t serial{};
    };
    struct Stream {
        int descriptor{}; bool eof{}, error{}; int pushed{-1}; std::shared_ptr<std::recursive_mutex> lock{std::make_shared<std::recursive_mutex>()};
        std::shared_ptr<StreamBackend> backend;   // set: the stream has no descriptor (fileno is EBADF)
        int orientation{};                        // fwide: < 0 bytes, > 0 wide, 0 not yet set
    };
    struct Directory { std::vector<std::filesystem::directory_entry> entries; std::size_t at{}; std::uint64_t buffer{}; std::optional<int> descriptor; };
    // A guest mmap: a private copy, an engine reservation, or (MAP_SHARED) a
    // view of a host file section that every process mapping the file shares.
    struct Mapping {
        std::uint64_t size{}; bool engine_reserved{};
        bool section{}, writable_section{};
        std::string object;          // host file identity ("<volume>-<index>") of a section view
        std::uint64_t file_offset{};
        std::uint64_t view{};        // bytes of the section view; the rest is past the end of the file
    };
    ProcessContext& context;
    std::filesystem::path root;
    std::mutex mutex;
    MallocTable allocations;
    std::uint64_t malloc_zone{};
    std::map<int, std::shared_ptr<File>> files;
    std::shared_ptr<NetworkSession> network_session;
    std::uint64_t network_identity{}, network_h_errno{};
    std::set<std::uint64_t> network_results;
    std::set<std::uint64_t> network_interface_results;
    std::map<std::string, std::uint64_t> network_strings;
    std::map<std::uint64_t, Stream> streams;
    std::map<std::uint64_t, Directory> directories;
    std::map<std::uint64_t, Mapping> mappings;
    // Anonymous private memory with Darwin range semantics (vm/vm_space.cpp),
    // on engines that support it; `mappings` keeps file copies and views.
    DarwinVmSpace vm_space{context};
    template <class Function> static auto vm_result(Function function) {
        try { return function(); }
        catch (const DarwinVmError& error) { throw ServiceError{error.value}; }
    }
    std::atomic<std::size_t> section_mappings{};   // MAP_SHARED views in `mappings` (fast path of shared_location)
    // copyfile_state_t (copyfile(3)): what copyfile_state_get/_set report and set.
    struct CopyState {
        std::int32_t source_fd{-1}, destination_fd{-1};
        std::uint64_t source_name{}, destination_name{};   // guest strings this state owns
        std::uint64_t status_callback{}, status_context{}, quarantine{};
        std::int64_t copied{};
        bool cloned{};
    };
    std::map<std::uint64_t, CopyState> copy_states;
    std::map<std::string, std::uint64_t> environment_strings;
    std::uint64_t environment_cell{};
    std::function<bool(std::uint64_t)> check_locale;
    std::array<bool, 3> closed_standard{};
    std::array<bool, 3> standard_cloexec{};
    std::array<std::uint64_t, 3> standard_streams{};
    bool installed{};
    struct SignalAction { std::uint64_t handler{}; std::uint32_t mask{}, flags{}; };
    std::array<SignalAction, 32> signal_actions{};
    std::map<std::uint64_t, std::uint32_t> signal_masks;
    // Signals whose handler ran (process/process_model.cpp's delivery): sigsuspend
    // and pause wait for the count to move. Guarded by `mutex`.
    std::condition_variable signal_handled;
    std::uint64_t handled_signals{};
    std::uint32_t last_handled_signal{};
    std::map<std::uint64_t, std::array<std::uint64_t, 2>> resource_limits;
    std::uint64_t creation_mask{022};

    // ---- process model (process/process_model.cpp) ------------------------------------
    // A descriptor table that replaces the process table on one thread: the
    // child side of an emulated fork, or posix_spawn file actions being
    // applied. Descriptors are per-slot (cloexec), open files are shared.
    struct OverlaySlot { std::shared_ptr<File> file; bool cloexec{}; };
    struct FdOverlay {
        std::map<int, OverlaySlot> slots;
        std::filesystem::path cwd;
        std::uint64_t umask{022};
    };
    static inline thread_local FdOverlay* overlay = nullptr;
    struct ForkChild;
    static inline thread_local ForkChild* fork_child = nullptr;
    struct Child {
        void* process{};                 // Windows process handle (null: pseudo child)
        std::uint32_t pid{}, pgid{};
        std::string program;             // guest path
        bool reaped{}, waitable{true};
        std::optional<std::uint32_t> pseudo_status;   // exit status word of a pseudo child
        // The signal this process ended the child with (TerminateProcess
        // succeeded), and whether that ending can no longer be avoided by the
        // child (SIGKILL was sent): then its wait status is reported at once.
        std::uint32_t killed_by{};
        bool kill_certain{};
    };
    std::map<std::uint32_t, Child> children;
    std::mutex children_mutex;
    // EVFILT_PROC: whether `pid` is a child (nullopt: not one, or reaped),
    // and its wait status once it has ended (inner nullopt: still running).
    // Never reaps (process_model.cpp).
    std::optional<std::optional<std::uint32_t>> child_status(std::uint32_t pid);
    // Children reaped by their wait status while the host still tears their
    // process down: their pids (no reuse, kill() answers ESRCH) and handles
    // live until the host process has really exited (process_model.cpp).
    struct EndingChildren {
        std::mutex mutex;
        std::set<std::uint32_t> pids;
    };
    std::shared_ptr<EndingChildren> ending_children = std::make_shared<EndingChildren>();
    void retire_child_process(void* process, std::uint32_t pid);
    std::uint32_t next_pseudo_pid{1000001};
    std::uint32_t own_pid{}, own_ppid{}, own_pgid{}, own_sid{};
    // Credentials {uid, euid, gid, egid}. A fork child's changes stay its own
    // until exec passes them on (process/process_model.cpp).
    std::array<std::atomic<std::uint32_t>, 4> credential_ids{};
    bool superuser{};
    std::array<std::uint32_t, 4> current_credentials() const;
    void set_credential(std::size_t index, std::uint32_t value);
    std::map<std::uint64_t, std::uint32_t> popen_streams;   // FILE* -> child pid
    struct SpawnFileActions;
    struct SpawnAttributes;
    std::map<std::uint64_t, std::shared_ptr<SpawnFileActions>> spawn_actions;
    std::map<std::uint64_t, std::shared_ptr<SpawnAttributes>> spawn_attributes;
    void note_child_allocation(std::uint64_t address);
    bool child_owns_block(std::uint64_t address);

    void validate_locale(std::uint64_t handle) { if (!check_locale(handle)) throw ServiceError{einval}; }

    using Operation = std::function<std::uint64_t(transmute::CpuState&, transmute::GuestMemory&)>;
    // The result goes to x0, and x1 is 0, as a Darwin syscall stub leaves it: XNU returns the syscall's second
    // value (0 for all but a few calls) in x1, and some callers read it. Chrome calls _sendto as a
    // two-register function and takes a nonzero x1 for a known macOS 13.3-26.0 kernel bug (crbug.com/400642488),
    // failing the write with ERR_NETWORK_CHANGED. Operations that return a pair use install_raw instead.
    void install(std::string name, Operation operation, std::uint64_t failure = UINT64_MAX) {
        static_cast<void>(context.install_function(std::move(name),
            [this, operation = std::move(operation), failure](auto& state, auto& memory) {
                try { state.x[0] = operation(state, memory); }
                catch (const ServiceError& error) { context.set_errno(error.value); state.x[0] = failure; }
                catch (const std::bad_alloc&) { context.set_errno(enomem); state.x[0] = failure; }
                state.x[1] = 0;
            }));
    }
    std::filesystem::path path(const transmute::GuestMemory& memory, std::uint64_t address) {
        return translate_path(text(memory, address));
    }
    std::filesystem::path filesystem_path(const transmute::GuestMemory& memory, std::uint64_t address) {
        const auto result = path(memory, address);
        if (is_random_device(result)) throw ServiceError{eperm};
        return result;
    }
    std::filesystem::path translate_path(std::string_view value) {
        if (value.empty()) throw ServiceError{enoent};
        auto result = std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(value.data()), value.size())).lexically_normal();
        // Reserved guest devices remain in their Darwin namespace and never
        // become a host filesystem pathname such as C:\dev\urandom.
        if (device_node(result) || is_pseudo_terminal_path(result)) return result;
#ifdef _WIN32
        if (result.has_root_name() && !result.has_root_directory()) throw ServiceError{einval};
        if (result.has_root_directory() && !result.has_root_name()) {
            if (value.find('\\') != std::string::npos) throw ServiceError{einval};
            const auto normalized = path_text(result);
            if (normalized == "/tmp" || normalized.starts_with("/tmp/")) {
                result = std::filesystem::temp_directory_path();
                if (normalized.size() > 5) result /= std::filesystem::path(std::u8string(
                    reinterpret_cast<const char8_t*>(normalized.data() + 5), normalized.size() - 5));
            } else if ((normalized == "/Volumes" || normalized == "/Volumes/") && !guest_root_mapping().host_generic.empty()) {
                // The directory above the host drives (/Volumes/C, ...): an
                // empty directory of the Alchemy root, so that walking a path
                // from "/" (mkdir -p) finds it.
                result = utf8_path(guest_root_mapping().host_generic + "/.alchemy/Volumes");
            } else if (normalized.starts_with("/Volumes/")) {
                // Only /Volumes/<drive letter> exists; any other volume name is absent.
                if (normalized.size() < 10 || (normalized.size() > 10 && normalized[10] != '/') ||
                    !((normalized[9] >= 'A' && normalized[9] <= 'Z') || (normalized[9] >= 'a' && normalized[9] <= 'z')))
                    throw ServiceError{enoent};
                std::string native{normalized[9], ':', '/'};
                if (normalized.size() > 11) native += normalized.substr(11);
                result = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(native.data()), native.size()));
            } else if (auto in_root = guest_root_host_path(normalized)) {
                result = std::move(*in_root); // /Applications and /Library live in the Alchemy root
            } else result = root / result.relative_path();
        }
#endif
        if (result.is_relative()) {
            if (overlay) result = overlay->cwd / result;
            else {
                std::lock_guard lock(mutex);
                result = context.working_directory / result;
            }
        }
        return result.lexically_normal();
    }
    // Zero-filled (engine heap blocks and fresh guest mappings both are).
    std::uint64_t allocate(std::uint64_t size, std::uint64_t alignment = 16) {
        if (size > allocation_limit) throw ServiceError{enomem};
        const auto bytes = std::max<std::uint64_t>(size, 1);
        std::uint64_t address{}, base{};
        if (alignment <= 16) {
            if (const auto heap = engine_heap_allocate(context.main_engine(), bytes, 16)) address = base = *heap;
        } else if (alignment <= 4096 && bytes <= 64 * 1024) {
            // posix_memalign (Blender: ~900 per frame, 64-byte aligned): an
            // over-allocated heap block instead of a guest mapping per call.
            if (const auto heap = engine_heap_allocate(context.main_engine(), bytes + alignment, 16)) {
                base = *heap;
                address = (base + alignment - 1) & ~(alignment - 1);
            }
        }
        if (!base) address = context.allocate_guest(bytes, alignment);
        allocations.insert(address, {size, base});
        if (fork_child) note_child_allocation(address);
        return address;
    }
    void release(std::uint64_t address) {
        if (!address) return;
        // The fork child frees a block the parent allocated: after a real
        // fork that frees the child's copy only, so the parent's block stays
        // (the child's image is replaced at exec).
        if (fork_child && !child_owns_block(address)) {
            if (!allocations.find(address)) throw std::runtime_error("Darwin free received an invalid allocation");
            return;
        }
        const auto block = allocations.take(address);
        if (!block) throw std::runtime_error("Darwin free received an invalid allocation");
        if (block->base) engine_heap_release(context.main_engine(), block->base);
        else context.release_guest(address);
    }
    std::uint64_t allocation_size(std::uint64_t address, const char* operation) const {
        const auto block = allocations.find(address);
        if (!block) throw std::runtime_error(std::string("Darwin ") + operation + " received an invalid allocation");
        return block->size;
    }
    std::shared_ptr<File> file(int descriptor) {
        if (overlay) {
            const auto found = overlay->slots.find(descriptor);
            if (found == overlay->slots.end() || found->second.file->standard >= 0) throw ServiceError{ebadf};
            return found->second.file;
        }
        std::lock_guard lock(mutex);
        const auto found = files.find(descriptor);
        if (found == files.end()) throw ServiceError{ebadf};
        return found->second;
    }
    // Darwin munmap on engines with VM ranges: any page range, over anonymous
    // memory (DarwinVmSpace), file and shared views in `mappings` (cut; the
    // parts left keep their file identity and offsets) or both. A range with
    // nothing mapped succeeds, as on Darwin.
    std::uint64_t unmap_ranges(std::uint64_t address, std::uint64_t length) {
        if (!length || address % darwin_arm64_page_size || address > UINT64_MAX - length) throw ServiceError{einval};
        const auto end = address + page_round(length);
        bool handled = false;
        {
            std::lock_guard lock(mutex);
            auto found = mappings.lower_bound(address);
            if (found != mappings.begin() && std::prev(found)->first + std::prev(found)->second.size > address) --found;
            std::vector<std::uint64_t> bases;
            for (; found != mappings.end() && found->first < end; ++found) bases.push_back(found->first);
            for (const auto base : bases) {
                const auto mapping = mappings.at(base);
                const auto from = std::max(address, base), to = std::min(end, base + mapping.size);
                // Private file copies live in DarwinVmSpace on these engines;
                // a record here is a section view (or a whole-only reservation).
                if (!mapping.section && (from != base || to != base + mapping.size))
                    EngineAdapter::require("range unmapping for partial Darwin munmap of a private mapping");
                handled = true;
                if (from == base && to == base + mapping.size) context.main_engine().unmap_memory(base);
                else engine_unmap_range(context.main_engine(), from, to - from);
                mappings.erase(base);
                if (mapping.section) section_mappings.fetch_sub(1);
                if (base < from) {
                    Mapping head = mapping;
                    head.size = from - base;
                    head.view = std::min(mapping.view, head.size);
                    mappings.emplace(base, head);
                    if (head.section) section_mappings.fetch_add(1);
                }
                if (to < base + mapping.size) {
                    Mapping tail = mapping;
                    tail.size = base + mapping.size - to;
                    tail.file_offset += to - base;
                    tail.view = mapping.view > to - base ? mapping.view - (to - base) : 0;
                    mappings.emplace(to, tail);
                    if (tail.section) section_mappings.fetch_add(1);
                }
            }
        }
        if (vm_space.intersects(address, end - address)) {
            handled = true;
            vm_result([&] { return vm_space.unmap(address, end - address); });
        }
        if (!handled) {
            // Darwin unmaps whatever is there; memory other providers own
            // (Mach VM pages, the malloc heap, images) is not mmap's to take.
            const auto region = engine_region_from(context.main_engine(), address);
            if (region && region->base < end && region->mapped)
                EngineAdapter::require("munmap of memory that mmap did not map (Mach VM pages, heap or image memory)");
        }
        return 0;
    }
    // One stderr line per distinct message (unsupported shared-memory requests).
    std::set<std::string> reported_notices;
    void notice(const std::string& message) {
        {
            std::lock_guard lock(mutex);
            if (!reported_notices.insert(message).second) return;
        }
        std::cerr << "alchemy: " << message << std::endl;
    }
    // "<volume serial>-<file index>": the same for every handle to one host file.
    static std::string host_file_identity(host::Handle file) {
        const auto details = host_metadata(file);
        char text[40];
        std::snprintf(text, sizeof(text), "%08x-%016llx", details.device, static_cast<unsigned long long>(details.inode));
        return text;
    }
    // mmap(MAP_SHARED) of a file or shm_open object: a view of a host file
    // section at a guest address chosen by the engine. Every Alchemy process
    // that maps the same host file sees the same pages, and stores reach the
    // file. Pages wholly past the end of the file stay inaccessible.
    // Returns 0 when the engine has no section views and the request is
    // read-only: the caller then maps a private copy of the file range.
    std::uint64_t map_shared_file(std::uint64_t address, std::uint64_t length, std::uint64_t protection, std::uint64_t flags,
                                  int descriptor, std::uint64_t offset) {
        // MAP_NORESERVE, MAP_HASSEMAPHORE and MAP_NOCACHE are accepted hints.
        constexpr std::uint64_t map_shared = 1, map_private = 2, map_fixed = 0x10, hints = 0x40 | 0x200 | 0x400;
        if (!length || length > (1ULL << 40) || (protection & ~7ULL) || (flags & map_private) ||
            (flags & ~(map_shared | map_fixed | hints)) || offset % darwin_arm64_page_size)
            throw ServiceError{einval};
        // A non-fixed address is a hint (tried below).
        if (flags & map_fixed) {
            notice("mmap(MAP_SHARED | MAP_FIXED) is unsupported: shared file views are placed by Alchemy");
            throw ServiceError{einval};
        }
        if (protection & 4) {
            notice("mmap(MAP_SHARED) with PROT_EXEC is unsupported: code is never run from a shared file view");
            throw ServiceError{eacces};
        }
        const auto size = page_round(length);
        const auto record = file(descriptor);
        if (record->directory || record->random_device || record->pipe || record->null_device || record->socket || record->queue)
            throw ServiceError{19};   // ENODEV: not a regular file
        if (!record->readable || ((protection & 2) && !record->writable)) throw ServiceError{eacces};
        host::Handle section;
        std::uint64_t view{};
        std::string object;
        {
            std::lock_guard lock(record->mutex);
            if (record->closed) throw ServiceError{ebadf};
            const auto end = file_size(record->native, file_failure);
            // A file that ends inside a page is viewed to its end; the rest of that page reads as zeros.
            view = offset < end ? std::min<std::uint64_t>(size, end - offset) : 0;
            if (!view) {
                notice("mmap(MAP_SHARED) of a range wholly past the end of a file is unsupported (file size " +
                       std::to_string(end) + ", offset " + std::to_string(offset) + ")");
                throw ServiceError{einval};
            }
            const auto created = host::sections::create_file_section(record->native, record->writable);
            if (!created) throw ServiceError{file_failure(created.error())};
            section = *created;
            try { object = host_file_identity(record->native); }
            catch (...) { host::close(section); throw; }
        }
        // The engine maps the host section (its handle value).
        const auto engine_section = reinterpret_cast<void*>(section.value);
        std::optional<std::uint64_t> mapped;
        try {
            // The address is a hint, tried first: SQLite grows a mapping in
            // place by mapping the file's next pages right after its view
            // (and remaps the whole file when that lands elsewhere).
            const auto hint = address & ~(darwin_arm64_page_size - 1);
            if (hint >= 0x100000000ULL && hint <= UINT64_MAX - size) {
                try {
                    mapped = engine_map_section(context.main_engine(), hint, size, engine_section, offset, view, record->writable,
                                                (protection & 3) != 0, (protection & 2) != 0);
                } catch (const std::exception&) {
                    mapped.reset();   // in use: anywhere
                }
            }
            if (!mapped)
                mapped = engine_map_section(context.main_engine(), 0, size, engine_section, offset, view, record->writable,
                                            (protection & 3) != 0, (protection & 2) != 0);
        } catch (const std::exception& error) {
            host::close(section);
            notice(std::string("mmap(MAP_SHARED) failed: ") + error.what());
            throw ServiceError{enomem};
        }
        host::close(section);
        if (!mapped) {
            // A read-only view (QFile::map, Qt's plugin metadata scan) is served
            // as a private copy by the caller: the guest cannot write through
            // it, but later writes to the file by others are not reflected.
            if (!(protection & 2)) return 0;
            notice("writable mmap(MAP_SHARED) of a file needs the fx engine (alchemy-fx); this engine has private mappings only");
            throw ServiceError{einval};
        }
        std::lock_guard lock(mutex);
        mappings.emplace(*mapped, Mapping{size, false, true, record->writable, std::move(object), offset, view});
        section_mappings.fetch_add(1);
        return *mapped;
    }
    // shm_open objects: host files in a per-root namespace directory, so that
    // ftruncate, fstat and mmap(MAP_SHARED) behave as for any file and every
    // Alchemy process using the same root shares them by name.
    std::filesystem::path shm_directory() {
        auto directory = configured_alchemy_root() ? *configured_alchemy_root() / ".alchemy" / "shm"
                                                   : std::filesystem::temp_directory_path() / "alchemy-shm";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) throw ServiceError{host_error(error)};
        return directory;
    }
    std::filesystem::path shm_path(const transmute::GuestMemory& memory, std::uint64_t address) {
        constexpr std::size_t name_limit = 31;   // PSHMNAMLEN
        std::string name;
        for (std::size_t index = 0;; ++index) {
            if (index > name_limit) throw ServiceError{63};   // ENAMETOOLONG
            const auto character = static_cast<char>(memory.read8(address + index));
            if (!character) break;
            name += character;
        }
        if (name.empty() || name == "/") throw ServiceError{einval};
        // Lower-case letters, digits and ".-_" are kept; every other byte is
        // escaped, upper case too (Darwin names are case-sensitive, NTFS is not).
        std::string encoded = "shm-";
        for (const auto character : name) {
            const auto byte = static_cast<unsigned char>(character);
            if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
                byte == '.' || byte == '_' || byte == '-') encoded += character;
            else {
                char escape[4];
                std::snprintf(escape, sizeof(escape), "%%%02x", byte);
                encoded += escape;
            }
        }
        return shm_directory() / encoded;
    }
    std::shared_ptr<File> descriptor_file(int descriptor) {
        if (overlay) {
            const auto found = overlay->slots.find(descriptor);
            if (found == overlay->slots.end()) throw ServiceError{ebadf};
            // The process's own standard streams stay virtual (null record).
            if (found->second.file->standard == descriptor) return {};
            return found->second.file;
        }
        std::lock_guard lock(mutex);
        const auto found = files.find(descriptor);
        if (found != files.end()) return found->second;
        if (descriptor < 0 || descriptor > 2 || closed_standard[static_cast<std::size_t>(descriptor)])
            throw ServiceError{ebadf};
        return {};
    }
    bool descriptor_cloexec(int descriptor) {
        if (overlay) {
            const auto found = overlay->slots.find(descriptor);
            if (found == overlay->slots.end()) throw ServiceError{ebadf};
            return found->second.cloexec;
        }
        std::shared_ptr<File> record;
        {
            std::lock_guard lock(mutex);
            const auto found = files.find(descriptor);
            if (found != files.end()) record = found->second;
            else {
                if (descriptor < 0 || descriptor > 2 || closed_standard[static_cast<std::size_t>(descriptor)])
                    throw ServiceError{ebadf};
                return standard_cloexec[static_cast<std::size_t>(descriptor)];
            }
        }
        std::lock_guard lock(record->mutex);
        if (record->closed) throw ServiceError{ebadf};
        return record->cloexec;
    }
    void set_descriptor_cloexec(int descriptor, bool value) {
        if (overlay) {
            const auto found = overlay->slots.find(descriptor);
            if (found == overlay->slots.end()) throw ServiceError{ebadf};
            found->second.cloexec = value;
            return;
        }
        std::shared_ptr<File> record;
        {
            std::lock_guard lock(mutex);
            const auto found = files.find(descriptor);
            if (found != files.end()) record = found->second;
            else {
                if (descriptor < 0 || descriptor > 2 || closed_standard[static_cast<std::size_t>(descriptor)])
                    throw ServiceError{ebadf};
                standard_cloexec[static_cast<std::size_t>(descriptor)] = value; return;
            }
        }
        std::lock_guard lock(record->mutex);
        if (record->closed) throw ServiceError{ebadf};
        record->cloexec = value;
    }
    int add_file(std::shared_ptr<File> value) {
        if (overlay) {
            // POSIX: the lowest free descriptor (a closed standard stream included).
            int descriptor = 0; while (descriptor < 4096 && overlay->slots.contains(descriptor)) ++descriptor;
            if (descriptor == 4096) throw ServiceError{emfile};
            const bool cloexec = value->cloexec;
            overlay->slots[descriptor] = OverlaySlot{std::move(value), cloexec};
            return descriptor;
        }
        std::lock_guard lock(mutex);
        int descriptor = 3; while (descriptor < 4096 && files.contains(descriptor)) ++descriptor;
        if (descriptor == 4096) throw ServiceError{emfile};
        files.emplace(descriptor, std::move(value)); return descriptor;
    }
    int duplicate(int descriptor, int minimum, std::optional<int> destination = {}, bool cloexec = false) {
        if (minimum < 0 || minimum >= 4096 || (destination && (*destination < 0 || *destination >= 4096)))
            throw ServiceError{einval};
        if (overlay) {
            const auto found = overlay->slots.find(descriptor);
            if (found == overlay->slots.end()) throw ServiceError{ebadf};
            if (destination == descriptor) return descriptor;
            int result = destination.value_or(minimum);
            if (!destination) {
                while (result < 4096 && overlay->slots.contains(result)) ++result;
                if (result == 4096) throw ServiceError{emfile};
            }
            overlay->slots[result] = OverlaySlot{found->second.file, cloexec};
            return result;
        }
        const auto original = descriptor_file(descriptor);
        if (destination == descriptor) return descriptor;
        auto copy = std::make_shared<File>(original ? original->io_gate : std::make_shared<std::mutex>());
        if (original) {
            std::lock_guard lock(original->mutex);
            if (original->closed) throw ServiceError{ebadf};
            copy->readable = original->readable; copy->writable = original->writable;
            copy->directory = original->directory; copy->path = original->path;
            copy->standard = original->standard; copy->flags = original->flags;
            copy->random_device = original->random_device; copy->random_flags = original->random_flags;
            copy->pipe = original->pipe; copy->pipe_status = original->pipe_status; copy->null_device = original->null_device;
            copy->flock_owner = original->flock_owner;
            if (copy->flock_owner) ++copy->flock_owner->aliases;
            copy->socket = original->socket;
            copy->queue = original->queue;
            if (copy->socket) ++copy->socket->aliases;
            if (original->native) {
                // The duplicate shares the file position, as dup(2).
                const auto duplicated = host::duplicate(original->native);
                if (!duplicated) throw ServiceError{crt_failure(duplicated.error())};
                copy->native = *duplicated;
            }
        } else {
            copy->standard = descriptor; copy->readable = descriptor == 0;
            copy->writable = descriptor != 0; copy->flags = descriptor == 0 ? 0 : 1;
        }
        copy->cloexec = cloexec;
        std::shared_ptr<File> replaced;
        int result = destination.value_or(std::max(3, minimum));
        {
            std::lock_guard lock(mutex);
            if (!destination) {
                while (result < 4096 && files.contains(result)) ++result;
                if (result == 4096) {
                    throw ServiceError{emfile};
                }
            }
            const auto old = files.find(result);
            if (old != files.end()) replaced = old->second;
            files[result] = std::move(copy);
            if (result < 3) closed_standard[static_cast<std::size_t>(result)] = false;
        }
        if (replaced) {
            std::lock_guard lock(replaced->mutex);
            if (replaced->native) host::close(replaced->native);
            replaced->native = {}; replaced->closed = true;
            replaced->release_flock();
            replaced->release_socket();
        }
        return result;
    }
    std::filesystem::path path_at(transmute::GuestMemory& memory, std::uint64_t descriptor, std::uint64_t address) {
        const auto value = text(memory, address);
        const auto candidate = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
        if (candidate.is_absolute() || candidate.has_root_directory() || candidate.has_root_name() || static_cast<std::int32_t>(descriptor) == -2)
            return path(memory, address);
        const auto record = file(static_cast<int>(descriptor));
        std::lock_guard lock(record->mutex);
        if (record->closed) throw ServiceError{ebadf};
        if (!record->directory) throw ServiceError{enotdir};
        return (record->path / candidate).lexically_normal();
    }
    std::filesystem::path filesystem_path_at(transmute::GuestMemory& memory, std::uint64_t descriptor, std::uint64_t address) {
        const auto result = path_at(memory, descriptor, address);
        if (is_random_device(result)) throw ServiceError{eperm};
        return result;
    }
    std::uint64_t new_directory(transmute::GuestMemory& memory, const std::filesystem::path& directory_path, std::optional<int> descriptor = {}) {
        if (is_random_device(directory_path)) throw ServiceError{enotdir};
        Directory value;
        std::error_code error;
        for (std::filesystem::directory_iterator it(directory_path, error), end; !error && it != end; it.increment(error)) value.entries.push_back(*it);
        if (error) throw ServiceError{host_error(error)};
        const auto address = context.allocate_guest(16);
        std::optional<int> owned_descriptor;
        try {
            value.buffer = context.allocate_guest(1048);
            owned_descriptor = descriptor ? *descriptor : open_path(directory_path, 0x100000, 0);
            value.descriptor = owned_descriptor;
            memory.write64(address, 0x414c434844495221ULL);
            std::lock_guard lock(mutex); directories.emplace(address, std::move(value));
        } catch (...) {
            if (owned_descriptor && !descriptor) static_cast<void>(close(*owned_descriptor));
            if (value.buffer) context.release_guest(value.buffer);
            context.release_guest(address); throw;
        }
        return address;
    }
    // The guest's per-user temporary directory with a trailing '/': TMPDIR
    // (every Alchemy process inherits one, services.cpp) or else /tmp/ (the
    // host temporary directory, translate_path). tmpfile and confstr's
    // _CS_DARWIN_USER_* names share it; NSTemporaryDirectory's "/tmp/" is the
    // same host directory.
    std::string temporary_directory() {
        std::string value;
        { std::lock_guard lock(mutex);
          for (const auto& entry : context.environment) if (entry.starts_with("TMPDIR=")) { value = entry.substr(7); break; } }
        if (value.empty() || !value.starts_with("/")) value = "/tmp";
        if (!value.ends_with("/")) value += '/';
        return value;
    }
    int private_temporary_file(const std::filesystem::path& name, bool anonymous) {
        if (is_random_device(name)) throw ServiceError{eperm};
        auto record = std::make_shared<File>();
        const auto opened = host::files::open(path_text(name), host::files::Access::read_write, host::files::Create::create_new,
            host::files::open_owner_only | (anonymous ? host::files::open_deletable : 0));
        if (!opened) throw ServiceError{file_failure(opened.error())};
        record->native = *opened;
        bool linked = true;
        try {
            record->readable = true; record->writable = true; record->path = name; record->flags = 2;
            record->flock_owner = std::make_shared<FlockOwner>();
            if (anonymous) {
                if (const auto removed = host::files::remove_open(record->native); !removed) throw ServiceError{file_failure(removed.error())};
                linked = false;
            }
            return add_file(std::move(record));
        } catch (...) {
            record.reset();
            if (linked) { std::error_code ignored; std::filesystem::remove(name, ignored); }
            throw;
        }
    }
    std::uint64_t create_temporary(transmute::GuestMemory& memory, std::uint64_t address,
        std::uint64_t suffix_length, bool directory) {
        const auto specification = text(memory, address);
        validate(memory, address, specification.size() + 1, true);
        if (suffix_length > specification.size() || specification.size() - suffix_length < 6)
            throw ServiceError{einval};
        const auto offset = specification.size() - static_cast<std::size_t>(suffix_length) - 6;
        if (specification.substr(offset, 6) != "XXXXXX") throw ServiceError{einval};
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            auto candidate = specification; candidate.replace(offset, 6, temporary_suffix());
            const auto name = translate_path(candidate);
            if (directory) {
                if (const auto created = host::files::create_directory(path_text(name), true); !created) {
                    const auto error = file_failure(created.error()); if (error == eexist) continue; throw ServiceError{error};
                }
                try { put_text(memory, address, candidate); return address; }
                catch (...) { std::error_code ignored; std::filesystem::remove(name, ignored); throw; }
            }
            int descriptor;
            try { descriptor = private_temporary_file(name, false); }
            catch (const ServiceError& error) { if (error.value == eexist) continue; throw; }
            try { put_text(memory, address, candidate); return static_cast<std::uint64_t>(descriptor); }
            catch (...) { static_cast<void>(close(descriptor)); std::error_code ignored; std::filesystem::remove(name, ignored); throw; }
        }
        throw ServiceError{eexist};
    }
    // fsync/F_FULLFSYNC of a directory descriptor: its entries reach the disk.
    // FlushFileBuffers needs a handle open for writing; NTFS journals directory
    // metadata, so a directory that cannot be opened that way is still durable.
    static void sync_directory(const std::filesystem::path& directory) {
        if (const auto synced = host::files::sync_directory(path_text(directory)); !synced) throw ServiceError{file_failure(synced.error())};
    }
    std::uint64_t flock(int descriptor, std::uint64_t operation) {
        const auto kind = operation & ~4ULL;
        if ((operation & ~15ULL) || (kind != 1 && kind != 2 && kind != 8)) throw ServiceError{einval};
        const auto record = file(descriptor);
        std::shared_ptr<FlockOwner> owner;
        struct Pin { host::Handle handle; ~Pin() { if (handle) host::close(handle); } } pin;
        {
            std::lock_guard lock(record->mutex);
            if (record->closed) throw ServiceError{ebadf};
            if (!record->native || !record->flock_owner) throw ServiceError{45};
            owner = record->flock_owner;
            const auto duplicated = host::duplicate(record->native);
            if (!duplicated) throw ServiceError{file_failure(duplicated.error())};
            pin.handle = *duplicated;
        }
        std::unique_lock lock(owner->gate);
        if (record->closed || !owner->aliases) throw ServiceError{ebadf};
        if (kind == 8) { owner->unlock(); return 0; }
        if (!owner->handle) {
            // Every Alchemy process locks the same rendezvous file for one host
            // file: its name is the file's identity.
            const auto identity = host::files::identity(pin.handle);
            if (!identity) throw ServiceError{45};
            constexpr std::string_view digits = "0123456789abcdef";
            std::string key;
            for (unsigned shift = 0; shift < 64; shift += 4) key += digits[(identity->volume >> shift) & 15];
            for (const auto byte : identity->id) { key += digits[byte >> 4]; key += digits[byte & 15]; }
            const auto directory = std::filesystem::temp_directory_path() / "alchemy-flock-v1";
            std::error_code error; std::filesystem::create_directory(directory, error);
            if (error) throw ServiceError{host_error(error)};
            const auto rendezvous = host::files::open(path_text(directory / (key + ".lock")), host::files::Access::read_write,
                                                      host::files::Create::open_or_create, host::files::open_owner_only);
            if (!rendezvous) throw ServiceError{file_failure(rendezvous.error())};
            owner->handle = *rendezvous;
        }
        if (owner->mode == kind) return 0;
        owner->unlock();
        // Poll nonblocking kernel locks so close and process shutdown can end
        // waits. Neither descriptor metadata nor guest memory is leased here.
        for (;;) {
            if (record->closed || !owner->aliases) throw ServiceError{ebadf};
            if (process_exiting(context)) throw ServiceError{89};
            if (owner->mode == kind) return 0;
            if (owner->mode) owner->unlock();
            const auto locked = host::files::lock(owner->handle, 0, 1, kind == 2);
            if (locked) { owner->mode = static_cast<unsigned>(kind); return 0; }
            if (locked.error() != host::Error::would_block) throw ServiceError{file_failure(locked.error())};
            if (operation & 4) throw ServiceError{35};
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            lock.lock();
        }
    }
    // The host access of Darwin O_RDONLY, O_WRONLY or O_RDWR.
    static host::files::Access access_of(std::uint64_t flags) {
        return (flags & 3) == 0 ? host::files::Access::read : (flags & 3) == 1 ? host::files::Access::write : host::files::Access::read_write;
    }
    int open(const transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t flags, std::uint64_t mode) {
        return open_path(path(memory, address), flags, mode);
    }
    int open_path(const std::filesystem::path& name, std::uint64_t flags, std::uint64_t mode) {
        if (const auto opened = open_pseudo_terminal(name, flags)) return *opened;
        if (is_terminal_device(name)) throw ServiceError{6};   // ENXIO: no controlling terminal or console device
        if (is_random_device(name)) {
            if (flags & 0x100000) throw ServiceError{enotdir};
            if ((flags & 3) != 0) throw ServiceError{eacces};
            if (flags & ~(4ULL | 0x1000000ULL)) throw ServiceError{einval};
            auto record = std::make_shared<File>(); record->random_device = true; record->readable = true;
            record->path = name; record->flags = flags; record->cloexec = (flags & 0x1000000) != 0;
            record->random_flags = std::make_shared<std::uint64_t>(flags & 4);
            return add_file(std::move(record));
        }
        // O_NONBLOCK and O_NOCTTY have no effect on files and devices here.
        // O_EVTONLY (0x8000): a descriptor for event notification only (kqueue
        // EVFILT_VNODE); it opens like O_RDONLY and never blocks deletion.
        constexpr std::uint64_t supported = 3 | 4 | 8 | 0x100 | 0x200 | 0x400 | 0x800 | 0x1000000 | 0x100000 | 0x20000 | 0x8000;
        if ((flags & ~supported) || (flags & 3) == 3) throw ServiceError{einval};
        // O_NOFOLLOW: a symbolic link (or junction, or recorded run-time link) as
        // the last component fails with ELOOP. Python's tempfile opens with it.
        if (flags & 0x100) {
            std::error_code error;
            const auto type = std::filesystem::symlink_status(name, error).type();
#ifdef _WIN32
            const bool junction = type == std::filesystem::file_type::junction;
#else
            const bool junction = false;
#endif
            if (!error && (type == std::filesystem::file_type::symlink || junction || installed_link(name)))
                throw ServiceError{62};
            flags &= ~std::uint64_t{0x100};
        }
        if (overlay) mode &= ~overlay->umask;
        else { std::lock_guard lock(mutex); mode &= ~creation_mask; }
        if (is_null_device(name)) {
            if (flags & 0x100000) throw ServiceError{enotdir};
            const auto null = host::files::open_null(access_of(flags));
            if (!null) throw ServiceError{file_failure(null.error())};
            auto record = std::make_shared<File>();
            record->native = *null; record->null_device = true; record->path = name;
            record->readable = (flags & 3) != 1; record->writable = (flags & 3) != 0;
            record->flags = flags & ~0x1000000ULL; record->cloexec = (flags & 0x1000000) != 0;
            return add_file(std::move(record));
        }
        if (flags & 0x100000) {
            if (flags & (3 | 8 | 0x200 | 0x400 | 0x800)) throw ServiceError{einval};
            std::error_code error;
            if (!std::filesystem::is_directory(name, error)) throw ServiceError{error ? host_error(error) : enotdir};
            auto record = std::make_shared<File>();
            record->native = {}; record->directory = true; record->path = name;
            record->flags = flags; record->cloexec = (flags & 0x1000000) != 0;
            return add_file(std::move(record));
        }
        // A directory opened read-only (O_RDONLY, O_EVTONLY) without
        // O_DIRECTORY is a directory descriptor too, as on Darwin.
        if ((flags & 3) == 0 && !(flags & (0x200 | 0x400))) {
            std::error_code error;
            if (std::filesystem::is_directory(name, error)) {
                auto record = std::make_shared<File>();
                record->native = {}; record->directory = true; record->path = name;
                record->flags = flags; record->cloexec = (flags & 0x1000000) != 0;
                return add_file(std::move(record));
            }
        }
        // O_CREAT, O_EXCL and O_TRUNC choose how the file is created; O_APPEND
        // is the record's (every write goes to the end of the file).
        using host::files::Create;
        const auto create = flags & 0x200 ? (flags & 0x800 ? Create::create_new : (flags & 0x400 ? Create::create_or_truncate : Create::open_or_create)) :
            (flags & 0x400 ? Create::truncate_existing : Create::open_existing);
        const auto opened = host::files::open(path_text(name), access_of(flags), create,
            (flags & 0x200) && !(mode & 0200) ? host::files::open_create_read_only : 0);
        if (!opened) throw ServiceError{file_failure(opened.error())};
        std::shared_ptr<File> record;
        try { record = std::make_shared<File>(); }
        catch (...) { host::close(*opened); throw; }
        record->native = *opened;
        record->readable = (flags & 3) != 1;
        record->writable = (flags & 3) != 0;
        record->path = name;
        record->flags = flags; record->cloexec = (flags & 0x1000000) != 0;
        record->flock_owner = std::make_shared<FlockOwner>();
        return add_file(std::move(record));
    }
    std::uint64_t close(int descriptor) {
        if (overlay) {
            // The open file stays with the parent (or closes with its last slot).
            if (!overlay->slots.erase(descriptor)) throw ServiceError{ebadf};
            return 0;
        }
        std::shared_ptr<File> record;
        {
            std::lock_guard lock(mutex);
            const auto found = files.find(descriptor);
            if (found != files.end()) {
                record = std::move(found->second); files.erase(found);
                if (descriptor < 3) closed_standard[static_cast<std::size_t>(descriptor)] = true;
            } else {
                if (descriptor < 0 || descriptor > 2 || closed_standard[static_cast<std::size_t>(descriptor)])
                    throw ServiceError{ebadf};
                closed_standard[static_cast<std::size_t>(descriptor)] = true; return 0;
            }
        }
        // Detach the descriptor before any blocking backend operation. A new
        // open or dup2 may reuse its number without this close erasing it.
        std::lock_guard file_lock(record->mutex);
        if (record->closed) throw ServiceError{ebadf};
        if (record->directory || record->standard >= 0 || record->random_device || record->queue) {
            record->closed = true; return 0;
        }
        if (record->socket) { record->closed = true; record->release_socket(); return 0; }
        if (!record->native) throw ServiceError{ebadf};
        // Closing any descriptor of a file releases all of this process's
        // fcntl record locks on it (record_locks.hpp).
        if (record->flock_owner && record_locks::any_held())
            record_locks::release_all(record_locks::file_key(static_cast<std::intptr_t>(record->native.value)));
        host::close(record->native);
        record->native = {};
        record->closed = true;
        record->release_flock();
        return 0;
    }
    std::uint64_t write(transmute::GuestMemory& memory, int descriptor, std::uint64_t address, std::uint64_t size) {
        if (size > INT64_MAX) throw ServiceError{einval};
        auto record = descriptor_file(descriptor);
        if (record) {
            if (record->directory) throw ServiceError{eisdir};
            if (!record->writable) throw ServiceError{ebadf};
        } else if (descriptor == 0) throw ServiceError{ebadf};
        if (record && record->socket)
            return record->socket->terminal ? terminal_write(memory, record, address, size) : network_io(memory, record, address, size, 0, true);
        if (record && record->pipe) return pipe_write(memory, record, address, size);
        validate(memory, address, size, false);
        std::array<std::uint8_t, 65536> buffer{};
        std::uint64_t total = 0;
        while (total < size) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size - total, buffer.size()));
            memory.read_bytes(add(address, total), std::span(buffer.data(), count));
            std::uint64_t written = 0;
            std::uint32_t error = 0;
            if (!record || record->standard >= 0) {
                const auto result = context.output(static_cast<unsigned>(record ? record->standard : descriptor), std::span(buffer.data(), count));
                if (result.bytes_written > count) throw std::runtime_error("invalid Darwin output byte count");
                written = result.bytes_written;
                error = result.error;
            } else {
                std::lock_guard lock(record->mutex);
                if (record->closed) { if (total) break; throw ServiceError{ebadf}; }
                // O_APPEND: every write goes to the end of the file.
                const auto at_end = (record->flags & 8) ? host::files::seek(record->native, 0, host::files::Whence::end) : host::Result<std::uint64_t>(0);
                const auto result = at_end ? host::files::write(record->native, std::as_bytes(std::span(buffer.data(), count))) : host::Result<std::size_t>(at_end.error());
                if (!result) error = crt_failure(result.error()); else written = static_cast<std::uint64_t>(*result);
            }
            total += written;
            if (error || written != count) {
                if (!total && error) throw ServiceError{error};
                break;
            }
        }
        return total;
    }
    std::uint64_t read(transmute::GuestMemory& memory, int descriptor, std::uint64_t address, std::uint64_t size) {
        if (size > INT64_MAX) throw ServiceError{einval};
        auto record = descriptor_file(descriptor);
        if (record && record->socket) return network_io(memory, record, address, size, 0, false);
        if (record) { if (record->directory) throw ServiceError{eisdir}; if (!record->readable) throw ServiceError{ebadf}; }
        else if (descriptor != 0) throw ServiceError{ebadf};
        if (record && record->pipe) return pipe_read(memory, record, address, size);
        validate(memory, address, size, true);
        std::array<std::uint8_t, 65536> buffer;   // only the received bytes are copied out (no zero fill)
        std::uint64_t total = 0;
        while (total < size) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size - total, buffer.size()));
            std::int64_t received;
            if (record && record->standard < 0) { std::lock_guard lock(record->mutex);
                if (record->closed) { if (total) break; throw ServiceError{ebadf}; }
                if (record->random_device) {
                    try { cryptographic_random(std::span(buffer.data(), count)); }
                    catch (const ServiceError&) { if (total) break; throw; }
                    received = static_cast<std::int64_t>(count);
                } else {
                    const auto result = host::files::read(record->native, std::as_writable_bytes(std::span(buffer.data(), count)));
                    if (!result) { if (total) break; throw ServiceError{crt_failure(result.error())}; }
                    received = static_cast<std::int64_t>(*result);
                }
            } else {
                // The host standard input; a closed pipe reads end of file.
                const auto input = host::process::standard_handle(host::process::Standard::input);
                const auto result = input ? host::files::read(input, std::as_writable_bytes(std::span(buffer.data(), count)))
                                          : host::Result<std::size_t>(host::Error::bad_handle);
                if (!result) throw ServiceError{result.error() == host::Error::bad_handle ? ebadf : eio};
                received = static_cast<std::int64_t>(*result);
            }
            memory.write_bytes(add(address, total), std::span(buffer.data(), static_cast<std::size_t>(received)));
            total += static_cast<std::uint64_t>(received);
            if (static_cast<std::uint64_t>(received) != count) break;
        }
        return total;
    }
    std::uint64_t seek(int descriptor, std::uint64_t offset, int whence) {
        if (whence < 0 || whence > 2) throw ServiceError{einval};
        auto record = file(descriptor);
        if (record->directory || record->random_device || record->standard >= 0 || record->socket || record->pipe) throw ServiceError{29};
        std::lock_guard lock(record->mutex);
        if (record->closed) throw ServiceError{ebadf};
        const auto result = host::files::seek(record->native, std::bit_cast<std::int64_t>(offset),
            whence == 0 ? host::files::Whence::set : whence == 1 ? host::files::Whence::current : host::files::Whence::end);
        if (!result) throw ServiceError{crt_failure(result.error())};
        return *result;
    }
    std::uint64_t positioned_io(transmute::GuestMemory& memory, int descriptor,
        std::uint64_t address, std::uint64_t size, std::uint64_t offset, bool writing) {
        const auto count = bounded_size(size);
        if (offset > INT64_MAX || size > INT64_MAX - offset) throw ServiceError{einval};
        const auto record = file(descriptor);
        if (record->directory) throw ServiceError{eisdir};
        if (record->standard >= 0 || record->random_device || record->socket || record->pipe) throw ServiceError{29};
        if (writing ? !record->writable : !record->readable) throw ServiceError{ebadf};
        validate(memory, address, size, !writing);
        std::vector<std::uint8_t> bytes(count);
        if (writing) memory.read_bytes(address, bytes);
        std::lock_guard lock(record->mutex);
        if (record->closed) throw ServiceError{ebadf};
        if (writing && (record->flags & 8)) throw ServiceError{enosys};
        // Duplicated handles share the position. The shared gate also covers
        // ordinary read/write/seek, so save/restore cannot race another alias.
        const auto previous = host::files::seek(record->native, 0, host::files::Whence::current);
        if (!previous) throw ServiceError{crt_failure(previous.error())};
        if (const auto moved = host::files::seek(record->native, static_cast<std::int64_t>(offset), host::files::Whence::set); !moved)
            throw ServiceError{crt_failure(moved.error())};
        const auto result = writing ? host::files::write(record->native, std::as_bytes(std::span(bytes.data(), count)))
                                    : host::files::read(record->native, std::as_writable_bytes(std::span(bytes.data(), count)));
        if (const auto restored = host::files::seek(record->native, static_cast<std::int64_t>(*previous), host::files::Whence::set); !restored)
            throw ServiceError{crt_failure(restored.error())};
        if (!result) throw ServiceError{crt_failure(result.error())};
        if (!writing) memory.write_bytes(address, std::span(bytes.data(), *result));
        return static_cast<std::uint64_t>(*result);
    }
    std::uint64_t vector_io(transmute::CpuState& state, transmute::GuestMemory& memory,
        bool writing, bool positioned) {
        const auto count = static_cast<std::int32_t>(state.x[2]);
        if (count < 0 || count > 1024) throw ServiceError{einval};
        validate(memory, state.x[1], static_cast<std::uint64_t>(count) * 16, false);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> vectors;
        std::uint64_t total = 0;
        for (int index = 0; index < count; ++index) {
            const auto cell = state.x[1] + static_cast<std::uint64_t>(index) * 16;
            const auto base = memory.read64(cell), length = memory.read64(cell + 8);
            if (length > transfer_limit - total) throw ServiceError{einval};
            validate(memory, base, length, !writing);
            vectors.emplace_back(base, length); total += length;
        }
        const auto temporary = context.allocate_guest(std::max<std::uint64_t>(total, 1));
        try {
            std::uint64_t cursor = 0;
            if (writing) for (const auto& [base, length] : vectors) {
                memory.copy_bytes(temporary + cursor, base, length); cursor += length;
            }
            const auto result = positioned ? positioned_io(memory, static_cast<int>(state.x[0]), temporary,
                total, state.x[3], writing) : (writing ? write(memory, static_cast<int>(state.x[0]), temporary, total) :
                    read(memory, static_cast<int>(state.x[0]), temporary, total));
            cursor = 0;
            if (!writing) for (const auto& [base, length] : vectors) {
                const auto copied = std::min(length, result - cursor);
                memory.copy_bytes(base, temporary + cursor, copied); cursor += copied;
                if (cursor == result) break;
            }
            context.release_guest(temporary); return result;
        } catch (...) { context.release_guest(temporary); throw; }
    }
    Stream stream(std::uint64_t address) {
        std::lock_guard lock(mutex);
        const auto found = streams.find(address);
        if (found == streams.end()) throw ServiceError{ebadf};
        return found->second;
    }
    std::uint64_t read_stream(transmute::GuestMemory& memory, std::uint64_t stream_address,
                              std::uint64_t address, std::uint64_t size) {
        const auto value = stream(stream_address);
        std::lock_guard stream_lock(*value.lock);
        validate(memory, address, size, true);
        std::uint64_t prefix = 0;
        { std::lock_guard lock(mutex); auto& current = streams.at(stream_address);
          if (size && current.pushed != -1) { memory.write8(address, static_cast<std::uint8_t>(current.pushed)); current.pushed = -1; prefix = 1; } }
        try {
            std::uint64_t count = 0;
            if (value.backend) {
                // A backend (a funopen read function) may return less than asked before its end.
                while (count < size - prefix) {
                    const auto got = value.backend->read(memory, address + prefix + count, size - prefix - count);
                    if (!got) break;
                    count += got;
                }
            } else count = read(memory, value.descriptor, address + prefix, size - prefix);
            if (count < size - prefix) {
                std::lock_guard lock(mutex); streams.at(stream_address).eof = true;
                memory.write8(stream_address + 16, static_cast<std::uint8_t>(memory.read8(stream_address + 16) | 0x20U));
            }
            return prefix + count;
        } catch (const ServiceError&) {
            std::lock_guard lock(mutex); streams.at(stream_address).error = true;
            memory.write8(stream_address + 16, static_cast<std::uint8_t>(memory.read8(stream_address + 16) | 0x40U)); throw;
        }
    }
    // Writes and seeks of a stream: its descriptor's, or its backend's.
    std::uint64_t stream_write(transmute::GuestMemory& memory, const Stream& value, std::uint64_t address, std::uint64_t size) {
        return value.backend ? value.backend->write(memory, address, size) : write(memory, value.descriptor, address, size);
    }
    std::uint64_t stream_seek(transmute::GuestMemory& memory, const Stream& value, std::uint64_t offset, int whence) {
        if (whence < 0 || whence > 2) throw ServiceError{einval};
        return value.backend ? value.backend->seek(memory, std::bit_cast<std::int64_t>(offset), whence) : seek(value.descriptor, offset, whence);
    }
    // A FILE over a backend (no descriptor: _file is -1).
    std::uint64_t new_backend_stream(std::shared_ptr<StreamBackend> backend, bool readable, bool writable) {
        const auto address = context.allocate_guest(152);
        try {
            auto& memory = static_cast<transmute::GuestMemory&>(context.calling_engine());
            memory.fill_bytes(address, 0, 152);
            memory.write64(address, address + 119);
            memory.write8(address + 16, static_cast<std::uint8_t>(readable && writable ? 18 : (readable ? 6 : 10)));
            memory.write8(address + 18, 0xff);
            memory.write8(address + 19, 0xff);
            memory.write64(address + 24, address + 119);
            memory.write32(address + 32, 1);
            Stream value;
            value.descriptor = -1;
            value.backend = std::move(backend);
            std::lock_guard lock(mutex); streams.emplace(address, std::move(value));
        } catch (...) { context.release_guest(address); throw; }
        return address;
    }
    std::uint64_t new_stream(int descriptor) {
        // Public Darwin FILE prefix, independently populated. Zero counters
        // force compiled getc/putc inline paths through our checked callbacks.
        const auto record = descriptor_file(descriptor);
        const auto flags = !record ? (descriptor == 0 ? 6 : 10) :
            (record->readable && record->writable ? 18 : (record->readable ? 6 : 10));
        const auto address = context.allocate_guest(152);
        try {
            auto& memory = static_cast<transmute::GuestMemory&>(context.calling_engine());
            memory.fill_bytes(address, 0, 152);
            memory.write64(address, address + 119);
            memory.write8(address + 16, static_cast<std::uint8_t>(flags));
            memory.write8(address + 18, static_cast<std::uint8_t>(descriptor));
            memory.write8(address + 19, static_cast<std::uint8_t>(descriptor >> 8));
            memory.write64(address + 24, address + 119);
            memory.write32(address + 32, 1);
            std::lock_guard lock(mutex); streams.emplace(address, Stream{descriptor});
        } catch (...) { context.release_guest(address); throw; }
        return address;
    }
    std::uint64_t output_text(transmute::GuestMemory& memory, int descriptor, std::string_view value) {
        const auto temporary = context.allocate_guest(std::max<std::size_t>(value.size(), 1));
        try {
            memory.write_bytes(temporary, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
            std::uint64_t total = 0;
            while (total < value.size()) {
                const auto result = write(memory, descriptor, temporary + total, value.size() - total);
                if (!result) throw ServiceError{eio};
                total += result;
            }
            context.release_guest(temporary);
            return total;
        } catch (...) { context.release_guest(temporary); throw; }
    }
    std::uint64_t output_stream(transmute::GuestMemory& memory, std::uint64_t address, std::string_view value) {
        const auto record = stream(address);
        std::lock_guard stream_lock(*record.lock);
        try {
            if (!record.backend) return output_text(memory, record.descriptor, value);
            const auto temporary = context.allocate_guest(std::max<std::size_t>(value.size(), 1));
            try {
                memory.write_bytes(temporary, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
                std::uint64_t total = 0;
                while (total < value.size()) {
                    const auto count = record.backend->write(memory, temporary + total, value.size() - total);
                    if (!count) throw ServiceError{eio};
                    total += count;
                }
                context.release_guest(temporary);
                return total;
            } catch (...) { context.release_guest(temporary); throw; }
        }
        catch (const ServiceError&) {
            std::lock_guard lock(mutex);
            const auto found = streams.find(address);
            if (found != streams.end()) found->second.error = true;
            throw;
        }
    }
    void update_environment() {
        auto& memory = context.calling_engine();
        std::vector<std::uint64_t> values;
        for (const auto& value : context.environment) {
            const auto found = environment_strings.find(value);
            if (found != environment_strings.end()) values.push_back(found->second);
            else {
                const auto address = context.allocate_guest(value.size() + 1);
                put_text(memory, address, value);
                environment_strings.emplace(value, address);
                values.push_back(address);
            }
        }
        const auto array = context.allocate_guest((values.size() + 1) * 8);
        for (std::size_t index = 0; index < values.size(); ++index) memory.write64(array + index * 8, values[index]);
        memory.write64(array + values.size() * 8, 0);
        memory.write64(environment_cell, array);
    }
    std::uint64_t stat(transmute::GuestMemory& memory, std::uint64_t pathname, std::uint64_t result, bool descriptor) {
        validate(memory, result, 144, true);
        if (descriptor) {
            const auto record = descriptor_file(static_cast<int>(pathname));
            if (record && record->random_device) {
                std::lock_guard lock(record->mutex); if (record->closed) throw ServiceError{ebadf};
                return random_device_stat(memory, result, record->path);
            }
            if (record && record->socket && record->socket->terminal) {
                std::lock_guard lock(record->mutex); if (record->closed) throw ServiceError{ebadf};
                return terminal_stat(memory, result, *record->socket);
            }
            if (record && record->socket) {
                std::lock_guard lock(record->mutex); if (record->closed) throw ServiceError{ebadf};
                memory.fill_bytes(result, 0, 144);
                memory.write32(result, 0x414c43); memory.write64(result + 8, record->socket->identity);
                memory.write8(result + 4, 0xb6); memory.write8(result + 5, 0xc1); // S_IFSOCK | 0666
                memory.write8(result + 6, 1); memory.write32(result + 112, 4096); return 0;
            }
            if (record && record->queue) throw ServiceError{45};
            if (record && record->pipe) {
                std::lock_guard lock(record->mutex); if (record->closed) throw ServiceError{ebadf};
                memory.fill_bytes(result, 0, 144);
                memory.write32(result, 0x414c43); memory.write64(result + 8, 0x5049504500000000ULL | static_cast<std::uint32_t>(record->native.value));
                memory.write8(result + 4, 0x80); memory.write8(result + 5, 0x11); // S_IFIFO | 0600
                memory.write8(result + 6, 1); memory.write32(result + 16, 1000); memory.write32(result + 20, 1000);
                memory.write32(result + 112, 4096); return 0;
            }
            if (record && record->null_device) {
                std::lock_guard lock(record->mutex); if (record->closed) throw ServiceError{ebadf};
                return device_stat(memory, result, device_nodes[0], 0);
            }
        } else if (const auto name = path(memory, pathname); is_random_device(name)) return random_device_stat(memory, result, name);
        else if (const auto* node = device_node(path(memory, pathname))) return device_stat(memory, result, *node, current_credentials()[0]);
        const bool null_device = descriptor ? [&] { const auto record = descriptor_file(static_cast<int>(pathname));
            return record && (record->null_device || record->standard >= 0); }() : is_null_device(path(memory, pathname));
        if (null_device || (descriptor && !descriptor_file(static_cast<int>(pathname)))) {
            // Alchemy owns its standard streams; no native Windows FILE or
            // handle is exposed. They are guest character devices.
            memory.fill_bytes(result, 0, 144);
            memory.write8(result + 4, 0xb6); memory.write8(result + 5, 0x21); // 0020666
            memory.write8(result + 6, 1); memory.write64(result + 8, pathname + 1);
            memory.write32(result + 16, 1000); memory.write32(result + 20, 1000);
            memory.write32(result + 112, 4096); return 0;
        }
        // One metadata query: Python's import system stats ~1500 paths at
        // startup. The permission bits and st_rdev follow what the Windows C
        // runtime's stat reports, so earlier guests see the same values.
        HostMetadata details;
        std::filesystem::path host;
        bool by_path = true;   // fstat of an open file knows no name: no execute bits from it, st_rdev 0
        if (descriptor) {
            const auto record = file(static_cast<int>(pathname));
            std::lock_guard lock(record->mutex);
            if (record->closed) throw ServiceError{ebadf};
            host = record->path;
            by_path = record->directory || !record->native;
            details = by_path ? host_metadata(record->path) : host_metadata(record->native);
        } else {
            host = path(memory, pathname);
            details = host_metadata(host);
        }
        // Read always, write unless read-only, execute for directories and
        // .exe/.cmd/.bat/.com, copied to group and other.
        std::uint32_t permissions = 0400 | (details.read_only ? 0 : 0200);
        if (details.directory) permissions |= 0100;
        else if (by_path) {
            auto extension = path_text(host.extension());
            for (auto& c : extension) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
            if (extension == ".exe" || extension == ".cmd" || extension == ".bat" || extension == ".com") permissions |= 0100;
        }
        permissions |= (permissions & 0700) >> 3;
        permissions |= (permissions & 0700) >> 6;
        if (details.private_owner) permissions = details.directory ? 0700 : 0600;
        // st_rdev: the drive number (0 = A:).
        std::uint32_t drive = 0;
        if (const auto volume = path_text(host.root_name()); by_path && volume.size() == 2 && volume[1] == ':')
            drive = static_cast<std::uint32_t>((volume[0] >= 'a' ? volume[0] - 32 : volume[0]) - 'A');
        memory.fill_bytes(result, 0, 144);
        memory.write32(result, details.device);
        memory.write64(result + 8, details.inode);
        const auto links = static_cast<std::uint16_t>(std::min<std::uint64_t>(details.links, UINT16_MAX));
        memory.write8(result + 6, static_cast<std::uint8_t>(links));
        memory.write8(result + 7, static_cast<std::uint8_t>(links >> 8));
        // Darwin uid/gid and BSD flags have no established host mapping.
        memory.write32(result + 16, 1000); memory.write32(result + 20, 1000);
        memory.write32(result + 24, drive);
        for (std::size_t index = 0; index < details.times.size(); ++index) {
            memory.write64(result + 32 + index * 16, details.times[index][0]);
            memory.write64(result + 40 + index * 16, details.times[index][1]);
        }
        memory.write64(result + 96, details.size);
        memory.write64(result + 104, (details.allocated + 511) / 512);
        memory.write32(result + 112, 4096);
        const auto mode = static_cast<std::uint16_t>(permissions | (details.directory ? 0040000 : 0100000));
        memory.write8(result + 4, static_cast<std::uint8_t>(mode)); memory.write8(result + 5, static_cast<std::uint8_t>(mode >> 8));
        return 0;
    }
    // Process model (process/process_model.cpp).
    std::uint64_t pipe_read(transmute::GuestMemory& memory, const std::shared_ptr<File>& record, std::uint64_t address, std::uint64_t size);
    std::uint64_t pipe_write(transmute::GuestMemory& memory, const std::shared_ptr<File>& record, std::uint64_t address, std::uint64_t size);
    std::uint16_t pipe_ready(const std::shared_ptr<File>& record, std::uint16_t events);
    std::uint64_t pipe_available(const std::shared_ptr<File>& record);
    std::array<int, 2> create_pipe(bool cloexec);
    std::array<SignalAction, 32>& signal_table();
    // The environment of the fork child running on this thread (nullptr in a
    // process); `create` copies the parent's on the child's first change.
    std::vector<std::string>* fork_child_environment(bool create = false);
    ExecEnvironment exec_environment();
    bool is_virtual_program(std::string_view value) { return virtual_program(value, exec_environment()); }
    void install_inherited_descriptors();
    bool install_descriptor(const ChildFd& fd);   // one ChildFd as guest descriptor fd.fd
    void install_process_services();
    void publish_identity();   // process_tree record of this process
    std::uint32_t process_id() const;
    bool fork_child_handles_exit() const noexcept;
    [[noreturn]] void fork_child_exit(std::uint32_t status);
    void install_all();
    void install_extra();
    void install_python_services();
    void install_memory_streams();   // fs/memory_streams.cpp
    void install_process_identity();
    void install_file_services();
    void install_libsystem_files();
    void install_terminals();
    void install_network_services();
    // Pseudo-terminals (fs/terminal.cpp).
    std::pair<std::shared_ptr<NetworkSocket>, std::shared_ptr<NetworkSocket>> connected_stream_pair();
    int open_master_terminal(std::uint64_t flags);
    std::optional<int> open_pseudo_terminal(const std::filesystem::path& name, std::uint64_t flags);
    std::shared_ptr<Terminal> terminal_for(std::uint32_t number);
    void attach_terminal(const std::shared_ptr<NetworkSocket>& socket, std::uint32_t number, bool master);
    std::uint64_t terminal_write(transmute::GuestMemory& memory, const std::shared_ptr<File>& record, std::uint64_t address, std::uint64_t size);
    std::uint64_t terminal_stat(transmute::GuestMemory& memory, std::uint64_t result, const NetworkSocket& socket);
    std::optional<std::uint64_t> terminal_ioctl(transmute::GuestMemory& memory, const std::shared_ptr<File>& record, std::uint64_t request, std::uint64_t argument);
    std::shared_ptr<NetworkSocket> terminal_socket(int descriptor);   // the descriptor's terminal end, or null
    bool is_terminal(int descriptor);   // isatty(3)
    std::map<std::uint32_t, std::weak_ptr<Terminal>> terminals;   // by number, the ones open in this process
    std::uint64_t terminal_name{};   // ptsname/ttyname's buffer
    std::shared_ptr<File> network_file(int descriptor);
    int add_socket(std::shared_ptr<NetworkSocket> socket);
    std::shared_ptr<NetworkSocket> create_socket(int domain, int type, int protocol);
    struct UnixAddress;
    UnixAddress unix_address(const transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t length, bool connecting);
    std::uint64_t network_message(transmute::GuestMemory& memory, int descriptor, std::uint64_t message, std::uint64_t flags, bool sending);
    std::uint64_t network_io(transmute::GuestMemory& memory, const std::shared_ptr<File>& record,
        std::uint64_t address, std::uint64_t size, std::uint64_t flags, bool sending,
        std::uint64_t peer = 0, std::uint64_t peer_length = 0);
    std::uint16_t network_ready(const std::shared_ptr<File>& record, std::uint16_t events);
    std::uint64_t network_read_hint(const std::shared_ptr<File>& record);
    std::shared_ptr<VnodeState> vnode_watch(const std::shared_ptr<File>& record);
    std::uint32_t vnode_poll(VnodeState& state);
    std::uint64_t network_poll(transmute::CpuState& state, transmute::GuestMemory& memory, bool select_mode);
    std::uint64_t syscall(std::uint64_t number, transmute::CpuState& state, transmute::GuestMemory& memory);
};

} // namespace alchemy
