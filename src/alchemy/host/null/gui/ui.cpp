// Null host backend, area "ui": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/ui.hpp"

namespace alchemy::host::ui {

Result<std::vector<Monitor>> monitors() { return Error::unsupported; }
Result<std::uint32_t> idle_ms() { return Error::unsupported; }
Result<KeyboardKind> keyboard_kind() { return Error::unsupported; }
Result<bool> reduce_motion() { return Error::unsupported; }
Result<bool> high_contrast() { return Error::unsupported; }
Result<bool> screen_reader() { return Error::unsupported; }
Result<bool> dark_appearance() { return Error::unsupported; }
Result<std::array<std::uint8_t, 3>> pixel_under_pointer() { return Error::unsupported; }
Status shell_open(std::string_view, std::string_view, std::string_view) { return Error::unsupported; }
Status reveal_file(std::string_view) { return Error::unsupported; }
Status reveal_files(std::span<const std::string>) { return Error::unsupported; }
void add_recent_document(std::string_view) {}
Result<std::string> desktop_picture() { return Error::unsupported; }
Status set_desktop_picture(std::string_view) { return Error::unsupported; }

Result<FrameTimer> FrameTimer::create() { return Error::unsupported; }
FrameTimer::FrameTimer() = default;
FrameTimer::FrameTimer(FrameTimer&&) noexcept = default;
FrameTimer& FrameTimer::operator=(FrameTimer&&) noexcept = default;
FrameTimer::~FrameTimer() = default;
Status FrameTimer::wait(std::uint64_t, std::uint32_t) { return Error::unsupported; }
Result<std::vector<StackFrame>> sample_thread_stack(std::uint64_t, std::size_t) { return Error::unsupported; }
std::unique_ptr<WindowSystem> create_window_server() { return nullptr; }   // no windows
Result<PixelSize> window_client_size(std::uintptr_t) { return Error::unsupported; }
Result<AlertAnswer> show_alert(const AlertDialog&) { return Error::unsupported; }
Result<std::optional<std::vector<std::string>>> run_file_dialog(const FileDialog&) { return Error::unsupported; }
std::vector<std::uintptr_t> disable_windows(std::span<const std::uintptr_t>, std::uintptr_t) { return {}; }
void enable_windows(std::span<const std::uintptr_t>) {}
Result<std::vector<std::string>> spelling_languages() { return Error::unsupported; }
Result<std::vector<Misspelling>> check_spelling(std::string_view, std::u16string_view) { return Error::unsupported; }
Result<std::vector<std::u16string>> spelling_suggestions(std::string_view, std::u16string_view) { return Error::unsupported; }

} // namespace alchemy::host::ui
