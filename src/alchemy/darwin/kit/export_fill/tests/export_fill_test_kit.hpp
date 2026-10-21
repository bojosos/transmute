#pragma once

// Shared fixture of the tests of the providers first written for a renderer plug-in
// (src/alchemy/darwin/kit/export_fill/tests/guest_services_tests.cpp and the framework tests split
// from it): every core provider, the headless frameworks and the rows of the
// former gap-fill group, in table order.
#include "alchemy/darwin/compression/install.hpp"
#include "alchemy/darwin/compression/zlib_codec.hpp"
#include "alchemy/darwin/dispatch/install.hpp"
#include "alchemy/darwin/accelerate/install.hpp"
#include "alchemy/darwin/applicationservices/install.hpp"
#include "alchemy/darwin/corefoundation/install.hpp"
#include "alchemy/darwin/coreservices/install.hpp"
#include "alchemy/darwin/diskarbitration/install.hpp"
#include "alchemy/darwin/foundation/install.hpp"
#include "alchemy/darwin/libsystem/install.hpp"
#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/security/md4.hpp"
#include "alchemy/darwin/corefoundation/cf_plist.hpp"

#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_mach_vm.hpp"
#include "alchemy/darwin_native.hpp"
#include "alchemy/darwin_numeric.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace alchemy::export_fill {
std::string test_base_name(std::string path);
std::string test_directory_name(std::string path);
double test_floattidf(std::uint64_t low, std::uint64_t high);
std::pair<std::uint64_t, std::uint64_t> test_udivmod(std::uint64_t al, std::uint64_t ah, std::uint64_t bl, std::uint64_t bh, bool remainder);
} // namespace alchemy::export_fill

namespace {
using Address = std::uint64_t;
namespace vd = alchemy::export_fill;

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}
std::vector<std::uint8_t> bytes(std::string_view text) { return {text.begin(), text.end()}; }

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> data) {
        return alchemy::OutputResult{data.size(), 0}; }};
    alchemy::DarwinServices services{process};
    // run_program's order: mach_vm, native, numeric (compiler-rt builtins, libm), then dispatch.
    bool vm{(alchemy::install_darwin_mach_vm(process), alchemy::install_darwin_native(process),
             alchemy::install_darwin_numeric(process), true)};
    alchemy::DarwinThreads threads{(alchemy::install_darwin_dispatch(process), process)};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    alchemy::FoundationBridge foundation{process, objc, cf};

    Fixture() {
        process.arguments = {"/Applications/Tool/tool.bin", "-version"};
        process.executable = std::filesystem::temp_directory_path() / "tool.bin";
        process.environment = {"ALCHEMY_TEST=1"};
        // The startup vector process startup builds (argv, NULL, envp, NULL, apple, NULL).
        {
            std::vector<Address> slots;
            const auto string = [this](std::string_view value) {
                const auto address = process.allocate_guest(value.size() + 1);
                std::vector<std::uint8_t> data(value.begin(), value.end());
                data.push_back(0);
                engine.write_bytes(address, data);
                return address;
            };
            for (const auto& value : process.arguments) slots.push_back(string(value));
            slots.push_back(0);
            for (const auto& value : process.environment) slots.push_back(string(value));
            slots.push_back(0);
            slots.push_back(string("executable_path=" + alchemy::guest_path_string(process.executable)));
            slots.push_back(0);
            const auto table = process.allocate_guest(slots.size() * 8);
            for (std::size_t index = 0; index < slots.size(); ++index) engine.write64(table + index * 8, slots[index]);
            process.startup_argc = process.arguments.size();
            process.startup_argv = table;
        }
        alchemy::install_darwin_frameworks(process, objc, cf);
        alchemy::gui::InstallContext context{process, objc, cf};
        // The rows of the former gap-fill group, in table order
        // (src/alchemy/gui/install.cpp).
        alchemy::compression::install_zlib(context);
        alchemy::libsystem::install_libsystem_gaps(context);
        alchemy::export_fill::Provider provider(process);
        alchemy::export_fill::install_cc_cryptor(provider);
        alchemy::applicationservices::install_applicationservices(context);
        alchemy::diskarbitration::install_diskarbitration(context);
        alchemy::gui::install_coreservices_carbon_core(context);
        alchemy::gui::install_corefoundation_time_zones_and_uuids(context);
        alchemy::gui::install_foundation_file_attribute_keys(context);
        alchemy::accelerate::install_accelerate_bnns(context);
        alchemy::export_fill::install_iokit_registry(provider, objc, cf);
        alchemy::export_fill::install_cf_extra(provider, objc, cf);
        alchemy::export_fill::install_cf_system(provider, cf);
        alchemy::export_fill::install_cf_preferences(provider, objc, cf);
        alchemy::export_fill::install_cf_locale(provider, objc, cf);
        alchemy::export_fill::install_foundation_classes(provider, objc, cf);
        alchemy::export_fill::install_coreservices(provider, objc, cf);
        alchemy::libsystem::install_libsystem_services(context);
        alchemy::dispatch::install_dispatch_data(context);
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
    Address sys(std::string_view name, std::initializer_list<Address> arguments = {}) {
        return call("/usr/lib/libSystem.B.dylib", name, arguments);
    }
    Address text(std::string_view value) {
        const auto address = process.allocate_guest(value.size() + 1);
        std::vector<std::uint8_t> data(value.begin(), value.end());
        data.push_back(0);
        engine.write_bytes(address, data);
        return address;
    }
    std::uint8_t byte(Address address) const {
        return static_cast<const transmute::GuestMemory&>(engine).read8(address);
    }
    std::string read(Address address) {
        const auto data = engine.read_string(address, 1, 4096);
        return {data.begin(), data.end()};
    }
};

constexpr auto libz = "/usr/lib/libz.1.dylib";
constexpr auto iokit = "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit";

std::vector<std::uint8_t> hex(std::string_view text) {
    std::vector<std::uint8_t> out;
    for (std::size_t index = 0; index + 1 < text.size(); index += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(text.substr(index, 2)), nullptr, 16)));
    return out;
}
std::vector<std::uint8_t> guest_bytes(Fixture& f, Address address, std::size_t size) {
    std::vector<std::uint8_t> out(size);
    if (size) f.engine.read_bytes(address, out);
    return out;
}
Address guest_buffer(Fixture& f, const std::vector<std::uint8_t>& data, std::size_t size = 0) {
    const auto address = f.process.allocate_guest(std::max<std::size_t>({size, data.size(), std::size_t{1}}));
    if (!data.empty()) f.engine.write_bytes(address, data);
    return address;
}

// dispatch_data_t, CommonCrypto ciphers and PBKDF2, and the daemon-only
// libSystem/IOKit/CF calls (libsystem_services.cpp, dispatch_data.cpp,
// cc_cryptor.cpp, cf_extra.cpp, iokit_registry.cpp).
} // namespace
