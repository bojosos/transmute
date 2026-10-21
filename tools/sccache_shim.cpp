// Compiler launcher between Ninja and sccache (docs/ALCHEMY_BUILD.md).
//
// On a cache hit sccache replays the /showIncludes notes of the compile that
// filled the cache, and their paths name that compile's checkout. Ninja would
// record them as this object's header dependencies, so a header change in this
// checkout would not rebuild the objects that include it. The launcher runs
// sccache with the same arguments and rewrites each include note that names a
// file in another checkout of the repository to the same file in this one.
//
// CMake builds it at configure time and writes <exe>.cfg next to it: the
// sccache path, this checkout's root and the compiler's /showIncludes prefix,
// one per line.
#include <windows.h>

#include <cctype>
#include <fstream>
#include <string>
#include <unordered_map>

namespace {

struct Config {
    std::string sccache;
    std::string root;      // this checkout, backslashes, no trailing separator
    std::string prefix;    // e.g. "Note: including file:"
};

bool is_separator(char c) { return c == '\\' || c == '/'; }

std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string normalize(std::string path) {
    for (auto& c : path) if (c == '/') c = '\\';
    while (path.size() > 3 && path.back() == '\\') path.pop_back();
    return path;
}

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool read_config(Config& config) {
    char exe[MAX_PATH];
    const auto length = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (length == 0 || length == MAX_PATH) return false;
    std::string path(exe, length);
    const auto dot = path.find_last_of('.');
    std::ifstream file(path.substr(0, dot) + ".cfg");
    std::string* fields[] = {&config.sccache, &config.root, &config.prefix};
    for (auto* field : fields) {
        if (!std::getline(file, *field)) return false;
        if (!field->empty() && field->back() == '\r') field->pop_back();
    }
    config.root = normalize(config.root);
    return !config.sccache.empty() && !config.root.empty() && !config.prefix.empty();
}

// The arguments after argv[0], exactly as Ninja passed them.
std::wstring arguments() {
    const wchar_t* line = GetCommandLineW();
    bool quoted = false;
    while (*line && (quoted || (*line != L' ' && *line != L'\t'))) {
        if (*line == L'"') quoted = !quoted;
        ++line;
    }
    while (*line == L' ' || *line == L'\t') ++line;
    return line;
}

std::wstring widen(const std::string& text) {
    const int size = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

class Rewriter {
public:
    explicit Rewriter(const Config& config) : root_(config.root), root_lower_(lower(config.root)) {}

    // The same file under this checkout, when the path names a file in another
    // checkout (a directory holding .git) and that file exists here.
    std::string rewrite(const std::string& raw) {
        const auto path = normalize(raw);
        const auto path_lower = lower(path);
        if (path_lower.size() > root_lower_.size() && path_lower.starts_with(root_lower_) &&
            is_separator(path_lower[root_lower_.size()])) {
            return raw;
        }
        const auto checkout = checkout_of(path);
        if (checkout.empty() || lower(checkout) == root_lower_) return raw;
        auto candidate = root_ + path.substr(checkout.size());
        return exists(candidate) ? candidate : raw;
    }

private:
    std::string checkout_of(const std::string& path) {
        std::string directory = path;
        std::string found;
        std::string visited[64];
        int count = 0;
        while (true) {
            const auto slash = directory.find_last_of('\\');
            if (slash == std::string::npos || slash < 3) break;
            directory.resize(slash);
            const auto key = lower(directory);
            if (const auto it = checkouts_.find(key); it != checkouts_.end()) { found = it->second; break; }
            if (count < 64) visited[count++] = key;
            if (exists(directory + "\\.git")) { found = directory; break; }
        }
        for (int i = 0; i < count; ++i) checkouts_[visited[i]] = found;
        return found;
    }

    std::string root_;
    std::string root_lower_;
    std::unordered_map<std::string, std::string> checkouts_;
};

void write_all(HANDLE out, const std::string& text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        DWORD written = 0;
        if (!WriteFile(out, text.data() + offset, static_cast<DWORD>(text.size() - offset), &written, nullptr)) return;
        offset += written;
    }
}

void emit(HANDLE out, const Config& config, Rewriter& rewriter, std::string line) {
    if (line.starts_with(config.prefix)) {
        auto begin = config.prefix.size();
        while (begin < line.size() && line[begin] == ' ') ++begin;
        auto end = line.size();
        while (end > begin && (line[end - 1] == '\n' || line[end - 1] == '\r')) --end;
        line = line.substr(0, begin) + rewriter.rewrite(line.substr(begin, end - begin)) + line.substr(end);
    }
    write_all(out, line);
}

}  // namespace

int main() {
    Config config;
    if (!read_config(config)) {
        static const char message[] = "sccache_shim: missing or invalid sccache_shim.cfg (reconfigure the build directory)\n";
        write_all(GetStdHandle(STD_ERROR_HANDLE), message);
        return 2;
    }

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) return 3;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_end;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    std::wstring command = L"\"" + widen(config.sccache) + L"\" " + arguments();
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) {
        static const char message[] = "sccache_shim: could not start sccache\n";
        write_all(GetStdHandle(STD_ERROR_HANDLE), message);
        return 4;
    }
    CloseHandle(write_end);
    CloseHandle(process.hThread);

    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    Rewriter rewriter(config);
    std::string pending;
    char buffer[16384];
    DWORD got = 0;
    while (ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr) && got != 0) {
        pending.append(buffer, got);
        std::size_t start = 0;
        for (auto newline = pending.find('\n'); newline != std::string::npos; newline = pending.find('\n', start)) {
            emit(out, config, rewriter, pending.substr(start, newline + 1 - start));
            start = newline + 1;
        }
        pending.erase(0, start);
    }
    if (!pending.empty()) emit(out, config, rewriter, pending);
    CloseHandle(read_end);

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
