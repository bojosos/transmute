#pragma once

// Registration audit: who registers each provider export, Objective-C class
// and native method, and which names are registered more than once.
//
// It is the to-do list of the provider refactor (docs/ALCHEMY_BRIEF_REFACTOR.md):
// each export and each selector should have exactly one owner, so install
// order stops mattering. Off by default; when off, every hook is one relaxed
// atomic load.
//
//   ALCHEMY_REGISTRATION_AUDIT=1        report (text) on stderr once the
//                                       providers are installed
//   ALCHEMY_REGISTRATION_AUDIT=<path>   write the report to <path> instead
//                                       (JSON when it ends in .json)
//
// `alchemy-fx doctor` installs every provider without running a guest, so
// `ALCHEMY_REGISTRATION_AUDIT=1 alchemy-fx doctor <any Mach-O>` is the
// cheapest way to get the report.
//
// What is recorded:
// - export: ProcessContext::install_export / install_function (insert-or-
//   assign). A second registration of the same (library, symbol) replaces the
//   first; the report lists every owner in order, the last one wins. The
//   libSystem aliases (/usr/lib/system/...) count as libSystem, as resolve()
//   does; an export registered under an alias is listed as "alias" because
//   resolve() never finds it there. Re-exporting the same data address (a
//   class symbol published again by a framework) is listed as "republish":
//   harmless, but the symbol still has two owners.
// - method: ObjcRuntime::add_native_method for a (class, selector, +/-) that
//   already has a native method.
// - class: ObjcRuntime::register_native_class (duplicates already throw).
// - fill skipped: a fill-only helper (gui::fill::Provider, export_fill::Provider)
//   that did not register a name because another owner already provides it.
//   Each one is either dead code or a second implementation.
// - fill: names that a fill-only helper did register. Fill-only is meant to
//   stay only for clearly marked stubs; this is the list of what is left.
// - runtime: an export that shadows DarwinRuntime's base table (the
//   ProcessContext::base_resolver at report time).
// - extension: a provider layering behaviour over another framework's export
//   through that framework's extension point (CoreFoundation::extend), for
//   the objects of one domain. Listed with each domain and owner, but not a
//   duplicate: the export keeps one owner.
//
// An owner is "<installer scope> <file>:<line>". install_darwin_gui names
// each installer (Scope); registrations outside a scope are "base". The file
// and line are those of the outermost audited call: a helper that registers
// on behalf of its caller opens a Site with the caller's location, so the
// owner is the provider's source line, not the helper's.

#include <cstdint>
#include <iosfwd>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy {
class ProcessContext;
}

namespace alchemy::registration_audit {

[[nodiscard]] bool enabled() noexcept;
// Tests turn the audit on or off programmatically (before installing).
void set_enabled(bool value) noexcept;
// Forgets everything recorded so far (tests).
void reset();

// The installer currently running (nested scopes: the innermost wins).
class Scope {
public:
    explicit Scope(std::string_view installer) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* previous_;
    bool active_;
};

// The source location registrations are attributed to while it is open
// (the outermost Site wins, so nested helpers keep the provider's line).
class Site {
public:
    explicit Site(const std::source_location& where = std::source_location::current()) noexcept;
    ~Site();
    Site(const Site&) = delete;
    Site& operator=(const Site&) = delete;

private:
    bool active_;
};

// `alias`: registered under a libSystem alias (/usr/lib/system/...), which
// resolve() never looks up, so the export is unreachable. `address`: the
// exported data address (0 for functions, whose addresses are always new).
void record_export(std::string_view library, std::string_view symbol, const std::source_location& where,
                   bool alias = false, std::uint64_t address = 0);
void record_class(std::string_view name, const std::source_location& where);
void record_method(std::string_view cls, std::string_view selector, bool class_method, const std::source_location& where);
// A fill-only helper registered `symbol` (fill) or left it to the existing
// owner (skipped).
void record_fill(std::string_view library, std::string_view symbol, bool skipped, const std::source_location& where);
// An extension of `name` ("<library> <symbol>" or a method) for `domain`.
void record_extension(std::string_view name, std::string_view domain, const std::source_location& where);

struct Entry {
    std::string kind;   // "export", "republish", "method", "class", "alias", "runtime", "fill-skipped"
    std::string name;   // "<library> <symbol>", "-[Class selector]", "Class"
    std::vector<std::string> owners;  // registration order; the last one wins
};
struct Report {
    std::uint64_t exports{}, methods{}, classes{}, fills{};
    std::vector<Entry> duplicates;     // export/method/class/runtime, sorted
    std::vector<Entry> fill_skipped;   // sorted
    std::vector<Entry> fill_only;      // one entry per owner file: name = file, owners = symbols
    std::vector<Entry> extensions;     // one entry per extended name: owners = "<domain>: <owner>"
};
// Builds the report. With `process`, exports that DarwinRuntime's base table
// (process->base_resolver) also resolves are listed as "runtime" duplicates.
[[nodiscard]] Report build(const ProcessContext* process = nullptr);
void write_text(const Report& report, std::ostream& out);
void write_json(const Report& report, std::ostream& out);
// Writes the report where ALCHEMY_REGISTRATION_AUDIT says, when enabled.
// run_program and doctor call it once every provider is installed.
void report(const ProcessContext& process);

} // namespace alchemy::registration_audit
