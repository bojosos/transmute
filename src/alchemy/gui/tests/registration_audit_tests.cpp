// Registration audit (alchemy/registration_audit.hpp): a name registered more
// than once is reported with every owner in registration order, and the full
// provider set (run_program's installers) stays at or under the ratchets
// below. The provider refactor (docs/ALCHEMY_BRIEF_REFACTOR.md) lowers them as
// it removes duplicates: lower them in the commit that removes some. The
// target is 0 for both.
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/registration_audit.hpp"
#include "alchemy/registration_inventory.hpp"

#include <algorithm>
#include <sstream>
#include <string>

namespace {
using namespace fill_test;
namespace audit = alchemy::registration_audit;

// Names registered more than once (export, method, class, alias), and
// fill-only registrations skipped because another owner provides the name.
constexpr std::size_t max_duplicates = 0;
constexpr std::size_t max_fill_skipped = 0;

const audit::Entry* find(const std::vector<audit::Entry>& entries, std::string_view kind, std::string_view name) {
    const auto found = std::find_if(entries.begin(), entries.end(),
        [&](const audit::Entry& entry) { return entry.kind == kind && entry.name == name; });
    return found == entries.end() ? nullptr : &*found;
}

void census(Fixture& f) {
    const auto report = audit::build(&f.process);
    audit::write_text(report, std::cout);
    std::cout << "duplicates " << report.duplicates.size() << " (ratchet " << max_duplicates << "), fill skipped "
              << report.fill_skipped.size() << " (ratchet " << max_fill_skipped << ")\n";
    require(report.exports > 1000 && report.methods > 1000 && report.classes > 100, "the audit sees the installers");
    require(report.duplicates.size() <= max_duplicates, "more duplicate registrations than the ratchet allows");
    require(report.fill_skipped.size() <= max_fill_skipped, "more skipped fill-only registrations than the ratchet allows");
}

void synthetic(Fixture& f) {
    audit::reset();
    constexpr std::string_view libz = "/usr/lib/libz.1.dylib";
    {
        audit::Scope scope("first_installer");
        f.process.install_export(std::string(libz), "_alchemy_audit_probe", {0x1000, alchemy::ExportKind::data});
    }
    {
        audit::Scope scope("second_installer");
        f.process.install_export(std::string(libz), "_alchemy_audit_probe", {0x2000, alchemy::ExportKind::data});
    }
    f.process.install_export("/usr/lib/system/libsystem_c.dylib", "_alchemy_audit_alias", {0x3000, alchemy::ExportKind::data});
    {
        audit::Scope scope("extending_installer");
        audit::record_extension(std::string(libz) + " _alchemy_audit_probe", "probe objects", std::source_location::current());
    }
    const auto report = audit::build();
    const auto* twice = find(report.duplicates, "export", std::string(libz) + " _alchemy_audit_probe");
    require(twice && twice->owners.size() == 2, "a name registered twice is listed with both owners");
    require(twice->owners[0].starts_with("first_installer gui/tests/registration_audit_tests.cpp:") &&
                twice->owners[1].starts_with("second_installer gui/tests/registration_audit_tests.cpp:"),
        "owners are the installer scope and source line, in registration order: " + twice->owners[0]);
    require(find(report.duplicates, "alias", "/usr/lib/libSystem.B.dylib _alchemy_audit_alias"),
        "a libSystem alias registration is listed (resolve never finds it)");
    require(report.duplicates.size() == 2, "nothing else is listed after reset (an extension is not a duplicate)");
    require(report.extensions.size() == 1 && report.extensions[0].owners.size() == 1 &&
                report.extensions[0].owners[0].starts_with("probe objects: extending_installer gui/tests/registration_audit_tests.cpp:"),
        "an extension is listed with its domain and owner");
    std::ostringstream inventory;
    audit::write_inventory(inventory);
    const auto text = inventory.str();
    const auto row = "export\t" + std::string(libz) + " _alchemy_audit_probe\tfirst_installer gui/tests/registration_audit_tests.cpp:";
    const auto at = text.find(row);
    require(at != std::string::npos && text.find("\tsecond_installer gui/tests/registration_audit_tests.cpp:", at) != std::string::npos,
        "the inventory lists a name with every owner in registration order");
    require(std::count(text.begin(), text.end(), '\n') == 2, "the inventory has one line per registered name");
}

} // namespace

int main() {
    audit::set_enabled(true);
    return run("registration audit", {{"census", census}, {"synthetic", synthetic}});
}
