#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <map>

// NSFileHandle, NSPipe and NSTask (Foundation) over the guest
// libSystem's descriptors and processes (docs/ALCHEMY_PROCESS.md): every
// operation is the libSystem call a Darwin Foundation would make, so files,
// pipes, /dev/null and child processes behave as the emulated libSystem
// defines them.
//
// NSFileHandle: reads and writes block like read(2)/write(2); -availableData
// reads to the end of a regular file and otherwise returns what one read
// delivers (blocking until data or end of file). The readability handler
// runs on a dispatch worker each time poll(2) reports the descriptor
// readable, until it is set to nil or the handle closes. The background
// reads (-readInBackgroundAndNotify, -readToEndOfFileInBackgroundAndNotify,
// -waitForDataInBackgroundAndNotify) read on a worker and post
// NSFileHandleReadCompletionNotification / ...ReadToEndOfFileCompletion... /
// NSFileHandleDataAvailableNotification on the main thread (they must be
// started from the main thread; other threads fail explicitly). Errors of
// the -...error: methods are NSPOSIXErrorDomain errors; the older methods
// fail explicitly where Foundation raises NSFileHandleOperationException.
//
// NSTask: -launch / -launchAndReturnError: posix_spawn the executable with
// the arguments, the environment (the current one when unset), the working
// directory and standard input/output/error taken from NSPipe or
// NSFileHandle objects (the parent then closes its copies of the child's pipe
// ends, as Foundation does). A dispatch worker waits for the child with
// waitpid; the termination handler runs on that worker, then
// NSTaskDidTerminateNotification is posted on the main thread.
// -waitUntilExit runs the current run loop until the task exits.
// -terminate/-interrupt/-suspend/-resume send SIGTERM/SIGINT/SIGSTOP/SIGCONT.
//
// NSStream, NSInputStream and NSOutputStream are Foundation's (darwin/foundation/streams.cpp).
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

constexpr std::uint64_t o_rdonly = 0, o_wronly = 1, o_rdwr = 2, o_cloexec = 0x1000000;
constexpr std::uint16_t pollin = 1, pollhup = 0x10, pollerr = 8, pollnval = 0x20;
constexpr std::uint64_t chunk = 64 * 1024;

struct HandleState final : ObjectPayload {
    std::mutex mutex;
    std::int32_t descriptor{-1};
    bool close_on_dealloc{}, closed{};
    Address readability{};           // copied block
    std::uint64_t reader_generation{};
};

struct TaskState final : ObjectPayload {
    std::mutex mutex;
    Address launch_path{}, arguments{}, environment{}, directory{};   // +1
    Address input{}, output{}, error{}, termination_handler{};        // +1 (handler: copied block)
    std::int32_t pid{};
    std::int32_t status{};
    std::int64_t reason{};            // NSTaskTerminationReasonExit = 1, UncaughtSignal = 2
    bool launched{}, running{};
};

struct Io {
    explicit Io(Support& value) : support(value) {}
    Support& support;
    Address handle_class{}, pipe_class{}, task_class{};
    Address main_entry{}, worker_entry{};
    std::mutex mutex;
    std::map<Address, std::function<void()>> work;   // dispatch_async_f contexts
    Address next{1};

    // ---- guest calls ----
    std::int64_t sys(std::string_view symbol, std::initializer_list<Address> arguments) {
        return static_cast<std::int64_t>(support.call(libsystem, symbol, arguments));
    }
    // Runs host work on the main queue or a global queue (a guest thread).
    void submit(bool main, std::function<void()> body) {
        Address id;
        { std::lock_guard lock(mutex); id = next++; work.emplace(id, std::move(body)); }
        Address queue;
        if (main) {
            const auto found = support.process.resolve(libsystem, "__dispatch_main_q");
            if (!found) fail("the main dispatch queue is not provided");
            queue = found->address;
        } else {
            queue = support.call(libsystem, "_dispatch_get_global_queue", {0, 0});
        }
        static_cast<void>(support.call(libsystem, "_dispatch_async_f", {queue, id, main ? main_entry : worker_entry}));
    }
    void run(Address id) {
        std::function<void()> body;
        {
            std::lock_guard lock(mutex);
            const auto found = work.find(id);
            if (found == work.end()) fail("NSFileHandle/NSTask work without its record");
            body = std::move(found->second);
            work.erase(found);
        }
        body();
    }
    bool on_main_thread() { return &support.process.calling_engine() == &support.process.main_engine(); }

    Address data(const std::vector<std::uint8_t>& bytes) {
        GuestBlock buffer(support, bytes.size(), false);
        if (!bytes.empty()) support.memory().write_bytes(buffer, bytes);
        return autoreleased(support, support.cf.invoke("CFDataCreate", std::array<Address, 3>{0, buffer, bytes.size()}));
    }
    std::vector<std::uint8_t> bytes_of(Address data_object) {
        std::vector<std::uint8_t> bytes(support.send_id(data_object, "length"));
        if (!bytes.empty()) support.memory().read_bytes(support.send_id(data_object, "bytes"), bytes);
        return bytes;
    }
    Address posix_error(std::uint32_t code) {
        return error(support, "NSPOSIXErrorDomain", code, "POSIX error " + std::to_string(code));
    }

    // ---- NSFileHandle ----
    std::shared_ptr<HandleState> handle_of(Address object) {
        auto state = support.objc.find_host_state<HandleState>(object);
        if (!state) fail("NSFileHandle is not initialized");
        return state;
    }
    std::int32_t descriptor(Address object) {
        const auto state = handle_of(object);
        std::lock_guard lock(state->mutex);
        if (state->closed) fail("NSFileHandleOperationException: the file handle is closed");
        return state->descriptor;
    }
    Address make_handle(std::int32_t fd, bool close_on_dealloc) {
        const auto object = support.send_id(handle_class, "alloc");
        const auto state = support.objc.host_state<HandleState>(object);
        state->descriptor = fd;
        state->close_on_dealloc = close_on_dealloc;
        return object;   // +1
    }
    // open(2) of a path; -1 with errno.
    std::int32_t open_path(const std::string& path, std::uint64_t flags) {
        GuestBlock text_path(support, path);
        return static_cast<std::int32_t>(sys("_open", {text_path, flags | o_cloexec, 0}));
    }
    bool regular_file(std::int32_t fd) {
        GuestBlock stat_buffer(support, 144);
        if (sys("_fstat", {static_cast<Address>(fd), stat_buffer}) != 0) return false;
        return (support.memory().read32(stat_buffer + 4) & 0xf000) == 0x8000;   // st_mode (16 bits at 4)
    }
    // Reads up to `limit` bytes; one read(2) unless `fill`. nullopt with errno.
    std::optional<std::vector<std::uint8_t>> read(std::int32_t fd, std::uint64_t limit, bool fill, std::uint32_t& error_code) {
        std::vector<std::uint8_t> result;
        GuestBlock buffer(support, chunk, false);
        while (result.size() < limit) {
            const auto want = std::min<std::uint64_t>(chunk, limit - result.size());
            const auto count = sys("_read", {static_cast<Address>(fd), buffer, want});
            if (count < 0) {
                error_code = guest_errno(support);
                if (error_code == 4) continue;   // EINTR
                return std::nullopt;
            }
            if (count == 0) break;
            const auto start = result.size();
            result.resize(start + static_cast<std::size_t>(count));
            support.memory().read_bytes(buffer, std::span(result.data() + start, static_cast<std::size_t>(count)));
            if (!fill) break;
        }
        return result;
    }
    bool write(std::int32_t fd, const std::vector<std::uint8_t>& bytes, std::uint32_t& error_code) {
        if (bytes.empty()) return true;
        GuestBlock buffer(support, bytes.size(), false);
        support.memory().write_bytes(buffer, bytes);
        std::size_t done{};
        while (done < bytes.size()) {
            const auto count = sys("_write", {static_cast<Address>(fd), buffer + done, bytes.size() - done});
            if (count < 0) {
                error_code = guest_errno(support);
                if (error_code == 4) continue;
                return false;
            }
            done += static_cast<std::size_t>(count);
        }
        return true;
    }
    // poll(2) for readability; the revents bits (0 on timeout).
    std::uint16_t poll(std::int32_t fd, std::int64_t timeout_ms) {
        GuestBlock entry(support, 8);
        support.memory().write32(entry, static_cast<std::uint32_t>(fd));
        support.memory().write32(entry + 4, pollin);   // events = POLLIN, revents = 0
        const auto count = sys("_poll", {entry, 1, static_cast<Address>(timeout_ms)});
        if (count <= 0) return 0;
        return static_cast<std::uint16_t>(support.memory().read32(entry + 4) >> 16);
    }
    void close(Address object, bool& ok, std::uint32_t& error_code) {
        const auto state = handle_of(object);
        std::int32_t fd;
        Address handler;
        {
            std::lock_guard lock(state->mutex);
            if (state->closed) { ok = true; return; }
            state->closed = true;
            fd = state->descriptor;
            handler = std::exchange(state->readability, 0);
            ++state->reader_generation;
        }
        block_release(support, handler);
        ok = sys("_close", {static_cast<Address>(fd)}) == 0;
        if (!ok) error_code = guest_errno(support);
    }
    // The readability loop on a worker: the handler runs while the
    // descriptor is readable and the handler is still the same generation.
    void start_reader(Address object, std::uint64_t generation) {
        const auto retained = objc_retain(support, object);
        submit(false, [this, retained, generation] {
            const auto state = handle_of(retained);
            for (;;) {
                std::int32_t fd;
                Address handler;
                {
                    std::lock_guard lock(state->mutex);
                    if (state->closed || state->reader_generation != generation || !state->readability) break;
                    fd = state->descriptor;
                    handler = block_copy(support, state->readability);
                }
                if (support.process.requested_exit()) { block_release(support, handler); break; }
                const auto events = poll(fd, 100);
                if (events & (pollin | pollhup | pollerr)) static_cast<void>(invoke_block(support, handler, {retained}));
                block_release(support, handler);
                if (events & pollnval) break;
                if ((events & pollhup) && !(events & pollin)) static_cast<void>(sys("_usleep", {50000}));   // at end of file: no busy loop
            }
            objc_release(support, retained);
        });
    }
    // Background read: `body` reads on a worker and returns the
    // notification's name and user info; it is posted on the main thread.
    void background(Address object, std::function<std::pair<std::string, std::vector<std::pair<std::string, Address>>>()> body) {
        if (!on_main_thread()) unsupported("NSFileHandle background reads started off the main thread");
        const auto retained = objc_retain(support, object);
        submit(false, [this, retained, body] {
            auto [name, info] = body();
            std::vector<Address> keep;
            for (auto& [key, value] : info) { static_cast<void>(key); keep.push_back(objc_retain(support, value)); }
            submit(true, [this, retained, name, info, keep] {
                std::vector<std::pair<Address, Address>> pairs;
                std::vector<Address> keys;
                for (const auto& [key, value] : info) {
                    keys.push_back(support.cf.string(key));
                    pairs.emplace_back(keys.back(), value);
                }
                const auto dictionary_object = pairs.empty() ? 0 : autoreleased(support, support.cf.dictionary(pairs));
                for (const auto key : keys) support.release(key);
                post_notification(support, name, retained, dictionary_object);
                for (const auto value : keep) objc_release(support, value);
                objc_release(support, retained);
            });
        });
    }

    // ---- NSTask ----
    std::shared_ptr<TaskState> task_of(Address object) { return support.objc.host_state<TaskState>(object); }
    // The descriptor of a standard stream object (NSPipe: the given end, NSFileHandle: its descriptor).
    std::optional<std::int32_t> stream_descriptor(Address object, bool child_reads) {
        if (!object) return std::nullopt;
        if (support.objc.find_host_state<HandleState>(object)) return descriptor(object);
        if (support.send(object, "isKindOfClass:", [&] { CallRegs r; r.x[2] = pipe_class; return r; }()).x0 & 0xff)
            return descriptor(support.send_id(object, child_reads ? "fileHandleForReading" : "fileHandleForWriting"));
        fail("NSInvalidArgumentException: an NSTask standard stream must be an NSPipe or an NSFileHandle");
    }
    // posix_spawn; 0 or an errno.
    std::uint32_t launch(Address task) {
        const auto state = task_of(task);
        Address path_object, arguments, environment, directory, input, output, error_object;
        {
            std::lock_guard lock(state->mutex);
            if (state->launched) fail("NSInvalidArgumentException: task already launched");
            path_object = state->launch_path; arguments = state->arguments; environment = state->environment;
            directory = state->directory; input = state->input; output = state->output; error_object = state->error;
        }
        if (!path_object) fail("NSInvalidArgumentException: NSTask launch path not set");
        const auto path = text(support, path_object);
        // argv and envp in guest memory.
        std::vector<std::string> argv{path};
        for (const auto value : arguments ? support.cf.array_values(arguments) : std::vector<Address>{}) argv.push_back(text(support, value));
        std::vector<std::unique_ptr<GuestBlock>> strings;
        const auto vector = [&](const std::vector<std::string>& values) {
            strings.push_back(std::make_unique<GuestBlock>(support, (values.size() + 1) * 8));
            const Address table = *strings.back();
            for (std::size_t index = 0; index < values.size(); ++index) {
                strings.push_back(std::make_unique<GuestBlock>(support, values[index]));
                support.memory().write64(table + index * 8, *strings.back());
            }
            return table;
        };
        const auto argv_table = vector(argv);
        Address envp_table;
        if (environment) {
            std::vector<std::string> values;
            for (const auto& [key, value] : support.cf.dictionary_entries(environment)) values.push_back(text(support, key) + "=" + text(support, value));
            envp_table = vector(values);
        } else {
            envp_table = support.memory().read64(support.call(libsystem, "__NSGetEnviron", {}));
        }
        GuestBlock actions(support, 8), pid_cell(support, 8);
        if (sys("_posix_spawn_file_actions_init", {actions}) != 0) fail("posix_spawn_file_actions_init failed");
        struct Destroy { Io& io; Address actions; ~Destroy() { try { static_cast<void>(io.sys("_posix_spawn_file_actions_destroy", {actions})); } catch (...) {} } } destroy{*this, actions};
        const std::array<std::pair<Address, bool>, 3> streams{{{input, true}, {output, false}, {error_object, false}}};
        for (std::size_t target = 0; target < streams.size(); ++target) {
            const auto fd = stream_descriptor(streams[target].first, streams[target].second);
            if (!fd) continue;
            if (sys("_posix_spawn_file_actions_adddup2", {actions, static_cast<Address>(*fd), target}) != 0) fail("posix_spawn_file_actions_adddup2 failed");
        }
        std::unique_ptr<GuestBlock> directory_text;
        if (directory) {
            directory_text = std::make_unique<GuestBlock>(support, text(support, directory));
            if (sys("_posix_spawn_file_actions_addchdir_np", {actions, *directory_text}) != 0) fail("posix_spawn_file_actions_addchdir_np failed");
        }
        GuestBlock path_text(support, path);
        const auto result = static_cast<std::uint32_t>(sys("_posix_spawn", {pid_cell, path_text, actions, 0, argv_table, envp_table}));
        if (result) return result;
        const auto pid = static_cast<std::int32_t>(support.memory().read32(pid_cell));
        {
            std::lock_guard lock(state->mutex);
            state->pid = pid;
            state->launched = state->running = true;
        }
        // The parent's copies of the child's pipe ends.
        for (const auto& [object, child_reads] : streams) {
            if (!object || !(support.send(object, "isKindOfClass:", [&] { CallRegs r; r.x[2] = pipe_class; return r; }()).x0 & 0xff)) continue;
            static_cast<void>(support.send_id(support.send_id(object, child_reads ? "fileHandleForReading" : "fileHandleForWriting"), "closeFile"));
        }
        const auto retained = objc_retain(support, task);
        submit(false, [this, retained, pid] {
            GuestBlock status_cell(support, 8);
            for (;;) {
                const auto waited = sys("_waitpid", {static_cast<Address>(pid), status_cell, 0});
                if (waited == pid) break;
                if (waited < 0 && guest_errno(support) != 4) break;
            }
            const auto status = support.memory().read32(status_cell);
            const auto state = task_of(retained);
            Address handler;
            {
                std::lock_guard lock(state->mutex);
                state->running = false;
                if ((status & 0x7f) == 0) { state->reason = 1; state->status = static_cast<std::int32_t>((status >> 8) & 0xff); }
                else { state->reason = 2; state->status = static_cast<std::int32_t>(status & 0x7f); }
                handler = block_copy(support, state->termination_handler);
            }
            if (handler) { static_cast<void>(invoke_block(support, handler, {retained})); block_release(support, handler); }
            submit(true, [this, retained] {
                post_notification(support, "NSTaskDidTerminateNotification", retained);
                objc_release(support, retained);
            });
        });
        return 0;
    }
};

} // namespace

void install_process_io(Support& support) {
    auto self = std::make_shared<Io>(support);
    self->main_entry = support.process.install_function("___alchemy_completion_io_main", [self](CpuState& s, GuestMemory&) { self->run(s.x[0]); s.x[0] = 0; }, "alchemy.internal");
    self->worker_entry = support.process.install_function("___alchemy_completion_io_worker", [self](CpuState& s, GuestMemory&) { self->run(s.x[0]); s.x[0] = 0; }, "alchemy.internal");
    for (const auto* name : {"NSFileHandleReadCompletionNotification", "NSFileHandleReadToEndOfFileCompletionNotification",
                             "NSFileHandleDataAvailableNotification", "NSFileHandleConnectionAcceptedNotification",
                             "NSFileHandleNotificationDataItem", "NSFileHandleNotificationFileHandleItem",
                             "NSFileHandleOperationException", "NSTaskDidTerminateNotification"})
        if (!support.process.resolve(foundation_library, "_" + std::string(name))) support.string_constant(foundation_library, "_" + std::string(name), name);
    // NSFileHandleError is the key "NSFileHandleError" (an NSNumber of the errno).
    if (!support.process.resolve(foundation_library, "_NSFileHandleNotificationMonitorModes"))
        support.string_constant(foundation_library, "_NSFileHandleNotificationMonitorModes", "NSFileHandleNotificationMonitorModes");

    // ---- NSFileHandle ----
    const auto fh = self->handle_class = support.declare_class("NSFileHandle", "NSObject", foundation_library);
    support.method(fh, "initWithFileDescriptor:closeOnDealloc:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->support.objc.host_state<HandleState>(s.x[0]);
        state->descriptor = static_cast<std::int32_t>(s.x[2]);
        state->close_on_dealloc = (s.x[3] & 0xff) != 0;
    }, "@@:iB");
    support.method(fh, "initWithFileDescriptor:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->support.objc.host_state<HandleState>(s.x[0]);
        state->descriptor = static_cast<std::int32_t>(s.x[2]);
    }, "@@:i");
    support.method(fh, "dealloc", [self, fh](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<HandleState>(s.x[0])) {
            bool close_it;
            Address handler;
            std::int32_t fd;
            {
                std::lock_guard lock(state->mutex);
                close_it = state->close_on_dealloc && !state->closed;
                state->closed = true;
                handler = std::exchange(state->readability, 0);
                fd = state->descriptor;
                ++state->reader_generation;
            }
            block_release(self->support, handler);
            if (close_it) static_cast<void>(self->sys("_close", {static_cast<Address>(fd)}));
        }
        static_cast<void>(self->support.objc.send_super(s.x[0], fh, self->support.objc.selector("dealloc")));
    }, "v@:");
    struct Standard { const char* name; std::int32_t fd; };
    for (const auto& standard : {Standard{"fileHandleWithStandardInput", 0}, Standard{"fileHandleWithStandardOutput", 1},
                                 Standard{"fileHandleWithStandardError", 2}}) {
        auto cached = std::make_shared<Address>();
        support.method(fh, standard.name, [self, cached, fd = standard.fd](CpuState& s, GuestMemory&) {
            if (!*cached) *cached = self->make_handle(fd, false);   // kept for the process
            s.x[0] = *cached;
        }, "@@:", true);
    }
    support.method(fh, "fileHandleWithNullDevice", [self](CpuState& s, GuestMemory&) {
        const auto fd = self->open_path("/dev/null", o_rdwr);
        if (fd < 0) fail("cannot open /dev/null");
        s.x[0] = autoreleased(self->support, self->make_handle(fd, true));
    }, "@@:", true);
    struct Opener { const char* path_selector; const char* url_selector; std::uint64_t flags; };
    for (const auto& opener : {Opener{"fileHandleForReadingAtPath:", "fileHandleForReadingFromURL:error:", o_rdonly},
                               Opener{"fileHandleForWritingAtPath:", "fileHandleForWritingToURL:error:", o_wronly},
                               Opener{"fileHandleForUpdatingAtPath:", "fileHandleForUpdatingURL:error:", o_rdwr}}) {
        support.method(fh, opener.path_selector, [self, flags = opener.flags](CpuState& s, GuestMemory&) {
            if (!s.x[2]) { s.x[0] = 0; return; }
            const auto fd = self->open_path(text(self->support, s.x[2]), flags);
            s.x[0] = fd < 0 ? 0 : autoreleased(self->support, self->make_handle(fd, true));
        }, "@@:@", true);
        support.method(fh, opener.url_selector, [self, flags = opener.flags](CpuState& s, GuestMemory&) {
            const auto path = s.x[2] ? self->support.cf.url_path(s.x[2]) : std::string{};
            const auto fd = path.empty() ? -1 : self->open_path(path, flags);
            if (fd < 0) {
                const auto code = path.empty() ? 2u : guest_errno(self->support);
                store(self->support, s.x[3], code == 2 ? error(self->support, "NSCocoaErrorDomain", flags == o_rdonly ? 260 : 4, "The file doesn't exist.")
                                                       : self->posix_error(code));
                s.x[0] = 0;
                return;
            }
            s.x[0] = autoreleased(self->support, self->make_handle(fd, true));
        }, "@@:@^@", true);
    }
    support.method(fh, "fileDescriptor", [self](CpuState& s, GuestMemory&) {
        const auto state = self->handle_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = static_cast<Address>(static_cast<std::uint32_t>(state->descriptor));
    }, "i@:");
    const auto read_all = [self](Address object, std::uint64_t limit, bool fill, Address error_out, bool raise) -> Address {
        std::uint32_t code{};
        const auto bytes = self->read(self->descriptor(object), limit, fill, code);
        if (!bytes) {
            if (raise) fail("NSFileHandleOperationException: read failed (errno " + std::to_string(code) + ")");
            store(self->support, error_out, self->posix_error(code));
            return 0;
        }
        return self->data(*bytes);
    };
    support.method(fh, "availableData", [self, read_all](CpuState& s, GuestMemory&) {
        const bool file = self->regular_file(self->descriptor(s.x[0]));
        s.x[0] = read_all(s.x[0], file ? UINT64_MAX : chunk, file, 0, true);
    }, "@@:");
    support.method(fh, "readDataToEndOfFile", [read_all](CpuState& s, GuestMemory&) { s.x[0] = read_all(s.x[0], UINT64_MAX, true, 0, true); }, "@@:");
    support.method(fh, "readDataToEndOfFileAndReturnError:", [read_all](CpuState& s, GuestMemory&) { s.x[0] = read_all(s.x[0], UINT64_MAX, true, s.x[2], false); }, "@@:^@");
    support.method(fh, "readDataOfLength:", [read_all](CpuState& s, GuestMemory&) { s.x[0] = read_all(s.x[0], s.x[2], true, 0, true); }, "@@:Q");
    support.method(fh, "readDataUpToLength:error:", [read_all](CpuState& s, GuestMemory&) { s.x[0] = read_all(s.x[0], s.x[2], true, s.x[3], false); }, "@@:Q^@");
    support.method(fh, "writeData:", [self](CpuState& s, GuestMemory&) {
        std::uint32_t code{};
        if (!self->write(self->descriptor(s.x[0]), self->bytes_of(s.x[2]), code))
            fail("NSFileHandleOperationException: write failed (errno " + std::to_string(code) + ")");
    }, "v@:@");
    support.method(fh, "writeData:error:", [self](CpuState& s, GuestMemory&) {
        std::uint32_t code{};
        const bool ok = self->write(self->descriptor(s.x[0]), self->bytes_of(s.x[2]), code);
        if (!ok) store(self->support, s.x[3], self->posix_error(code));
        s.x[0] = ok;
    }, "B@:@^@");
    const auto seek = [self](Address object, std::int64_t offset, std::uint64_t whence, std::uint32_t& code) {
        const auto result = self->sys("_lseek", {static_cast<Address>(self->descriptor(object)), static_cast<Address>(offset), whence});
        if (result < 0) code = guest_errno(self->support);
        return result;
    };
    support.method(fh, "offsetInFile", [seek](CpuState& s, GuestMemory&) {
        std::uint32_t code{};
        const auto result = seek(s.x[0], 0, 1, code);
        if (result < 0) fail("NSFileHandleOperationException: lseek failed (errno " + std::to_string(code) + ")");
        s.x[0] = static_cast<Address>(result);
    }, "Q@:");
    support.method(fh, "getOffset:error:", [seek, self](CpuState& s, GuestMemory& m) {
        std::uint32_t code{};
        const auto result = seek(s.x[0], 0, 1, code);
        if (result < 0) { store(self->support, s.x[3], self->posix_error(code)); s.x[0] = 0; return; }
        if (s.x[2]) m.write64(s.x[2], static_cast<Address>(result));
        s.x[0] = 1;
    }, "B@:^Q^@");
    support.method(fh, "seekToEndOfFile", [seek](CpuState& s, GuestMemory&) {
        std::uint32_t code{};
        const auto result = seek(s.x[0], 0, 2, code);
        if (result < 0) fail("NSFileHandleOperationException: lseek failed (errno " + std::to_string(code) + ")");
        s.x[0] = static_cast<Address>(result);
    }, "Q@:");
    support.method(fh, "seekToEndReturningOffset:error:", [seek, self](CpuState& s, GuestMemory& m) {
        std::uint32_t code{};
        const auto result = seek(s.x[0], 0, 2, code);
        if (result < 0) { store(self->support, s.x[3], self->posix_error(code)); s.x[0] = 0; return; }
        if (s.x[2]) m.write64(s.x[2], static_cast<Address>(result));
        s.x[0] = 1;
    }, "B@:^Q^@");
    support.method(fh, "seekToFileOffset:", [seek](CpuState& s, GuestMemory&) {
        std::uint32_t code{};
        if (seek(s.x[0], static_cast<std::int64_t>(s.x[2]), 0, code) < 0)
            fail("NSFileHandleOperationException: lseek failed (errno " + std::to_string(code) + ")");
    }, "v@:Q");
    support.method(fh, "seekToOffset:error:", [seek, self](CpuState& s, GuestMemory&) {
        std::uint32_t code{};
        const bool ok = seek(s.x[0], static_cast<std::int64_t>(s.x[2]), 0, code) >= 0;
        if (!ok) store(self->support, s.x[3], self->posix_error(code));
        s.x[0] = ok;
    }, "B@:Q^@");
    const auto simple = [self](Address object, std::string_view call, std::initializer_list<Address> extra, std::uint32_t& code) {
        std::vector<Address> values{static_cast<Address>(self->descriptor(object))};
        values.insert(values.end(), extra.begin(), extra.end());
        Address result;
        if (values.size() == 1) result = self->support.call(libsystem, call, {values[0]});
        else result = self->support.call(libsystem, call, {values[0], values[1]});
        const bool ok = static_cast<std::int64_t>(result) == 0;
        if (!ok) code = guest_errno(self->support);
        return ok;
    };
    support.method(fh, "synchronizeFile", [simple](CpuState& s, GuestMemory&) {
        std::uint32_t code{};
        if (!simple(s.x[0], "_fsync", {}, code)) fail("NSFileHandleOperationException: fsync failed (errno " + std::to_string(code) + ")");
    }, "v@:");
    support.method(fh, "synchronizeAndReturnError:", [simple, self](CpuState& s, GuestMemory&) {
        std::uint32_t code{};
        const bool ok = simple(s.x[0], "_fsync", {}, code);
        if (!ok) store(self->support, s.x[2], self->posix_error(code));
        s.x[0] = ok;
    }, "B@:^@");
    support.method(fh, "truncateFileAtOffset:", [simple, seek](CpuState& s, GuestMemory&) {
        std::uint32_t code{};
        if (!simple(s.x[0], "_ftruncate", {s.x[2]}, code) || seek(s.x[0], static_cast<std::int64_t>(s.x[2]), 0, code) < 0)
            fail("NSFileHandleOperationException: ftruncate failed (errno " + std::to_string(code) + ")");
    }, "v@:Q");
    support.method(fh, "truncateAtOffset:error:", [simple, seek, self](CpuState& s, GuestMemory&) {
        std::uint32_t code{};
        const bool ok = simple(s.x[0], "_ftruncate", {s.x[2]}, code) && seek(s.x[0], static_cast<std::int64_t>(s.x[2]), 0, code) >= 0;
        if (!ok) store(self->support, s.x[3], self->posix_error(code));
        s.x[0] = ok;
    }, "B@:Q^@");
    support.method(fh, "closeFile", [self](CpuState& s, GuestMemory&) {
        bool ok{};
        std::uint32_t code{};
        self->close(s.x[0], ok, code);
        if (!ok) fail("NSFileHandleOperationException: close failed (errno " + std::to_string(code) + ")");
    }, "v@:");
    support.method(fh, "closeAndReturnError:", [self](CpuState& s, GuestMemory&) {
        bool ok{};
        std::uint32_t code{};
        self->close(s.x[0], ok, code);
        if (!ok) store(self->support, s.x[2], self->posix_error(code));
        s.x[0] = ok;
    }, "B@:^@");
    support.method(fh, "readabilityHandler", [self](CpuState& s, GuestMemory&) {
        const auto state = self->handle_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->readability;
    }, "@?@:");
    support.method(fh, "setReadabilityHandler:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->handle_of(s.x[0]);
        const auto copy = block_copy(self->support, s.x[2]);
        Address old;
        std::uint64_t generation;
        {
            std::lock_guard lock(state->mutex);
            old = std::exchange(state->readability, copy);
            generation = ++state->reader_generation;
        }
        block_release(self->support, old);
        if (copy) self->start_reader(s.x[0], generation);
    }, "v@:@?");
    support.method(fh, "writeabilityHandler", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@?@:");
    support.method(fh, "setWriteabilityHandler:", [](CpuState& s, GuestMemory&) {
        if (s.x[2]) unsupported("-[NSFileHandle setWriteabilityHandler:]");
    }, "v@:@?");
    // Background reads (modes are not distinguished: the notification is posted on the main thread).
    const auto read_in_background = [self](Address object, bool to_end) {
        self->background(object, [self, object, to_end] {
            std::uint32_t code{};
            const auto fd = self->descriptor(object);
            const auto bytes = self->read(fd, to_end ? UINT64_MAX : chunk, to_end, code);
            std::vector<std::pair<std::string, Address>> info;
            if (bytes) info.emplace_back("NSFileHandleNotificationDataItem", self->data(*bytes));
            else info.emplace_back("NSFileHandleError", number(self->support, code));
            return std::pair{std::string(to_end ? "NSFileHandleReadToEndOfFileCompletionNotification" : "NSFileHandleReadCompletionNotification"), info};
        });
    };
    support.method(fh, "readInBackgroundAndNotify", [read_in_background](CpuState& s, GuestMemory&) { read_in_background(s.x[0], false); }, "v@:");
    support.method(fh, "readInBackgroundAndNotifyForModes:", [read_in_background](CpuState& s, GuestMemory&) { read_in_background(s.x[0], false); }, "v@:@");
    support.method(fh, "readToEndOfFileInBackgroundAndNotify", [read_in_background](CpuState& s, GuestMemory&) { read_in_background(s.x[0], true); }, "v@:");
    support.method(fh, "readToEndOfFileInBackgroundAndNotifyForModes:", [read_in_background](CpuState& s, GuestMemory&) { read_in_background(s.x[0], true); }, "v@:@");
    const auto wait_for_data = [self](Address object) {
        self->background(object, [self, object] {
            const auto fd = self->descriptor(object);
            while (!(self->poll(fd, 100) & (pollin | pollhup | pollerr | pollnval)) && !self->support.process.requested_exit()) {}
            return std::pair{std::string("NSFileHandleDataAvailableNotification"), std::vector<std::pair<std::string, Address>>{}};
        });
    };
    support.method(fh, "waitForDataInBackgroundAndNotify", [wait_for_data](CpuState& s, GuestMemory&) { wait_for_data(s.x[0]); }, "v@:");
    support.method(fh, "waitForDataInBackgroundAndNotifyForModes:", [wait_for_data](CpuState& s, GuestMemory&) { wait_for_data(s.x[0]); }, "v@:@");

    // ---- NSPipe ----
    struct PipeState final : ObjectPayload { Address reading{}, writing{}; };   // +1 handles
    const auto pipe = self->pipe_class = support.declare_class("NSPipe", "NSObject", foundation_library);
    support.method(pipe, "init", [self](CpuState& s, GuestMemory&) {
        GuestBlock fds(self->support, 8);
        if (self->sys("_pipe", {fds}) != 0) { self->support.release(s.x[0]); s.x[0] = 0; return; }   // nil, as Foundation when out of descriptors
        const auto state = self->support.objc.host_state<PipeState>(s.x[0]);
        state->reading = self->make_handle(static_cast<std::int32_t>(self->support.memory().read32(fds)), true);
        state->writing = self->make_handle(static_cast<std::int32_t>(self->support.memory().read32(fds + 4)), true);
    }, "@@:");
    support.method(pipe, "pipe", [self](CpuState& s, GuestMemory&) {
        s.x[0] = autoreleased(self->support, self->support.send_id(self->support.send_id(s.x[0], "alloc"), "init"));
    }, "@@:", true);
    support.method(pipe, "fileHandleForReading", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->support.objc.host_state<PipeState>(s.x[0])->reading;
    }, "@@:");
    support.method(pipe, "fileHandleForWriting", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->support.objc.host_state<PipeState>(s.x[0])->writing;
    }, "@@:");
    support.method(pipe, "dealloc", [self, pipe](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<PipeState>(s.x[0])) {
            objc_release(self->support, std::exchange(state->reading, 0));
            objc_release(self->support, std::exchange(state->writing, 0));
        }
        static_cast<void>(self->support.objc.send_super(s.x[0], pipe, self->support.objc.selector("dealloc")));
    }, "v@:");

    // ---- NSTask ----
    const auto task = self->task_class = support.declare_class("NSTask", "NSObject", foundation_library);
    support.method(task, "init", [self](CpuState& s, GuestMemory&) { static_cast<void>(self->task_of(s.x[0])); }, "@@:");
    struct Field { const char* getter; const char* setter; Address TaskState::*member; bool copy; };
    for (const auto& field : {Field{"launchPath", "setLaunchPath:", &TaskState::launch_path, true},
                              Field{"arguments", "setArguments:", &TaskState::arguments, true},
                              Field{"environment", "setEnvironment:", &TaskState::environment, true},
                              Field{"currentDirectoryPath", "setCurrentDirectoryPath:", &TaskState::directory, true},
                              Field{"standardInput", "setStandardInput:", &TaskState::input, false},
                              Field{"standardOutput", "setStandardOutput:", &TaskState::output, false},
                              Field{"standardError", "setStandardError:", &TaskState::error, false}}) {
        support.method(task, field.getter, [self, member = field.member](CpuState& s, GuestMemory&) {
            const auto state = self->task_of(s.x[0]);
            std::lock_guard lock(state->mutex);
            s.x[0] = (*state).*member;
        }, "@@:");
        support.method(task, field.setter, [self, member = field.member, copy = field.copy](CpuState& s, GuestMemory&) {
            const auto state = self->task_of(s.x[0]);
            const auto value = s.x[2] ? (copy ? self->support.send_id(s.x[2], "copy") : objc_retain(self->support, s.x[2])) : 0;
            Address old;
            {
                std::lock_guard lock(state->mutex);
                if (state->launched) { objc_release(self->support, value); fail("NSInvalidArgumentException: task already launched"); }
                old = std::exchange((*state).*member, value);
            }
            objc_release(self->support, old);
        }, "v@:@");
    }
    support.method(task, "executableURL", [self](CpuState& s, GuestMemory&) {
        const auto state = self->task_of(s.x[0]);
        Address path;
        { std::lock_guard lock(state->mutex); path = state->launch_path; }
        s.x[0] = path ? autoreleased(self->support, self->support.cf.file_url(text(self->support, path), false)) : 0;
    }, "@@:");
    support.method(task, "setExecutableURL:", [self](CpuState& s, GuestMemory&) {
        const auto path = s.x[2] ? string(self->support, self->support.cf.url_path(s.x[2])) : 0;
        static_cast<void>(self->support.send_id(s.x[0], "setLaunchPath:", {path}));
    }, "v@:@");
    support.method(task, "currentDirectoryURL", [self](CpuState& s, GuestMemory&) {
        const auto state = self->task_of(s.x[0]);
        Address path;
        { std::lock_guard lock(state->mutex); path = state->directory; }
        s.x[0] = path ? autoreleased(self->support, self->support.cf.file_url(text(self->support, path), true)) : 0;
    }, "@@:");
    support.method(task, "setCurrentDirectoryURL:", [self](CpuState& s, GuestMemory&) {
        const auto path = s.x[2] ? string(self->support, self->support.cf.url_path(s.x[2])) : 0;
        static_cast<void>(self->support.send_id(s.x[0], "setCurrentDirectoryPath:", {path}));
    }, "v@:@");
    support.method(task, "terminationHandler", [self](CpuState& s, GuestMemory&) {
        const auto state = self->task_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->termination_handler;
    }, "@?@:");
    support.method(task, "setTerminationHandler:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->task_of(s.x[0]);
        const auto copy = block_copy(self->support, s.x[2]);
        Address old;
        { std::lock_guard lock(state->mutex); old = std::exchange(state->termination_handler, copy); }
        block_release(self->support, old);
    }, "v@:@?");
    support.method(task, "launch", [self](CpuState& s, GuestMemory&) {
        if (const auto code = self->launch(s.x[0]))
            fail("NSInvalidArgumentException: NSTask launch path not accessible (posix_spawn errno " + std::to_string(code) + ")");
    }, "v@:");
    support.method(task, "launchAndReturnError:", [self](CpuState& s, GuestMemory&) {
        const auto code = self->launch(s.x[0]);
        if (code) store(self->support, s.x[2], code == 2 ? error(self->support, "NSCocoaErrorDomain", 4, "The file doesn't exist.") : self->posix_error(code));
        s.x[0] = code == 0;
    }, "B@:^@");
    support.method(task, "launchedTaskWithLaunchPath:arguments:", [self](CpuState& s, GuestMemory&) {
        const auto object = self->support.send_id(self->support.send_id(s.x[0], "alloc"), "init");
        static_cast<void>(self->support.send_id(object, "setLaunchPath:", {s.x[2]}));
        static_cast<void>(self->support.send_id(object, "setArguments:", {s.x[3]}));
        static_cast<void>(self->support.send(object, "launch"));
        s.x[0] = autoreleased(self->support, object);
    }, "@@:@@", true);
    support.method(task, "launchedTaskWithExecutableURL:arguments:error:terminationHandler:", [self](CpuState& s, GuestMemory&) {
        const auto object = self->support.send_id(self->support.send_id(s.x[0], "alloc"), "init");
        static_cast<void>(self->support.send_id(object, "setExecutableURL:", {s.x[2]}));
        static_cast<void>(self->support.send_id(object, "setArguments:", {s.x[3]}));
        static_cast<void>(self->support.send_id(object, "setTerminationHandler:", {s.x[5]}));
        const bool ok = (self->support.send_id(object, "launchAndReturnError:", {s.x[4]}) & 0xff) != 0;
        s.x[0] = ok ? autoreleased(self->support, object) : 0;
        if (!ok) self->support.release(object);
    }, "@@:@@^@@?", true);
    const auto read_task = [self](Address object, auto read) {
        const auto state = self->task_of(object);
        std::lock_guard lock(state->mutex);
        return read(*state);
    };
    support.method(task, "processIdentifier", [read_task](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(static_cast<std::uint32_t>(read_task(s.x[0], [](TaskState& v) { return v.pid; })));
    }, "i@:");
    support.method(task, "isRunning", [read_task](CpuState& s, GuestMemory&) { s.x[0] = read_task(s.x[0], [](TaskState& v) { return v.running; }); }, "B@:");
    support.method(task, "terminationStatus", [read_task](CpuState& s, GuestMemory&) {
        const auto [launched, running, status] = read_task(s.x[0], [](TaskState& v) { return std::tuple{v.launched, v.running, v.status}; });
        if (!launched || running) fail("NSInvalidArgumentException: -[NSTask terminationStatus] of a task that has not terminated");
        s.x[0] = static_cast<Address>(static_cast<std::uint32_t>(status));
    }, "i@:");
    support.method(task, "terminationReason", [read_task](CpuState& s, GuestMemory&) {
        const auto [launched, running, reason] = read_task(s.x[0], [](TaskState& v) { return std::tuple{v.launched, v.running, v.reason}; });
        if (!launched || running) fail("NSInvalidArgumentException: -[NSTask terminationReason] of a task that has not terminated");
        s.x[0] = static_cast<Address>(reason);
    }, "q@:");
    for (const auto& [name, signal] : std::array<std::pair<const char*, Address>, 4>{{{"terminate", 15}, {"interrupt", 2}, {"suspend", 17}, {"resume", 19}}}) {
        const bool returns_bool = std::string_view(name) == "suspend" || std::string_view(name) == "resume";
        support.method(task, name, [self, read_task, signal = signal, returns_bool](CpuState& s, GuestMemory&) {
            const auto [launched, running, pid] = read_task(s.x[0], [](TaskState& v) { return std::tuple{v.launched, v.running, v.pid}; });
            if (!launched) fail("NSInvalidArgumentException: task not launched");
            const bool ok = running && self->sys("_kill", {static_cast<Address>(pid), signal}) == 0;
            if (returns_bool) s.x[0] = ok;
        }, returns_bool ? "B@:" : "v@:");
    }
    support.method(task, "waitUntilExit", [self, read_task](CpuState& s, GuestMemory&) {
        const auto common = self->support.process.resolve(core_foundation, "_kCFRunLoopDefaultMode");
        const auto mode = common ? self->support.memory().read64(common->address) : 0;
        while (read_task(s.x[0], [](TaskState& v) { return v.running; })) {
            if (self->support.process.requested_exit()) fail("process exit while waiting for an NSTask");
            if (mode) {
                CallRegs r;
                r.x[0] = mode;
                r.set_d(0, 0.01);
                r.x[1] = 1;
                static_cast<void>(self->support.call(core_foundation, "_CFRunLoopRunInMode", r));
            } else {
                static_cast<void>(self->sys("_usleep", {10000}));
            }
        }
    }, "v@:");
    support.method(task, "dealloc", [self, task](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<TaskState>(s.x[0])) {
            for (auto* slot : {&state->launch_path, &state->arguments, &state->environment, &state->directory,
                               &state->input, &state->output, &state->error})
                objc_release(self->support, std::exchange(*slot, 0));
            block_release(self->support, std::exchange(state->termination_handler, 0));
        }
        static_cast<void>(self->support.objc.send_super(s.x[0], task, self->support.objc.selector("dealloc")));
    }, "v@:");
}

} // namespace alchemy::gui::completion_kit
