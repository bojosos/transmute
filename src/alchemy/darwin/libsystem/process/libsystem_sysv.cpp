#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sections.hpp"
#include "alchemy/host/sync.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>


// System V semaphores (semget, semctl, semop) shared by all Alchemy
// processes of the session. The sets live in host shared memory, never in
// guest memory: a named file mapping ("Local\alchemy.sysv.sem") holds a
// fixed table of sets, guarded by a named mutex, so semop's all-or-nothing
// operation arrays are atomic across processes. Blocking operations wait by
// re-checking every few milliseconds (no lost wake-ups; latency instead of a
// kernel wait queue) and return EINTR when the process is asked to exit.
// SEM_UNDO adjustments are applied when this process exits normally.
//
// Divergences from Darwin: at most 128 sets of at most 64 semaphores; the
// table disappears with the last Alchemy process using it (Darwin keeps sets
// until IPC_RMID or reboot); every Alchemy process runs as the same virtual
// user, so permission bits are recorded but every access is the owner's.
namespace alchemy::export_fill {
namespace {

// <sys/ipc.h>, <sys/sem.h>, <sys/errno.h> (Darwin)
constexpr std::uint32_t ipc_creat = 0x200, ipc_excl = 0x400, ipc_nowait = 0x800, sem_undo = 0x1000;
constexpr std::uint32_t ipc_rmid = 0, ipc_set = 1, ipc_stat = 2, getncnt = 3, getpid = 4, getval = 5, getall = 6, getzcnt = 7,
    setval = 8, setall = 9;
constexpr std::uint32_t enoent_value = 2, eexist_value = 17, enospc = 28, erange_value = 34, e2big = 7, efbig = 27, eidrm = 90;
constexpr std::int32_t sem_value_limit = 32767;   // SEMVMX
constexpr std::size_t max_sets = 128, max_semaphores = 64, max_operations = 32;

struct Semaphore { std::int32_t value, pid, ncnt, zcnt; };
struct Set {
    std::int32_t used, key, sequence, nsems;
    std::uint32_t mode, uid, gid;
    std::int64_t otime, ctime;
    Semaphore semaphores[max_semaphores];
};
struct Table { std::uint32_t magic; std::uint32_t pad; Set sets[max_sets]; };
constexpr std::uint32_t table_magic = 0x53595356;   // "SYSV"

std::int64_t now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

class SharedTable {
public:
    SharedTable() {
        // Named objects of the login session, shared by every Alchemy process.
        if (const auto mutex = host::sync::create_mutex("alchemy.sysv.sem.lock")) mutex_ = *mutex;
        if (const auto mapping = host::sections::open_named("alchemy.sysv.sem", sizeof(Table))) {
            mapping_ = mapping->handle;
            if (const auto view = host::sections::map(mapping_, 0, sizeof(Table), true)) table_ = reinterpret_cast<Table*>(*view);
        }
        if (!mutex_ || !table_) {
            release();
            throw std::runtime_error("Alchemy System V semaphores: the shared table could not be created");
        }
        Guard guard(*this);
        if (table_->magic != table_magic) { std::memset(table_, 0, sizeof(Table)); table_->magic = table_magic; }
    }
    ~SharedTable() { release(); }
    struct Guard {
        explicit Guard(SharedTable& owner) : owner_(owner) {
            // An abandoned lock (a process died holding it) is still acquired.
            const std::array mutexes{owner_.mutex_};
            if (!host::sync::wait_any(mutexes, std::nullopt)) throw std::runtime_error("Alchemy System V semaphores: lock failed");
        }
        ~Guard() { static_cast<void>(host::sync::unlock_mutex(owner_.mutex_)); }
        SharedTable& owner_;
    };
    Table& table() { return *table_; }

private:
    void release() noexcept {
        if (table_) static_cast<void>(host::sections::unmap(reinterpret_cast<std::uintptr_t>(table_)));
        if (mapping_) host::close(mapping_);
        if (mutex_) host::close(mutex_);
        table_ = nullptr; mapping_ = {}; mutex_ = {};
    }
    host::Handle mutex_, mapping_;
    Table* table_{};
};

std::int32_t identifier(std::size_t index, const Set& set) { return static_cast<std::int32_t>(set.sequence * static_cast<std::int32_t>(max_sets) + static_cast<std::int32_t>(index)); }

// The live set named by a semid, or nullptr.
Set* lookup(Table& table, std::int64_t semid) {
    if (semid < 0) return nullptr;
    const auto index = static_cast<std::size_t>(semid % static_cast<std::int64_t>(max_sets));
    auto& set = table.sets[index];
    return set.used && identifier(index, set) == semid ? &set : nullptr;
}

std::int32_t process_id() { return static_cast<std::int32_t>(host::process::current_pid()); }

struct State {
    SharedTable shared;
    std::mutex mutex;
    std::map<std::pair<std::int64_t, std::uint16_t>, std::int32_t> undo;   // (semid, semaphore) -> adjustment
    ~State() {
        try {
            SharedTable::Guard guard(shared);
            for (const auto& [where, adjustment] : undo) {
                if (auto* set = lookup(shared.table(), where.first); set && where.second < set->nsems) {
                    auto& value = set->semaphores[where.second].value;
                    value = std::clamp(value + adjustment, 0, sem_value_limit);
                }
            }
        } catch (...) {
        }
    }
};

} // namespace

void install_libsystem_sysv(Provider& p) {
    auto& process = p.process();
    std::shared_ptr<State> state;
    try { state = std::make_shared<State>(); }
    catch (const std::exception&) {
        // No shared table: the calls are unavailable (ENOSYS), reported once each.
        auto reported = std::make_shared<std::pair<std::mutex, std::set<std::string>>>();
        for (const auto* name : {"_semget", "_semctl", "_semop"}) {
            p.libsystem(name, [&process, reported, name](CpuState&, GuestMemory&) -> std::uint64_t {
                bool first;
                { std::lock_guard lock(reported->first); first = reported->second.insert(name).second; }
                if (first) {
                    const std::string message = std::string("Alchemy: ") + (name + 1) +
                        " is unsupported (no System V shared memory between Alchemy processes)\n";
                    static_cast<void>(process.output(2, std::span(reinterpret_cast<const std::uint8_t*>(message.data()), message.size())));
                }
                throw Errno{enosys};
            });
        }
        return;
    }

    // int semget(key_t, int nsems, int semflg)
    p.libsystem("_semget", [state](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto key = static_cast<std::int32_t>(s.x[0]);
        const auto nsems = static_cast<std::int32_t>(s.x[1]);
        const auto flags = static_cast<std::uint32_t>(s.x[2]);
        SharedTable::Guard guard(state->shared);
        auto& table = state->shared.table();
        if (key != 0) {
            for (std::size_t index = 0; index < max_sets; ++index) {
                auto& set = table.sets[index];
                if (!set.used || set.key != key) continue;
                if ((flags & ipc_creat) && (flags & ipc_excl)) throw Errno{eexist_value};
                if (nsems > set.nsems || nsems < 0) throw Errno{einval};
                return static_cast<std::uint64_t>(identifier(index, set));
            }
            if (!(flags & ipc_creat)) throw Errno{enoent_value};
        }
        if (nsems <= 0 || nsems > static_cast<std::int32_t>(max_semaphores)) throw Errno{einval};
        for (std::size_t index = 0; index < max_sets; ++index) {
            auto& set = table.sets[index];
            if (set.used) continue;
            const auto sequence = set.sequence + 1;
            std::memset(&set, 0, sizeof(Set));
            set.used = 1;
            set.key = key;
            set.sequence = sequence;
            set.nsems = nsems;
            set.mode = flags & 0777;
            set.uid = set.gid = 1000;
            set.ctime = now();
            return static_cast<std::uint64_t>(identifier(index, set));
        }
        throw Errno{enospc};
    });

    // int semop(int semid, struct sembuf* sops, size_t nsops)
    // struct sembuf { unsigned short sem_num; short sem_op; short sem_flg; }
    p.libsystem("_semop", [&process, state](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto semid = static_cast<std::int32_t>(s.x[0]);
        const auto count = s.x[2];
        if (count > max_operations) throw Errno{e2big};
        struct Operation { std::uint16_t number; std::int16_t op, flags; };
        std::vector<Operation> operations(static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < operations.size(); ++i) {
            const auto at = s.x[1] + i * 6;
            operations[i] = {static_cast<std::uint16_t>(m.read8(at) | (m.read8(at + 1) << 8)),
                             static_cast<std::int16_t>(m.read8(at + 2) | (m.read8(at + 3) << 8)),
                             static_cast<std::int16_t>(m.read8(at + 4) | (m.read8(at + 5) << 8))};
        }
        auto delay = std::chrono::milliseconds(1);
        bool counted = false;
        for (;;) {
            {
                SharedTable::Guard guard(state->shared);
                auto* set = lookup(state->shared.table(), semid);
                if (!set) throw Errno{counted ? eidrm : einval};
                bool ready = true;
                std::size_t blocking = 0;
                for (std::size_t i = 0; i < operations.size() && ready; ++i) {
                    const auto& operation = operations[i];
                    if (operation.number >= set->nsems) throw Errno{efbig};
                    const auto value = set->semaphores[operation.number].value;
                    if (operation.op < 0 && value < -operation.op) { ready = false; blocking = i; }
                    else if (operation.op == 0 && value != 0) { ready = false; blocking = i; }
                    else if (operation.op > 0 && value + operation.op > sem_value_limit) throw Errno{erange_value};
                }
                // Values change only when every operation can proceed.
                if (ready) {
                    if (counted) {
                        for (const auto& operation : operations) {
                            auto& semaphore = set->semaphores[operation.number];
                            if (operation.op < 0 && semaphore.ncnt) --semaphore.ncnt;
                            if (operation.op == 0 && semaphore.zcnt) --semaphore.zcnt;
                        }
                    }
                    std::lock_guard lock(state->mutex);
                    for (const auto& operation : operations) {
                        auto& semaphore = set->semaphores[operation.number];
                        semaphore.value += operation.op;
                        semaphore.pid = process_id();
                        if ((operation.flags & sem_undo) && operation.op != 0) state->undo[{semid, operation.number}] -= operation.op;
                    }
                    set->otime = now();
                    return 0;
                }
                if (operations[blocking].flags & ipc_nowait) throw Errno{eagain};
                if (!counted) {
                    auto& semaphore = set->semaphores[operations[blocking].number];
                    if (operations[blocking].op < 0) ++semaphore.ncnt; else ++semaphore.zcnt;
                    counted = true;
                }
            }
            if (process_exiting(process)) throw Errno{eintr};
            std::this_thread::sleep_for(delay);
            delay = std::min(delay * 2, std::chrono::milliseconds(10));
        }
    });

    // int semctl(int semid, int semnum, int cmd, ...): the union semun
    // argument arrives in a variadic stack slot.
    p.libsystem("_semctl", [state](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto semid = static_cast<std::int32_t>(s.x[0]);
        const auto number = static_cast<std::int32_t>(s.x[1]);
        const auto command = static_cast<std::uint32_t>(s.x[2]);
        const auto argument = variadic(s, m, 0);
        SharedTable::Guard guard(state->shared);
        auto* set = lookup(state->shared.table(), semid);
        if (!set) throw Errno{einval};
        const auto semaphore = [&]() -> Semaphore& {
            if (number < 0 || number >= set->nsems) throw Errno{einval};
            return set->semaphores[number];
        };
        switch (command) {
        case ipc_rmid: {
            std::memset(set->semaphores, 0, sizeof(set->semaphores));
            set->used = 0;
            std::lock_guard lock(state->mutex);
            std::erase_if(state->undo, [semid](const auto& entry) { return entry.first.first == semid; });
            return 0;
        }
        case getval: return static_cast<std::uint64_t>(semaphore().value);
        case getpid: return static_cast<std::uint64_t>(semaphore().pid);
        case getncnt: return static_cast<std::uint64_t>(semaphore().ncnt);
        case getzcnt: return static_cast<std::uint64_t>(semaphore().zcnt);
        case setval: {
            const auto value = static_cast<std::int32_t>(argument);
            if (value < 0 || value > sem_value_limit) throw Errno{erange_value};
            semaphore().value = value;
            set->ctime = now();
            std::lock_guard lock(state->mutex);
            state->undo.erase({semid, static_cast<std::uint16_t>(number)});
            return 0;
        }
        case getall:
            for (std::int32_t i = 0; i < set->nsems; ++i) write16(m, argument + static_cast<std::uint64_t>(i) * 2, static_cast<std::uint16_t>(set->semaphores[i].value));
            return 0;
        case setall: {
            for (std::int32_t i = 0; i < set->nsems; ++i) {
                const auto value = static_cast<std::int32_t>(m.read8(argument + static_cast<std::uint64_t>(i) * 2) |
                                                             (m.read8(argument + static_cast<std::uint64_t>(i) * 2 + 1) << 8));
                if (value > sem_value_limit) throw Errno{erange_value};
                set->semaphores[i].value = value;
            }
            set->ctime = now();
            std::lock_guard lock(state->mutex);
            std::erase_if(state->undo, [semid](const auto& entry) { return entry.first.first == semid; });
            return 0;
        }
        case ipc_stat: {
            // struct semid_ds (Darwin, 4-byte packing): ipc_perm {uid, gid, cuid,
            // cgid, mode u16, seq u16, key}, sem_base, sem_nsems u16, sem_otime,
            // pad, sem_ctime, pad[5].
            std::vector<std::uint8_t> bytes(72, 0);
            const auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(bytes.data() + at, &v, 4); };
            const auto put64 = [&](std::size_t at, std::int64_t v) { std::memcpy(bytes.data() + at, &v, 8); };
            put32(0, set->uid); put32(4, set->gid); put32(8, set->uid); put32(12, set->gid);
            put32(16, (set->mode & 0xffffU) | (static_cast<std::uint32_t>(set->sequence & 0xffff) << 16));
            put32(20, static_cast<std::uint32_t>(set->key));
            put32(28, static_cast<std::uint32_t>(set->nsems));
            put64(32, set->otime);
            put64(44, set->ctime);
            m.write_bytes(argument, bytes);
            return 0;
        }
        case ipc_set: {
            const auto mode = m.read32(argument + 16) & 0x1ffU;
            set->mode = mode;
            set->ctime = now();
            return 0;
        }
        default: throw Errno{einval};
        }
    });
}

} // namespace alchemy::export_fill
