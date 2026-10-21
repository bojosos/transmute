#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/path.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace {
constexpr std::string_view system_library = "/usr/lib/libSystem.B.dylib";
void require(bool value, std::string_view description) {
    if (!value) throw std::runtime_error(std::string(description));
}
struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    std::string output;
    alchemy::ProcessContext context{engine, [this](unsigned descriptor, auto bytes) {
        require(descriptor == 1 || descriptor == 2, "invalid output descriptor");
        output.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return alchemy::OutputResult{bytes.size(), 0};
    }};
    alchemy::DarwinServices services{context};
    alchemy::DarwinThreads threads{context};
    Fixture() { context.working_directory = std::filesystem::current_path(); }
    transmute::GuestMemory& memory() { return engine; }
    std::uint64_t call(std::string_view name, std::initializer_list<std::uint64_t> arguments = {}) {
        const auto symbol = context.resolve(system_library, name);
        require(symbol.has_value(), "missing Python service symbol");
        return engine.invoke_callback(symbol->address, std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t string(std::string_view value) {
        const auto address = context.allocate_guest(value.size() + 1);
        engine.write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
        memory().write8(address + value.size(), 0); return address;
    }
    std::uint32_t error() { return engine.read32(context.current_thread()->errno_address); }
    void slot(std::uint64_t value) { engine.write64((engine.state().sp - 256) & ~15ULL, value); }
    std::string read_string(std::uint64_t address) {
        std::string value;
        for (unsigned index = 0; index < 4096; ++index) {
            const auto byte = memory().read8(address + index); if (!byte) return value;
            value += static_cast<char>(byte);
        }
        throw std::runtime_error("unterminated test string");
    }
};

struct TemporaryDirectory {
    Fixture& fixture;
    std::filesystem::path path;
    explicit TemporaryDirectory(Fixture& f) : fixture(f) {
        const auto specification = f.string("/tmp/alchemy-file-services-XXXXXX");
        require(f.call("_mkdtemp", {specification}) == specification, "temporary directory creation failed");
        path = f.context.map_guest_path(f.read_string(specification));
    }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    std::uint64_t guest(std::string_view leaf) {
        return fixture.string(alchemy::guest_path_string(path / std::filesystem::path(leaf)));
    }
};

void descriptor_io_and_stdio() {
    Fixture f;
    const auto filename = std::filesystem::temp_directory_path() /
        ("alchemy-python-services-" + std::to_string(f.call("_getpid")) + ".bin");
    const auto path = f.string(alchemy::guest_path_string(filename));
    const auto buffer = f.context.allocate_guest(64), vectors = f.context.allocate_guest(32);
    f.slot(0600);
    const auto descriptor = f.call("_open", {path, 2 | 0x200 | 0x400});
    require(descriptor != UINT64_MAX, "file creation failed");
    f.engine.write_bytes(buffer, std::array<std::uint8_t, 6>{1, 2, 3, 4, 5, 6});
    f.engine.write64(vectors, buffer); f.engine.write64(vectors + 8, 2);
    f.engine.write64(vectors + 16, buffer + 2); f.engine.write64(vectors + 24, 4);
    require(f.call("_writev", {descriptor, vectors, 2}) == 6, "writev byte count incorrect");
    require(f.call("_lseek", {descriptor, 1, 0}) == 1, "initial seek failed");
    const auto copy = f.call("_dup2", {descriptor, 20});
    require(copy == 20 && f.call("_read", {copy, buffer + 16, 1}) == 1 && f.memory().read8(buffer + 16) == 2,
        "dup2 failed to share file offset");
    require(f.call("_lseek", {descriptor, 0, 1}) == 2, "dup2 offset not shared");
    require(f.call("_pread", {descriptor, buffer + 16, 3, 3}) == 3 && f.memory().read8(buffer + 16) == 4,
        "pread content incorrect");
    require(f.call("_lseek", {copy, 0, 1}) == 2, "pread changed shared offset");
    require(f.call("_pwrite", {copy, buffer, 1, 4}) == 1 && f.call("_lseek", {descriptor, 0, 1}) == 2,
        "pwrite changed shared offset");
    f.engine.write64(vectors, buffer + 16); f.engine.write64(vectors + 8, 2);
    f.engine.write64(vectors + 16, buffer + 32); f.engine.write64(vectors + 24, 4);
    require(f.call("_preadv", {descriptor, vectors, 2, 2}) == 4 && f.memory().read8(buffer + 16) == 3 &&
        f.memory().read8(buffer + 32) == 1 && f.memory().read8(buffer + 33) == 6, "preadv scatter/partial count incorrect");
    require(f.call("_pread", {descriptor, 1, 1, 0}) == UINT64_MAX && f.error() == 14, "bad pread pointer not EFAULT");
    require(f.call("_readv", {descriptor, vectors, UINT64_MAX}) == UINT64_MAX && f.error() == 22, "negative vector count accepted");
    f.slot(0xa5a5a5a500000001ULL);
    require(f.call("_fcntl", {copy, 2}) == 0 && f.call("_fcntl", {copy, 1}) == 1, "FD_CLOEXEC int stack argument/padding incorrect");
    require(f.call("_fcntl", {descriptor, 1}) == 0, "descriptor flags leaked across dup2");
    f.slot(30); const auto second = f.call("_fcntl", {descriptor, 67});
    require(second == 30 && f.call("_fcntl", {second, 1}) == 1, "F_DUPFD_CLOEXEC failed");
    require(f.call("_fsync", {descriptor}) == 0, "fsync failed");
    require(f.call("_access", {path, 4}) == 0, "read access failed");
    const auto stream = f.call("_fdopen$DARWIN_EXTSN", {descriptor, f.string("r+")});
    require(stream != 0 && f.engine.query_region(stream)->size >= 152 && f.engine.read32(stream + 8) == 0,
        "Darwin FILE prefix/counter layout incorrect");
    static_cast<void>(f.call("_rewind", {stream}));
    f.engine.write32(stream + 8, UINT32_MAX); // stock compiled getc decrements this first
    require(f.call("___srget", {stream}) == 1 && f.engine.read32(stream + 8) == 0, "inline getc refill contract incorrect");
    static_cast<void>(f.call("_fclose", {stream}));
    require(f.call("_read", {copy, buffer, 1}) == 1, "closing fdopen also closed a duplicate");
    static_cast<void>(f.call("_close", {copy})); static_cast<void>(f.call("_close", {second}));
    require(f.call("_unlink", {path}) == 0, "fixture file cleanup failed");
    const auto standard_copy = f.call("_dup", {1});
    require(f.call("_write", {standard_copy, f.string("abc"), 3}) == 3 && f.output == "abc", "standard duplicate bypassed shared output");
    static_cast<void>(f.call("_close", {standard_copy}));
    require(f.call("_write", {1, f.string("d"), 1}) == 1 && f.output == "abcd", "closing duplicate closed original stdout");
}

void descriptor_inheritance_controls() {
    Fixture f;
    const auto descriptor = f.call("_open", {f.string("/dev/urandom"), 0}), duplicate = f.call("_dup", {descriptor});
    require(descriptor != UINT64_MAX && duplicate != UINT64_MAX, "inheritance descriptor fixture failed");
    // These Darwin IOC_VOID requests have no variadic parameter. A poisoned
    // stack cell must never be dereferenced or treated as a pointer argument.
    f.slot(UINT64_MAX);
    require(f.call("_ioctl", {descriptor, 0x20006601}) == 0 && f.call("_fcntl", {descriptor, 1}) == 1,
        "FIOCLEX did not set descriptor close-on-exec");
    require(f.call("_fcntl", {duplicate, 1}) == 0, "FIOCLEX changed duplicate descriptor flags");
    require(f.call("_ioctl", {descriptor, 0x20006602}) == 0 && f.call("_fcntl", {descriptor, 1}) == 0,
        "FIONCLEX did not clear descriptor close-on-exec");
    const auto cloexec_copy = f.call("_dup", {descriptor});
    require(f.call("_ioctl", {cloexec_copy, 0x20006601}) == 0 && f.call("_close", {cloexec_copy}) == 0,
        "duplicate inheritance control/lifetime failed");
    require(f.call("_ioctl", {cloexec_copy, 0x20006601}) == UINT64_MAX && f.error() == 9,
        "closed ioctl descriptor not EBADF");
    require(f.call("_ioctl", {99999, 0x20006601}) == UINT64_MAX && f.error() == 9,
        "invalid ioctl descriptor not EBADF");
    require(f.call("_ioctl", {descriptor, 0x12345678}) == UINT64_MAX && f.error() == 25,
        "inapplicable ioctl request not ENOTTY");
    require(f.call("_ioctl", {1, 0x20006601}) == 0 && f.call("_fcntl", {1, 1}) == 1 &&
        f.call("_write", {1, f.string("a"), 1}) == 1, "standard descriptor inheritance flags lost output backend");
    require(f.call("_ioctl", {1, 0x20006602}) == 0 && f.call("_fcntl", {1, 1}) == 0,
        "standard descriptor inheritance reset failed");
    require(f.call("_close", {descriptor}) == 0 && f.call("_close", {duplicate}) == 0,
        "inheritance descriptor cleanup failed");
}

void relative_files_and_identity() {
    Fixture f;
    const auto directory = std::filesystem::temp_directory_path() /
        ("alchemy-python-at-" + std::to_string(f.call("_getpid")));
    std::filesystem::create_directory(directory);
    const auto path = f.string(alchemy::guest_path_string(directory));
    const auto descriptor = f.call("_open", {path, 0x100000});
    require(descriptor != UINT64_MAX, "directory open failed");
    f.slot(0600);
    const auto created = f.call("_openat", {descriptor, f.string("one"), 2 | 0x200 | 0x400});
    require(created != UINT64_MAX, "relative file create failed");
    require(f.call("_faccessat", {descriptor, f.string("one"), 0, 0}) == 0, "relative access failed");
    const auto metadata = f.context.allocate_guest(144);
    require(f.call("_fstatat", {descriptor, f.string("one"), metadata, 0}) == 0 && f.engine.read64(metadata + 96) == 0,
        "relative stat/layout failed");
    static_cast<void>(f.call("_close", {created}));
    require(f.call("_renameat", {descriptor, f.string("one"), descriptor, f.string("two")}) == 0, "relative rename failed");
    require(f.call("_unlinkat", {descriptor, f.string("two"), 0}) == 0, "relative unlink failed");
    require(f.call("_fpathconf", {descriptor, 4}) == 255, "name length profile incorrect");
    static_cast<void>(f.call("_close", {descriptor})); std::filesystem::remove(directory);
    require(f.call("_getuid") == 1000 && f.call("_geteuid") == 1000 && f.call("_getgid") == 1000,
        "virtual identity incorrect");
    require(f.call("_getgroups$DARWIN_EXTSN", {0, 0}) == 1 && f.call("_getgroups", {1, metadata}) == 1 &&
        f.engine.read32(metadata) == 1000, "virtual group layout incorrect");
    const auto uname = f.context.allocate_guest(1280);
    require(f.call("_uname", {uname}) == 0 && f.memory().read8(uname) == 'D' && f.memory().read8(uname + 1024) == 'a', "utsname fields incorrect");
    const auto limits = f.context.allocate_guest(16);
    require(f.call("_getrlimit", {3, limits}) == 0 && f.engine.read64(limits) ==
        f.engine.stack_bounds().second - f.engine.stack_bounds().first, "reported stack does not match engine");
    require(f.call("_setrlimit", {3, limits}) == 0, "unchanged resource limit rejected");
    f.engine.write64(limits, 2 * f.engine.read64(limits)); f.engine.write64(limits + 8, f.engine.read64(limits));
    require(f.call("_setrlimit", {3, limits}) == UINT64_MAX && f.error() == 78, "unsupported resource limit change pretended success");
    // The reference engine cannot run a fork child on its parent's stack.
    require(f.call("_fork") == UINT64_MAX && f.error() == 78, "fork did not fail ENOSYS");
    require(f.call("_setuid", {0}) == UINT64_MAX && f.error() == 1, "credential change did not fail EPERM");
    require(f.call("_posix_spawn", {0, 0, 0, 0, 0, 0}) == 14, "spawn did not return a direct error number (EFAULT)");
}

void signals_threads_and_once() {
    Fixture f;
    const auto action = f.context.allocate_guest(16), previous = f.context.allocate_guest(16);
    f.engine.write64(action, 0x12345000); f.engine.write32(action + 8, 1); f.engine.write32(action + 12, 2);
    require(f.call("_sigaction", {2, action, previous}) == 0 && f.engine.read64(previous) == 0, "signal registration failed");
    require(f.call("_sigaction", {2, 0, previous}) == 0 && f.engine.read64(previous) == 0x12345000 &&
        f.engine.read32(previous + 8) == 1, "signal query layout incorrect");
    require(f.call("_sigaction", {9, action, 0}) == UINT64_MAX && f.error() == 22, "SIGKILL handler accepted");
    // raise() delivers synchronously to the calling thread: the handler runs now.
    const auto handler = f.context.allocate_guest(24), slot = f.context.allocate_guest(8);
    f.engine.write32(handler, 0x58000061); f.engine.write32(handler + 4, 0xf9000020); // ldr x1, #12; str x0, [x1]
    f.engine.write32(handler + 8, 0xd65f03c0); f.engine.write64(handler + 12, slot);   // ret; .quad slot
    f.engine.set_memory_access(handler, true, false, true);
    f.engine.write64(slot, 0);
    const auto installed = f.context.allocate_guest(16);
    f.engine.write64(installed, handler); f.engine.write32(installed + 8, 0); f.engine.write32(installed + 12, 0);
    require(f.call("_sigaction", {2, installed, 0}) == 0 && f.call("_raise", {2}) == 0 && f.engine.read64(slot) == 2,
        "raise did not run the installed handler");
    f.engine.write64(installed, 1);   // SIG_IGN
    require(f.call("_sigaction", {2, installed, 0}) == 0 && f.call("_raise", {2}) == 0, "ignored signal not ignored");
    f.engine.write64(installed, 0x12345000);
    f.engine.write32(action, (1U << 1) | (1U << 8) | (1U << 16));
    require(f.call("_pthread_sigmask", {3, action, previous}) == 0 && f.engine.read32(previous) == 0, "pthread mask installation failed");
    require(f.call("_pthread_sigmask", {0, 0, previous}) == 0 && f.engine.read32(previous) == (1U << 1), "unblockable signal mask bits retained");
    require(f.call("_pthread_sigmask", {0, action, previous}) == 22, "pthread mask error convention incorrect");
    require(f.call("_pthread_threadid_np", {0, previous}) == 0 && f.engine.read64(previous) == f.context.current_thread()->identity,
        "pthread identity query failed");
    const auto attribute = f.context.allocate_guest(64), thread = f.context.allocate_guest(8), result = f.context.allocate_guest(8);
    require(f.call("_pthread_attr_init", {attribute}) == 0 && f.call("_pthread_attr_setscope", {attribute, 1}) == 0,
        "default pthread attributes rejected");
    const auto body = f.context.allocate_guest(8);
    f.engine.write32(body, 0x91000400); f.engine.write32(body + 4, 0xd65f03c0); // add x0,x0,#1; ret
    f.engine.set_memory_access(body, true, false, true);
    require(f.call("_pthread_create", {thread, attribute, body, 26}) == 0 &&
        f.call("_pthread_join", {f.engine.read64(thread), result}) == 0 && f.engine.read64(result) == 27, "default attributed worker failed");
    const auto requested_stack = 2 * (f.engine.stack_bounds().second - f.engine.stack_bounds().first);
    require(f.call("_pthread_attr_setstacksize", {attribute, requested_stack}) == 0,
        "valid stack request not stored");
    require(f.call("_pthread_attr_setstacksize", {attribute, 16383}) == 22 &&
        f.call("_pthread_attr_setstacksize", {attribute, requested_stack + 1}) == 22,
        "undersized or unaligned stack attribute accepted");
    require(f.call("_pthread_attr_getstacksize", {attribute, result}) == 0 && f.engine.read64(result) == requested_stack,
        "rejected stack requests changed valid attribute");
    f.engine.write64(result, 0);
    require(f.call("_pthread_create", {thread, attribute, body, 41}) == 0 &&
        f.call("_pthread_join", {f.engine.read64(thread), result}) == 0 && f.engine.read64(result) == 42,
        "worker with requested stack size failed");
    require(f.call("_pthread_attr_destroy", {attribute}) == 0, "pthread attribute cleanup failed");
    unsigned calls = 0;
    const auto callback = f.context.install_function("_python_service_once_test", [&calls](auto& state, auto&) {
        require(state.x[0] == 27, "dispatch_once_f lost context"); ++calls;
    });
    const auto predicate = f.context.allocate_guest(8);
    static_cast<void>(f.call("_dispatch_once_f", {predicate, 27, callback}));
    static_cast<void>(f.call("_dispatch_once_f", {predicate, 27, callback}));
    require(calls == 1 && f.engine.read64(predicate) == UINT64_MAX, "dispatch_once_f duplicated callback/completion word");
}

void random_device() {
    Fixture f;
    const auto path = f.string("/dev/urandom");
    const auto descriptor = f.call("_open", {path, 4 | 0x1000000});
    require(descriptor >= 3 && descriptor != UINT64_MAX, "virtual urandom open failed");
    require(f.call("_fcntl", {descriptor, 1}) == 1 && f.call("_fcntl", {descriptor, 3}) == 4,
        "urandom nonblocking/close-on-exec flags incorrect");
    const auto other = f.call("_dup", {descriptor});
    require(other != UINT64_MAX && f.call("_fcntl", {other, 1}) == 0, "urandom duplicate retained FD_CLOEXEC");
    f.slot(0); require(f.call("_fcntl", {other, 4}) == 0 && f.call("_fcntl", {descriptor, 3}) == 0,
        "urandom duplicate did not share status flags");
    const auto metadata = f.context.allocate_guest(144), copy = f.context.allocate_guest(144);
    require(f.call("_fstat", {descriptor, metadata}) == 0 && f.call("_fstat", {other, copy}) == 0 &&
        f.engine.read64(metadata + 8) == f.engine.read64(copy + 8) && f.engine.read32(metadata) == f.engine.read32(copy),
        "urandom duplicate identity changed");
    require((f.memory().read8(metadata + 5) & 0xf0) == 0x20 && f.engine.read64(metadata + 96) == 0,
        "urandom was not a character device");
    require(f.call("_stat", {path, copy}) == 0 && f.engine.read64(copy + 8) == f.engine.read64(metadata + 8),
        "urandom pathname identity disagrees with descriptor");
    const auto reopened = f.call("_open", {path, 0});
    require(f.call("_fstat", {reopened, copy}) == 0 && f.engine.read64(copy + 8) == f.engine.read64(metadata + 8),
        "urandom identity was not stable across opens");
    const auto storage = f.context.allocate_guest(70032);
    f.engine.fill_bytes(storage, 0xcc, 70032);
    require(f.call("_read", {descriptor, storage + 16, 70000}) == 70000 && f.memory().read8(storage) == 0xcc &&
        f.memory().read8(storage + 70016) == 0xcc, "urandom read count/checked boundaries incorrect");
    std::array<std::uint8_t, 128> first{}, second{};
    f.engine.read_bytes(storage + 16, first);
    require(f.call("_read", {other, storage + 16, first.size()}) == first.size(), "urandom duplicate could not read");
    f.engine.read_bytes(storage + 16, second);
    require(first != second, "urandom returned a deterministic repeated block");
    require(f.call("_read", {descriptor, 0, 0}) == 0, "zero length urandom read failed");
    require(f.call("_read", {descriptor, 1, 1}) == UINT64_MAX && f.error() == 14, "urandom bad buffer did not return EFAULT");
    require(f.call("_lseek", {descriptor, 0, 0}) == UINT64_MAX && f.error() == 29, "urandom seek did not return ESPIPE");
    require(f.call("_pread", {descriptor, storage, 1, 0}) == UINT64_MAX && f.error() == 29, "urandom positioned read accepted");
    require(f.call("_write", {descriptor, storage, 1}) == UINT64_MAX && f.error() == 9, "readonly urandom accepted writes");
    require(f.call("_open", {path, 1}) == UINT64_MAX && f.error() == 13, "writable urandom open accepted");
    require(f.call("_open", {path, 0x100000}) == UINT64_MAX && f.error() == 20, "directory urandom open accepted");
    require(f.call("_open", {path, 0x400}) == UINT64_MAX && f.error() == 22, "urandom truncate flag accepted");
    require(f.call("_unlink", {path}) == UINT64_MAX && f.error() == 1, "urandom immutable path could be removed");
    require(f.call("_chmod", {path, 0600}) == UINT64_MAX && f.error() == 1, "urandom permissions could be changed");
    require(f.call("_access", {path, 4}) == 0 && f.call("_access", {path, 2}) == UINT64_MAX && f.error() == 13,
        "urandom access profile incorrect");
    require(f.call("_getentropy", {storage + 16, 256}) == 0, "shared getentropy backend failed");
    require(f.call("_getentropy", {storage, 257}) == UINT64_MAX && f.error() == 5, "getentropy oversize error incorrect");
    static_cast<void>(f.call("_close", {descriptor}));
    require(f.call("_fstat", {descriptor, metadata}) == UINT64_MAX && f.error() == 9, "closed urandom descriptor still valid");
    require(f.call("_read", {other, storage, 1}) == 1, "closing urandom also closed duplicate");
    static_cast<void>(f.call("_close", {other})); static_cast<void>(f.call("_close", {reopened}));
}

void temporary_files_and_directory_descriptors() {
    Fixture f;
    TemporaryDirectory directory(f);
    const auto first_template = directory.guest("one-XXXXXX"), second_template = directory.guest("one-XXXXXX");
    const auto first = f.call("_mkstemp", {first_template}), second = f.call("_mkstemp", {second_template});
    require(first != UINT64_MAX && second != UINT64_MAX && first != second, "mkstemp descriptor allocation failed");
    const auto first_name = f.read_string(first_template);
    require(first_name != f.read_string(second_template) && first_name.find("XXXXXX") == std::string::npos,
        "mkstemp did not replace templates uniquely");
    const auto payload = f.string("temporary payload"), buffer = f.context.allocate_guest(32);
    const auto permissions = f.context.allocate_guest(144);
    require(f.call("_fstat", {first, permissions}) == 0 &&
        ((f.memory().read8(permissions + 4) | (f.memory().read8(permissions + 5) << 8)) & 0777) == 0600,
        "temporary file permissions are not 0600");
    require(f.call("_stat", {first_template, permissions}) == 0 &&
        ((f.memory().read8(permissions + 4) | (f.memory().read8(permissions + 5) << 8)) & 0777) == 0600,
        "temporary pathname permissions disagree with descriptor");
    require(f.call("_write", {first, payload, 17}) == 17 && f.call("_lseek", {first, 0, 0}) == 0 &&
        f.call("_read", {first, buffer, 17}) == 17 && f.memory().read8(buffer) == 't', "mkstemp read/write access failed");
    const auto suffix_template = directory.guest("suffix-XXXXXX.dat");
    const auto suffix = f.call("_mkstemps", {suffix_template, 4});
    require(suffix != UINT64_MAX && f.read_string(suffix_template).ends_with(".dat"), "mkstemps changed suffix");
    require(f.call("_mkstemps", {directory.guest("bad-XXXXXX"), UINT32_MAX}) == UINT64_MAX && f.error() == 22,
        "negative mkstemps suffix accepted");
    require(f.call("_mkstemps", {directory.guest("bad-XXXXXX.dat"), 1000}) == UINT64_MAX && f.error() == 22,
        "oversized mkstemps suffix accepted");
    require(f.call("_mkstemp", {directory.guest("malformed-XXXXX")}) == UINT64_MAX && f.error() == 22,
        "malformed template accepted");
    require(f.call("_mkstemp", {1}) == UINT64_MAX && f.error() == 14, "bad template pointer not EFAULT");
    const auto readonly = directory.guest("readonly-XXXXXX");
    f.engine.set_memory_access(readonly, true, false, false);
    const auto before = std::distance(std::filesystem::directory_iterator(directory.path), std::filesystem::directory_iterator{});
    require(f.call("_mkstemp", {readonly}) == UINT64_MAX && f.error() == 14, "readonly file template accepted");
    require(f.call("_mkdtemp", {readonly}) == 0 && f.error() == 14, "readonly directory template accepted");
    require(std::distance(std::filesystem::directory_iterator(directory.path), std::filesystem::directory_iterator{}) == before,
        "invalid template created a host object");
    const auto child = directory.guest("child-XXXXXX");
    require(f.call("_mkdtemp", {child}) == child && std::filesystem::is_directory(f.context.map_guest_path(f.read_string(child))),
        "mkdtemp did not create a directory");
    require(f.call("_stat", {child, permissions}) == 0 &&
        ((f.memory().read8(permissions + 4) | (f.memory().read8(permissions + 5) << 8)) & 0777) == 0700,
        "temporary directory permissions are not 0700");
    require(f.call("_mkdtemp", {directory.guest("bad-directory")}) == 0 && f.error() == 22, "bad mkdtemp error convention");
    for (const auto descriptor : {first, second, suffix}) require(f.call("_close", {descriptor}) == 0, "temporary descriptor close failed");

    const auto stream = f.call("_tmpfile");
    require(stream != 0, "tmpfile failed");
    const auto descriptor = f.call("_fileno", {stream}), duplicate = f.call("_dup", {descriptor});
    require(duplicate != UINT64_MAX && f.call("_fwrite", {payload, 1, 17, stream}) == 17, "tmpfile write failed");
    static_cast<void>(f.call("_rewind", {stream}));
    const auto metadata = f.context.allocate_guest(144);
    require(f.call("_fstat", {descriptor, metadata}) == 0 && f.engine.read64(metadata + 96) == 17,
        "anonymous tmpfile descriptor metadata failed");
    require(f.call("_fclose", {stream}) == 0 && f.call("_read", {duplicate, buffer, 17}) == 17,
        "fclose deleted tmpfile while duplicate remained");
    require(f.call("_close", {duplicate}) == 0 && f.call("_fstat", {duplicate, metadata}) == UINT64_MAX && f.error() == 9,
        "last tmpfile close left valid descriptor");
    require(f.call("_setenv", {f.string("TMPDIR"), directory.guest("missing-directory"), 1}) == 0,
        "temporary directory environment setup failed");
    require(f.call("_tmpfile") == 0 && f.error() == 2, "tmpfile ignored invalid TMPDIR");
    require(f.call("_unsetenv", {f.string("TMPDIR")}) == 0, "temporary directory environment cleanup failed");

    const auto handle = f.call("_opendir", {directory.guest("")});
    require(handle != 0, "opendir failed");
    const auto directory_fd = f.call("_dirfd", {handle}), directory_copy = f.call("_dup", {directory_fd});
    require(directory_fd != UINT64_MAX && directory_fd == f.call("_dirfd", {handle}) && directory_copy != UINT64_MAX,
        "dirfd did not return stable owned descriptor");
    require(f.call("_fstat", {directory_fd, metadata}) == 0 && (f.memory().read8(metadata + 5) & 0xf0) == 0x40,
        "dirfd did not describe directory");
    require(f.call("_closedir", {handle}) == 0 && f.call("_fstat", {directory_fd, metadata}) == UINT64_MAX && f.error() == 9,
        "closedir did not close its descriptor");
    require(f.call("_fstat", {directory_copy, metadata}) == 0, "closedir also closed caller duplicate");
    const auto adopted = f.call("_fdopendir", {directory_copy});
    require(adopted != 0 && f.call("_dirfd", {adopted}) == directory_copy && f.call("_closedir", {adopted}) == 0,
        "fdopendir descriptor adoption failed");
    require(f.call("_fstat", {directory_copy, metadata}) == UINT64_MAX && f.error() == 9,
        "fdopendir did not transfer ownership");
    require(f.call("_dirfd", {handle}) == UINT64_MAX && f.error() == 9, "closed DIR accepted by dirfd");
    require(f.call("_dirfd", {1}) == UINT64_MAX && f.error() == 9, "invalid DIR accepted by dirfd");
}

void descriptor_pressure_and_replacement_lifetimes() {
    Fixture f;
    TemporaryDirectory directory(f);
    const auto template_address = directory.guest("pressure-XXXXXX"), urandom = f.string("/dev/urandom");
    const auto limits = f.context.allocate_guest(16);
    require(f.call("_getrlimit", {8, limits}) == 0, "descriptor resource limit unavailable");
    const auto limit = f.engine.read64(limits);
    require(limit > 3 && limit < 65536, "unbounded descriptor limit in test profile");
    std::vector<std::uint64_t> descriptors;
    for (std::uint64_t attempt = 0; attempt < limit; ++attempt) {
        const auto descriptor = f.call("_open", {urandom, 0});
        if (descriptor == UINT64_MAX) { require(f.error() == 24, "descriptor exhaustion error incorrect"); break; }
        descriptors.push_back(descriptor);
    }
    require(descriptors.size() == limit - 3, "descriptor limit did not include standard descriptors");
    f.slot(UINT64_MAX);
    require(f.call("_ioctl", {1, 0x20006601}) == 0 && f.call("_fcntl", {1, 1}) == 1 &&
        f.call("_ioctl", {1, 0x20006602}) == 0, "standard inheritance flags consumed a descriptor under exhaustion");
    require(f.call("_mkstemp", {template_address}) == UINT64_MAX && f.error() == 24 &&
        f.read_string(template_address).ends_with("XXXXXX"), "failed temporary creation changed guest template/error");
    require(f.call("_setenv", {f.string("TMPDIR"), directory.guest(""), 1}) == 0, "temporary pressure path setup failed");
    require(f.call("_tmpfile") == 0 && f.error() == 24, "exhausted tmpfile did not fail EMFILE");
    require(f.call("_opendir", {directory.guest("")}) == 0 && f.error() == 24, "exhausted opendir did not fail EMFILE");
    require(std::filesystem::is_empty(directory.path), "failed temporary creation leaked a host object");
    const auto reusable = descriptors.back(); descriptors.pop_back();
    require(f.call("_close", {reusable}) == 0, "pressure descriptor close failed");
    const auto recovered = f.call("_mkstemp", {template_address});
    require(recovered == reusable && f.call("_close", {recovered}) == 0, "descriptor slot did not recover after failure");
    for (const auto descriptor : descriptors) require(f.call("_close", {descriptor}) == 0, "pressure descriptor cleanup failed");

    const auto old_template = directory.guest("old-XXXXXX"), new_template = directory.guest("new-XXXXXX");
    const auto target = f.call("_mkstemp", {old_template}), source = f.call("_mkstemp", {new_template});
    const auto old_alias = f.call("_dup", {target}), contender = f.call("_open", {old_template, 2});
    require(target != UINT64_MAX && source != UINT64_MAX && old_alias != UINT64_MAX && contender != UINT64_MAX,
        "replacement fixture creation failed");
    require(f.call("_flock", {target, 2}) == 0, "replacement old lock setup failed");
    const auto byte = f.string("B");
    require(f.call("_write", {source, byte, 1}) == 1 && f.call("_lseek", {source, 0, 0}) == 0,
        "replacement source write failed");
    require(f.call("_dup2", {source, target}) == target && f.call("_read", {target, byte, 1}) == 1 && f.memory().read8(byte) == 'B',
        "dup2 did not retarget descriptor");
    require(f.call("_flock", {contender, 2 | 4}) == UINT64_MAX && f.error() == 35,
        "dup2 prematurely released old duplicate lock owner");
    require(f.call("_close", {target}) == 0 && f.call("_lseek", {source, 0, 0}) == 0 && f.call("_read", {source, byte, 1}) == 1,
        "closing replacement also closed its source");
    require(f.call("_close", {old_alias}) == 0 && f.call("_flock", {contender, 2 | 4}) == 0,
        "replacement leaked last old lock reference");
    const auto invalid_mode = f.call("_fdopen", {source, f.string("invalid")});
    require(invalid_mode == 0 && f.error() == 22 && f.call("_lseek", {source, 0, 0}) == 0,
        "failed fdopen consumed caller-owned descriptor");
    require(f.call("_close", {source}) == 0 && f.call("_fdopen", {source, f.string("r")}) == 0 && f.error() == 9,
        "fdopen accepted a closed descriptor");
    require(f.call("_close", {contender}) == 0, "replacement fixture close failed");
}

#ifdef _WIN32
void child_flock_probe(const std::filesystem::path& path, bool busy) {
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    require(length && length < executable.size(), "test executable path unavailable"); executable.resize(length);
    auto command = L"\"" + executable + L"\" --flock-probe \"" + path.native() + L"\" " + (busy ? L"busy" : L"free");
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION child{};
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &child) != 0, "flock child creation failed");
    const auto wait = WaitForSingleObject(child.hProcess, 10000);
    DWORD status = 1;
    if (wait != WAIT_OBJECT_0) { TerminateProcess(child.hProcess, 1); WaitForSingleObject(child.hProcess, 10000); }
    else require(GetExitCodeProcess(child.hProcess, &status) != 0, "flock child status unavailable");
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    require(wait == WAIT_OBJECT_0 && status == 0, "cross-process flock contract failed");
}
#endif

void advisory_locks_and_waits() {
    Fixture f;
    TemporaryDirectory directory(f);
    const auto specification = directory.guest("locks-XXXXXX");
    const auto owner = f.call("_mkstemp", {specification}), alias = f.call("_dup", {owner});
    const auto competitor = f.call("_open", {specification, 2}), other = f.call("_open", {specification, 2});
    require(owner != UINT64_MAX && alias != UINT64_MAX && competitor != UINT64_MAX && other != UINT64_MAX,
        "lock fixture opens failed");
    require(f.call("_flock", {owner, 2 | 4}) == 0 && f.call("_flock", {alias, 2 | 4}) == 0,
        "duplicate did not share exclusive lock owner");
    require(f.call("_flock", {competitor, 1 | 4}) == UINT64_MAX && f.error() == 35, "exclusive lock did not exclude shared open");
    require(f.call("_write", {competitor, f.string("A"), 1}) == 1, "flock incorrectly made data I/O mandatory");
    const auto original_path = f.context.map_guest_path(f.read_string(specification));
#ifdef _WIN32
    child_flock_probe(original_path, true);
#endif
    const auto renamed = directory.path / "renamed-lock-file";
    std::filesystem::rename(original_path, renamed);
    const auto hardlink = directory.path / "hardlink-lock-file";
    std::filesystem::create_hard_link(renamed, hardlink);
    const auto hardlink_fd = f.call("_open", {f.string(alchemy::guest_path_string(hardlink)), 2});
    require(hardlink_fd != UINT64_MAX && f.call("_flock", {hardlink_fd, 2 | 4}) == UINT64_MAX && f.error() == 35,
        "hardlink/rename changed lock identity");
    require(f.call("_close", {owner}) == 0 && f.call("_flock", {competitor, 2 | 4}) == UINT64_MAX && f.error() == 35,
        "first duplicate close released lock");
    require(f.call("_close", {alias}) == 0 && f.call("_flock", {competitor, 1 | 4}) == 0 &&
        f.call("_flock", {other, 1 | 4}) == 0, "last duplicate close did not release lock/shared locks conflict");
    require(f.call("_flock", {competitor, 2 | 4}) == UINT64_MAX && f.error() == 35,
        "shared-to-exclusive conversion ignored competing reader");
    require(f.call("_flock", {other, 8}) == 0 && f.call("_flock", {competitor, 2 | 4}) == 0,
        "unlock or exclusive conversion failed");
    require(f.call("_flock", {99999, 2}) == UINT64_MAX && f.error() == 9, "invalid lock descriptor accepted");
    require(f.call("_flock", {competitor, 3}) == UINT64_MAX && f.error() == 22, "conflicting lock modes accepted");
    const auto device = f.call("_open", {f.string("/dev/urandom"), 0});
    require(f.call("_flock", {device, 2}) == UINT64_MAX && f.error() == 45, "device lock claimed unsupported success");
    require(f.call("_close", {device}) == 0, "device close failed");

    const auto worker_symbol = f.context.resolve(system_library, "_flock");
    require(worker_symbol.has_value(), "flock export missing");
    auto worker = f.engine.fork_thread(0, 0);
    const auto waiter_alias = f.call("_dup", {other});
    std::promise<void> started;
    auto wait = std::async(std::launch::async, [&] {
        const std::array<std::uint64_t, 2> arguments{other, 2}; started.set_value();
        return worker->invoke_callback(worker_symbol->address, arguments);
    });
    started.get_future().wait(); std::this_thread::sleep_for(std::chrono::milliseconds(30));
    require(wait.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout, "contended blocking flock returned early");
    require(f.call("_flock", {waiter_alias, 8}) == 0, "duplicate unlock blocked behind another waiter");
    require(f.call("_close", {other}) == 0, "closing blocked descriptor deadlocked");
    if (wait.wait_for(std::chrono::seconds(2)) != std::future_status::ready) f.context.request_exit(1);
    require(wait.get() == UINT64_MAX && worker->read32(f.context.thread(*worker)->errno_address) == 9,
        "closed descriptor did not cancel its waiter");
    f.context.release_thread(*worker);
    require(f.call("_close", {waiter_alias}) == 0, "last waiter duplicate close failed");
    std::promise<void> retry_started;
    auto retry = std::async(std::launch::async, [&] {
        const std::array<std::uint64_t, 2> arguments{hardlink_fd, 2}; retry_started.set_value();
        return worker->invoke_callback(worker_symbol->address, arguments);
    });
    retry_started.get_future().wait(); std::this_thread::sleep_for(std::chrono::milliseconds(30));
    require(retry.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout, "second contended lock returned early");
    require(f.call("_flock", {competitor, 8}) == 0, "remaining lock release failed");
    if (retry.wait_for(std::chrono::seconds(2)) != std::future_status::ready) f.context.request_exit(1);
    require(retry.get() == 0 && f.call("_flock", {hardlink_fd, 8}) == 0, "blocking lock did not resume after unlock");
#ifdef _WIN32
    child_flock_probe(renamed, false);
#endif
    require(f.call("_flock", {competitor, 2}) == 0, "lock setup before shutdown failed");
    std::promise<void> shutdown_started;
    auto shutdown_wait = std::async(std::launch::async, [&] {
        const std::array<std::uint64_t, 2> arguments{hardlink_fd, 2}; shutdown_started.set_value();
        return worker->invoke_callback(worker_symbol->address, arguments);
    });
    shutdown_started.get_future().wait(); std::this_thread::sleep_for(std::chrono::milliseconds(30));
    require(shutdown_wait.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout, "shutdown lock was not contended");
    f.context.request_exit(0);
    require(shutdown_wait.wait_for(std::chrono::seconds(2)) == std::future_status::ready && shutdown_wait.get() == UINT64_MAX &&
        worker->read32(f.context.thread(*worker)->errno_address) == 89, "shutdown did not cancel blocked lock");
    f.context.release_thread(*worker);
    require(f.call("_close", {competitor}) == 0 && f.call("_close", {hardlink_fd}) == 0, "lock descriptor cleanup failed");

    Fixture sleeps;
    require(sleeps.call("_sleep", {0}) == 0 && sleeps.call("_usleep", {0}) == 0, "zero-duration sleep failed");
    const auto begin = std::chrono::steady_clock::now();
    require(sleeps.call("_usleep", {2000}) == 0 && std::chrono::steady_clock::now() - begin >= std::chrono::microseconds(2000),
        "usleep returned before its interval");
    sleeps.context.request_exit(0);
    const auto cancellation_begin = std::chrono::steady_clock::now();
    require(sleeps.call("_sleep", {3600}) == 3600 && sleeps.error() == 89, "sleep shutdown remainder/error incorrect");
    require(sleeps.call("_usleep", {UINT32_MAX}) == UINT64_MAX && sleeps.error() == 89, "usleep shutdown cancellation incorrect");
    require(std::chrono::steady_clock::now() - cancellation_begin < std::chrono::seconds(1), "shutdown sleep did not stop promptly");
}
}

int run_tests(const std::filesystem::path& probe = {}, bool busy = false) {
    try {
        if (!probe.empty()) {
            Fixture f;
            const auto descriptor = f.call("_open", {f.string(alchemy::guest_path_string(probe)), 2});
            require(descriptor != UINT64_MAX, "flock child open failed");
            const auto result = f.call("_flock", {descriptor, 2 | 4});
            require(busy ? result == UINT64_MAX && f.error() == 35 : result == 0, "flock child result incorrect");
            return 0;
        }
        descriptor_io_and_stdio(); descriptor_inheritance_controls(); relative_files_and_identity(); signals_threads_and_once(); random_device();
        temporary_files_and_directory_descriptors(); descriptor_pressure_and_replacement_lifetimes(); advisory_locks_and_waits();
        std::cout << "Alchemy Python services passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--flock-probe") return run_tests(argv[2], std::wstring_view(argv[3]) == L"busy");
    return run_tests();
}
#else
int main() { return run_tests(); }
#endif
