// The Foundation-facing libSystem pieces first written for QLab (split from the
// QLab runtime tests): Foundation locks (process/foundation_threads.cpp),
// NSFileHandle and NSPipe (process/process_io.cpp), os_log message composition
// (misc/log_store.cpp), the C leftovers of misc/system_c.cpp, NSXPC*
// (misc/xpc.cpp), and fts(3) and _c_locale (misc/fts_err.cpp).
#include "alchemy/darwin/kit/completion/runtime/tests/runtime_test_kit.hpp"

namespace {

void test_locks(Fixture& f) {
    const auto lock = f.send(f.send(f.cls("NSLock"), "alloc"), "init");
    require(f.send(lock, "tryLock") & 1, "tryLock on a free lock");
    require(!(f.send(lock, "tryLock") & 1), "tryLock on a held (non-recursive) lock fails");
    f.send(lock, "unlock");
    f.send(lock, "lock");
    f.send(lock, "unlock");
    f.send(lock, "setName:", {f.cf.string("g2")});
    require(f.text(f.send(lock, "name")) == "g2", "lock name");
    f.send(lock, "release");
    const auto recursive = f.send(f.send(f.cls("NSRecursiveLock"), "alloc"), "init");
    require((f.send(recursive, "tryLock") & 1) && (f.send(recursive, "tryLock") & 1), "a recursive lock re-enters");
    f.send(recursive, "unlock");
    f.send(recursive, "unlock");
    f.send(recursive, "release");
    const auto condition = f.send(f.send(f.cls("NSConditionLock"), "alloc"), "initWithCondition:", {3});
    require(f.send(condition, "condition") == 3, "condition value");
    require(!(f.send(condition, "tryLockWhenCondition:", {4}) & 1) && (f.send(condition, "tryLockWhenCondition:", {3}) & 1), "tryLockWhenCondition:");
    f.send(condition, "unlockWithCondition:", {7});
    require(f.send(condition, "condition") == 7, "unlockWithCondition:");
    f.send(condition, "release");
}

void test_file_handles(Fixture& f) {
    // A pipe: write, read by length, close the writer, read to end of file.
    const auto pipe = f.send(f.cls("NSPipe"), "pipe");
    const auto writer = f.send(pipe, "fileHandleForWriting"), reader = f.send(pipe, "fileHandleForReading");
    f.send(writer, "writeData:", {f.data("hello pipe")});
    require(f.bytes(f.send(reader, "readDataOfLength:", {5})) == "hello", "readDataOfLength: from a pipe");
    require(f.bytes(f.send(reader, "availableData")) == " pipe", "availableData returns what one read delivers");
    f.send(writer, "writeData:", {f.data("tail")});
    f.send(writer, "closeFile");
    require(f.bytes(f.send(reader, "readDataToEndOfFile")) == "tail", "readDataToEndOfFile after the writer closed");
    require(f.send(f.send(reader, "availableData"), "length") == 0, "availableData at end of file is empty");

    // A file: reading, seeking, offsets, errors.
    const auto host = std::filesystem::temp_directory_path() / "alchemy_g2_file_handle.txt";
    { std::ofstream out(host, std::ios::binary); out << "0123456789"; }
    const auto handle = f.send(f.cls("NSFileHandle"), "fileHandleForReadingAtPath:", {f.cf.string(guest_path_of(host))});
    require(handle != 0, "fileHandleForReadingAtPath:");
    require(f.bytes(f.send(handle, "availableData")) == "0123456789", "availableData reads a regular file to its end");
    const auto buffer = f.process.allocate_guest(8);
    require(f.send(handle, "seekToOffset:error:", {3, 0}) & 1, "seekToOffset:error:");
    require(f.send(handle, "offsetInFile") == 3, "offsetInFile");
    require(f.bytes(f.send(handle, "readDataUpToLength:error:", {4, buffer})) == "3456", "readDataUpToLength:error:");
    require(f.send(handle, "closeAndReturnError:", {buffer}) & 1, "closeAndReturnError:");
    require(f.send(f.cls("NSFileHandle"), "fileHandleForReadingAtPath:", {f.cf.string(guest_path_of(host) + ".missing")}) == 0, "a missing file gives nil");
    f.engine.write64(buffer, 0);
    const auto url = f.send(f.cls("NSURL"), "fileURLWithPath:", {f.cf.string(guest_path_of(host) + ".missing")});
    require(f.send(f.cls("NSFileHandle"), "fileHandleForReadingFromURL:error:", {url, buffer}) == 0 && f.engine.read64(buffer) != 0,
            "the URL variant reports an error");
    std::filesystem::remove(host);
}

void test_log_composition() {
    namespace logs = alchemy::gui::completion_kit::logs;
    // %d (4 bytes), %{public}s (pointer), %@ (object), %.2f (8 bytes), %{private}s redacted (no data), %%.
    std::vector<std::uint8_t> buffer{0x02, 6};
    const auto add = [&](std::uint8_t descriptor, std::uint64_t value, std::uint8_t size) {
        buffer.push_back(descriptor);
        buffer.push_back(size);
        for (std::uint8_t i = 0; i < size; ++i) buffer.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    add(0x00, static_cast<std::uint32_t>(-42), 4);
    add(0x22, 0x1000, 8);
    add(0x40, 0x2000, 8);
    add(0x00, std::bit_cast<std::uint64_t>(3.14159), 8);
    add(0x21, 0, 0);
    add(0x00, 255, 8);
    const auto text = logs::compose("v=%d name=%{public}s obj=%@ pi=%.2f secret=%{private}s hex=%llx 100%%", buffer,
        [](std::uint64_t pointer) { return pointer == 0x1000 ? std::string("cue") : std::string("?"); },
        [](std::uint64_t object) { return object == 0x2000 ? std::string("<Workspace>") : std::string("?"); });
    require(text == "v=-42 name=cue obj=<Workspace> pi=3.14 secret=<private> hex=ff 100%", "os_log composition: " + text);
}

void test_system_functions(Fixture& f) {
    const auto lib = std::string_view("/usr/lib/libSystem.B.dylib");
    // OSAtomic and the queue.
    const auto cell = f.process.allocate_guest(64);
    f.engine.write32(cell, 41);
    require(f.call(lib, "_OSAtomicIncrement32Barrier", {cell}) == 42 && f.engine.read32(cell) == 42, "OSAtomicIncrement32Barrier");
    f.engine.write64(cell + 8, 0);
    require(f.call(lib, "_OSAtomicTestAndSetBarrier", {9, cell + 8}) == 0 && static_cast<GuestMemory&>(f.engine).read8(cell + 9) == 0x40, "OSAtomicTestAndSet bit 9");
    require(f.call(lib, "_OSAtomicTestAndClearBarrier", {9, cell + 8}) == 1 && static_cast<GuestMemory&>(f.engine).read8(cell + 9) == 0, "OSAtomicTestAndClear");
    const auto head = f.process.allocate_guest(16), a = f.process.allocate_guest(16), b = f.process.allocate_guest(16);
    f.engine.write64(head, 0);
    f.engine.write64(head + 8, 0);
    f.call(lib, "_OSAtomicEnqueue", {head, a, 8});
    f.call(lib, "_OSAtomicEnqueue", {head, b, 8});
    require(f.call(lib, "_OSAtomicDequeue", {head, 8}) == b && f.call(lib, "_OSAtomicDequeue", {head, 8}) == a &&
            f.call(lib, "_OSAtomicDequeue", {head, 8}) == 0, "OSAtomicEnqueue/Dequeue is LIFO");
    const auto wide = f.process.allocate_guest(8), fifo = f.process.allocate_guest(24);
    f.engine.write64(wide, 41);
    require(f.call(lib, "_OSAtomicIncrement64", {wide}) == 42 && f.call(lib, "_OSAtomicDecrement64Barrier", {wide}) == 41 &&
            f.engine.read64(wide) == 41, "OSAtomicIncrement64/Decrement64");
    f.engine.write64(fifo, 0);
    f.engine.write64(fifo + 8, 0);
    f.call(lib, "_OSAtomicFifoEnqueue", {fifo, a, 8});
    f.call(lib, "_OSAtomicFifoEnqueue", {fifo, b, 8});
    require(f.call(lib, "_OSAtomicFifoDequeue", {fifo, 8}) == a && f.call(lib, "_OSAtomicFifoDequeue", {fifo, 8}) == b &&
            f.call(lib, "_OSAtomicFifoDequeue", {fifo, 8}) == 0, "OSAtomicFifoEnqueue/Dequeue is FIFO");
    // termios
    const auto termios = f.process.allocate_guest(72);
    for (Address at = 0; at < 72; at += 8) f.engine.write64(termios + at, ~Address{0});
    f.call(lib, "_cfmakeraw", {termios});
    require((f.engine.read64(termios + 24) & 0x108) == 0 && (f.engine.read64(termios + 16) & 0x300) == 0x300 &&
            static_cast<GuestMemory&>(f.engine).read8(termios + 48) == 1, "cfmakeraw clears ICANON/ECHO, sets CS8 and VMIN 1");
    f.call(lib, "_cfsetspeed", {termios, 115200});
    require(f.engine.read64(termios + 56) == 115200 && f.engine.read64(termios + 64) == 115200, "cfsetspeed");
    // arc4random_uniform, memset_pattern4
    for (int i = 0; i < 50; ++i) require(f.call(lib, "_arc4random_uniform", {7}) < 7, "arc4random_uniform bound");
    const auto pattern = f.process.allocate_guest(16);
    f.engine.write32(pattern, 0x04030201);
    f.call(lib, "_memset_pattern4", {cell, pattern, 10});
    require(static_cast<GuestMemory&>(f.engine).read8(cell + 9) == 0x02 && f.engine.read32(cell + 4) == 0x04030201, "memset_pattern4");
    // ASL: send, then search with a prefix query.
    const auto message = f.call(lib, "_asl_new", {0});
    f.call(lib, "_asl_set", {message, f.c_string("Message"), f.c_string("cue fired")});
    f.call(lib, "_asl_send", {0, message});
    const auto other = f.call(lib, "_asl_new", {0});
    f.call(lib, "_asl_set", {other, f.c_string("Message"), f.c_string("something else")});
    f.call(lib, "_asl_send", {0, other});
    const auto query = f.call(lib, "_asl_new", {1});
    f.call(lib, "_asl_set_query", {query, f.c_string("Message"), f.c_string("CUE"), 0x10 | 0x20 | 1});   // casefold prefix equal
    const auto response = f.call(lib, "_asl_search", {0, query});
    const auto found = f.call(lib, "_asl_next", {response});
    require(found && f.text(f.cf.string(alchemy::gui::completion_kit::rt::c_string(f.engine, f.call(lib, "_asl_get", {found, f.c_string("Message")})))) == "cue fired" &&
            f.call(lib, "_asl_next", {response}) == 0, "asl_search matches by prefix, case folded");
    f.call(lib, "_asl_release", {response});
    // Foundation geometry strings: a rect comes back in d0-d3.
    alchemy::CallRegs rect;
    rect.x[0] = f.cf.string("{{1.5, -2}, {30, 40}}");
    const auto result = f.support->call(alchemy::foundation_library, "_NSRectFromString", rect);
    require(result.d(0) == 1.5 && result.d(1) == -2 && result.d(2) == 30 && result.d(3) == 40, "NSRectFromString");
    // CFMessagePort within the process.
    static Address received;
    received = 0;
    const auto callout = f.process.install_function("_g2_test_port_callout", [&f](CpuState& s, GuestMemory&) {
        received = s.x[1];
        s.x[0] = f.data("reply");
    });
    const auto cf_lib = alchemy::core_foundation_library;
    const auto name = f.cf.string("g2.test.port");
    const auto local = f.call(cf_lib, "_CFMessagePortCreateLocal", {0, name, callout, 0, 0});
    require(local && f.call(cf_lib, "_CFMessagePortCreateLocal", {0, name, callout, 0, 0}) == 0, "one local port per name");
    const auto remote = f.call(cf_lib, "_CFMessagePortCreateRemote", {0, name});
    require(remote && f.call(cf_lib, "_CFMessagePortCreateRemote", {0, f.cf.string("g2.missing")}) == 0, "remote lookup by name");
    const auto reply_cell = f.process.allocate_guest(8);
    // SInt32 CFMessagePortSendRequest(remote, msgid, data, sendTimeout(d0), rcvTimeout(d1), replyMode, returnData)
    alchemy::CallRegs send;
    send.x[0] = remote; send.x[1] = 7; send.x[2] = f.data("ping");
    send.x[3] = f.engine.read64(f.symbol(cf_lib, "_kCFRunLoopDefaultMode")); send.x[4] = reply_cell;
    send.set_d(0, 1.0); send.set_d(1, 1.0);
    require(f.support->call(cf_lib, "_CFMessagePortSendRequest", send).x0 == 0 && received == 7 && f.bytes(f.engine.read64(reply_cell)) == "reply",
            "CFMessagePortSendRequest runs the local callout and returns its reply");
    f.call(cf_lib, "_CFMessagePortInvalidate", {local});
    require(!(f.call(cf_lib, "_CFMessagePortIsValid", {remote}) & 0xff), "an invalidated local port invalidates its remotes");

    // os_log -> OSLogStore (current process).
    const auto log = f.call(lib, "_os_log_create", {f.c_string("com.figure53.qlab.test"), f.c_string("unit")});
    const auto buffer = f.process.allocate_guest(16);
    const std::array<std::uint8_t, 8> bytes{0x00, 1, 0x00, 4, 5, 0, 0, 0};
    f.engine.write_bytes(buffer, bytes);
    f.call(lib, "__os_log_impl", {0, log, 0x10, f.c_string("cue %d failed"), buffer, bytes.size()});
    const auto store_class = f.cls("OSLogStore");
    const auto error_cell = f.process.allocate_guest(8);
    f.engine.write64(error_cell, 0);
    require(f.send(store_class, "localStoreAndReturnError:", {error_cell}) == 0 && f.engine.read64(error_cell) != 0, "the local store needs an entitlement");
    const auto store = f.send(store_class, "storeWithScope:error:", {1, 0});
    const auto enumerator = f.send(store, "entriesEnumeratorWithOptions:position:predicate:error:", {1, 0, 0, 0});
    const auto entry = f.send(enumerator, "nextObject");
    require(entry && f.text(f.send(entry, "composedMessage")) == "cue 5 failed" && f.send(entry, "level") == 4 &&
            f.text(f.send(entry, "subsystem")) == "com.figure53.qlab.test", "OSLogStore enumerates this process's entries");
}

void test_xpc(Fixture& f) {
    // XPC: configuration is kept, connecting fails explicitly.
    const auto connection = f.send(f.send(f.cls("NSXPCConnection"), "alloc"), "initWithServiceName:", {f.cf.string("org.sparkle-project.Downloader")});
    const auto interface = f.send(f.cls("NSXPCInterface"), "interfaceWithProtocol:", {0});
    f.send(connection, "setRemoteObjectInterface:", {interface});
    require(f.send(connection, "remoteObjectInterface") == interface && f.text(f.send(connection, "serviceName")) == "org.sparkle-project.Downloader",
            "NSXPCConnection configuration");
    require(failure_of([&] { f.send(connection, "resume"); }).starts_with("unsupported:"), "NSXPCConnection resume fails explicitly");
}

void test_libsystem_extras(Fixture& f) {
    namespace fs = std::filesystem;
    const auto lib = std::string_view("/usr/lib/libSystem.B.dylib");
    // _c_locale is a locale the *_l functions accept.
    const auto c_locale = f.symbol(lib, "__c_locale");
    require(f.call(lib, "_isdigit_l", {'7', c_locale}) == 1, "isdigit_l with &_c_locale");

    // fts: pre-order directories, files, post-order directories, sorted by name.
    const auto base = fs::temp_directory_path() / "alchemy_g2_fts";
    fs::remove_all(base);
    fs::create_directories(base / "b");
    std::ofstream(base / "a.txt", std::ios::binary) << "a";
    std::ofstream(base / "b" / "c.txt", std::ios::binary) << "c";
    const auto compare = f.process.install_function("_g2_test_fts_compare", [&f](CpuState& s, GuestMemory& m) {
        const auto a = m.read64(s.x[0]), b = m.read64(s.x[1]);
        const auto left = alchemy::gui::completion_kit::rt::c_string(m, a + 104, 1024), right = alchemy::gui::completion_kit::rt::c_string(m, b + 104, 1024);
        s.x[0] = static_cast<Address>(static_cast<std::int64_t>(left < right ? -1 : left > right ? 1 : 0));
        static_cast<void>(f);
    });
    const auto argv = f.process.allocate_guest(16);
    f.engine.write64(argv, f.c_string(guest_path_of(base)));
    f.engine.write64(argv + 8, 0);
    const auto tree = f.call(lib, "_fts_open", {argv, 0x010 | 0x004, compare});   // FTS_PHYSICAL | FTS_NOCHDIR
    require(tree != 0, "fts_open");
    std::vector<std::string> seen;
    for (auto entry = f.call(lib, "_fts_read", {tree}); entry; entry = f.call(lib, "_fts_read", {tree})) {
        auto& m = static_cast<GuestMemory&>(f.engine);
        const auto info = m.read32(entry + 88) & 0xffff;
        const auto level = static_cast<std::int16_t>(m.read32(entry + 84) >> 16);
        seen.push_back(alchemy::gui::completion_kit::rt::c_string(m, entry + 104, 1024) + ":" + std::to_string(info) + ":" + std::to_string(level));
    }
    const std::vector<std::string> expected{"alchemy_g2_fts:1:0", "a.txt:8:1", "b:1:1", "c.txt:8:2", "b:6:1", "alchemy_g2_fts:6:0"};
    std::string got;
    for (const auto& value : seen) got += value + " ";
    require(seen == expected, "fts order and info: " + got);
    require(f.call(lib, "_fts_close", {tree}) == 0, "fts_close");
    fs::remove_all(base);
}

} // namespace

int main() {
    try {
        test_log_composition();
        Fixture fixture;
        test_locks(fixture);
        test_file_handles(fixture);
        test_system_functions(fixture);
        test_xpc(fixture);
        test_libsystem_extras(fixture);
        std::cout << "Alchemy libSystem Foundation pieces tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
