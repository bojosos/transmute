// Null host backend, area "ui": the GUI run loop's waits and the dialog-loop
// hook. Every call fails with Error::unsupported (include/alchemy/host/host.hpp).
#include "alchemy/host/ui.hpp"

namespace alchemy::host::ui {

std::size_t wait_limit() noexcept { return 64; }
Result<std::size_t> wait_for_input(std::span<const Handle>, std::uint32_t, bool) { return Error::unsupported; }
bool is_gui_thread() noexcept { return false; }
bool in_modal_loop() noexcept { return false; }
Status install_dialog_loop_hook(std::function<void()>, std::uint32_t) { return Error::unsupported; }
void enable_dialog_loop_ticks(bool) noexcept {}
void remove_dialog_loop_hook() noexcept {}

} // namespace alchemy::host::ui
