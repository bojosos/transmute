#pragma once

// Shared fixture of the tests split from the QLab runtime tests
// (darwin/libsystem/tests/foundation_system_tests.cpp,
// darwin/dispatch/tests/dispatch_objects_tests.cpp): the core providers, the run
// loop, SystemConfiguration, Security and network, and the QLab runtime group
// over one Support.
#include "alchemy/darwin/kit/completion/runtime/common.hpp"
#include "alchemy/darwin/foundation/invocation_core.hpp"
#include "alchemy/darwin/libsystem/misc/log_store.hpp"
#include "alchemy/darwin/foundation/sdef.hpp"
#include "alchemy/darwin/foundation/undo_core.hpp"

#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/gui/runloop.hpp"

#include <atomic>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Address = std::uint64_t;
using transmute::CpuState;
using transmute::GuestMemory;
namespace completion_kit = alchemy::gui::completion_kit;
namespace inv = alchemy::gui::completion_kit::invocation;
namespace undo = alchemy::gui::completion_kit::undo;

void require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}

template <class F>
std::string failure_of(F&& run) {
    try { run(); } catch (const std::exception& error) { return error.what(); }
    return {};
}

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> bytes) {
        return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices services{process};
    alchemy::DarwinThreads threads{(alchemy::install_darwin_dispatch(process), process)};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    alchemy::FoundationBridge foundation{process, objc, cf};
    std::shared_ptr<completion_kit::Support> support;

    Fixture() {
        alchemy::install_darwin_frameworks(process, objc, cf);
        alchemy::gui::InstallContext context{process, objc, cf};
        alchemy::gui::install_runloop(context);
        alchemy::gui::install_system_configuration(context);
        alchemy::gui::install_security(context);
        alchemy::gui::install_network(context);
        support = std::make_shared<completion_kit::Support>(process, objc, cf);
        completion_kit::install_dispatch_objects(*support);
        completion_kit::install_exceptions(*support);
        completion_kit::install_threads(*support);
        completion_kit::install_locks(*support);
        completion_kit::install_invocations(*support);
        completion_kit::install_undo_manager(*support);
        completion_kit::install_key_value_coding(*support);
        completion_kit::install_collections(*support);
        completion_kit::install_key_value_observing(*support);
        completion_kit::install_bindings(*support);
        completion_kit::install_kvc_collections(*support);
        completion_kit::install_notification_queue(*support);
        completion_kit::install_process_io(*support);
        completion_kit::install_files(*support);
        completion_kit::install_bundles(*support);
        completion_kit::install_os_log(*support);
        completion_kit::install_system_functions(*support);
        completion_kit::install_os_log_store(*support);
        completion_kit::install_scripting(*support);
        completion_kit::install_xpc(*support);
        completion_kit::install_foundation(*support);   // NSCache
        completion_kit::install_cf_extras(*support);
    }
    ~Fixture() {
        try { process.request_exit(0); threads.join_workers(); } catch (...) {}
    }
    Address symbol(std::string_view library, std::string_view name) {
        const auto found = process.resolve(library, name);
        require(found.has_value(), "missing symbol " + std::string(name));
        return found->address;
    }
    Address call(std::string_view library, std::string_view name, std::initializer_list<Address> arguments = {}) {
        return engine.invoke_callback(symbol(library, name), std::span(arguments.begin(), arguments.size()));
    }
    Address libsystem(std::string_view name, std::initializer_list<Address> arguments = {}) { return call("/usr/lib/libSystem.B.dylib", name, arguments); }
    Address cls(std::string_view name) { const auto value = objc.find_class(name); require(value != 0, "missing class " + std::string(name)); return value; }
    Address send(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        alchemy::CallRegs registers;
        std::size_t index = 2;
        for (const auto argument : arguments) registers.x[index++] = argument;
        return objc.send(receiver, selector, registers).x0;
    }
    Address c_string(std::string_view text) {
        const auto address = process.allocate_guest(text.size() + 1);
        engine.write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        static_cast<GuestMemory&>(engine).write8(address + text.size(), 0);
        return address;
    }
    std::string text(Address string) { return string ? cf.utf8(string) : std::string("(nil)"); }
    // A compiler-emitted (global) block whose invoke is `invoke`.
    Address block(Address invoke) {
        const auto value = process.allocate_guest(32), descriptor = process.allocate_guest(16);
        engine.write64(descriptor, 0);
        engine.write64(descriptor + 8, 32);
        engine.write64(value, symbol("/usr/lib/libSystem.B.dylib", "__NSConcreteGlobalBlock"));
        engine.write32(value + 8, 1U << 28);
        engine.write32(value + 12, 0);
        engine.write64(value + 16, invoke);
        engine.write64(value + 24, descriptor);
        return value;
    }
    Address data(std::string_view bytes) {
        const auto buffer = process.allocate_guest(bytes.size() + 1);
        engine.write_bytes(buffer, std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
        return cf.invoke("CFDataCreate", std::array<Address, 3>{0, buffer, bytes.size()});
    }
    std::string bytes(Address data_object) {
        std::string result(static_cast<std::size_t>(send(data_object, "length")), '\0');
        if (!result.empty()) engine.read_bytes(send(data_object, "bytes"), std::span(reinterpret_cast<std::uint8_t*>(result.data()), result.size()));
        return result;
    }
};

std::string guest_path_of(const std::filesystem::path& host) {
    auto text = host.generic_string();
    return "/Volumes/" + text.substr(0, 1) + text.substr(2);
}

} // namespace
