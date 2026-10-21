// Descriptors of the root guest process (alchemy --fd N=MODE:TARGET,
// include/alchemy/darwin_process.hpp): this driver starts the root_fds guest
// fixture with host objects at descriptors 3 and 4 (inherited anonymous
// pipes, named pipes, files) and checks what the guest read and wrote, the
// descriptors' access modes, close and end of file in both directions, a guest
// child inheriting them, and the launcher's errors for bad specs.
//
// Usage: alchemy_root_descriptor_tests <alchemy(.exe|-fx.exe)> <root_fds>
#define NOMINMAX
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Outcome { DWORD status{}; std::string output; };

std::wstring wide(const std::string& text) {
    std::wstring result(text.size(), L'\0');
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), static_cast<int>(result.size()));
    result.resize(static_cast<std::size_t>(length));
    return result;
}

std::wstring quote(const std::string& argument) {
    if (!argument.empty() && argument.find_first_of(" \t\"") == std::string::npos) return wide(argument);
    std::wstring text = L"\"";
    for (const auto ch : wide(argument)) { if (ch == L'"') text += L'\\'; text += ch; }
    return text + L"\"";
}

// Runs `command` in a kill-on-close job with stdout/stderr captured. `during`
// runs while the process is alive (it serves the guest's pipes).
template <class During>
Outcome run(const std::vector<std::string>& command, During during, DWORD timeout_ms = 120'000) {
    const auto job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 0)) throw std::runtime_error("CreatePipe failed");
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    std::wstring line;
    for (auto argument : command) {
        if (line.empty()) for (auto& c : argument) if (c == '/') c = '\\';
        line += (line.empty() ? L"" : L" ") + quote(argument);
    }
    const auto input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = write;
    startup.hStdError = write;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
    CloseHandle(write);
    CloseHandle(input);
    if (!created) { CloseHandle(read); CloseHandle(job); throw std::runtime_error("cannot start the Alchemy process"); }
    AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    Outcome outcome;
    std::thread reader([&] {
        char buffer[4096]; DWORD count = 0;
        while (ReadFile(read, buffer, sizeof(buffer), &count, nullptr) && count) outcome.output.append(buffer, count);
    });
    during();
    const bool timed_out = WaitForSingleObject(process.hProcess, timeout_ms) != WAIT_OBJECT_0;
    if (timed_out) { TerminateProcess(process.hProcess, 0xdead); WaitForSingleObject(process.hProcess, INFINITE); }
    GetExitCodeProcess(process.hProcess, &outcome.status);
    CloseHandle(job);
    reader.join();
    CloseHandle(read);
    if (timed_out) outcome.output += "\n[test: timed out]";
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return outcome;
}
Outcome run(const std::vector<std::string>& command) { return run(command, [] {}); }

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string hex(HANDLE handle) {
    std::ostringstream text;
    text << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(handle);
    return text.str();
}

std::string read_to_end(HANDLE handle) {
    std::string result;
    char buffer[4096]; DWORD count = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &count, nullptr) && count) result.append(buffer, count);
    return result;
}

std::string file_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text; text << file.rdbuf();
    return text.str();
}

void put_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

int failures = 0;
void check(bool ok, const std::string& name, const Outcome& outcome, const std::string& detail = {}) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << name << "\n";
    if (!ok) {
        ++failures;
        std::cout << "    status " << outcome.status << (detail.empty() ? "" : ", " + detail) << "\n" << outcome.output << "\n";
    }
}

// Inherited anonymous pipes: the host writes fd 3's data and closes its end
// (the guest reads EOF); the guest's close of fd 4 is the host reader's EOF.
void anonymous_pipes(const std::string& alchemy, const std::string& fixture) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE in_read{}, in_write{}, out_read{}, out_write{};
    CreatePipe(&in_read, &in_write, &inherit, 0);
    CreatePipe(&out_read, &out_write, &inherit, 0);
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    std::string received;
    const auto outcome = run({alchemy, "--fd", "3=r:handle:" + hex(in_read), "--fd", "4=w:handle:" + hex(out_write), fixture, "echo"}, [&] {
        CloseHandle(in_read); CloseHandle(out_write);   // the child has its own copies
        DWORD put = 0;
        WriteFile(in_write, "ping\n", 5, &put, nullptr);
        CloseHandle(in_write);
        received = read_to_end(out_read);
    });
    CloseHandle(out_read);
    check(outcome.status == 0 && contains(outcome.output, "ok echo") && received == "got:ping\n",
          "inherited anonymous pipes at fd 3/4: read, write, close, EOF", outcome, "fd 4 delivered '" + received + "'");
}

// Named pipes by path: the guest opens them as a client.
void named_pipes(const std::string& alchemy, const std::string& fixture) {
    const auto base = "\\\\.\\pipe\\alchemy-root-fds-" + std::to_string(GetCurrentProcessId());
    const auto in_name = base + "-in", out_name = base + "-out";
    auto in = CreateNamedPipeW(wide(in_name).c_str(), PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
    const auto out = CreateNamedPipeW(wide(out_name).c_str(), PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
    std::string received;
    const auto outcome = run({alchemy, "--fd", "3=r:" + in_name, "--fd", "4=w:" + out_name, fixture, "echo"}, [&] {
        // The launcher opens both before the guest starts.
        const BOOL in_connected = ConnectNamedPipe(in, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        const BOOL out_connected = ConnectNamedPipe(out, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!in_connected || !out_connected) return;
        DWORD put = 0;
        WriteFile(in, "{\"id\":1}\0", 9, &put, nullptr);
        FlushFileBuffers(in);   // until the guest has read it
        CloseHandle(in);        // end of file for the guest's reader
        in = nullptr;
        received = read_to_end(out);
    });
    if (in) CloseHandle(in);
    CloseHandle(out);
    check(outcome.status == 0 && contains(outcome.output, "ok echo") && received == std::string("got:{\"id\":1}\0", 13),
          "named pipes at fd 3/4 by path (NUL-terminated messages)", outcome, "fd 4 delivered " + std::to_string(received.size()) + " bytes");
}

void files(const std::string& alchemy, const std::string& fixture, const std::filesystem::path& scratch) {
    const auto input = scratch / "input.txt", output = scratch / "output.txt";
    put_file(input, "file data\n");
    put_file(output, "old contents that w truncates\n");
    auto outcome = run({alchemy, "--fd", "3=r:" + input.string(), "--fd", "4=w:" + output.string(), fixture, "echo"});
    check(outcome.status == 0 && contains(outcome.output, "ok echo") && file_text(output) == "got:file data\n",
          "files at fd 3 (r) and 4 (w truncates)", outcome, "output '" + file_text(output) + "'");
    const auto created = scratch / "created.txt";
    std::filesystem::remove(created);
    outcome = run({alchemy, "--fd", "3=r:" + input.string(), "--fd", "4=w:" + created.string(), fixture, "echo"});
    check(outcome.status == 0 && file_text(created) == "got:file data\n", "w creates a missing file", outcome);
    put_file(output, "first\n");
    outcome = run({alchemy, "--fd", "4=a:" + output.string(), fixture, "append"});
    check(outcome.status == 0 && contains(outcome.output, "ok append") && file_text(output) == "first\nmore\n",
          "a: O_WRONLY|O_APPEND", outcome, "output '" + file_text(output) + "'");
    put_file(output, "abc");
    outcome = run({alchemy, "--fd", "3=rw:" + output.string(), fixture, "rw"});
    check(outcome.status == 0 && contains(outcome.output, "ok rw") && file_text(output) == "abcrw:abc",
          "rw: O_RDWR, read then write at the position", outcome, "output '" + file_text(output) + "'");
}

void inheritance(const std::string& alchemy, const std::string& fixture, const std::filesystem::path& scratch) {
    const auto output = scratch / "inherit.txt";
    const auto outcome = run({alchemy, "--fd", "4=w:" + output.string(), fixture, "inherit"});
    check(outcome.status == 0 && contains(outcome.output, "ok inherit") && file_text(output) == "child\nparent\n",
          "a guest child inherits fd 4 through its descriptor table", outcome, "output '" + file_text(output) + "'");
}

void errors(const std::string& alchemy, const std::string& fixture, const std::filesystem::path& scratch) {
    auto outcome = run({alchemy, fixture, "absent"});
    check(outcome.status == 0 && contains(outcome.output, "ok absent"), "without --fd, fds 3 and 4 are closed", outcome);
    const auto input = (scratch / "input.txt").string();
    const struct { std::vector<std::string> spec; const char* message; const char* name; } cases[] = {
        {{"--fd", "1=r:" + input}, "the descriptor must be 3..4095", "--fd 1 (a standard stream) is refused"},
        {{"--fd", "3=x:" + input}, "the mode must be r, w, a or rw", "an unknown mode is refused"},
        {{"--fd", "3" + input}, "expected N=MODE:TARGET", "a spec without N= is refused"},
        {{"--fd", "3=r:" + (scratch / "missing.txt").string()}, "cannot open (not found)", "a missing file for r is refused"},
        {{"--fd", "3=r:handle:0x7ff0"}, "not an open file or pipe handle", "a handle that is not open is refused"},
        {{"--fd", "3=r:" + input, "--fd", "3=r:" + input}, "given twice", "the same descriptor twice is refused"},
    };
    for (const auto& [spec, message, name] : cases) {
        std::vector<std::string> command{alchemy};
        command.insert(command.end(), spec.begin(), spec.end());
        command.insert(command.end(), {fixture, "absent"});
        outcome = run(command);
        check(outcome.status == 1 && contains(outcome.output, message) && !contains(outcome.output, "ok absent"), name, outcome);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: alchemy_root_descriptor_tests <alchemy> <root_fds>\n";
        return 2;
    }
    const std::string alchemy = argv[1], fixture = argv[2];
    char temp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, temp);
    const auto scratch = std::filesystem::path(temp) / ("alchemy-root-fds-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(scratch);
    try {
        anonymous_pipes(alchemy, fixture);
        named_pipes(alchemy, fixture);
        files(alchemy, fixture, scratch);
        inheritance(alchemy, fixture, scratch);
        errors(alchemy, fixture, scratch);
    } catch (const std::exception& error) {
        std::cout << "  FAIL " << error.what() << "\n";
        ++failures;
    }
    std::error_code ignored;
    std::filesystem::remove_all(scratch, ignored);
    std::cout << (failures ? "FAILED: " : "passed, failures: ") << failures << "\n";
    return failures ? 1 : 0;
}
