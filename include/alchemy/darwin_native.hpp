#pragma once

#include <string_view>

namespace alchemy {
class ProcessContext;

void install_darwin_native(ProcessContext& context);
[[nodiscard]] std::string_view deferred_native_api(std::string_view symbol);
} // namespace alchemy
