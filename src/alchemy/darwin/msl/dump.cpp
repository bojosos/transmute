#include "alchemy/msl/compiler.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <system_error>

// ALCHEMY_MSL_DUMP: the shader corpus hook. Metal (A5) calls dump_source for
// every newLibraryWithSource: before compiling; the MSL compiler's corpus
// tests (A6) are built from the dumped files.
namespace alchemy::msl {
namespace {

std::filesystem::path dump_directory() {
    static const std::filesystem::path directory = [] {
#ifdef _MSC_VER
        wchar_t* buffer = nullptr;
        std::size_t length = 0;
        std::filesystem::path value;
        if (_wdupenv_s(&buffer, &length, L"ALCHEMY_MSL_DUMP") == 0 && buffer) {
            value = buffer;
            std::free(buffer);
        }
        return value;
#else
        const char* value = std::getenv("ALCHEMY_MSL_DUMP");
        return value ? std::filesystem::path(value) : std::filesystem::path{};
#endif
    }();
    return directory;
}

// 64-bit FNV-1a: stable across runs and builds, so file names dedupe a corpus.
std::uint64_t fingerprint(std::string_view source) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto character : source) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

std::string safe_label(std::string_view label) {
    std::string result;
    for (const auto character : label.substr(0, 48)) {
        const bool plain = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') || character == '_' || character == '-';
        result += plain ? character : '_';
    }
    return result.empty() ? "library" : result;
}

} // namespace

std::optional<std::string> dump_library_data(std::string_view bytes) {
    const auto directory = dump_directory();
    if (directory.empty()) return std::nullopt;
    static std::mutex mutex;
    static bool reported{};
    std::lock_guard lock(mutex);
    try {
        static constexpr char digits[] = "0123456789abcdef";
        std::string hash(16, '0');
        auto value = fingerprint(bytes);
        for (auto index = hash.size(); index-- > 0; value >>= 4) hash[index] = digits[value & 15];
        const auto path = directory / ("metallib-" + hash + ".metallib");
        std::error_code error;
        if (std::filesystem::exists(path, error)) return path.string();
        std::filesystem::create_directories(directory, error);
        std::ofstream file(path, std::ios::binary);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file) throw std::runtime_error("cannot write " + path.string());
        return path.string();
    } catch (const std::exception& failure) {
        if (!reported) {
            reported = true;
            std::cerr << "alchemy: ALCHEMY_MSL_DUMP (metallib) failed: " << failure.what() << std::endl;
        }
        return std::nullopt;
    }
}

std::optional<std::string> dump_source(std::string_view source, const CompileOptions& options) {
    const auto directory = dump_directory();
    if (directory.empty()) return std::nullopt;
    static std::mutex mutex;
    static bool reported{};
    std::lock_guard lock(mutex);
    try {
        static constexpr char digits[] = "0123456789abcdef";
        std::string hash(16, '0');
        auto value = fingerprint(source);
        for (auto index = hash.size(); index-- > 0; value >>= 4) hash[index] = digits[value & 15];
        const auto stem = directory / (safe_label(options.label) + "-" + hash);
        auto path = stem;
        path += ".metal";
        std::error_code error;
        if (std::filesystem::exists(path, error)) return path.string();
        std::filesystem::create_directories(directory, error);
        {
            std::ofstream file(path, std::ios::binary);
            file.write(source.data(), static_cast<std::streamsize>(source.size()));
            if (!file) throw std::runtime_error("cannot write " + path.string());
        }
        auto options_path = stem;
        options_path += ".options";
        std::ofstream file(options_path, std::ios::binary);
        file << "language " << options.language_major << '.' << options.language_minor << '\n'
             << "fast_math " << options.fast_math << '\n'
             << "preserve_invariance " << options.preserve_invariance << '\n'
             << "subgroup_size " << options.subgroup_size << '\n';
        for (const auto& [name, macro] : options.macros) file << "define " << name << '=' << macro << '\n';
        if (!file) throw std::runtime_error("cannot write " + options_path.string());
        return path.string();
    } catch (const std::exception& failure) {
        if (!reported) {
            reported = true;
            std::cerr << "alchemy: ALCHEMY_MSL_DUMP failed: " << failure.what() << std::endl;
        }
        return std::nullopt;
    }
}

CompileOptions read_dump_options(const std::filesystem::path& source) {
    CompileOptions options;
    options.label = source.stem().string();
    auto options_path = source;
    options_path.replace_extension(".options");
    std::ifstream file(options_path, std::ios::binary);
    if (!file) return options;
    for (std::string line; std::getline(file, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto space = line.find(' ');
        if (space == std::string::npos) continue;
        const auto key = line.substr(0, space), value = line.substr(space + 1);
        if (key == "language") {
            const auto dot = value.find('.');
            options.language_major = static_cast<std::uint32_t>(std::stoul(value.substr(0, dot)));
            options.language_minor = dot == std::string::npos ? 0 : static_cast<std::uint32_t>(std::stoul(value.substr(dot + 1)));
        } else if (key == "fast_math") {
            options.fast_math = value == "1";
        } else if (key == "preserve_invariance") {
            options.preserve_invariance = value == "1";
        } else if (key == "subgroup_size") {
            options.subgroup_size = static_cast<std::uint32_t>(std::stoul(value));
        } else if (key == "define") {
            const auto equals = value.find('=');
            options.macros.emplace_back(value.substr(0, equals), equals == std::string::npos ? "1" : value.substr(equals + 1));
        }
    }
    return options;
}

} // namespace alchemy::msl
