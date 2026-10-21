#include "prelude.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>

// Precompiled preludes (owner: A6). See prelude.hpp.
namespace alchemy::msl::detail {
namespace {

constexpr std::size_t minimum_prelude = 16 * 1024;   // bytes of rewritten source
constexpr std::size_t remembered_libraries = 16;
constexpr std::size_t maximum_preludes = 32;          // per configuration, on disk

std::atomic<bool>& preludes_enabled() {
    static std::atomic<bool> value{environment("ALCHEMY_MSL_PRELUDE") != "0" && environment("ALCHEMY_MSL_PCH") != "0"};
    return value;
}

// ALCHEMY_MSL_TIMING also reports prelude use.
bool trace() {
    static const bool value = !environment("ALCHEMY_MSL_TIMING").empty();
    return value;
}

using Mark = std::pair<std::size_t, std::uint64_t>;   // (prefix length, FNV-1a of the prefix)

std::string hex(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (auto index = text.size(); index-- > 0; value >>= 4) text[index] = digits[value & 15];
    return text;
}

struct Configuration {
    bool listed{};
    std::map<Mark, std::filesystem::path> ready;      // precompiled preludes
    std::set<Mark> building, refused;
    std::deque<std::vector<Mark>> recent;             // split-point marks of earlier libraries
};

struct Registry {
    std::mutex mutex;
    std::map<std::uint64_t, Configuration> configurations;
    PreludeStatistics statistics;
};

Registry& registry() {
    static Registry value;
    return value;
}

std::filesystem::path prelude_directory() { return stdlib_directory() / "preludes"; }

// p-<configuration>-<length>-<hash>.pch
std::filesystem::path prelude_path(std::uint64_t configuration, const Mark& mark) {
    return prelude_directory() /
           ("p-" + hex(configuration) + "-" + std::to_string(mark.first) + "-" + hex(mark.second) + ".pch");
}

void list_directory(std::uint64_t key, Configuration& configuration) {
    configuration.listed = true;
    std::error_code error;
    const auto prefix = "p-" + hex(key) + "-";
    for (std::filesystem::directory_iterator it(prelude_directory(), error), end; !error && it != end;
         it.increment(error)) {
        const auto name = it->path().filename().string();
        if (name.rfind(prefix, 0) != 0 || it->path().extension() != ".pch") continue;
        const auto stem = it->path().stem().string().substr(prefix.size());
        const auto dash = stem.find('-');
        if (dash == std::string::npos || stem.size() != dash + 17) continue;
        Mark mark{};
        mark.first = std::strtoull(stem.substr(0, dash).c_str(), nullptr, 10);
        mark.second = std::strtoull(stem.substr(dash + 1).c_str(), nullptr, 16);
        if (mark.first) configuration.ready[mark] = it->path();
    }
}

// Keeps the newest maximum_preludes PCHs of a configuration.
void evict(std::uint64_t key) {
    std::error_code error;
    const auto prefix = "p-" + hex(key) + "-";
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    for (std::filesystem::directory_iterator it(prelude_directory(), error), end; !error && it != end;
         it.increment(error)) {
        const auto name = it->path().filename().string();
        if (name.rfind(prefix, 0) == 0 && it->path().extension() == ".pch")
            files.emplace_back(std::filesystem::last_write_time(it->path(), error), it->path());
    }
    if (files.size() <= maximum_preludes) return;
    std::sort(files.begin(), files.end());
    for (std::size_t i = 0; i + maximum_preludes < files.size(); ++i) std::filesystem::remove(files[i].second, error);
}

} // namespace

ClangOutput compile_rewritten(const RewriteResult& rewritten, const CompileOptions& options) {
    const bool disabled = !preludes_enabled().load();
    const auto& source = rewritten.source;
    auto& reg = registry();
    if (disabled || rewritten.split_points.empty()) {
        if (trace())
            std::cerr << "alchemy-msl: " << options.label << " no prelude ("
                      << (disabled ? "disabled" : "no split points") << ")\n";
        std::lock_guard lock(reg.mutex);
        ++reg.statistics.misses;
        return compile_to_ir(source, options);
    }
    // Prefix hashes at the split points worth precompiling.
    std::vector<Mark> marks;
    std::vector<const SplitPoint*> points;
    {
        std::uint64_t hash = fnv1a(std::string_view{});
        std::size_t at = 0;
        for (const auto& split : rewritten.split_points) {
            hash = fnv1a(std::string_view(source).substr(at, split.offset - at), hash);
            at = split.offset;
            if (split.offset < minimum_prelude) continue;
            marks.emplace_back(split.offset, hash);
            points.push_back(&split);
        }
    }
    const auto key = ir_configuration_hash(options);
    std::optional<std::size_t> use;        // index into marks
    std::optional<std::size_t> build;
    {
        std::lock_guard lock(reg.mutex);
        auto& configuration = reg.configurations[key];
        if (!configuration.listed) list_directory(key, configuration);
        for (std::size_t i = marks.size(); i-- > 0;) {
            if (configuration.ready.count(marks[i])) {
                use = i;
                break;
            }
        }
        if (!use) {
            // The longest prefix shared with an earlier library.
            std::size_t best = marks.size();
            for (const auto& other : configuration.recent) {
                for (std::size_t i = marks.size(); i-- > 0;) {
                    if (best != marks.size() && i <= best) break;
                    if (std::binary_search(other.begin(), other.end(), marks[i])) {
                        best = i;
                        break;
                    }
                }
            }
            if (best != marks.size() && !configuration.building.count(marks[best]) &&
                !configuration.refused.count(marks[best])) {
                configuration.building.insert(marks[best]);
                build = best;
            }
        }
        if (!marks.empty()) configuration.recent.push_front(marks);   // sorted: offsets ascend
        if (configuration.recent.size() > remembered_libraries) configuration.recent.pop_back();
    }
    if (build) {
        const auto& mark = marks[*build];
        const auto path = prelude_path(key, mark);
        std::error_code error;
        std::filesystem::create_directories(prelude_directory(), error);
        std::string header = "#include <metal_stdlib>\n";
        header.append(source, 0, mark.first);
        header += "\n";
        header += user_code_end;
        const auto built = build_pch(header, path, options);
        std::lock_guard lock(reg.mutex);
        auto& configuration = reg.configurations[key];
        configuration.building.erase(mark);
        if (built.ok) {
            configuration.ready[mark] = path;
            ++reg.statistics.built;
            use = build;
        } else {
            configuration.refused.insert(mark);
            if (trace()) {
                std::string first = built.diagnostics.empty() ? "clang failed" : built.diagnostics.front().message;
                std::cerr << "alchemy-msl: " << options.label << " prelude of " << mark.first
                          << " bytes not precompiled: " << first << '\n';
            }
        }
        if (trace() && built.ok)
            std::cerr << "alchemy-msl: " << options.label << " precompiled a prelude of " << mark.first << " bytes\n";
        if (built.ok) evict(key);
    }
    if (use) {
        const auto& mark = marks[*use];
        const auto& split = *points[*use];
        std::filesystem::path pch;
        {
            std::lock_guard lock(reg.mutex);
            const auto& ready = reg.configurations[key].ready;
            if (const auto found = ready.find(mark); found != ready.end()) pch = found->second;
        }
        if (!pch.empty()) {
            std::string rest = user_code_begin;
            rest += "# " + std::to_string(split.line) + " \"" + split.file + "\"\n";
            rest.append(source, mark.first, std::string::npos);
            auto result = compile_to_ir(rest, options, pch);
            std::lock_guard lock(reg.mutex);
            if (!result.pch_rejected) {
                ++reg.statistics.hits;
                if (trace()) std::cerr << "alchemy-msl: " << options.label << " prelude " << mark.first << " bytes\n";
                return result;
            }
            auto& configuration = reg.configurations[key];
            configuration.ready.erase(mark);
            configuration.refused.insert(mark);
            ++reg.statistics.rejected;
        }
    }
    {
        std::lock_guard lock(reg.mutex);
        ++reg.statistics.misses;
    }
    if (trace())
        std::cerr << "alchemy-msl: " << options.label << " no prelude (" << rewritten.split_points.size()
                  << " split points, " << marks.size() << " past " << minimum_prelude << " bytes)\n";
    return compile_to_ir(source, options);
}

PreludeStatistics prelude_statistics() {
    auto& reg = registry();
    std::lock_guard lock(reg.mutex);
    return reg.statistics;
}

void reset_preludes() {
    auto& reg = registry();
    std::lock_guard lock(reg.mutex);
    reg.configurations.clear();
}

void set_preludes_enabled(bool enabled) { preludes_enabled() = enabled; }

} // namespace alchemy::msl::detail
