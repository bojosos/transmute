#include "win32_native_imports.hpp"

#define NOMINMAX
#include <Windows.h>
#include <gl/GL.h>

#include <bit>
#include <cstdint>
#include <string_view>

namespace transmute {
namespace {

constexpr char lower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool equal_dll(std::string_view value, std::string_view expected) noexcept {
    if (value.size() != expected.size()) return false;
    for (std::size_t i = 0; i != value.size(); ++i)
        if (lower(value[i]) != expected[i]) return false;
    return true;
}

bool eligible_dll(std::string_view dll) noexcept {
    if (equal_dll(dll, "kernel32.dll") || equal_dll(dll, "kernelbase.dll")) return true;
    constexpr std::string_view prefix = "api-ms-win-core-";
    if (dll.size() <= prefix.size() + 4 ||
        !equal_dll(dll.substr(dll.size() - 4), ".dll")) return false;
    return equal_dll(dll.substr(0, prefix.size()), prefix);
}

template<auto Function>
void no_argument(CpuState& state) noexcept {
    state.x[0] = static_cast<std::uint64_t>(Function());
}

void set_error_mode(CpuState& state) noexcept {
    state.x[0] = SetErrorMode(static_cast<UINT>(state.x[0]));
}

template<auto Function>
void opaque_pointer(CpuState& state) noexcept {
    // EncodePointer/DecodePointer transform a bit pattern without dereferencing
    // it. This permits arbitrary guest pointer values without a memory bypass.
    const auto pointer = reinterpret_cast<void*>(static_cast<std::uintptr_t>(state.x[0]));
    state.x[0] = reinterpret_cast<std::uintptr_t>(Function(pointer));
}

template<auto Function>
void key_state(CpuState& state) noexcept {
    state.x[0] = static_cast<std::uint16_t>(Function(static_cast<int>(state.x[0])));
}

template<auto Function>
void map_key(CpuState& state) noexcept {
    state.x[0] = Function(static_cast<UINT>(state.x[0]), static_cast<UINT>(state.x[1]));
}

void system_metrics(CpuState& state) noexcept {
    state.x[0] = static_cast<std::uint32_t>(GetSystemMetrics(static_cast<int>(state.x[0])));
}

void clear(CpuState& state) noexcept { ::glClear(static_cast<GLbitfield>(state.x[0])); }
void clear_color(CpuState& state) noexcept {
    ::glClearColor(std::bit_cast<float>(static_cast<std::uint32_t>(state.v[0][0])),
        std::bit_cast<float>(static_cast<std::uint32_t>(state.v[1][0])),
        std::bit_cast<float>(static_cast<std::uint32_t>(state.v[2][0])),
        std::bit_cast<float>(static_cast<std::uint32_t>(state.v[3][0])));
}
void viewport(CpuState& state) noexcept {
    ::glViewport(static_cast<GLint>(state.x[0]), static_cast<GLint>(state.x[1]),
        static_cast<GLsizei>(state.x[2]), static_cast<GLsizei>(state.x[3]));
}
void flush(CpuState&) noexcept { ::glFlush(); }
void finish(CpuState&) noexcept { ::glFinish(); }

} // namespace

NativeImportCall resolve_direct_win32_import(const Import& imported) noexcept {
    if (equal_dll(imported.dll, "user32.dll")) {
        if (imported.function == "GetKeyState") return key_state<GetKeyState>;
        if (imported.function == "GetAsyncKeyState") return key_state<GetAsyncKeyState>;
        if (imported.function == "MapVirtualKeyA") return map_key<MapVirtualKeyA>;
        if (imported.function == "MapVirtualKeyW") return map_key<MapVirtualKeyW>;
        if (imported.function == "GetSystemMetrics") return system_metrics;
        return nullptr;
    }
    if (equal_dll(imported.dll, "opengl32.dll")) {
        if (imported.function == "glClear") return clear;
        if (imported.function == "glClearColor") return clear_color;
        if (imported.function == "glViewport") return viewport;
        if (imported.function == "glGetError") return no_argument<glGetError>;
        if (imported.function == "glFlush") return flush;
        if (imported.function == "glFinish") return finish;
        return nullptr;
    }
    if (!eligible_dll(imported.dll)) return nullptr;
    const auto& name = imported.function;
    if (name == "GetCurrentThreadId") return no_argument<GetCurrentThreadId>;
    if (name == "GetCurrentProcessId") return no_argument<GetCurrentProcessId>;
    if (name == "GetTickCount") return no_argument<GetTickCount>;
    if (name == "GetTickCount64") return no_argument<GetTickCount64>;
    if (name == "IsDebuggerPresent") return no_argument<IsDebuggerPresent>;
    if (name == "GetErrorMode") return no_argument<GetErrorMode>;
    if (name == "SetErrorMode") return set_error_mode;
    if (name == "GetACP") return no_argument<GetACP>;
    if (name == "GetOEMCP") return no_argument<GetOEMCP>;
    if (name == "EncodePointer") return opaque_pointer<EncodePointer>;
    if (name == "DecodePointer") return opaque_pointer<DecodePointer>;
    return nullptr;
}

} // namespace transmute
