#pragma once

// The registration audit's inventory (alchemy/registration_audit.hpp): every
// export, Objective-C method and class registered so far, with all its owners.
// ALCHEMY_REGISTRATION_AUDIT=<path>.tsv writes it instead of the report, so
//   ALCHEMY_REGISTRATION_AUDIT=before.tsv alchemy-fx doctor <any Mach-O>
// before and after a change, and a diff of the two files, shows which names
// the change added, removed or moved between owners. Kept out of
// registration_audit.hpp, which most of the providers include.

#include <iosfwd>

namespace alchemy::registration_audit {

// One line per name, sorted by kind, then name:
// "<kind>\t<name>\t<owner>[\t<owner>...]" (kind class, export or method;
// owners as in the report, in registration order).
void write_inventory(std::ostream& out);

} // namespace alchemy::registration_audit
