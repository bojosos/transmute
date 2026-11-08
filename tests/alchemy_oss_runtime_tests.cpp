// Tests of a vendored open-source /usr/lib runtime library (libffi, SQLite, ICU, ...): the loader's
// allow-list and provenance checks, doctor, and a guest fixture run in alchemy-fx processes and,
// with --in-process, under alchemy in this process (interpreter and JIT).
//
// Usage: alchemy_oss_runtime_tests [--in-process] <install name> <upstream> <fixture> <expected output>
//            <scratch dir> <alchemy-fx.exe> <runtime dir> [<more runtime dirs>...]
// The first runtime directory holds the library under test (its file is the install name's file
// name, with provenance.json beside it); the others hold runtimes it depends on (LLVM's libc++).
// The fixture's stdout must equal the expected-output file (CR characters ignored), at the default
// and a rebased load address.
#define NOMINMAX
#include <windows.h>

#include "alchemy/inspection.hpp"
#include "alchemy/process.hpp"
#include "alchemy/runtime_libraries.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    require(input.good(), "cannot read " + file.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& file, const std::string& text) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string without_cr(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

struct Test {
    std::string install_name, upstream;
    std::filesystem::path program, scratch;
    std::string expected, alchemy_fx;
    std::vector<std::filesystem::path> runtimes;
    bool in_process{};
    std::string file_name() const { return std::filesystem::path(install_name).filename().string(); }
};

// The runtime directories with the first replaced by `replacement`.
std::vector<std::filesystem::path> with_first(const Test& test, const std::filesystem::path& replacement) {
    auto directories = test.runtimes;
    directories.front() = replacement;
    return directories;
}

void copy_runtime(const Test& test, const std::filesystem::path& target, bool provenance) {
    std::filesystem::remove_all(target);
    std::filesystem::create_directories(target);
    std::filesystem::copy_file(test.runtimes.front() / test.file_name(), target / test.file_name());
    if (provenance) std::filesystem::copy_file(test.runtimes.front() / "provenance.json", target / "provenance.json");
}

std::string doctor(const Test& test, const std::vector<std::filesystem::path>& runtimes, int& status) {
    alchemy::RunOptions options;
    options.runtime_directories = runtimes;
    std::ostringstream report;
    status = alchemy::inspect_program(test.program, options, true, false, true, report);
    return report.str();
}

// The fixture under alchemy in this process: interpreter and JIT, default and rebased load base.
void run_in_process(const Test& test) {
    for (const bool jit : {false, true}) for (const bool rebased : {false, true}) {
        alchemy::RunOptions options;
        options.jit = options.jit_stats = jit;
        options.runtime_directories = test.runtimes;
        options.instruction_limit = 2'000'000'000;
        if (rebased) options.load_base = 0x120000000ULL;
        std::string out, err;
        const auto writer = [&](unsigned descriptor, std::span<const std::uint8_t> bytes) -> alchemy::OutputResult {
            (descriptor == 1 ? out : err).append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return {bytes.size(), 0};
        };
        const auto process = alchemy::run_program(test.program, {}, options, writer);
        const auto mode = std::string(jit ? "JIT" : "interpreter") + (rebased ? " rebased" : " default base");
        require(process.exit_code == 0 && without_cr(out) == test.expected,
                "alchemy " + mode + " mismatch: status " + std::to_string(process.exit_code) + " output " + out + err);
        require(!jit || process.jit.native_guest_instructions != 0, "alchemy " + mode + ": no translated guest code executed");
        std::cout << "  ok   alchemy " << mode << '\n';
    }
}

void in_process(const Test& test) {
    const auto* identity = alchemy::runtime_library_identity(test.install_name);
    require(identity != nullptr, test.install_name + " is not allow-listed");
    require(identity->upstream == test.upstream, test.install_name + " is allow-listed for another upstream");
    int status = 0;
    const auto report = doctor(test, test.runtimes, status);
    require(status == 0 && report.find("\"identity\":\"" + test.install_name + "\",\"status\":\"guest\"") != std::string::npos,
            "doctor did not resolve " + test.install_name + ": " + report);
    std::cout << "  ok   doctor resolves " << test.install_name << '\n';

    const auto refused = [&](const std::string& name, const std::vector<std::filesystem::path>& runtimes, const std::string& reason) {
        int refused_status = 0;
        const auto text = doctor(test, runtimes, refused_status);
        require(refused_status != 0 && text.find(reason) != std::string::npos,
                name + ": expected \"" + reason + "\", got status " + std::to_string(refused_status) + " " + text);
        std::cout << "  ok   refused: " << name << '\n';
    };
    const auto plain = test.scratch / "no-provenance";
    copy_runtime(test, plain, false);
    refused("missing provenance", with_first(test, plain), "missing provenance.json");

    const auto tampered = test.scratch / "tampered";
    copy_runtime(test, tampered, true);
    auto bytes = read(tampered / test.file_name());
    bytes.back() = static_cast<char>(bytes.back() ^ 0x5a);
    write(tampered / test.file_name(), bytes);
    refused("modified library", with_first(test, tampered), "SHA-256 differs from provenance.json");

    const auto foreign = test.scratch / "foreign-source";
    copy_runtime(test, foreign, true);
    auto provenance = read(foreign / "provenance.json");
    const auto at = provenance.find(test.upstream);
    require(at != std::string::npos, "provenance.json lacks the upstream source");
    provenance.replace(at, test.upstream.size(), "https://example.invalid/upstream");
    write(foreign / "provenance.json", provenance);
    refused("foreign upstream", with_first(test, foreign), "does not record upstream " + test.upstream);
}

std::wstring wide(const std::string& text) {
    std::wstring result(text.size(), L'\0');
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), static_cast<int>(result.size()));
    result.resize(static_cast<std::size_t>(length));
    return result;
}

std::wstring quote(const std::string& argument) {
    std::wstring text = L"\"";
    for (const auto ch : wide(argument)) { if (ch == L'"') text += L'\\'; text += ch; }
    return text + L"\"";
}

struct Run { DWORD status{}; std::string out, err; };

Run process(const std::vector<std::string>& command) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE out_read = nullptr, out_write = nullptr, err_read = nullptr, err_write = nullptr;
    if (!CreatePipe(&out_read, &out_write, &inherit, 0) || !CreatePipe(&err_read, &err_write, &inherit, 0))
        throw std::runtime_error("CreatePipe failed");
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
    std::wstring line;
    for (const auto& argument : command) line += (line.empty() ? L"" : L" ") + quote(argument);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = out_write;
    startup.hStdError = err_write;
    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    CloseHandle(out_write);
    CloseHandle(err_write);
    if (!created) { CloseHandle(out_read); CloseHandle(err_read); throw std::runtime_error("cannot start alchemy-fx"); }
    Run result;
    const auto drain = [](HANDLE pipe, std::string& text) {
        char buffer[4096];
        DWORD count = 0;
        while (ReadFile(pipe, buffer, sizeof(buffer), &count, nullptr) && count) text.append(buffer, count);
    };
    std::thread out_reader(drain, out_read, std::ref(result.out));
    std::thread err_reader(drain, err_read, std::ref(result.err));
    const bool timed_out = WaitForSingleObject(info.hProcess, 240'000) != WAIT_OBJECT_0;
    if (timed_out) { TerminateProcess(info.hProcess, 0xdead); WaitForSingleObject(info.hProcess, INFINITE); }
    out_reader.join();
    err_reader.join();
    CloseHandle(out_read);
    CloseHandle(err_read);
    GetExitCodeProcess(info.hProcess, &result.status);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    if (timed_out) throw std::runtime_error("alchemy-fx timed out");
    return result;
}

void fx(const Test& test) {
    for (const bool rebased : {false, true}) {
        std::vector<std::string> command{test.alchemy_fx};
        for (const auto& runtime : test.runtimes) { command.push_back("--runtime-dir"); command.push_back(runtime.string()); }
        if (rebased) { command.push_back("--load-base"); command.push_back("0x120000000"); }
        command.push_back(test.program.string());
        const auto result = process(command);
        const std::string mode = rebased ? "rebased" : "default base";
        require(result.status == 0 && without_cr(result.out) == test.expected,
                "alchemy-fx " + mode + " mismatch: status " + std::to_string(result.status) + " output " + result.out + result.err);
        std::cout << "  ok   alchemy-fx " << mode << '\n';
    }
    std::vector<std::string> command{test.alchemy_fx};
    for (std::size_t index = 1; index < test.runtimes.size(); ++index) {
        command.push_back("--runtime-dir");
        command.push_back(test.runtimes[index].string());
    }
    command.push_back(test.program.string());
    const auto refused = process(command);
    require(refused.status != 0 && refused.out.empty() &&
            refused.err.find("missing guest dylib: " + test.install_name) != std::string::npos,
            "alchemy-fx without the runtime: status " + std::to_string(refused.status) + " " + refused.out + refused.err);
    std::cout << "  ok   alchemy-fx refuses a missing runtime\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        Test test;
        int first = 1;
        if (argc > 1 && std::string(argv[1]) == "--in-process") { test.in_process = true; ++first; }
        if (argc - first < 7)
            throw std::runtime_error("usage: alchemy_oss_runtime_tests [--in-process] <install name> <upstream> <fixture> "
                                     "<expected output> <scratch dir> <alchemy-fx> <runtime dir>...");
        test.install_name = argv[first];
        test.upstream = argv[first + 1];
        test.program = argv[first + 2];
        test.expected = without_cr(read(argv[first + 3]));
        test.scratch = argv[first + 4];
        test.alchemy_fx = argv[first + 5];
        for (int index = first + 6; index < argc; ++index) test.runtimes.emplace_back(argv[index]);
        std::filesystem::create_directories(test.scratch);
        in_process(test);
        if (test.in_process) run_in_process(test);
        fx(test);
        std::cout << "Alchemy runtime " << test.install_name << " tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
