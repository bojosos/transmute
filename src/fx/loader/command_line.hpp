#pragma once

// transmute-fx command-line handling (owner: loader).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace transmute::fx::loader {

// Quotes one argument so CommandLineToArgvW / the CRT parse it back unchanged.
std::wstring quote_argument(std::wstring_view argument, bool force_quotes = false);

// Guest command line: the program path (always quoted) followed by the
// arguments, each re-quoted per Windows rules.
std::wstring build_guest_command_line(std::wstring_view program,
                                      const std::vector<std::wstring>& arguments);

// Parses a stack size: decimal or 0x-hex bytes with an optional k/m/g suffix
// (KiB/MiB/GiB; "kb", "mib" etc. accepted). Rounded up to 64 KiB. False on
// malformed, zero or absurd (> 1 TiB) values.
bool parse_stack_size(std::wstring_view text, std::uint64_t& bytes);

struct CliOptions {
    std::vector<std::wstring> dll_directories;
    std::uint64_t stack_reserve = 0;       // --stack-size (0 = not given)
    std::vector<std::wstring> aot_images;
    bool aot_background = false;           // --aot-background: cold AOT files, idle priority
    unsigned aot_threads = 0;              // --aot-threads (0 = one per hardware thread)
    std::wstring cache_directory;          // --cache-dir (empty = default)
    bool no_cache = false;
    bool profile = false;
    bool verbose = false;
    bool help = false;
    std::wstring program;
    std::vector<std::wstring> arguments;
};

// transmute-fx [--dll-dir <dir>]... [--no-cache] [--profile] [--aot <image>]
//              [--stack-size <bytes|NNk|NNm>] [--verbose] [--] <program.exe> [args...]
// Options are recognised only before the program path.
bool parse_cli(int argc, const wchar_t* const* argv, CliOptions& out, std::wstring& error);

const wchar_t* cli_usage();

} // namespace transmute::fx::loader
