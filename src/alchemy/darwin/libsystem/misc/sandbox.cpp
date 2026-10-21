#include "alchemy/gui/value_provider.hpp"

// libsandbox for Chrome's images, cut from darwin/coreservices/services.cpp
// (install_services calls it in place): profiles cannot be compiled or
// applied (the macOS sandbox is not supported; Chrome runs with
// --no-sandbox); parameter lists are real.
namespace alchemy::gui::values {
namespace {

using fill::core_foundation;
using fill::status;

constexpr std::int32_t e_notsup = 45;

} // namespace

void Provider::install_sandbox() {
    ProcessContext::add_provider_library(std::string(libsandbox));
    const auto params_class = cls(libsandbox, "__AlchemySandboxParams", "NSObject");
    object_properties(params_class, {"values"});
    raw(libsandbox, "_sandbox_create_params", [this, params_class](CpuState& s, GuestMemory&) {
        const auto params = create(params_class);
        const auto values = call(core_foundation, "_CFDictionaryCreateMutable", {0, 0,
            symbol(core_foundation, "_kCFTypeDictionaryKeyCallBacks"), symbol(core_foundation, "_kCFTypeDictionaryValueCallBacks")});
        store(params, "values", values);
        release(values);
        s.x[0] = params;
    });
    // int sandbox_set_param(params, const char *key, const char *value)
    raw(libsandbox, "_sandbox_set_param", [this](CpuState& s, GuestMemory& m) {
        const auto values = s.x[0] ? stored(s.x[0], "values") : 0;
        if (!values || !s.x[1]) { set_errno(22); s.x[0] = status(-1); return; }
        const auto key = cf.string(read_cstring(m, s.x[1]));
        const auto value = cf.string(read_cstring(m, s.x[2]));
        static_cast<void>(call(core_foundation, "_CFDictionarySetValue", {values, key, value}));
        release(key);
        release(value);
        s.x[0] = 0;
    });
    raw(libsandbox, "_sandbox_free_params", [this](CpuState& s, GuestMemory&) { release(s.x[0]); });
    // sandbox_compile_string(profile, params, char **error): profiles cannot be compiled.
    raw(libsandbox, "_sandbox_compile_string", [this](CpuState& s, GuestMemory& m) {
        if (s.x[2]) m.write64(s.x[2], strdup("Alchemy does not support the macOS sandbox"));
        set_errno(e_notsup);
        s.x[0] = 0;
    });
    raw(libsandbox, "_sandbox_compile_file", [this](CpuState& s, GuestMemory& m) {
        if (s.x[2]) m.write64(s.x[2], strdup("Alchemy does not support the macOS sandbox"));
        set_errno(e_notsup);
        s.x[0] = 0;
    });
    raw(libsandbox, "_sandbox_free_profile", [](CpuState&, GuestMemory&) {});   // only NULL profiles exist
    raw(libsandbox, "_sandbox_apply", [this](CpuState& s, GuestMemory&) { set_errno(e_notsup); s.x[0] = status(-1); });
    if (const auto found = process.resolve(fill::libsystem, "_sandbox_free_error"); found && !has(libsandbox, "_sandbox_free_error"))
        process.install_export(std::string(libsandbox), "_sandbox_free_error", {found->address, ExportKind::function});
}

} // namespace alchemy::gui::values
