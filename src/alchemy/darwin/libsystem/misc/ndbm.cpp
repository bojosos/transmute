#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// ndbm(3): dbm_open, dbm_fetch, dbm_store, dbm_delete, dbm_firstkey, dbm_nextkey, dbm_error,
// dbm_clearerr and dbm_close (Python's dbm.ndbm). The database is the file "<name>.db",
// Alchemy's own format: a header line, then one record per change (store or delete),
// appended as it happens and replayed on open; dbm_close rewrites the file when it holds
// mostly superseded records. Files are reached through the guest's own open/read/write, so
// paths and permissions are the guest's. datum is {void* dptr; size_t dsize} and travels
// in two registers; fetched values and keys stay valid until the next call on that DBM.
namespace alchemy::export_fill {
namespace {

constexpr std::string_view header = "ALCHEMY-NDBM 1\n";
constexpr std::uint32_t o_rdonly = 0, o_wronly = 1, o_rdwr = 2, o_accmode = 3, o_creat = 0x200, o_trunc = 0x400;
constexpr std::uint32_t eftype = 79;   // "Inappropriate file type or format"

struct Database {
    std::int64_t fd{-1};
    std::string path;
    bool writable{}, failed{};
    std::map<std::string, std::string> entries;
    std::uint64_t records{};               // records in the file (live and superseded)
    std::optional<std::string> cursor;     // the key dbm_firstkey/dbm_nextkey returned last
    std::uint64_t value_buffer{}, value_capacity{}, key_buffer{}, key_capacity{};
};

struct State {
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<Database>> databases;
};

void append_u32(std::string& out, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) out.push_back(static_cast<char>(value >> (8 * index)));
}
std::uint32_t read_u32(const std::string& in, std::size_t at) {
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) value |= static_cast<std::uint32_t>(static_cast<unsigned char>(in[at + index])) << (8 * index);
    return value;
}
std::string store_record(const std::string& key, const std::string& value) {
    std::string out(1, 'S');
    append_u32(out, static_cast<std::uint32_t>(key.size())); append_u32(out, static_cast<std::uint32_t>(value.size()));
    return out + key + value;
}
std::string delete_record(const std::string& key) {
    std::string out(1, 'D');
    append_u32(out, static_cast<std::uint32_t>(key.size()));
    return out + key;
}

} // namespace

void install_libsystem_ndbm(Provider& p) {
    const auto state = std::make_shared<State>();
    auto& process = p.process();

    // Guest file calls.
    const auto write_all = [p](GuestMemory& m, std::int64_t fd, const std::string& bytes) {
        if (bytes.empty()) return true;
        const auto buffer = p.malloc(bytes.size());
        m.write_bytes(buffer, std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
        std::uint64_t done = 0;
        bool ok = true;
        while (done < bytes.size()) {
            const auto written = static_cast<std::int64_t>(p.call(libsystem_library, "_write", {static_cast<std::uint64_t>(fd), buffer + done, bytes.size() - done}));
            if (written <= 0) { ok = false; break; }
            done += static_cast<std::uint64_t>(written);
        }
        static_cast<void>(p.call(libsystem_library, "_free", {buffer}));
        return ok;
    };
    const auto open_file = [p](GuestMemory& m, const std::string& path, std::uint32_t flags, std::uint64_t mode) {
        const auto name = p.malloc(path.size() + 1);
        put_string(m, name, path);
        CallRegs registers{};
        registers.x[0] = name; registers.x[1] = flags;
        registers.push_variadic(mode);
        const auto fd = static_cast<std::int64_t>(static_cast<std::int32_t>(p.call_regs(libsystem_library, "_open", registers)));
        const auto error = p.guest_errno();
        static_cast<void>(p.call(libsystem_library, "_free", {name}));
        return std::pair{fd, error};
    };
    const auto read_all = [p](GuestMemory& m, std::int64_t fd) -> std::optional<std::string> {
        std::string bytes;
        const auto buffer = p.malloc(65536);
        std::optional<std::string> result;
        for (;;) {
            const auto count = static_cast<std::int64_t>(p.call(libsystem_library, "_read", {static_cast<std::uint64_t>(fd), buffer, 65536}));
            if (count < 0) break;
            if (count == 0) { result = std::move(bytes); break; }
            std::vector<std::uint8_t> chunk(static_cast<std::size_t>(count));
            m.read_bytes(buffer, chunk);
            bytes.append(chunk.begin(), chunk.end());
        }
        static_cast<void>(p.call(libsystem_library, "_free", {buffer}));
        return result;
    };
    // Rewrites `db` as its live entries only: "<path>.tmp", then rename over the file.
    const auto compact = [p, open_file, write_all](GuestMemory& m, Database& db) {
        std::string bytes(header);
        for (const auto& [key, value] : db.entries) bytes += store_record(key, value);
        const auto temporary = db.path + ".tmp";
        const auto fd = open_file(m, temporary, o_wronly | o_creat | o_trunc, 0644).first;
        if (fd < 0) return;
        const bool ok = write_all(m, fd, bytes);
        static_cast<void>(p.call(libsystem_library, "_close", {static_cast<std::uint64_t>(fd)}));
        const auto from = p.malloc(temporary.size() + 1), to = p.malloc(db.path.size() + 1);
        put_string(m, from, temporary); put_string(m, to, db.path);
        if (!ok || static_cast<std::int32_t>(p.call(libsystem_library, "_rename", {from, to})) != 0)
            static_cast<void>(p.call(libsystem_library, "_unlink", {from}));
        static_cast<void>(p.call(libsystem_library, "_free", {from}));
        static_cast<void>(p.call(libsystem_library, "_free", {to}));
    };
    const auto find = [state](std::uint64_t handle) {
        std::lock_guard lock(state->mutex);
        const auto found = state->databases.find(handle);
        if (found == state->databases.end()) throw Errno{einval};
        return found->second;
    };
    // Copies `bytes` into the DBM's buffer (grown with guest malloc) and returns it as a datum.
    const auto datum = [p](CpuState& s, GuestMemory& m, std::uint64_t& buffer, std::uint64_t& capacity, const std::string& bytes) {
        if (bytes.size() + 1 > capacity) {
            if (buffer) static_cast<void>(p.call(libsystem_library, "_free", {buffer}));
            capacity = std::max<std::uint64_t>(64, bytes.size() + 1);
            buffer = p.malloc(capacity);
        }
        if (!bytes.empty()) m.write_bytes(buffer, std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
        s.x[1] = bytes.size();
        return buffer;
    };
    const auto key_of = [](GuestMemory& m, std::uint64_t pointer, std::uint64_t size) {
        if (size > (1ULL << 30)) throw Errno{einval};
        std::string key(static_cast<std::size_t>(size), '\0');
        if (size) {
            if (!pointer) throw Errno{efault};
            m.read_bytes(pointer, std::span(reinterpret_cast<std::uint8_t*>(key.data()), key.size()));
        }
        return key;
    };

    // DBM* dbm_open(const char* file, int flags, mode_t mode)
    p.libsystem("_dbm_open", [p, state, open_file, read_all, write_all, &process](CpuState& s, GuestMemory& m) -> std::uint64_t {
        auto db = std::make_shared<Database>();
        db->path = guest_string(m, s.x[0], 1020) + ".db";
        auto flags = static_cast<std::uint32_t>(s.x[1]);
        if ((flags & o_accmode) == o_wronly) flags = (flags & ~o_accmode) | o_rdwr;   // the log is read back on open
        db->writable = (flags & o_accmode) == o_rdwr;
        const auto opened = open_file(m, db->path, flags, s.x[2] & 07777);
        const auto fd = opened.first;
        if (fd < 0) throw Errno{opened.second};
        db->fd = fd;
        const auto close_fail = [&](std::uint32_t value) {
            static_cast<void>(p.call(libsystem_library, "_close", {static_cast<std::uint64_t>(fd)}));
            throw Errno{value};
        };
        const auto bytes = read_all(m, fd);
        if (!bytes) close_fail(p.guest_errno());
        if (bytes->empty()) {
            if (db->writable && !write_all(m, fd, std::string(header))) close_fail(p.guest_errno());
        } else {
            if (bytes->compare(0, header.size(), header) != 0) close_fail(eftype);
            // Replay; a record cut short at the end (an interrupted append) is ignored.
            for (std::size_t at = header.size(); at < bytes->size();) {
                const auto kind = (*bytes)[at];
                if ((kind != 'S' && kind != 'D') || at + 5 > bytes->size()) break;
                const auto key_size = read_u32(*bytes, at + 1);
                if (kind == 'S') {
                    if (at + 9 > bytes->size()) break;
                    const auto value_size = read_u32(*bytes, at + 5);
                    if (at + 9 + key_size + value_size > bytes->size()) break;
                    db->entries[bytes->substr(at + 9, key_size)] = bytes->substr(at + 9 + key_size, value_size);
                    at += 9 + key_size + value_size;
                } else {
                    if (at + 5 + key_size > bytes->size()) break;
                    db->entries.erase(bytes->substr(at + 5, key_size));
                    at += 5 + key_size;
                }
                ++db->records;
            }
        }
        const auto handle = process.allocate_guest(16);
        std::lock_guard lock(state->mutex);
        state->databases[handle] = db;
        return handle;
    }, 0);

    // void dbm_close(DBM*)
    p.libsystem("_dbm_close", [p, state, compact, &process](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::shared_ptr<Database> db;
        {
            std::lock_guard lock(state->mutex);
            const auto found = state->databases.find(s.x[0]);
            if (found == state->databases.end()) return 0;
            db = found->second;
            state->databases.erase(found);
        }
        static_cast<void>(p.call(libsystem_library, "_close", {static_cast<std::uint64_t>(db->fd)}));
        if (db->writable && db->records > 2 * db->entries.size() + 16) compact(m, *db);
        for (const auto buffer : {db->value_buffer, db->key_buffer})
            if (buffer) static_cast<void>(p.call(libsystem_library, "_free", {buffer}));
        process.release_guest(s.x[0]);
        return 0;
    });

    // datum dbm_fetch(DBM*, datum key): {NULL, 0} when the key is absent.
    p.libsystem("_dbm_fetch", [find, datum, key_of](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto db = find(s.x[0]);
        const auto key = key_of(m, s.x[1], s.x[2]);
        const auto found = db->entries.find(key);
        if (found == db->entries.end()) { s.x[1] = 0; return 0; }
        return datum(s, m, db->value_buffer, db->value_capacity, found->second);
    }, 0);

    // int dbm_store(DBM*, datum key, datum content, int mode): DBM_INSERT 0 keeps an existing
    // key (returns 1), DBM_REPLACE 1 overwrites; -1 on error.
    p.libsystem("_dbm_store", [find, key_of, write_all](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto db = find(s.x[0]);
        if (!db->writable) throw Errno{eperm};
        const auto mode = static_cast<std::uint32_t>(s.x[5]);
        if (mode > 1) throw Errno{einval};
        const auto key = key_of(m, s.x[1], s.x[2]), value = key_of(m, s.x[3], s.x[4]);
        if (mode == 0 && db->entries.contains(key)) return 1;
        if (!write_all(m, db->fd, store_record(key, value))) { db->failed = true; throw Errno{eio}; }
        db->entries[key] = value;
        ++db->records;
        return 0;
    });

    // int dbm_delete(DBM*, datum key): 0, or -1 when the key is absent or on error.
    p.libsystem("_dbm_delete", [find, key_of, write_all](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto db = find(s.x[0]);
        if (!db->writable) throw Errno{eperm};
        const auto key = key_of(m, s.x[1], s.x[2]);
        if (!db->entries.contains(key)) return UINT64_MAX;
        if (!write_all(m, db->fd, delete_record(key))) { db->failed = true; throw Errno{eio}; }
        db->entries.erase(key);
        ++db->records;
        return 0;
    });

    // datum dbm_firstkey(DBM*) / dbm_nextkey(DBM*): the keys in byte order; {NULL, 0} at the end.
    for (const bool first : {true, false})
        p.libsystem(first ? "_dbm_firstkey" : "_dbm_nextkey", [find, datum, first](CpuState& s, GuestMemory& m) -> std::uint64_t {
            const auto db = find(s.x[0]);
            auto next = db->entries.begin();
            if (!first) {
                if (!db->cursor) { s.x[1] = 0; return 0; }
                next = db->entries.upper_bound(*db->cursor);
            }
            if (next == db->entries.end()) { db->cursor.reset(); s.x[1] = 0; return 0; }
            db->cursor = next->first;
            return datum(s, m, db->key_buffer, db->key_capacity, next->first);
        }, 0);

    // int dbm_error(DBM*) / dbm_clearerr(DBM*): the error indicator a failed write sets.
    p.libsystem("_dbm_error", [find](CpuState& s, GuestMemory&) -> std::uint64_t { return find(s.x[0])->failed ? 1 : 0; });
    p.libsystem("_dbm_clearerr", [find](CpuState& s, GuestMemory&) -> std::uint64_t { find(s.x[0])->failed = false; return 0; });
}

} // namespace alchemy::export_fill
