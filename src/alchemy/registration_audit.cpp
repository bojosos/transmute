#include "alchemy/registration_audit.hpp"
#include "alchemy/registration_inventory.hpp"

#include "alchemy/process_context.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <ostream>
#include <string>

namespace alchemy::registration_audit {
namespace {

// ALCHEMY_REGISTRATION_AUDIT, read once; empty when off.
const std::string& setting() {
    static const std::string value = [] {
        std::string text;
#ifdef _WIN32
        char* buffer{};
        std::size_t length{};
        if (_dupenv_s(&buffer, &length, "ALCHEMY_REGISTRATION_AUDIT") == 0 && buffer) {
            text = buffer;
            std::free(buffer);
        }
#else
        if (const char* buffer = std::getenv("ALCHEMY_REGISTRATION_AUDIT")) text = buffer;
#endif
        return text == "0" ? std::string() : text;
    }();
    return value;
}

std::atomic<bool> on{!setting().empty()};

thread_local const char* current_scope = nullptr;
thread_local const std::source_location* current_site = nullptr;
thread_local std::source_location site_storage;

struct State {
    std::mutex mutex;
    std::map<std::string, std::vector<std::string>> exports, methods, classes, aliases, skipped;
    std::map<std::string, std::vector<std::uint64_t>> addresses;  // export key -> data address per registration
    std::map<std::string, std::vector<std::string>> fill_only;   // owner file -> symbols
    std::map<std::string, std::vector<std::string>> extensions;  // extended name -> "<domain>: <owner>"
    std::map<std::string, std::pair<std::string, std::string>> export_names;  // key -> (library, symbol)
    std::uint64_t export_count{}, method_count{}, class_count{}, fill_count{};
};
State& state() {
    static State value;
    return value;
}

// "src/alchemy/darwin/appkit/window.cpp" -> "darwin/appkit/window.cpp"; include/... kept.
std::string short_file(const char* file) {
    std::string path = file ? file : "?";
    std::replace(path.begin(), path.end(), '\\', '/');
    for (const std::string_view marker : {"/src/alchemy/", "/src/", "/include/"}) {
        if (const auto at = path.rfind(marker); at != std::string::npos)
            return path.substr(at + (marker == "/include/" ? 1 : marker.size()));
    }
    return path;
}

const std::source_location& site(const std::source_location& where) {
    return current_site ? *current_site : where;
}

std::string owner(const std::source_location& where) {
    const auto& at = site(where);
    return std::string(current_scope ? current_scope : "base") + " " + short_file(at.file_name()) + ":" +
        std::to_string(at.line());
}

void json_string(std::ostream& out, std::string_view text) {
    out << '"';
    for (const char c : text) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (static_cast<unsigned char>(c) < 0x20) out << ' ';
        else out << c;
    }
    out << '"';
}

void json_entries(std::ostream& out, const std::vector<Entry>& entries, std::string_view owners_key) {
    out << '[';
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        out << (i ? ",\n  " : "\n  ") << "{\"kind\":";
        json_string(out, entry.kind);
        out << ",\"name\":";
        json_string(out, entry.name);
        out << ",\"" << owners_key << "\":[";
        for (std::size_t j = 0; j < entry.owners.size(); ++j) {
            if (j) out << ',';
            json_string(out, entry.owners[j]);
        }
        out << "]}";
    }
    out << (entries.empty() ? "]" : "\n]");
}

} // namespace

bool enabled() noexcept { return on.load(std::memory_order_relaxed); }
void set_enabled(bool value) noexcept { on.store(value, std::memory_order_relaxed); }

void reset() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.exports.clear(); s.methods.clear(); s.classes.clear(); s.aliases.clear(); s.skipped.clear();
    s.fill_only.clear(); s.export_names.clear(); s.addresses.clear(); s.extensions.clear();
    s.export_count = s.method_count = s.class_count = s.fill_count = 0;
}

Scope::Scope(std::string_view installer) noexcept : previous_(current_scope), active_(enabled()) {
    // Installer names are string literals (install_darwin_gui's table).
    if (active_) current_scope = installer.data();
}
Scope::~Scope() {
    if (active_) current_scope = previous_;
}

Site::Site(const std::source_location& where) noexcept : active_(enabled() && !current_site) {
    if (active_) {
        site_storage = where;
        current_site = &site_storage;
    }
}
Site::~Site() {
    if (active_) current_site = nullptr;
}

void record_export(std::string_view library, std::string_view symbol, const std::source_location& where, bool alias,
                   std::uint64_t address) {
    if (!enabled()) return;
    // Objective-C native methods are installed as hidden functions; they are
    // audited per selector (record_method).
    if (symbol.starts_with("___alchemy_objc_method_")) return;
    auto& s = state();
    std::string key(library);
    key.append(1, ' ').append(symbol);
    auto who = owner(where);
    std::lock_guard lock(s.mutex);
    ++s.export_count;
    if (alias) s.aliases[key].push_back(who);
    s.export_names.try_emplace(key, std::string(library), std::string(symbol));
    s.addresses[key].push_back(address);
    s.exports[std::move(key)].push_back(std::move(who));
}

void record_class(std::string_view name, const std::source_location& where) {
    if (!enabled()) return;
    auto& s = state();
    auto who = owner(where);
    std::lock_guard lock(s.mutex);
    ++s.class_count;
    s.classes[std::string(name)].push_back(std::move(who));
}

void record_method(std::string_view cls, std::string_view selector, bool class_method, const std::source_location& where) {
    if (!enabled()) return;
    auto& s = state();
    std::string key = class_method ? "+[" : "-[";
    key.append(cls).append(1, ' ').append(selector).append(1, ']');
    auto who = owner(where);
    std::lock_guard lock(s.mutex);
    ++s.method_count;
    s.methods[std::move(key)].push_back(std::move(who));
}

void record_fill(std::string_view library, std::string_view symbol, bool skipped, const std::source_location& where) {
    if (!enabled()) return;
    auto& s = state();
    std::string key(library);
    key.append(1, ' ').append(symbol);
    auto who = owner(where);
    std::lock_guard lock(s.mutex);
    if (skipped) {
        s.skipped[std::move(key)].push_back(std::move(who));
        return;
    }
    ++s.fill_count;
    const auto& at = site(where);
    s.fill_only[short_file(at.file_name())].emplace_back(symbol);
}

void record_extension(std::string_view name, std::string_view domain, const std::source_location& where) {
    if (!enabled()) return;
    auto& s = state();
    auto who = std::string(domain) + ": " + owner(where);
    std::lock_guard lock(s.mutex);
    s.extensions[std::string(name)].push_back(std::move(who));
}

Report build(const ProcessContext* process) {
    auto& s = state();
    Report report;
    std::lock_guard lock(s.mutex);
    report.exports = s.export_count;
    report.methods = s.method_count;
    report.classes = s.class_count;
    report.fills = s.fill_count;
    const auto duplicates = [&](std::string_view kind, const auto& table) {
        for (const auto& [name, owners] : table)
            if (owners.size() > 1) report.duplicates.push_back({std::string(kind), name, owners});
    };
    for (const auto& [name, owners] : s.exports) {
        if (owners.size() < 2) continue;
        const auto& seen = s.addresses.at(name);
        const bool same = seen.front() && std::all_of(seen.begin(), seen.end(), [&](auto value) { return value == seen.front(); });
        report.duplicates.push_back({same ? "republish" : "export", name, owners});
    }
    duplicates("method", s.methods);
    duplicates("class", s.classes);
    for (const auto& [name, owners] : s.aliases)
        report.duplicates.push_back({"alias", name, owners});
    if (process && process->base_resolver) {
        for (const auto& [key, names] : s.export_names) {
            if (!process->base_resolver(names.first, names.second)) continue;
            auto owners = s.exports.at(key);
            owners.insert(owners.begin(), "DarwinRuntime base table");
            report.duplicates.push_back({"runtime", key, std::move(owners)});
        }
    }
    for (const auto& [name, owners] : s.skipped) {
        Entry entry{"fill-skipped", name, owners};
        if (const auto found = s.exports.find(name); found != s.exports.end())
            entry.owners.insert(entry.owners.begin(), "provided by " + found->second.back());
        else
            entry.owners.insert(entry.owners.begin(), "provided by DarwinRuntime base table");
        report.fill_skipped.push_back(std::move(entry));
    }
    for (const auto& [file, symbols] : s.fill_only)
        report.fill_only.push_back({"fill", file, symbols});
    for (const auto& [name, owners] : s.extensions)
        report.extensions.push_back({"extension", name, owners});
    std::sort(report.duplicates.begin(), report.duplicates.end(),
        [](const Entry& a, const Entry& b) { return a.kind != b.kind ? a.kind < b.kind : a.name < b.name; });
    return report;
}

void write_text(const Report& report, std::ostream& out) {
    out << "alchemy registration audit: " << report.exports << " export, " << report.methods << " method and "
        << report.classes << " class registrations; " << report.duplicates.size()
        << " names registered more than once; " << report.fill_skipped.size()
        << " fill-only registrations skipped; " << report.fills << " fill-only registrations in "
        << report.fill_only.size() << " files\n";
    std::map<std::string, std::size_t> kinds;
    for (const auto& entry : report.duplicates) ++kinds[entry.kind];
    out << "duplicates by kind:";
    for (const auto& [kind, count] : kinds) out << ' ' << kind << ' ' << count;
    out << '\n';
    for (const auto& entry : report.duplicates) {
        out << "duplicate " << entry.kind << ' ' << entry.name << '\n';
        for (const auto& who : entry.owners) out << "    " << who << '\n';
    }
    for (const auto& entry : report.fill_skipped) {
        out << "fill-skipped " << entry.name << '\n';
        for (const auto& who : entry.owners) out << "    " << who << '\n';
    }
    for (const auto& entry : report.fill_only)
        out << "fill-only " << entry.name << ": " << entry.owners.size() << " names\n";
    for (const auto& entry : report.extensions) {
        out << "extension " << entry.name << '\n';
        for (const auto& who : entry.owners) out << "    " << who << '\n';
    }
}

void write_json(const Report& report, std::ostream& out) {
    out << "{\"schema\":1,\"exports\":" << report.exports << ",\"methods\":" << report.methods
        << ",\"classes\":" << report.classes << ",\"fills\":" << report.fills << ",\n\"duplicates\":";
    json_entries(out, report.duplicates, "owners");
    out << ",\n\"fill_skipped\":";
    json_entries(out, report.fill_skipped, "owners");
    out << ",\n\"fill_only\":";
    json_entries(out, report.fill_only, "symbols");
    out << ",\n\"extensions\":";
    json_entries(out, report.extensions, "owners");
    out << "}\n";
}

void write_inventory(std::ostream& out) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto rows = [&](std::string_view kind, const auto& table) {
        for (const auto& [name, owners] : table) {
            out << kind << '\t' << name;
            for (const auto& who : owners) out << '\t' << who;
            out << '\n';
        }
    };
    rows("class", s.classes);
    rows("export", s.exports);
    rows("method", s.methods);
}

void report(const ProcessContext& process) {
    // Only when ALCHEMY_REGISTRATION_AUDIT asks for a report; a test that turns
    // the audit on with set_enabled() reads it through build() instead.
    if (!enabled() || setting().empty()) return;
    const auto result = build(&process);
    const auto& target = setting();
    if (target.empty() || target == "1") {
        write_text(result, std::cerr);
        std::cerr.flush();
        return;
    }
    const std::string path = target;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::cerr << "alchemy: registration audit: cannot write " << path << std::endl;
        return;
    }
    if (path.ends_with(".tsv")) write_inventory(file);
    else if (path.ends_with(".json")) write_json(result, file);
    else write_text(result, file);
}

} // namespace alchemy::registration_audit
