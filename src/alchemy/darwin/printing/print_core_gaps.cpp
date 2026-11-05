#include "alchemy/gui/value_provider.hpp"

// The values provider's PrintCore entry (darwin/coreservices/services.cpp's
// install_services calls it). PrintCore is print_core.cpp's now (both builds,
// installed first), the only owner of the PM names, so nothing is added here.
namespace alchemy::gui::values {

void Provider::install_print_core() {}

} // namespace alchemy::gui::values
