#pragma once

// Shared fixture of the strict-mode provider tests (machsys, fwgaps, media,
// devices, printing): a process with every headless and GUI provider
// installed in run_program's order, and helpers that call exports through
// their guest addresses with the Apple arm64 register image.

#include "alchemy/call_regs.hpp"
#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_providers.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/gui/install.hpp"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fill_test {

using Address = std::uint64_t;

inline void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

struct Import { const char* library; const char* symbol; };
inline constexpr Import strict_imports[] = {
#include "alchemy/gui/tests/strict_imports.inc"
};

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> bytes) {
        return alchemy::OutputResult{bytes.size(), 0}; }};
    // Every provider, in run_program's order (alchemy/darwin_providers.hpp).
    alchemy::DarwinProviders providers{process};
    alchemy::DarwinServices& services{providers.services};
    alchemy::DarwinThreads& threads{providers.threads};
    alchemy::ObjcRuntime& objc{providers.objc};
    alchemy::CoreFoundation& cf{providers.cf};
    alchemy::FoundationBridge& foundation{providers.foundation};

    ~Fixture() {
        try { process.request_exit(0); threads.join_workers(); } catch (...) {}
    }
    transmute::GuestMemory& memory() { return engine; }
    Address symbol(std::string_view library, std::string_view name) {
        const auto found = process.resolve(library, name);
        require(found.has_value(), "missing " + std::string(library) + " " + std::string(name));
        return found->address;
    }
    Address call(std::string_view library, std::string_view name, std::initializer_list<Address> arguments = {}) {
        return engine.invoke_callback(symbol(library, name), std::span(arguments.begin(), arguments.size()));
    }
    alchemy::CallResult regs(std::string_view library, std::string_view name, const alchemy::CallRegs& registers) {
        return alchemy::engine_call_regs(engine, symbol(library, name), registers);
    }
    std::int32_t status(std::string_view library, std::string_view name, std::initializer_list<Address> arguments = {}) {
        return static_cast<std::int32_t>(call(library, name, arguments));
    }
    Address block(std::size_t size, std::uint8_t fill = 0) {
        const auto address = process.allocate_guest(size ? size : 1, 16);
        engine.write_bytes(address, std::vector<std::uint8_t>(size ? size : 1, fill));
        return address;
    }
    Address text(std::string_view value) {
        const auto address = process.allocate_guest(value.size() + 1, 1);
        std::vector<std::uint8_t> bytes(value.begin(), value.end());
        bytes.push_back(0);
        engine.write_bytes(address, bytes);
        return address;
    }
    std::string read_text(Address address, std::size_t maximum = 4096) {
        if (!address) return {};
        bool terminated{};
        const auto bytes = engine.read_string(address, 1, maximum, &terminated);
        return {bytes.begin(), bytes.end()};
    }
    // The CFString value of a `const CFStringRef` data export.
    std::string string_constant(std::string_view library, std::string_view name) {
        return cf.utf8(engine.read64(symbol(library, name)));
    }
    Address send(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        alchemy::CallRegs registers{};
        unsigned index = 2;
        for (const auto argument : arguments) registers.x[index++] = argument;
        return objc.send(receiver, selector, registers).x0;
    }
    Address cls(std::string_view name) {
        const auto found = objc.find_class(name);
        require(found != 0, "missing class " + std::string(name));
        return found;
    }
};

// Every strict-mode import of `libraries` resolves.
inline void imports_resolve(Fixture& f, std::initializer_list<std::string_view> libraries) {
    std::size_t checked = 0;
    for (const auto& item : strict_imports) {
        bool selected = false;
        for (const auto library : libraries) selected = selected || library == item.library;
        if (!selected) continue;
        require(f.process.resolve(item.library, item.symbol).has_value(),
                std::string("strict import unresolved: ") + item.library + " " + item.symbol);
        ++checked;
    }
    require(checked > 0, "no strict imports selected");
    for (const auto library : libraries)
        require(alchemy::ProcessContext::is_provider_library(library), "not a provider identity: " + std::string(library));
}

// FILL_TEST_ONLY=<name>[,<name>...] runs a subset (debugging aid).
inline int run(std::string_view suite, const std::vector<std::pair<const char*, std::function<void(Fixture&)>>>& tests) {
    std::string only;
    if (const char* value = std::getenv("FILL_TEST_ONLY")) only = std::string(",") + value + ",";
    try {
        Fixture f;
        for (const auto& [name, test] : tests) {
            if (!only.empty() && only.find(std::string(",") + name + ",") == std::string::npos) continue;
            std::cout << "[" << suite << "] " << name << std::endl;
            test(f);
        }
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << suite << " tests passed\n";
    return 0;
}

} // namespace fill_test
