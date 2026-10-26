// XPC objects and connections (misc/xpc_objects.cpp): dictionaries, arrays, scalars, data,
// UUIDs, descriptors, copies and equality, the type descriptors and singletons, and
// connections whose service is not there.
#include "alchemy/darwin/kit/completion/runtime/tests/runtime_test_kit.hpp"

#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view sys = "/usr/lib/libSystem.B.dylib";

template <class Condition> void eventually(Condition done, std::string_view what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("timed out: " + std::string(what));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
template <class Operation> bool stops(Operation operation) {
    try { operation(); } catch (const std::exception&) { return true; }
    return false;
}
std::string read(Fixture& f, Address address) {
    std::string value;
    for (Address at = address; address; ++at) { const auto c = static_cast<GuestMemory&>(f.engine).read8(at); if (!c) break; value.push_back(static_cast<char>(c)); }
    return value;
}
Address data_symbol(Fixture& f, std::string_view name) { return f.engine.read64(f.symbol(sys, name)); }

void test_types(Fixture& f) {
    const auto dictionary = f.libsystem("_xpc_dictionary_create", {0, 0, 0});
    require(dictionary && f.libsystem("_xpc_get_type", {dictionary}) == f.symbol(sys, "__xpc_type_dictionary"), "xpc_get_type is XPC_TYPE_DICTIONARY");
    require(read(f, f.libsystem("_xpc_type_get_name", {f.symbol(sys, "__xpc_type_array")})) == "array", "xpc_type_get_name");
    require(f.objc.find_class("OS_xpc_dictionary") == f.symbol(sys, "__xpc_type_dictionary"), "XPC_TYPE_* are the OS_xpc_* classes");
    const auto yes = f.libsystem("_xpc_bool_create", {1});
    require(yes == f.symbol(sys, "__xpc_bool_true") && f.libsystem("_xpc_bool_create", {0}) == f.symbol(sys, "__xpc_bool_false"), "XPC_BOOL_TRUE/FALSE are singletons");
    require(f.libsystem("_xpc_bool_get_value", {yes}) == 1, "xpc_bool_get_value");
    require(read(f, data_symbol(f, "__xpc_error_key_description")) == "XPCErrorDescription", "XPC_ERROR_KEY_DESCRIPTION");
    const auto invalid = f.symbol(sys, "__xpc_error_connection_invalid");
    require(f.libsystem("_xpc_get_type", {invalid}) == f.symbol(sys, "__xpc_type_error"), "XPC_ERROR_CONNECTION_INVALID is an error");
    require(read(f, f.libsystem("_xpc_dictionary_get_string", {invalid, f.c_string("XPCErrorDescription")})) == "Connection invalid", "an error's description");
    f.libsystem("_xpc_release", {dictionary});
}

void test_scalars(Fixture& f) {
    const auto number = f.libsystem("_xpc_int64_create", {static_cast<Address>(-42)});
    require(static_cast<std::int64_t>(f.libsystem("_xpc_int64_get_value", {number})) == -42, "int64");
    require(f.libsystem("_xpc_uint64_get_value", {number}) == 0, "a getter of another type reads 0");
    const auto text = f.libsystem("_xpc_string_create", {f.c_string("hello")});
    require(read(f, f.libsystem("_xpc_string_get_string_ptr", {text})) == "hello" && f.libsystem("_xpc_string_get_length", {text}) == 5, "strings");
    const auto data = f.libsystem("_xpc_data_create", {f.c_string("abcdef"), 6});
    const auto buffer = f.process.allocate_guest(8);
    require(f.libsystem("_xpc_data_get_length", {data}) == 6 && f.libsystem("_xpc_data_get_bytes", {data, buffer, 2, 10}) == 4, "xpc_data_get_bytes clips");
    require(read(f, f.libsystem("_xpc_data_get_bytes_ptr", {data})).substr(0, 6) == "abcdef", "data bytes");
    const auto uuid = f.c_string("0123456789abcdef");
    const auto uuid_object = f.libsystem("_xpc_uuid_create", {uuid});
    require(read(f, f.libsystem("_xpc_uuid_get_bytes", {uuid_object})).substr(0, 16) == "0123456789abcdef", "uuid");
    const auto date = f.libsystem("_xpc_date_create", {1234});
    require(f.libsystem("_xpc_date_get_value", {date}) == 1234, "dates");
    alchemy::CallRegs registers{};
    registers.set_d(0, 2.5);
    const auto real = alchemy::engine_call_regs(f.engine, f.symbol(sys, "_xpc_double_create"), registers).x0;
    require(read(f, f.libsystem("_xpc_copy_description", {real})) == "<double: 2.5>", "doubles");
    const auto null = f.libsystem("_xpc_null_create");
    require(f.libsystem("_xpc_get_type", {null}) == f.symbol(sys, "__xpc_type_null"), "null");
    for (const auto object : {number, text, data, uuid_object, date, real, null}) f.libsystem("_xpc_release", {object});
}

void test_dictionaries(Fixture& f) {
    const auto dictionary = f.libsystem("_xpc_dictionary_create_empty");
    f.libsystem("_xpc_dictionary_set_string", {dictionary, f.c_string("name"), f.c_string("alchemy")});
    f.libsystem("_xpc_dictionary_set_int64", {dictionary, f.c_string("count"), 7});
    f.libsystem("_xpc_dictionary_set_bool", {dictionary, f.c_string("on"), 1});
    f.libsystem("_xpc_dictionary_set_data", {dictionary, f.c_string("blob"), f.c_string("xyz"), 3});
    require(f.libsystem("_xpc_dictionary_get_count", {dictionary}) == 4, "four keys");
    require(read(f, f.libsystem("_xpc_dictionary_get_string", {dictionary, f.c_string("name")})) == "alchemy", "get_string");
    require(f.libsystem("_xpc_dictionary_get_int64", {dictionary, f.c_string("count")}) == 7, "get_int64");
    require(f.libsystem("_xpc_dictionary_get_bool", {dictionary, f.c_string("on")}) == 1, "get_bool");
    require(f.libsystem("_xpc_dictionary_get_string", {dictionary, f.c_string("count")}) == 0, "a value of another type reads NULL");
    const auto length = f.process.allocate_guest(8);
    require(read(f, f.libsystem("_xpc_dictionary_get_data", {dictionary, f.c_string("blob"), length})).substr(0, 3) == "xyz" &&
            f.engine.read64(length) == 3, "get_data");
    // Replacing and removing (a NULL value removes the key).
    f.libsystem("_xpc_dictionary_set_int64", {dictionary, f.c_string("count"), 8});
    require(f.libsystem("_xpc_dictionary_get_int64", {dictionary, f.c_string("count")}) == 8, "set replaces");
    f.libsystem("_xpc_dictionary_set_value", {dictionary, f.c_string("on"), 0});
    require(f.libsystem("_xpc_dictionary_get_count", {dictionary}) == 3 && f.libsystem("_xpc_dictionary_get_value", {dictionary, f.c_string("on")}) == 0, "NULL removes");
    // xpc_dictionary_apply visits the keys in order and stops when the block returns false.
    static std::vector<std::string> keys;
    static Fixture* fixture = &f;
    const auto visit = f.process.install_function("_xpc_test_visit", [](CpuState& s, GuestMemory&) {
        keys.push_back(read(*fixture, s.x[1]));
        s.x[0] = keys.size() < 2 ? 1 : 0;
    });
    require(f.libsystem("_xpc_dictionary_apply", {dictionary, f.block(visit)}) == 0 && keys == std::vector<std::string>{"name", "count"}, "dictionary_apply stops");
    // Copies are deep and equal; a changed copy is not.
    const auto copy = f.libsystem("_xpc_copy", {dictionary});
    require(copy != dictionary && f.libsystem("_xpc_equal", {copy, dictionary}) == 1, "xpc_copy is equal");
    require(f.libsystem("_xpc_hash", {copy}) == f.libsystem("_xpc_hash", {dictionary}), "equal objects hash alike");
    f.libsystem("_xpc_dictionary_set_string", {copy, f.c_string("name"), f.c_string("other")});
    require(f.libsystem("_xpc_equal", {copy, dictionary}) == 0, "a changed copy differs");
    require(read(f, f.libsystem("_xpc_dictionary_get_string", {dictionary, f.c_string("name")})) == "alchemy", "the original is untouched");
    // Values are retained by the dictionary: one released by its creator stays readable.
    const auto nested = f.libsystem("_xpc_array_create_empty");
    f.libsystem("_xpc_dictionary_set_value", {dictionary, f.c_string("list"), nested});
    f.libsystem("_xpc_release", {nested});
    require(f.libsystem("_xpc_dictionary_get_array", {dictionary, f.c_string("list")}) == nested, "dictionaries retain their values");
    require(read(f, f.libsystem("_xpc_copy_description", {dictionary})).starts_with("<dictionary: { count = 4"), "description");
    f.libsystem("_xpc_release", {copy});
    f.libsystem("_xpc_release", {dictionary});
}

void test_arrays(Fixture& f) {
    const auto array = f.libsystem("_xpc_array_create", {0, 0});
    f.libsystem("_xpc_array_set_string", {array, UINT64_MAX, f.c_string("a")});
    f.libsystem("_xpc_array_set_int64", {array, UINT64_MAX, 5});
    f.libsystem("_xpc_array_append_value", {array, f.libsystem("_xpc_bool_create", {1})});
    require(f.libsystem("_xpc_array_get_count", {array}) == 3, "XPC_ARRAY_APPEND appends");
    require(read(f, f.libsystem("_xpc_array_get_string", {array, 0})) == "a" && f.libsystem("_xpc_array_get_int64", {array, 1}) == 5 &&
            f.libsystem("_xpc_array_get_bool", {array, 2}) == 1, "typed array getters");
    f.libsystem("_xpc_array_set_int64", {array, 1, 6});
    require(f.libsystem("_xpc_array_get_int64", {array, 1}) == 6 && f.libsystem("_xpc_array_get_value", {array, 9}) == 0, "set_value replaces; past the end is NULL");
    require(stops([&] { f.libsystem("_xpc_array_set_int64", {array, 9, 1}); }), "an index past the end stops");
    static std::vector<Address> indices;
    const auto visit = f.process.install_function("_xpc_test_array_visit", [](CpuState& s, GuestMemory&) { indices.push_back(s.x[1]); s.x[0] = 1; });
    require(f.libsystem("_xpc_array_apply", {array, f.block(visit)}) == 1 && indices == std::vector<Address>{0, 1, 2}, "array_apply");
    f.libsystem("_xpc_release", {array});
}

void test_descriptors(Fixture& f) {
    const auto pipe = f.process.allocate_guest(8);
    require(f.libsystem("_pipe", {pipe}) == 0, "pipe");
    const auto read_end = f.engine.read32(pipe), write_end = f.engine.read32(pipe + 4);
    const auto object = f.libsystem("_xpc_fd_create", {write_end});
    require(object != 0, "xpc_fd_create");
    f.libsystem("_close", {write_end});
    const auto duplicate = f.libsystem("_xpc_fd_dup", {object});
    require(static_cast<std::int32_t>(duplicate) >= 0 && f.libsystem("_write", {duplicate, f.c_string("ok"), 2}) == 2, "the object keeps its own descriptor");
    f.libsystem("_close", {duplicate});
    f.libsystem("_xpc_release", {object});
    // Every write end is now closed: the reader sees the two bytes, then end of file.
    const auto buffer = f.process.allocate_guest(8);
    require(f.libsystem("_read", {read_end, buffer, 8}) == 2 && f.libsystem("_read", {read_end, buffer, 8}) == 0, "releasing the object closes its descriptor");
    f.libsystem("_close", {read_end});
}

void test_connections(Fixture& f) {
    static std::atomic<Address> event{0}, reply{0};
    const auto handler = f.process.install_function("_xpc_test_event", [](CpuState& s, GuestMemory&) { event = s.x[1]; s.x[0] = 0; });
    const auto replied = f.process.install_function("_xpc_test_reply", [](CpuState& s, GuestMemory&) { reply = s.x[1]; s.x[0] = 0; });
    const auto connection = f.libsystem("_xpc_connection_create_mach_service", {f.c_string("com.example.helper"), 0, 0});
    require(connection && f.libsystem("_xpc_get_type", {connection}) == f.symbol(sys, "__xpc_type_connection"), "a connection object");
    require(read(f, f.libsystem("_xpc_connection_get_name", {connection})) == "com.example.helper", "xpc_connection_get_name");
    f.libsystem("_xpc_connection_set_event_handler", {connection, f.block(handler)});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(event == 0, "nothing is delivered before resume");
    f.libsystem("_xpc_connection_resume", {connection});
    const auto invalid = f.symbol(sys, "__xpc_error_connection_invalid");
    eventually([&] { return event == invalid; }, "the handler learns the service is not there");
    const auto message = f.libsystem("_xpc_dictionary_create_empty");
    f.libsystem("_xpc_connection_send_message", {connection, message});
    f.libsystem("_xpc_connection_send_message_with_reply", {connection, message, 0, f.block(replied)});
    eventually([&] { return reply == invalid; }, "a reply handler receives XPC_ERROR_CONNECTION_INVALID");
    require(f.libsystem("_xpc_connection_send_message_with_reply_sync", {connection, message}) == invalid, "the synchronous reply is the error");
    require(f.libsystem("_xpc_dictionary_create_reply", {message}) == 0, "a message not received has no reply");
    f.libsystem("_xpc_connection_set_context", {connection, 0x1234});
    require(f.libsystem("_xpc_connection_get_context", {connection}) == 0x1234 && f.libsystem("_xpc_connection_get_pid", {connection}) == 0, "context; no peer");
    const auto endpoint = f.libsystem("_xpc_endpoint_create", {connection});
    const auto from_endpoint = f.libsystem("_xpc_connection_create_from_endpoint", {endpoint});
    require(from_endpoint && read(f, f.libsystem("_xpc_connection_get_name", {from_endpoint})) == "com.example.helper", "connections from endpoints");
    require(stops([&] { f.libsystem("_xpc_connection_resume", {message}); }), "a connection call on a dictionary stops");
    require(stops([&] { f.libsystem("_xpc_main", {f.block(handler)}); }), "xpc_main stops");
    f.libsystem("_xpc_connection_cancel", {connection});
    for (const auto object : {message, endpoint, from_endpoint, connection}) f.libsystem("_xpc_release", {object});
    // Shared memory names the region itself.
    const auto region = f.process.allocate_guest(64), mapped = f.process.allocate_guest(8);
    const auto shmem = f.libsystem("_xpc_shmem_create", {region, 64});
    require(f.libsystem("_xpc_shmem_map", {shmem, mapped}) == 64 && f.engine.read64(mapped) == region, "xpc_shmem_map");
    f.libsystem("_xpc_release", {shmem});
}

} // namespace

int main() {
    try {
        Fixture fixture;
        alchemy::export_fill::Provider provider(fixture.process);
        alchemy::export_fill::install_libsystem_xpc(provider, fixture.objc);
        const auto stage = [](std::string_view name) { std::cout << "[stage] " << name << std::endl; };
        stage("types"); test_types(fixture);
        stage("scalars"); test_scalars(fixture);
        stage("dictionaries"); test_dictionaries(fixture);
        stage("arrays"); test_arrays(fixture);
        stage("descriptors"); test_descriptors(fixture);
        stage("connections"); test_connections(fixture);
        std::cout << "Alchemy XPC tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
