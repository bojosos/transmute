#include "alchemy/darwin/foundation/headless_shells.hpp"

// alchemy-fx (darwin/foundation/headless_shells.hpp): cf_locale.cpp registers
// CFDateFormatterCreate and CFDateFormatterGetFormat.
namespace alchemy::foundation_shells {

void install_cf_date_formatter_shell(ProcessContext&) {}

} // namespace alchemy::foundation_shells
