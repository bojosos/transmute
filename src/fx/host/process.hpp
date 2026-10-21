#pragma once

// Host area "process": this process and helper processes
// (docs/FX_HOST_SEAMS.md, "process").

#include "fx/host/host.hpp"

#include <cstdint>
#include <string>

namespace transmute::fx::host {

std::uint32_t process_id() noexcept;
// Milliseconds since this process was created (the precise wall clock).
double process_age_ms() noexcept;
// This process's executable (empty if it cannot be read).
std::wstring executable_path();

// Process scheduling class (Windows power throttling; no-ops elsewhere):
// true if a parent or launcher already set an explicit execution-speed state.
bool process_qos_explicit() noexcept;
// `eco`: EcoQoS (efficiency cores), otherwise HighQoS.
void set_process_qos(bool eco) noexcept;
// Idle CPU priority for the whole process (background helpers).
void set_process_idle_priority() noexcept;

// True once the process is exiting (DLL detach of the whole process).
bool process_shutting_down() noexcept;

// Ends the process at once with `exit_code`: no exit handlers, no thread
// detach (TerminateProcess; _exit on Linux).
void terminate_process(std::uint32_t exit_code) noexcept;

// Starts `program` with `command_line` (the whole line, program included) as
// a detached background helper: no console, no inherited handles, idle
// priority, outside the caller's job when that allows it. `on_exit(context)`
// runs on a host thread-pool thread once the helper exits. False if it could
// not be started.
bool spawn_background_process(const std::wstring& program, const std::wstring& command_line,
                              void (*on_exit)(void* context), void* context);

} // namespace transmute::fx::host
