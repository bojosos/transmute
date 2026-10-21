// Guest libexpat runtime tests.
//
// Usage: alchemy_expat_tests <expat_app> <expat runtime dir> <scratch dir> [alchemy-fx.exe]
//
// Without alchemy-fx: runs expat_app in-process under alchemy (interpreter and
// JIT, default and rebased load bases), checks doctor, and checks that the
// /usr/lib/libexpat.1.dylib identity is refused without matching provenance.
// With alchemy-fx: runs expat_app in alchemy-fx processes at both load bases.
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
    "Expat 2.8.5: elements 4 depth 2 attributes 4 comments 1 pi 1 cdata 1 doctype installer default 77 lines 10 "
    "text f\xC3\xBCrBlendera<b&ccaf\xC3\xA9\xE2\x98\xBA&<done> checksum 471889384\n"
    "Expat error 7 (mismatched tag) line 3; namespace urn:alchemy|c; encoding \xD1\x81\xD1\x82\n";

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
    options.instruction_limit = 200'000'000;
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
    for (const bool jit : {false, true}) for (const bool rebased : {false, true}) {
        alchemy::RunOptions options;
        options.jit = options.jit_stats = jit;
        options.runtime_directories.push_back(runtime);
        if (rebased) options.load_base = 0x120000000ULL;
        const auto result = run(program, options);
        const auto mode = std::string(jit ? "JIT" : "interpreter") + (rebased ? " rebased" : " default base");
        require(result.status == 0 && result.out == expected && result.err.empty(),
                "expat " + mode + " mismatch: status " + std::to_string(result.status) + " output " + result.out + result.err);
        require(!jit || result.native != 0, "expat " + mode + ": no translated guest code executed");
        std::cout << "  ok   alchemy " << mode << '\n';
    }
}

void copy_runtime(const std::filesystem::path& runtime, const std::filesystem::path& target, bool provenance) {
    std::filesystem::remove_all(target);
    std::filesystem::create_directories(target);
    std::filesystem::copy_file(runtime / "libexpat.1.dylib", target / "libexpat.1.dylib");
    if (provenance) std::filesystem::copy_file(runtime / "provenance.json", target / "provenance.json");
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

void hashes(const std::filesystem::path& scratch) {
    const auto file = scratch / "sha256.bin";
    const std::pair<std::string, std::string> vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {std::string(1'000'000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}};
    for (const auto& [text, digest] : vectors) {
        write(file, text);
        require(alchemy::sha256_file(file) == digest, "SHA-256 mismatch for a " + std::to_string(text.size()) + "-byte vector");
    }
    std::cout << "  ok   SHA-256 vectors\n";
}

void in_process(const std::filesystem::path& program, const std::filesystem::path& runtime, const std::filesystem::path& scratch) {
    hashes(scratch);
    require(alchemy::runtime_library_identity("/usr/lib/libexpat.1.dylib") != nullptr, "libexpat identity is not allow-listed");
    require(alchemy::runtime_library_identity("/usr/lib/libxml2.2.dylib") == nullptr, "unexpected allow-listed identity");
    require(alchemy::runtime_library_identity("/usr/lib/libexpat.dylib") == nullptr, "unversioned name must not be allow-listed");
    positive(program, runtime);

    alchemy::RunOptions doctor;
    doctor.runtime_directories.push_back(runtime);
    std::ostringstream report;
    require(alchemy::inspect_program(program, doctor, true, false, true, report) == 0, "doctor rejected the expat fixture: " + report.str());
    require(report.str().find("\"identity\":\"/usr/lib/libexpat.1.dylib\",\"status\":\"guest\"") != std::string::npos,
            "doctor did not resolve the runtime identity: " + report.str());
    std::cout << "  ok   doctor resolves /usr/lib/libexpat.1.dylib\n";

    alchemy::RunOptions none;
    expect_refused("no runtime directory", program, none, "missing guest dylib: /usr/lib/libexpat.1.dylib");

    const auto plain = scratch / "no-provenance";
    copy_runtime(runtime, plain, false);
    alchemy::RunOptions options;
    options.runtime_directories.push_back(plain);
    expect_refused("missing provenance", program, options, "missing provenance.json");

    const auto tampered = scratch / "tampered";
    copy_runtime(runtime, tampered, true);
    auto bytes = read(tampered / "libexpat.1.dylib");
    bytes.back() = static_cast<char>(bytes.back() ^ 0x5a);
    write(tampered / "libexpat.1.dylib", bytes);
    options.runtime_directories = {tampered};
    expect_refused("modified library", program, options, "SHA-256 differs from provenance.json");
    std::ostringstream tampered_report;
    require(alchemy::inspect_program(program, options, true, false, false, tampered_report) != 0 &&
            tampered_report.str().find("SHA-256 differs") != std::string::npos,
            "doctor accepted a modified runtime: " + tampered_report.str());
    std::cout << "  ok   doctor reports a modified runtime\n";

    const auto foreign = scratch / "foreign-source";
    copy_runtime(runtime, foreign, true);
    auto provenance = read(foreign / "provenance.json");
    const std::string upstream = "https://github.com/libexpat/libexpat";
    const auto at = provenance.find(upstream);
    require(at != std::string::npos, "provenance.json lacks the upstream source");
    provenance.replace(at, upstream.size(), "https://example.invalid/expat");
    write(foreign / "provenance.json", provenance);
    options.runtime_directories = {foreign};
    expect_refused("foreign upstream", program, options, "does not record upstream https://github.com/libexpat/libexpat");

    // A valid copy offered as an ordinary guest dylib directory is never used
    // for an Apple identity.
    alchemy::RunOptions dylib_dir;
    dylib_dir.dylib_directories.push_back(runtime);
    expect_refused("dylib directory", program, dylib_dir, "missing guest dylib: /usr/lib/libexpat.1.dylib");
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

void fx(const std::string& alchemy_fx, const std::filesystem::path& program, const std::filesystem::path& runtime) {
    for (const bool rebased : {false, true}) {
        std::vector<std::string> command{alchemy_fx, "--runtime-dir", runtime.string()};
        if (rebased) { command.push_back("--load-base"); command.push_back("0x120000000"); }
        command.push_back(program.string());
        const auto result = process(command);
        const std::string mode = rebased ? "rebased" : "default base";
        require(result.status == 0 && result.out == expected,
                "alchemy-fx " + mode + " mismatch: status " + std::to_string(result.status) + " output " + result.out + result.err);
        std::cout << "  ok   alchemy-fx " << mode << '\n';
    }
    const auto refused = process({alchemy_fx, program.string()});
    require(refused.status != 0 && refused.out.empty() &&
            refused.err.find("missing guest dylib: /usr/lib/libexpat.1.dylib") != std::string::npos,
            "alchemy-fx without a runtime directory: status " + std::to_string(refused.status) + " " + refused.out + refused.err);
    std::cout << "  ok   alchemy-fx refuses a missing runtime\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4 && argc != 5) throw std::runtime_error("usage: alchemy_expat_tests <expat_app> <runtime dir> <scratch dir> [alchemy-fx]");
        const std::filesystem::path program = argv[1], runtime = argv[2], scratch = argv[3];
        std::filesystem::create_directories(scratch);
        if (argc == 5) fx(argv[4], program, runtime);
        else in_process(program, runtime, scratch);
        std::cout << "Alchemy guest libexpat tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
