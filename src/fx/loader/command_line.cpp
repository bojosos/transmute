#include "fx/loader/command_line.hpp"

#include <cwchar>

namespace transmute::fx::loader {

std::wstring quote_argument(std::wstring_view argument, bool force_quotes) {
    const bool needs_quotes = force_quotes || argument.empty() ||
                              argument.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quotes) return std::wstring(argument);
    std::wstring out;
    out.reserve(argument.size() + 2);
    out.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');   // escape the backslashes and the quote
        } else {
            out.append(backslashes, L'\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');   // before the closing quote
    out.push_back(L'"');
    return out;
}

std::wstring build_guest_command_line(std::wstring_view program,
                                      const std::vector<std::wstring>& arguments) {
    // argv[0] is parsed without backslash escapes; a path never contains quotes.
    std::wstring line = L"\"";
    line.append(program);
    line.push_back(L'"');
    for (const auto& argument : arguments) {
        line.push_back(L' ');
        line += quote_argument(argument);
    }
    return line;
}

bool parse_stack_size(std::wstring_view text, std::uint64_t& bytes) {
    constexpr wchar_t kTab = 9;
    while (!text.empty() && (text.front() == L' ' || text.front() == kTab)) text.remove_prefix(1);
    while (!text.empty() && (text.back() == L' ' || text.back() == kTab)) text.remove_suffix(1);
    if (text.empty()) return false;
    unsigned base = 10;
    if (text.size() > 2 && text[0] == L'0' && (text[1] == L'x' || text[1] == L'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    std::uint64_t value = 0;
    std::size_t digits = 0;
    for (; digits < text.size(); ++digits) {
        const wchar_t c = text[digits];
        unsigned d;
        if (c >= L'0' && c <= L'9') d = static_cast<unsigned>(c - L'0');
        else if (base == 16 && c >= L'a' && c <= L'f') d = static_cast<unsigned>(c - L'a' + 10);
        else if (base == 16 && c >= L'A' && c <= L'F') d = static_cast<unsigned>(c - L'A' + 10);
        else break;
        if (value > (UINT64_MAX - d) / base) return false;
        value = value * base + d;
    }
    if (digits == 0) return false;
    std::wstring suffix;
    for (wchar_t c : text.substr(digits)) {
        if (c == L' ') continue;
        suffix.push_back(c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c);
    }
    unsigned shift = 0;
    if (suffix.empty() || suffix == L"b") shift = 0;
    else if (suffix == L"k" || suffix == L"kb" || suffix == L"kib") shift = 10;
    else if (suffix == L"m" || suffix == L"mb" || suffix == L"mib") shift = 20;
    else if (suffix == L"g" || suffix == L"gb" || suffix == L"gib") shift = 30;
    else return false;
    constexpr std::uint64_t kMax = 1ull << 40;
    if (value == 0 || value > (kMax >> shift)) return false;
    value <<= shift;
    constexpr std::uint64_t kGranularity = 64 * 1024;
    bytes = (value + kGranularity - 1) / kGranularity * kGranularity;
    return true;
}

bool parse_cli(int argc, const wchar_t* const* argv, CliOptions& out, std::wstring& error) {
    out = CliOptions{};
    int i = 1;
    auto value = [&](const wchar_t* option, std::wstring& target) {
        if (i + 1 >= argc) {
            error = std::wstring(option) + L" requires a value";
            return false;
        }
        target = argv[++i];
        return true;
    };
    for (; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--") {
            ++i;
            break;
        }
        if (arg.size() < 2 || arg[0] != L'-' || arg[1] != L'-') {
            if (arg == L"-h" || arg == L"/?") {
                out.help = true;
                continue;
            }
            break;
        }
        if (arg == L"--dll-dir") {
            std::wstring dir;
            if (!value(L"--dll-dir", dir)) return false;
            out.dll_directories.push_back(dir);
        } else if (arg == L"--aot") {
            std::wstring image;
            if (!value(L"--aot", image)) return false;
            out.aot_images.push_back(image);
        } else if (arg == L"--aot-background") {
            out.aot_background = true;
        } else if (arg == L"--aot-threads") {
            std::wstring n;
            if (!value(L"--aot-threads", n)) return false;
            wchar_t* end = nullptr;
            const unsigned long threads = std::wcstoul(n.c_str(), &end, 10);
            if (n.empty() || *end != 0 || threads > 256) {
                error = L"invalid --aot-threads " + n;
                return false;
            }
            out.aot_threads = static_cast<unsigned>(threads);
        } else if (arg == L"--cache-dir") {
            if (!value(L"--cache-dir", out.cache_directory)) return false;
        } else if (arg == L"--stack-size") {
            std::wstring size;
            if (!value(L"--stack-size", size)) return false;
            if (!parse_stack_size(size, out.stack_reserve)) {
                error = L"invalid --stack-size " + size + L" (expected bytes, NNk or NNm)";
                return false;
            }
        } else if (arg == L"--no-cache") {
            out.no_cache = true;
        } else if (arg == L"--profile") {
            out.profile = true;
        } else if (arg == L"--verbose") {
            out.verbose = true;
        } else if (arg == L"--help") {
            out.help = true;
        } else {
            error = L"unknown option " + std::wstring(arg);
            return false;
        }
    }
    if (i < argc) {
        out.program = argv[i++];
        for (; i < argc; ++i) out.arguments.emplace_back(argv[i]);
    }
    if (out.program.empty() && out.aot_images.empty() && !out.help) {
        error = L"missing program";
        return false;
    }
    return true;
}

const wchar_t* cli_usage() {
    return L"usage: transmute-fx [--dll-dir <dir>]... [--no-cache] [--profile] [--aot <image>]\n"
           L"                    [--stack-size <bytes|NNk|NNm>] [--verbose] [--] <program.exe> [args...]\n"
           L"  --dll-dir <dir>     add a guest DLL directory (searched after the program's directory)\n"
           L"  --no-cache          disable the persistent translation cache\n"
           L"  --profile           enable runtime counters\n"
           L"  --aot <image>       translate an ARM64 image ahead of time into the cache\n"
           L"  --aot-threads <n>   AOT worker threads (default: one per hardware thread)\n"
           L"  --aot-background    with --aot: write the cold AOT file (<stem>.fxa) that lookups\n"
           L"                      fall back to, at background priority, skipping images that\n"
           L"                      have one (what background AOT of guest processes runs)\n"
           L"  --cache-dir <dir>   translation cache directory (default: TRANSMUTE_FX_CACHE_DIR,\n"
           L"                      else %LOCALAPPDATA%\\transmute\\fx-cache)\n"
           L"  --stack-size <n>    default guest stack reserve for the main thread and threads\n"
           L"                      created with a zero stack size, e.g. 16m (overrides\n"
           L"                      TRANSMUTE_FX_STACK_SIZE and the EXE's SizeOfStackReserve)\n"
           L"  --verbose           report unresolved imports while loading\n";
}

} // namespace transmute::fx::loader
