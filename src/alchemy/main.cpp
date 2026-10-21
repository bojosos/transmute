#include "alchemy/alchemy_root.hpp"
#include "alchemy/darwin_process.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/process.hpp"
#include "alchemy/macho.hpp"
#include "alchemy/path.hpp"
#include "alchemy/inspection.hpp"
#include "alchemy/perf.hpp"
#ifdef ALCHEMY_WITH_GUI
#include "alchemy/gui/install.hpp"
#endif

#include <charconv>
#include <cwchar>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>


namespace {

// UTF-8 absolute form of a host path given on the command line.
std::string absolute_text(std::string_view value) {
    const auto bytes = std::filesystem::absolute(alchemy::utf8_path(value)).u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::uint64_t number(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("expected an unsigned decimal or hexadecimal number");
    }
    return value;
}

void usage() {
    std::cout << "Alchemy: ARM64 macOS application compatibility on x64 Windows\n"
                 "Usage: alchemy [options] program [arguments...]\n"
                 "       alchemy doctor [options] program\n"
                 "       alchemy install [--root DIRECTORY] [--links FILE] [--replace] [--no-infer]\n"
                 "                       [--no-native-links] Bundle.app\n"
                 "Options: --jit --jit-stats --load-base ADDRESS --root DIRECTORY\n"
                 "         --dylib-dir DIRECTORY --runtime-dir DIRECTORY --instruction-limit COUNT\n"
                 "         --fd N=MODE:TARGET  the program starts with descriptor N (3..4095) open on\n"
                 "                 TARGET: a host path (file, named pipe) or handle:VALUE (an inherited\n"
                 "                 handle); MODE r, w (create/truncate), a (append) or rw\n"
                 "Doctor:  --deps --scan-bundle --json\n"
                 "Root:    guest /Applications and /Library live in the Alchemy root (--root, ALCHEMY_ROOT,\n"
                 "         or the root containing the program; install defaults to "
                 "%LOCALAPPDATA%\\Alchemy\\root)\n"
                 "Profile: ARM64 headless runtime, independent Darwin replacements\n";
}

std::optional<std::string> environment_value(const std::vector<std::string>& environment, std::string_view key) {
    for (const auto& entry : environment) {
        if (entry.size() > key.size() && entry.compare(0, key.size(), key) == 0 && entry[key.size()] == '=')
            return entry.substr(key.size() + 1);
    }
    return std::nullopt;
}

int install(const std::vector<std::string>& arguments, const std::vector<std::string>& environment) {
    alchemy::InstallOptions options;
    std::optional<std::filesystem::path> root;
    std::size_t index = 2;
    for (; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == "--") { ++index; break; }
        if (!argument.starts_with('-')) break;
        if (argument == "--replace") options.replace = true;
        else if (argument == "--no-infer") options.infer_framework_links = false;
        else if (argument == "--no-native-links") options.allow_native_symlinks = false;
        else if (argument == "--root" || argument == "--links") {
            if (++index == arguments.size()) throw std::runtime_error(argument + " requires a value");
            if (argument == "--root") root = alchemy::utf8_path(arguments[index]);
            else options.links = alchemy::utf8_path(arguments[index]);
        } else throw std::runtime_error("unknown install option: " + argument);
    }
    if (index + 1 != arguments.size()) throw std::runtime_error("install accepts one .app directory");
    const auto lower = [](std::string value) {
        for (auto& character : value) if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
        return value;
    };
    if (lower(arguments[index]).ends_with(".dmg"))
        throw std::runtime_error("disk images are extracted by scripts/alchemy_install_dmg.ps1, which records their links");
    options.source = alchemy::utf8_path(arguments[index]);
    if (!root) {
        if (const auto value = environment_value(environment, "ALCHEMY_ROOT"); value && !value->empty()) root = alchemy::utf8_path(*value);
        else root = alchemy::default_alchemy_root();
    }
    options.root = *root;
    const auto result = alchemy::install_app(options, std::cout);
    std::cout << "Guest path: /Applications/" << alchemy::guest_path_string(result.destination.filename()) << '\n';
    return 0;
}

int launch(const std::vector<std::string>& arguments, const std::vector<std::string>& environment) {
    try {
        if (arguments.size() == 1) {
            usage();
            return 1;
        }
        if (arguments[1] == "install") return install(arguments, environment);
        std::size_t index = 1;
#ifdef ALCHEMY_WITH_GUI
        if (arguments[index] == "msl-seed") {
            if (arguments.size() < 5) {
                std::cerr << "usage: alchemy-fx msl-seed <corpus directory> <executable> <seed root> [threads]\n";
                return 1;
            }
            const unsigned threads = arguments.size() > 5 ? static_cast<unsigned>(number(arguments[5])) : 4U;
            return alchemy::gui::seed_msl_corpus(alchemy::utf8_path(arguments[2]), alchemy::utf8_path(arguments[3]),
                                                 alchemy::utf8_path(arguments[4]), threads);
        }
#endif
        const bool inspect = arguments[index] == "doctor";
        if (inspect) ++index;
        alchemy::RunOptions options;
        std::optional<std::filesystem::path> root;
        std::optional<alchemy::ChildLaunch> child;
        bool json = false, recursive = false, scan_bundle = false;
        for (; index < arguments.size(); ++index) {
            const auto& argument = arguments[index];
            if (argument == "--help" || argument == "-h") {
                usage();
                return 0;
            }
            if (argument == "--") { ++index; break; }
            if (!argument.starts_with('-')) break;
            if (argument == "--jit") { options.jit = true; options.launch_options.push_back(argument); }
            else if (argument == "--jit-stats") { options.jit = true; options.jit_stats = true; options.launch_options.push_back("--jit"); }
            else if (!inspect && argument == "--alchemy-child-spec") {
                // Internal: this process is a guest child (darwin_process.hpp).
                if (++index == arguments.size()) throw std::runtime_error(argument + " requires a value");
                child = alchemy::read_child_launch(arguments[index]);
            }
            else if (!inspect && argument == "--fd") {
                // A descriptor of the guest program; its children inherit it
                // through their descriptor tables, not their command lines.
                if (++index == arguments.size()) throw std::runtime_error(argument + " requires a value");
                auto descriptor = alchemy::open_root_descriptor(arguments[index]);
                for (const auto& other : options.descriptors)
                    if (other.fd == descriptor.fd) {
                        alchemy::host::close(alchemy::host::Handle{static_cast<std::uintptr_t>(descriptor.handle)});
                        throw std::runtime_error("--fd " + std::to_string(descriptor.fd) + " given twice");
                    }
                options.descriptors.push_back(std::move(descriptor));
            }
            else if (inspect && argument == "--json") json = true;
            else if (inspect && argument == "--deps") recursive = true;
            else if (inspect && argument == "--scan-bundle") scan_bundle = true;
            else if (argument == "--load-base" || argument == "--dylib-dir" || argument == "--root" ||
                     argument == "--runtime-dir" || argument == "--instruction-limit") {
                if (++index == arguments.size()) throw std::runtime_error(argument + " requires a value");
                if (argument != "--root") {
                    // Children start in the guest's working directory: directories are absolute.
                    options.launch_options.push_back(argument);
                    options.launch_options.push_back(argument == "--dylib-dir" || argument == "--runtime-dir"
                        ? absolute_text(arguments[index]) : arguments[index]);
                }
                if (argument == "--load-base") options.load_base = number(arguments[index]);
                else if (argument == "--root") root = alchemy::utf8_path(arguments[index]);
                else if (argument == "--dylib-dir") {
                    options.dylib_directories.push_back(alchemy::utf8_path(arguments[index]));
                } else if (argument == "--runtime-dir") {
                    options.runtime_directories.push_back(alchemy::utf8_path(arguments[index]));
                } else {
                    options.instruction_limit = number(arguments[index]);
                    if (options.instruction_limit == 0) throw std::runtime_error("instruction limit must be positive");
                }
            } else throw std::runtime_error("unknown option: " + argument);
        }
        if (child && !child->tool.empty()) {
            // A built-in tool child (the shell): no guest program.
            if (root) alchemy::configure_alchemy_root(*root);
            return alchemy::run_child_tool(*child, options.launch_options);
        }
        if (index == arguments.size()) throw std::runtime_error("missing guest executable path");
        // The Alchemy root: explicit, from the environment, or the root that
        // contains the program. Without one, guest paths keep the drive mapping.
        const auto& program = arguments[index++];
        if (!root) {
            if (const auto value = environment_value(environment, "ALCHEMY_ROOT"); value && !value->empty()) root = alchemy::utf8_path(*value);
            else if (!program.starts_with('/')) root = alchemy::find_alchemy_root(alchemy::utf8_path(program));
        }
        if (root) alchemy::configure_alchemy_root(*root);
        // A guest-style /Applications/... program path names the root's copy.
        const auto mapped = program.starts_with('/') ? alchemy::guest_root_host_path(program) : std::nullopt;
        const auto path = mapped ? *mapped : alchemy::utf8_path(program);
        if (inspect) {
            if (index != arguments.size()) throw std::runtime_error("doctor accepts one image path");
            return alchemy::inspect_program(path, options, recursive, scan_bundle, json, std::cout);
        }
        const std::vector<std::string> guest_arguments(arguments.begin() + static_cast<std::ptrdiff_t>(index),
                                                       arguments.end());
        if (child) options.child = std::make_shared<const alchemy::ChildLaunch>(std::move(*child));
        const auto result = alchemy::run_program(path, guest_arguments, options, {},
                                                 options.child ? options.child->envp : environment);
        if (options.jit_stats) {
            std::cerr << "Alchemy JIT: native guest instructions " << result.jit.native_guest_instructions
                      << ", native blocks " << result.jit.native_blocks
                      << ", fallback instructions " << result.jit.fallback_instructions << '\n';
        }
        // A child ended by a signal's default action tells its guest parent so
        // (WIFSIGNALED); a top-level process keeps the shell status 128 + signal.
        if (options.child && result.signal)
            return static_cast<int>(alchemy::child_signal_exit_marker | static_cast<std::uint32_t>(result.signal));
        return static_cast<int>(result.exit_code);
    } catch (const std::exception& error) {
        std::cerr << "alchemy: " << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
// A UTF-16 command-line argument (wmain) in UTF-8; unpaired surrogates are
// rejected, as WideCharToMultiByte(WC_ERR_INVALID_CHARS) did.
std::string utf8(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        std::uint32_t code = static_cast<std::uint16_t>(value[index]);
        if (code >= 0xd800 && code <= 0xdbff) {
            const std::uint32_t low = index + 1 < value.size() ? static_cast<std::uint16_t>(value[index + 1]) : 0;
            if (low < 0xdc00 || low > 0xdfff) throw std::runtime_error("invalid Unicode command line or environment");
            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
            ++index;
        } else if (code >= 0xdc00 && code <= 0xdfff) {
            throw std::runtime_error("invalid Unicode command line or environment");
        }
        if (code < 0x80) {
            result.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            result.push_back(static_cast<char>(0xc0 | (code >> 6)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            result.push_back(static_cast<char>(0xe0 | (code >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            result.push_back(static_cast<char>(0xf0 | (code >> 18)));
            result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }
    return result;
}
#endif

// The host environment, without the hidden per-drive "=C:=C:\..." entries.
std::vector<std::string> host_environment() {
    std::vector<std::string> environment;
    for (auto& entry : alchemy::host::process::environment_block())
        if (!entry.empty() && entry.front() != '=') environment.push_back(std::move(entry));
    return environment;
}

} // namespace

#ifdef _WIN32
// --load-base from the raw arguments, before anything allocates: the options
// launch() reads, up to the program path. Empty when absent or not a number.
static std::optional<std::uint64_t> early_load_base(int argc, wchar_t** argv) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument.empty() || argument[0] != L'-' || argument == L"--") return std::nullopt;
        const bool valued = argument == L"--load-base" || argument == L"--dylib-dir" || argument == L"--root" ||
                            argument == L"--runtime-dir" || argument == L"--instruction-limit" ||
                            argument == L"--alchemy-child-spec" || argument == L"--fd";
        if (!valued) continue;
        if (++index == argc) return std::nullopt;
        if (argument != L"--load-base") continue;
        // As number(): 0x-prefixed hexadecimal, else decimal.
        const std::wstring_view text(argv[index]);
        const bool hex = text.starts_with(L"0x") || text.starts_with(L"0X");
        wchar_t* end = nullptr;
        const auto value = std::wcstoull(argv[index] + (hex ? 2 : 0), &end, hex ? 16 : 10);
        if (end == argv[index] + (hex ? 2 : 0) || *end) return std::nullopt;
        return value;
    }
    return std::nullopt;
}

int wmain(int argc, wchar_t** argv) {
    // First, while nothing else has allocated: the executable's load range.
    alchemy::engine_hold_load_range<transmute::ReferenceEngine>(early_load_base(argc, argv));
    alchemy::perf::start();
    try {
        std::vector<std::string> arguments;
        for (int index = 0; index < argc; ++index) arguments.push_back(utf8(argv[index]));
        return launch(arguments, host_environment());
    } catch (const std::exception& error) {
        std::cerr << "alchemy: " << error.what() << '\n';
        return 1;
    }
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> arguments(argv, argv + argc);
    return launch(arguments, host_environment());
}
#endif
