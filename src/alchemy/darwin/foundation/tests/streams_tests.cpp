// Streams (darwin/foundation/streams.cpp) over the full provider set
// (fill_fixture.hpp): CFReadStream/CFWriteStream and NSInputStream/NSOutputStream
// on data, memory, fixed buffers, files, bound pairs and loopback TCP sockets,
// stream properties, and events delivered on a dispatch queue to a CF client.
// Run with ALCHEMY_NETWORK=loopback (ctest sets nothing; loopback is allowed
// under every policy).
#include "alchemy/gui/tests/fill_fixture.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

namespace {
using namespace fill_test;

constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view foundation = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
enum : Address { not_open = 0, opening = 1, open = 2, at_end = 5, closed = 6, error_status = 7 };
bool yes(Address value) { return (value & 0xff) != 0; }

Address bytes(Fixture& f, std::string_view text) {
    const auto block = f.block(text.size() + 1);
    f.engine.write_bytes(block, std::vector<std::uint8_t>(text.begin(), text.end()));
    return block;
}
std::string read_bytes(Fixture& f, Address address, std::size_t length) {
    std::vector<std::uint8_t> out(length);
    if (length) f.engine.read_bytes(address, out);
    return {out.begin(), out.end()};
}
std::string data_text(Fixture& f, Address data) {
    return read_bytes(f, f.call(cf, "_CFDataGetBytePtr", {data}), static_cast<std::size_t>(f.call(cf, "_CFDataGetLength", {data})));
}
template <class Condition>
void eventually(Condition condition, const std::string& what) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (condition()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(false, what);
}

void data_streams(Fixture& f) {
    const auto null_allocator = f.engine.read64(f.symbol(cf, "_kCFAllocatorNull"));
    const auto stream = f.call(cf, "_CFReadStreamCreateWithBytesNoCopy", {0, bytes(f, "hello"), 5, null_allocator});
    require(f.call(cf, "_CFGetTypeID", {stream}) == f.call(cf, "_CFReadStreamGetTypeID"), "CFReadStream type id");
    require(f.call(cf, "_CFReadStreamGetStatus", {stream}) == not_open, "not open yet");
    require(yes(f.call(cf, "_CFReadStreamOpen", {stream})) && f.call(cf, "_CFReadStreamGetStatus", {stream}) == open, "open");
    const auto buffer = f.block(16);
    require(f.call(cf, "_CFReadStreamRead", {stream, buffer, 3}) == 3 && read_bytes(f, buffer, 3) == "hel", "read part");
    require(yes(f.call(cf, "_CFReadStreamHasBytesAvailable", {stream})), "more bytes");
    require(f.call(cf, "_CFReadStreamRead", {stream, buffer, 10}) == 2 && read_bytes(f, buffer, 2) == "lo", "read the rest");
    require(f.call(cf, "_CFReadStreamGetStatus", {stream}) == at_end && f.call(cf, "_CFReadStreamRead", {stream, buffer, 10}) == 0,
            "at the end");
    static_cast<void>(f.call(cf, "_CFReadStreamClose", {stream}));
    require(f.call(cf, "_CFReadStreamGetStatus", {stream}) == closed, "closed");
    // NSInputStream over an NSData; -getBuffer:length: sees the data in place.
    const auto data = f.call(cf, "_CFDataCreate", {0, bytes(f, "abcdef"), 6});
    const auto input = f.send(f.cls("NSInputStream"), "inputStreamWithData:", {data});
    static_cast<void>(f.send(input, "open"));
    const auto pointer = f.block(8), length = f.block(8);
    require(yes(f.send(input, "getBuffer:length:", {pointer, length})) && f.engine.read64(length) == 6 &&
                read_bytes(f, f.engine.read64(pointer), 6) == "abcdef", "-getBuffer:length:");
    require(f.send(input, "streamStatus") == at_end, "getBuffer consumed the data");
    const auto again = f.send(f.cls("NSInputStream"), "inputStreamWithData:", {data});
    static_cast<void>(f.send(again, "open"));
    require(f.send(again, "read:maxLength:", {buffer, 4}) == 4 && read_bytes(f, buffer, 4) == "abcd", "-read:maxLength:");
    require(f.call(cf, "_CFReadStreamRead", {again, buffer, 8}) == 2, "CF reads the same NSInputStream (toll-free)");
}

void memory_and_buffer_streams(Fixture& f) {
    const auto memory = f.call(cf, "_CFWriteStreamCreateWithAllocatedBuffers", {0, 0});
    require(f.call(cf, "_CFGetTypeID", {memory}) == f.call(cf, "_CFWriteStreamGetTypeID"), "CFWriteStream type id");
    static_cast<void>(f.call(cf, "_CFWriteStreamOpen", {memory}));
    require(yes(f.call(cf, "_CFWriteStreamCanAcceptBytes", {memory})), "memory accepts bytes");
    require(f.call(cf, "_CFWriteStreamWrite", {memory, bytes(f, "abc"), 3}) == 3 && f.call(cf, "_CFWriteStreamWrite", {memory, bytes(f, "de"), 2}) == 2,
            "writes to memory");
    const auto key = f.engine.read64(f.symbol(cf, "_kCFStreamPropertyDataWritten"));
    require(data_text(f, f.call(cf, "_CFWriteStreamCopyProperty", {memory, key})) == "abcde", "kCFStreamPropertyDataWritten");
    const auto output = f.send(f.cls("NSOutputStream"), "outputStreamToMemory");
    static_cast<void>(f.send(output, "open"));
    require(f.send(output, "write:maxLength:", {bytes(f, "xyz"), 3}) == 3, "-write:maxLength:");
    const auto ns_key = f.engine.read64(f.symbol(foundation, "_NSStreamDataWrittenToMemoryStreamKey"));
    require(data_text(f, f.send(output, "propertyForKey:", {ns_key})) == "xyz", "NSStreamDataWrittenToMemoryStreamKey");
    const auto target = f.block(8, 0);
    const auto fixed = f.call(cf, "_CFWriteStreamCreateWithBuffer", {0, target, 4});
    static_cast<void>(f.call(cf, "_CFWriteStreamOpen", {fixed}));
    require(f.call(cf, "_CFWriteStreamWrite", {fixed, bytes(f, "abcdef"), 6}) == 4 && read_bytes(f, target, 4) == "abcd", "a fixed buffer fills");
    require(f.call(cf, "_CFWriteStreamGetStatus", {fixed}) == at_end && f.call(cf, "_CFWriteStreamWrite", {fixed, bytes(f, "g"), 1}) == 0,
            "a full buffer accepts nothing");
}

void file_streams(Fixture& f) {
    const auto directory = std::filesystem::temp_directory_path() / ("alchemy-streams-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto path = (directory / "stream.txt").generic_string();
    const auto ns_path = f.cf.string(path);
    const auto output = f.send(f.send(f.cls("NSOutputStream"), "alloc"), "initToFileAtPath:append:", {ns_path, 0});
    static_cast<void>(f.send(output, "open"));
    require(f.send(output, "streamStatus") == open, "file output opens");
    require(f.send(output, "write:maxLength:", {bytes(f, "first "), 6}) == 6, "file write");
    static_cast<void>(f.send(output, "close"));
    const auto appender = f.send(f.send(f.cls("NSOutputStream"), "alloc"), "initToFileAtPath:append:", {ns_path, 1});
    static_cast<void>(f.send(appender, "open"));
    static_cast<void>(f.send(appender, "write:maxLength:", {bytes(f, "second"), 6}));
    static_cast<void>(f.send(appender, "close"));
    const auto input = f.send(f.cls("NSInputStream"), "inputStreamWithFileAtPath:", {ns_path});
    static_cast<void>(f.send(input, "open"));
    const auto buffer = f.block(64);
    require(f.send(input, "read:maxLength:", {buffer, 64}) == 12 && read_bytes(f, buffer, 12) == "first second", "append and read back");
    require(f.send(input, "read:maxLength:", {buffer, 64}) == 0 && f.send(input, "streamStatus") == at_end, "end of file");
    const auto offset_key = f.engine.read64(f.symbol(cf, "_kCFStreamPropertyFileCurrentOffset"));
    const auto seeking = f.send(f.cls("NSInputStream"), "inputStreamWithFileAtPath:", {ns_path});
    static_cast<void>(f.send(seeking, "open"));
    require(yes(f.call(cf, "_CFReadStreamSetProperty", {seeking, offset_key, f.cf.integer(6, 4)})), "set the file offset");
    require(f.call(cf, "_CFReadStreamRead", {seeking, buffer, 64}) == 6 && read_bytes(f, buffer, 6) == "second", "read from the offset");
    require(f.cf.number_value(f.call(cf, "_CFReadStreamCopyProperty", {seeking, offset_key})).value_or(0) == 12, "kCFStreamPropertyFileCurrentOffset");
    const auto missing = f.send(f.cls("NSInputStream"), "inputStreamWithFileAtPath:", {f.cf.string((directory / "missing.txt").generic_string())});
    static_cast<void>(f.send(missing, "open"));
    const auto error = f.send(missing, "streamError");
    require(f.send(missing, "streamStatus") == error_status && error && f.send(error, "code") == 2, "ENOENT for a missing file");
    for (const auto object : {output, appender}) static_cast<void>(f.call(cf, "_CFRelease", {object}));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void bound_pairs(Fixture& f) {
    const auto pair = f.block(16);
    static_cast<void>(f.call(cf, "_CFStreamCreateBoundPair", {0, pair, pair + 8, 16}));
    const auto reader = f.engine.read64(pair), writer = f.engine.read64(pair + 8);
    static_cast<void>(f.call(cf, "_CFReadStreamOpen", {reader}));
    static_cast<void>(f.call(cf, "_CFWriteStreamOpen", {writer}));
    require(!yes(f.call(cf, "_CFReadStreamHasBytesAvailable", {reader})), "an empty pair");
    require(f.call(cf, "_CFWriteStreamWrite", {writer, bytes(f, "ping"), 4}) == 4, "write into the pair");
    require(yes(f.call(cf, "_CFReadStreamHasBytesAvailable", {reader})), "bytes reach the reader");
    const auto buffer = f.block(16);
    require(f.call(cf, "_CFReadStreamRead", {reader, buffer, 16}) == 4 && read_bytes(f, buffer, 4) == "ping", "read from the pair");
    static_cast<void>(f.call(cf, "_CFWriteStreamClose", {writer}));
    require(f.call(cf, "_CFReadStreamRead", {reader, buffer, 16}) == 0 && f.call(cf, "_CFReadStreamGetStatus", {reader}) == at_end,
            "closing the writer ends the reader");
}

void events(Fixture& f) {
    std::atomic<std::uint64_t> seen{};
    std::atomic<Address> seen_info{};
    const auto callback = f.process.install_function("_alchemy_test_stream_client", [&](transmute::CpuState& s, transmute::GuestMemory&) {
        seen |= s.x[1];
        seen_info = s.x[2];
        s.x[0] = 0;
    }, "alchemy.test");
    const auto null_allocator = f.engine.read64(f.symbol(cf, "_kCFAllocatorNull"));
    const auto stream = f.call(cf, "_CFReadStreamCreateWithBytesNoCopy", {0, bytes(f, "event"), 5, null_allocator});
    const auto context = f.block(40);
    f.engine.write64(context + 8, 0x1234);
    require(yes(f.call(cf, "_CFReadStreamSetClient", {stream, 1 | 2 | 16, callback, context})), "CFReadStreamSetClient");
    const auto queue = f.call(libsystem, "_dispatch_get_global_queue", {0, 0});
    static_cast<void>(f.call(cf, "_CFReadStreamSetDispatchQueue", {stream, queue}));
    static_cast<void>(f.call(cf, "_CFReadStreamOpen", {stream}));
    eventually([&] { return (seen.load() & (1 | 2)) == (1 | 2); }, "OpenCompleted and HasBytesAvailable on the queue");
    require(seen_info.load() == 0x1234, "the client's info");
    const auto buffer = f.block(16);
    static_cast<void>(f.call(cf, "_CFReadStreamRead", {stream, buffer, 16}));
    eventually([&] { return (seen.load() & 16) != 0; }, "EndEncountered after the last read");
    static_cast<void>(f.call(cf, "_CFReadStreamSetDispatchQueue", {stream, 0}));
    static_cast<void>(f.call(cf, "_CFReadStreamClose", {stream}));
}

void sockets(Fixture& f) {
    // A loopback listener through the guest libSystem.
    const auto listener = static_cast<std::int32_t>(f.call(libsystem, "_socket", {2, 1, 0}));   // AF_INET, SOCK_STREAM
    require(listener >= 0, "listener socket");
    const auto address = f.block(16);
    f.memory().write8(address, 16);              // sin_len
    f.memory().write8(address + 1, 2);           // AF_INET
    f.memory().write32(address + 4, 0x0100007f);   // 127.0.0.1 (network order)
    require(f.call(libsystem, "_bind", {static_cast<Address>(listener), address, 16}) == 0, "bind");
    require(f.call(libsystem, "_listen", {static_cast<Address>(listener), 4}) == 0, "listen");
    const auto length = f.block(4);
    f.memory().write32(length, 16);
    require(f.call(libsystem, "_getsockname", {static_cast<Address>(listener), address, length}) == 0, "getsockname");
    const auto port = static_cast<std::uint16_t>((f.memory().read8(address + 2) << 8) | f.memory().read8(address + 3));
    const auto pair = f.block(16);
    static_cast<void>(f.call(cf, "_CFStreamCreatePairWithSocketToHost", {0, f.cf.string("127.0.0.1"), port, pair, pair + 8}));
    const auto reader = f.engine.read64(pair), writer = f.engine.read64(pair + 8);
    require(yes(f.call(cf, "_CFReadStreamOpen", {reader})) && yes(f.call(cf, "_CFWriteStreamOpen", {writer})), "open the socket pair");
    const auto server = static_cast<std::int32_t>(f.call(libsystem, "_accept", {static_cast<Address>(listener), 0, 0}));
    require(server >= 0, "accept");
    eventually([&] { return f.call(cf, "_CFReadStreamGetStatus", {reader}) == open; }, "the connection opens");
    require(f.call(libsystem, "_write", {static_cast<Address>(server), bytes(f, "hi"), 2}) == 2, "server write");
    const auto buffer = f.block(16);
    require(f.call(cf, "_CFReadStreamRead", {reader, buffer, 16}) == 2 && read_bytes(f, buffer, 2) == "hi", "read from the socket");
    require(f.call(cf, "_CFWriteStreamWrite", {writer, bytes(f, "yo"), 2}) == 2, "write to the socket");
    require(f.call(libsystem, "_read", {static_cast<Address>(server), buffer, 16}) == 2 && read_bytes(f, buffer, 2) == "yo", "server read");
    const auto host_key = f.engine.read64(f.symbol(cf, "_kCFStreamPropertySocketRemoteHostName"));
    require(f.cf.utf8(f.call(cf, "_CFReadStreamCopyProperty", {reader, host_key})) == "127.0.0.1", "kCFStreamPropertySocketRemoteHostName");
    static_cast<void>(f.call(libsystem, "_close", {static_cast<Address>(server)}));
    require(f.call(cf, "_CFReadStreamRead", {reader, buffer, 16}) == 0 && f.call(cf, "_CFReadStreamGetStatus", {reader}) == at_end,
            "the server's close ends the stream");
    static_cast<void>(f.call(cf, "_CFReadStreamClose", {reader}));
    static_cast<void>(f.call(cf, "_CFWriteStreamClose", {writer}));
    static_cast<void>(f.call(libsystem, "_close", {static_cast<Address>(listener)}));
    // TLS is not provided on socket streams.
    const auto settings = f.engine.read64(f.symbol("/System/Library/Frameworks/CFNetwork.framework/Versions/A/CFNetwork", "_kCFStreamPropertySSLSettings"));
    bool refused = false;
    try { static_cast<void>(f.call(cf, "_CFReadStreamSetProperty", {reader, settings, f.cf.string("x")})); } catch (const std::exception&) { refused = true; }
    require(refused, "TLS settings stop explicitly");
}

} // namespace

int main() {
    return run("foundation streams", {{"data streams", data_streams}, {"memory and buffer streams", memory_and_buffer_streams},
        {"file streams", file_streams}, {"bound pairs", bound_pairs}, {"events", events}, {"sockets", sockets}});
}
