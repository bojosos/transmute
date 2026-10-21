#pragma once

#include "transmute/a64.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace transmute {

class GuestProcess;

struct ChildBootstrap {
    std::wstring guest_command_line;
    std::vector<wchar_t> guest_environment;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> inherited_handles;
    std::array<std::uint64_t, 3> standard_handle_tokens{};
};

ChildBootstrap read_child_bootstrap(std::uint64_t inherited_mapping,
                                    std::uint32_t mapped_bytes);

class Win32Bridge {
public:
    Win32Bridge();
    ~Win32Bridge();
    Win32Bridge(const Win32Bridge&) = delete;
    Win32Bridge& operator=(const Win32Bridge&) = delete;
    void set_guest_command_line(std::wstring command_line);
    void configure_child_launch(std::filesystem::path host_executable,
                                std::vector<std::filesystem::path> dll_directories,
                                std::optional<std::uint64_t> instruction_limit,
                                bool jit);
    void apply_child_bootstrap(ChildBootstrap bootstrap);
    void initialize_thread_environment(ReferenceEngine& engine,
                                       std::uint64_t image_base);
    void initialize_worker_thread_environment(ReferenceEngine& engine,
                                              std::uint64_t image_base);
    void initialize_static_tls(ReferenceEngine& engine);
    void initialize_guest_dlls(ReferenceEngine& engine,
                               std::span<PeImage* const> dependency_order);
    struct DynamicTlsRegistration {
        std::uint32_t index{};
        std::uint32_t previous_index{};
        std::vector<std::pair<std::uint64_t, std::uint64_t>> slots;
        bool active{};
    };
    [[nodiscard]] DynamicTlsRegistration initialize_dynamic_tls(
        ReferenceEngine& engine, const PeImage& module);
    void rollback_dynamic_tls(ReferenceEngine& engine, const PeImage& module,
                              DynamicTlsRegistration& registration);
    // Register the executable first, then DLLs in process attachment order.
    void register_guest_module(const PeImage& module);
    void unregister_guest_module(const PeImage& module);
    [[nodiscard]] std::optional<std::string> describe_guest_address(
        std::uint64_t address) const;
    void begin_dynamic_module_initialization();
    void end_dynamic_module_initialization();
    [[nodiscard]] std::uint64_t load_guest_dll(
        GuestProcess& process, const std::filesystem::path& requested,
        const std::vector<std::filesystem::path>& directories,
        bool include_requested_directory, ReferenceEngine& caller);
    void set_guest_dll_loader(std::function<std::uint64_t(
        const std::filesystem::path&,
        const std::vector<std::filesystem::path>&, bool,
        ReferenceEngine&)> loader);
    void set_guest_callback_invoker(
        std::function<std::uint64_t(std::uint64_t, std::uint64_t)> invoker);
    void set_guest_tls_callback_invoker(
        std::function<std::uint64_t(std::uint64_t, std::uint64_t,
                                    std::uint64_t, std::uint64_t)> invoker);
    void set_guest_window_callback_invoker(
        std::function<std::uint64_t(std::uint64_t, std::uint64_t,
                                    std::uint64_t, std::uint64_t,
                                    std::uint64_t)> invoker);
    void set_guest_binary_callback_invoker(
        std::function<std::uint64_t(std::uint64_t, std::uint64_t,
                                    std::uint64_t)> invoker);
    std::intptr_t host_window_message(void* native_window, unsigned message,
                                      std::uintptr_t wparam, std::intptr_t lparam,
                                      void* context) noexcept;
    void cleanup_process_exit();
    void cleanup_worker_thread(ReferenceEngine& engine);
    void cleanup_fls();
    void check_worker_failures();
    std::optional<std::uint32_t> dispatch(const Import& imported, CpuState& state,
                                           GuestMemory& memory);

private:
    struct State;
    std::unique_ptr<State> state_;
    void notify_thread_libraries(ReferenceEngine& engine,
                                 std::uint32_t reason,
                                 std::uint64_t completed_tls_callback = 0);
    // Called with the loader mutex and State::mutex held, after callbacks end.
    void release_worker_thread_environment(ReferenceEngine& engine);
    // Called with the loader mutex held; releases all per-thread bridge state.
    void release_worker_thread_values(ReferenceEngine& engine);
    std::optional<std::uint32_t> dispatch_registry(
        const Import& imported, CpuState& state, GuestMemory& memory);
    bool dispatch_gui(const Import& imported, CpuState& state, GuestMemory& memory);
    bool dispatch_gl_extension(std::string_view name, std::uintptr_t native,
                               CpuState& state, GuestMemory& memory);
    std::uint64_t copy_gl_string(const char* text, GuestMemory& memory);
    std::uint64_t allocate_guest_bstr(GuestMemory& memory,
                                      std::wstring_view value);
    std::optional<std::wstring> read_guest_bstr(GuestMemory& memory,
                                                std::uint64_t pointer);
    bool free_guest_bstr(GuestMemory& memory, std::uint64_t pointer);
    std::uint64_t wrap_shell_automation(GuestMemory& memory,
                                         void* host, unsigned kind);
    void shell_automation_call(unsigned kind, unsigned slot,
                               CpuState& state, GuestMemory& memory);
    void ensure_guest_environment();
    std::optional<std::wstring> guest_environment_value(
        std::wstring_view name, bool crt);
    void rebuild_crt_environment(GuestMemory& memory, bool wide);
    void update_guest_environment(GuestMemory& memory, std::wstring name,
                                  std::optional<std::wstring> value, bool crt);
};

// Dispatches the small set of Win32 imports currently supported by the host.
// An empty result means that the guest should resume at its link register.
std::optional<std::uint32_t> dispatch_win32_import(const Import& imported,
                                                    CpuState& state,
                                                    GuestMemory& memory);

} // namespace transmute
