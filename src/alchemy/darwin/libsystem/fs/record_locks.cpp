#include "alchemy/record_locks.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sections.hpp"
#include "alchemy/host/sync.hpp"
#include "alchemy/process_tree.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace alchemy::record_locks {
namespace {

struct Entry {
    std::uint32_t host_pid, guest_pid;
    std::int32_t type;
    std::uint32_t reserved;
    std::uint64_t host_created;   // host::process::start_time of the owner (a reused host pid is not the owner)
    std::int64_t start, end;
};
constexpr std::uint32_t capacity = 500;
struct Table {
    static constexpr std::uint32_t expected_magic = 0x6b636c72;   // "rlck"
    std::uint32_t magic, count;
    Entry entries[capacity];
};

std::atomic<unsigned> held_files{0};

bool overlaps(const Entry& entry, std::int64_t start, std::int64_t end) { return entry.start < end && start < entry.end; }

std::uint64_t own_created() {
    static const std::uint64_t value = host::process::start_time().value_or(0);
    return value;
}

bool owner_running(const Entry& entry) {
    if (entry.host_pid == host::process::current_pid()) return entry.host_created == own_created();
    const auto created = host::process::start_time(entry.host_pid);
    if (!created) return created.error() == host::Error::access_denied;   // exists, but not ours to ask: keep it
    const auto running = host::process::is_running(entry.host_pid);
    return running && *running && *created == entry.host_created;
}

// The named mutex and table of one file. This process keeps them open while
// it holds locks there (the objects live while any process has them open).
struct File {
    host::Handle mutex, mapping;
    Table* table{};
    unsigned own{};   // this process's entries
};
std::mutex files_mutex;
std::map<std::string, File, std::less<>> files;

// Opens (or creates) the objects of `key`; files_mutex is held.
File* open_file(const std::string& key) {
    if (const auto found = files.find(key); found != files.end()) return &found->second;
    const auto base = "alchemy-record-locks-" + key;
    File file;
    const auto mutex = host::sync::create_mutex(base + "-mutex");
    if (!mutex) return nullptr;
    file.mutex = *mutex;
    if (const auto mapping = host::sections::open_named(base + "-table", sizeof(Table))) {
        file.mapping = mapping->handle;
        if (const auto view = host::sections::map(file.mapping, 0, sizeof(Table), true)) file.table = reinterpret_cast<Table*>(*view);
    }
    if (!file.table) {
        if (file.mapping) host::close(file.mapping);
        host::close(file.mutex);
        return nullptr;
    }
    return &files.emplace(key, file).first->second;
}

void close_if_unused(const std::string& key) {
    const auto found = files.find(key);
    if (found == files.end() || found->second.own) return;
    static_cast<void>(host::sections::unmap(reinterpret_cast<std::uintptr_t>(found->second.table)));
    host::close(found->second.mapping);
    host::close(found->second.mutex);
    files.erase(found);
}

struct Guard {
    host::Handle mutex;
    explicit Guard(host::Handle value) : mutex(value) {
        // An abandoned lock (the previous holder died) is acquired all the same;
        // the table is still well-formed (entries are written whole before
        // `count` covers them).
        const std::array mutexes{mutex};
        static_cast<void>(host::sync::wait_any(mutexes, std::nullopt));
    }
    ~Guard() { static_cast<void>(host::sync::unlock_mutex(mutex)); }
};

// Drops entries of processes that no longer run; table mutex held.
void sweep(Table& table) {
    if (table.magic != Table::expected_magic) { table.magic = Table::expected_magic; table.count = 0; }
    table.count = std::min(table.count, capacity);
    std::uint32_t kept = 0;
    for (std::uint32_t index = 0; index < table.count; ++index)
        if (owner_running(table.entries[index])) table.entries[kept++] = table.entries[index];
    table.count = kept;
}

bool own_entry(const Entry& entry) { return entry.host_pid == host::process::current_pid() && entry.host_created == own_created(); }

std::optional<Conflict> first_conflict(const Table& table, std::int64_t start, std::int64_t end, int type) {
    for (std::uint32_t index = 0; index < table.count; ++index) {
        const auto& entry = table.entries[index];
        if (own_entry(entry) || !overlaps(entry, start, end)) continue;
        if (type == read_lock && entry.type == read_lock) continue;
        return Conflict{entry.type, entry.start, entry.end, entry.guest_pid};
    }
    return std::nullopt;
}

std::uint32_t guest_pid() {
    if (const auto self = process_tree::self()) return self->pid;
    return host::process::current_pid();
}

// Replaces this process's locks on [start, end) by `type`; returns this
// process's entry count, or nullopt when the table has no room.
std::optional<unsigned> apply(Table& table, std::int64_t start, std::int64_t end, int type) {
    std::vector<Entry> others, own;
    for (std::uint32_t index = 0; index < table.count; ++index)
        (own_entry(table.entries[index]) ? own : others).push_back(table.entries[index]);
    std::vector<Entry> next;
    for (const auto& entry : own) {
        if (!overlaps(entry, start, end)) { next.push_back(entry); continue; }
        if (entry.start < start) { auto left = entry; left.end = start; next.push_back(left); }
        if (end < entry.end) { auto right = entry; right.start = end; next.push_back(right); }
    }
    if (type != unlock) {
        Entry added{};
        added.host_pid = host::process::current_pid(); added.guest_pid = guest_pid(); added.type = type;
        added.host_created = own_created(); added.start = start; added.end = end;
        next.push_back(added);
    }
    // Coalesce adjacent ranges of one type.
    std::sort(next.begin(), next.end(), [](const Entry& a, const Entry& b) { return a.start < b.start; });
    std::vector<Entry> merged;
    for (const auto& entry : next) {
        if (!merged.empty() && merged.back().type == entry.type && merged.back().end >= entry.start) merged.back().end = std::max(merged.back().end, entry.end);
        else merged.push_back(entry);
    }
    if (others.size() + merged.size() > capacity) return std::nullopt;
    std::uint32_t count = 0;
    for (const auto& entry : others) table.entries[count++] = entry;
    for (const auto& entry : merged) table.entries[count++] = entry;
    table.count = count;
    return static_cast<unsigned>(merged.size());
}

} // namespace

std::string file_key(std::intptr_t native) {
    const auto identity = host::files::identity(host::Handle{static_cast<std::uintptr_t>(native)});
    if (!identity) return {};
    constexpr std::string_view digits = "0123456789abcdef";
    std::string key;
    for (unsigned shift = 0; shift < 64; shift += 4) key += digits[(identity->volume >> shift) & 15];
    for (const auto byte : identity->id) { key += digits[byte >> 4]; key += digits[byte & 15]; }
    return key;
}

int set(const std::string& key, std::int64_t start, std::int64_t end, int type, bool wait, const std::function<bool()>& stop) {
    if (key.empty()) return 45;   // ENOTSUP
    if (start >= end) return 0;
    for (auto delay = std::chrono::milliseconds(1);;) {
        {
            std::lock_guard files_lock(files_mutex);
            auto* file = open_file(key);
            if (!file) return 77;   // ENOLCK
            const auto before = file->own;
            std::optional<unsigned> result;
            bool conflict = false;
            {
                Guard guard(file->mutex);
                sweep(*file->table);
                if (type != unlock && first_conflict(*file->table, start, end, type)) conflict = true;
                else result = apply(*file->table, start, end, type);
            }
            if (!conflict) {
                if (!result) { close_if_unused(key); return 77; }
                file->own = *result;
                if (!before && file->own) held_files.fetch_add(1);
                if (before && !file->own) { held_files.fetch_sub(1); close_if_unused(key); }
                return 0;
            }
            close_if_unused(key);
        }
        if (!wait) return 35;   // EAGAIN
        if (stop && stop()) return 89;
        std::this_thread::sleep_for(delay);
        delay = std::min(delay * 2, std::chrono::milliseconds(20));
    }
}

std::optional<Conflict> test(const std::string& key, std::int64_t start, std::int64_t end, int type) {
    if (key.empty() || start >= end) return std::nullopt;
    std::lock_guard files_lock(files_mutex);
    auto* file = open_file(key);
    if (!file) return std::nullopt;
    std::optional<Conflict> result;
    {
        Guard guard(file->mutex);
        sweep(*file->table);
        result = first_conflict(*file->table, start, end, type == read_lock ? read_lock : write_lock);
    }
    close_if_unused(key);
    return result;
}

void release_all(const std::string& key) {
    if (key.empty()) return;
    std::lock_guard files_lock(files_mutex);
    const auto found = files.find(key);
    if (found == files.end()) return;
    {
        Guard guard(found->second.mutex);
        static_cast<void>(apply(*found->second.table, INT64_MIN, to_end, unlock));
    }
    if (found->second.own) held_files.fetch_sub(1);
    found->second.own = 0;
    close_if_unused(key);
}

bool any_held() noexcept { return held_files.load(std::memory_order_relaxed) != 0; }

} // namespace alchemy::record_locks
