#pragma once

#include "alchemy/gui/install.hpp"

// AddressBook's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::addressbook {

// install.cpp (fill-only, through gui::values::Provider).
void install_addressbook(gui::InstallContext& context);

} // namespace alchemy::addressbook
