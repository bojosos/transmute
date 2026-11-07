// Guest libxml2 runtime tests.
//
// Usage: alchemy_libxml2_tests <libxml2_app> <libxml2 runtime dir> <scratch dir> [alchemy-fx.exe]
//
// Without alchemy-fx: runs libxml2_app in-process under alchemy (JIT, default and
// rebased load bases), checks doctor, checks that without the runtime the
// declaration-only identity leaves the fixture's imports unresolved, and that a
// modified library is refused. With alchemy-fx: runs libxml2_app in an alchemy-fx
// process.
#define NOMINMAX
#include <windows.h>

#include "alchemy/inspection.hpp"
#include "alchemy/process.hpp"
#include "alchemy/runtime_libraries.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

const std::string expected =
    "tree: root catalog owner Alchemy items 2 sum 7 second <second> note caf\xC3\xA9\n"
    "dump: 120 bytes\n"
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<a:plist xmlns:a=\"urn:alchemy\" version=\"1.0\">\n"
    "  <a:key>x&lt;y</a:key>\n"
    "</a:plist>\n"
    "utf-32be: \xC3\xA9\xE2\x98\xBA\n"
    "html: paragraphs 2\n"
    "reader: elements 4; writer: <?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<w k=\"v&amp;\">body</w>\n"
    "error: doc null code 76 calls yes\n";

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Run { std::uint32_t status{}; std::string out, err; std::uint64_t native{}; };

Run run(const std::filesystem::path& program, alchemy::RunOptions options) {
    Run result;
    const auto writer = [&](unsigned descriptor, std::span<const std::uint8_t> bytes) -> alchemy::OutputResult {
        (descriptor == 1 ? result.out : result.err).append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return {bytes.size(), 0};
    };
    options.instruction_limit = 2'000'000'000;
    const auto process = alchemy::run_program(program, {}, options, writer);
    result.status = process.exit_code;
    result.native = process.jit.native_guest_instructions;
    return result;
}

std::string failure(const std::filesystem::path& program, alchemy::RunOptions options) {
    try { const auto result = run(program, std::move(options)); }
    catch (const std::exception& error) { return error.what(); }
    return {};
}

void positive(const std::filesystem::path& program, const std::filesystem::path& runtime) {
    for (const bool rebased : {false, true}) {
        alchemy::RunOptions options;
        options.jit = options.jit_stats = true;
        options.runtime_directories.push_back(runtime);
        if (rebased) options.load_base = 0x120000000ULL;
        const auto result = run(program, options);
        const auto mode = std::string("JIT") + (rebased ? " rebased" : " default base");
        require(result.status == 0 && result.out == expected && result.err.empty(),
                "libxml2 " + mode + " mismatch: status " + std::to_string(result.status) + " output " + result.out + result.err);
        require(result.native != 0, "libxml2 " + mode + ": no translated guest code executed");
        std::cout << "  ok   alchemy " << mode << '\n';
    }
}

std::string read(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& file, const std::string& text) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << text;
}

void expect_refused(const std::string& name, const std::filesystem::path& program, const alchemy::RunOptions& options,
                    const std::string& reason) {
    const auto message = failure(program, options);
    require(message.find(reason) != std::string::npos, name + ": expected \"" + reason + "\", got \"" + message + "\"");
    std::cout << "  ok   refused: " << name << '\n';
}

void in_process(const std::filesystem::path& program, const std::filesystem::path& runtime, const std::filesystem::path& scratch) {
    const auto* identity = alchemy::runtime_library_identity("/usr/lib/libxml2.2.dylib");
    require(identity != nullptr, "libxml2 identity is not allow-listed");
    require(identity->provider_fallback, "libxml2 must stay a declaration-only identity without its runtime");
    positive(program, runtime);

    alchemy::RunOptions doctor;
    doctor.runtime_directories.push_back(runtime);
    std::ostringstream report;
    require(alchemy::inspect_program(program, doctor, true, false, true, report) == 0, "doctor rejected the libxml2 fixture: " + report.str());
    require(report.str().find("\"identity\":\"/usr/lib/libxml2.2.dylib\",\"status\":\"guest\"") != std::string::npos,
            "doctor did not resolve the runtime identity: " + report.str());
    std::cout << "  ok   doctor resolves /usr/lib/libxml2.2.dylib\n";

    // Without the runtime the identity is declaration-only, so the fixture's
    // strong imports stay unresolved.
    alchemy::RunOptions none;
    expect_refused("no runtime directory", program, none, "unresolved strong import _xml");

    const auto tampered = scratch / "tampered";
    std::filesystem::remove_all(tampered);
    std::filesystem::create_directories(tampered);
    auto bytes = read(runtime / "libxml2.2.dylib");
    bytes.back() = static_cast<char>(bytes.back() ^ 0x5a);
    write(tampered / "libxml2.2.dylib", bytes);
    std::filesystem::copy_file(runtime / "provenance.json", tampered / "provenance.json");
    alchemy::RunOptions options;
    options.runtime_directories = {tampered};
    expect_refused("modified library", program, options, "SHA-256 differs from provenance.json");
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
    DWORD status = 0;
    GetExitCodeProcess(info.hProcess, &status);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    if (timed_out) throw std::runtime_error("alchemy-fx timed out");
    result.status = status;
    return result;
}

void fx(const std::filesystem::path& program, const std::filesystem::path& runtime, const std::filesystem::path& exe) {
    const auto result = process({exe.string(), "--runtime-dir", runtime.string(), program.string()});
    require(result.status == 0 && result.out == expected,
            "alchemy-fx libxml2 mismatch: status " + std::to_string(result.status) + " output " + result.out + result.err);
    std::cout << "  ok   alchemy-fx\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4 && argc != 5) throw std::runtime_error("usage: alchemy_libxml2_tests <libxml2_app> <runtime dir> <scratch dir> [alchemy-fx.exe]");
        const std::filesystem::path program = argv[1], runtime = argv[2], scratch = argv[3];
        std::filesystem::create_directories(scratch);
        if (argc == 5) fx(program, runtime, argv[4]);
        else in_process(program, runtime, scratch);
        std::cout << "alchemy_libxml2_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "alchemy_libxml2_tests failed: " << error.what() << '\n';
        return 1;
    }
}
