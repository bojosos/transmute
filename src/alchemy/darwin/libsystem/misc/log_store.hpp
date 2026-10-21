#pragma once

// The process's os_log entries (recorded by kit/completion/oslog.cpp, read by
// OSLogStore in oslog_store.cpp) and the composition of an os_log message
// from its format string and argument buffer (host-only core, unit tested).

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::completion_kit::logs {

struct Entry {
    double date{};                 // absolute time (seconds since 2001)
    std::uint8_t type{};           // os_log_type_t: 0 default, 1 info, 2 debug, 16 error, 17 fault
    std::string subsystem, category, message, format, sender;
    std::uint64_t thread{};
};

void record(Entry entry);
[[nodiscard]] std::vector<Entry> entries();

// Composes a message: `buffer` is os_log's argument buffer (a summary byte,
// a count byte, then per argument a descriptor byte, a size byte and the
// data). Pointer arguments are resolved through `c_string` (%s) and
// `describe` (%@); a private argument without data prints "<private>".
// Supported conversions: d i u o x X c p s @ f F e E g G a A %, with the
// h hh l ll q j z t L length modifiers, flags, width and precision, and
// os_log's {public}/{private}/{...} annotations.
[[nodiscard]] std::string compose(std::string_view format, std::span<const std::uint8_t> buffer,
                                  const std::function<std::string(std::uint64_t)>& c_string,
                                  const std::function<std::string(std::uint64_t)>& describe);

} // namespace alchemy::gui::completion_kit::logs
