#pragma once

#include "alchemy/darwin/media/hal_core.hpp"

#include <memory>

namespace alchemy::gui::completion_kit::media::hal {

// The host's audio endpoints (host::audio).
std::unique_ptr<Backend> make_host_backend();

} // namespace alchemy::gui::completion_kit::media::hal
