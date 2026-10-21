// Cross-process memory (include/alchemy/cross_process_memory.hpp) between real
// alchemy-fx host processes: this executable starts copies of itself as the
// other side and passes section tokens over a pipe.
// - share: vm_allocate memory exported to a child; stores in either process
//   are seen in the other, also through a vm_remap alias in the child.
// - readonly: a read-only export cannot be mapped writable; no PROT_EXEC.
// - creator-exits: the creating process exits; the receiver maps the section
//   afterwards and finds its contents.
// - receiver-exits: the child maps, writes and exits; the parent's mapping
//   keeps the memory and the child's store.
// - ool: out-of-line copy semantics (export_copy / receive_copy).
// - discard: an undelivered token's handle is closed.
// - views: two views of one section in one process, VM_FLAGS_OVERWRITE over
//   Mach-owned pages, maximum protection.
#include "alchemy/cross_process_memory.hpp"
#include "alchemy/darwin_mach_vm.hpp"
#include "alchemy/gui/tests/fill_fixture.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace {

using fill_test::Fixture;
using fill_test::require;
using Address = std::uint64_t;
namespace xmem = alchemy::xmem;
constexpr std::string_view sys = "/usr/lib/libSystem.B.dylib";
constexpr std::uint64_t page = 16384;
constexpr std::uint32_t task_self = 0xa1c1;
constexpr std::uint32_t read = 1, write = 2, execute = 4;

std::string token_text(const xmem::Token& token) {
    std::ostringstream out;
    out << token.handle << ' ' << token.offset << ' ' << token.size << ' ' << (token.writable ? 1 : 0);
    return out.str();
}
xmem::Token parse_token(const std::string& line) {
    std::istringstream in(line);
    xmem::Token token;
    int writable = 0;
    in >> token.handle >> token.offset >> token.size >> writable;
    require(!in.fail(), "bad token line: " + line);
    token.writable = writable != 0;
    return token;
}

std::string read_line(HANDLE input) {
    std::string line;
    char c = 0;
    DWORD got = 0;
    while (ReadFile(input, &c, 1, &got, nullptr) && got == 1) {
        if (c == '\n') return line;
        if (c != '\r') line += c;
    }
    return line;
}
void write_line(HANDLE output, const std::string& line) {
    const auto text = line + "\n";
    DWORD written = 0;
    require(WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size(),
            "pipe write failed");
}

// A copy of this executable running `mode` with pipes on stdin and stdout.
struct Child {
    PROCESS_INFORMATION info{};
    HANDLE to{}, from{};
    explicit Child(const std::string& mode) {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE child_in{}, child_out{};
        require(CreatePipe(&child_in, &to, &inherit, 0) && CreatePipe(&from, &child_out, &inherit, 0), "CreatePipe failed");
        SetHandleInformation(to, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(from, HANDLE_FLAG_INHERIT, 0);
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(self) + L"\" " + std::wstring(mode.begin(), mode.end()) + L" " +
                               std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = child_in;
        startup.hStdOutput = child_out;
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &info),
                "CreateProcess failed");
        CloseHandle(child_in);
        CloseHandle(child_out);
    }
    ~Child() {
        if (info.hProcess) {
            if (WaitForSingleObject(info.hProcess, 30000) != WAIT_OBJECT_0) TerminateProcess(info.hProcess, 99);
            CloseHandle(info.hProcess);
            CloseHandle(info.hThread);
        }
        CloseHandle(to);
        CloseHandle(from);
    }
    std::uint32_t pid() const { return info.dwProcessId; }
    std::string line() { return read_line(from); }
    void send(const std::string& text) { write_line(to, text); }
    DWORD wait() {
        require(WaitForSingleObject(info.hProcess, 60000) == WAIT_OBJECT_0, "child did not exit");
        DWORD code = 0;
        GetExitCodeProcess(info.hProcess, &code);
        return code;
    }
};

Address allocate(Fixture& f, std::uint64_t size) {
    Address address{};
    require(alchemy::allocate_darwin_vm(f.process, size, address) == 0 && address, "vm_allocate failed");
    return address;
}
std::uint32_t deallocate(Fixture& f, Address address, std::uint64_t size) {
    return static_cast<std::uint32_t>(f.call(sys, "_vm_deallocate", {task_self, address, size}));
}
std::uint32_t protect(Fixture& f, Address address, std::uint64_t size, std::uint32_t protection) {
    return static_cast<std::uint32_t>(f.call(sys, "_vm_protect", {task_self, address, size, 0, protection}));
}
Address map(Fixture& f, const xmem::Section& section, std::uint64_t offset, std::uint64_t size, std::uint32_t protection) {
    std::uint32_t result = 0;
    const auto address = xmem::map_section(f.process, section, offset, size, 0, protection, result);
    require(address && result == 0, "map_section failed: " + std::to_string(result));
    return *address;
}

// ---- parent side ------------------------------------------------------------------

void share(Fixture& f) {
    const auto memory = allocate(f, 2 * page);
    f.engine.write64(memory + page + 8, 0x5ec7105ULL);
    const auto found = xmem::section_of(f.process, memory, 2 * page);
    require(found.has_value() && found->second == 0, "section_of(vm_allocate memory) failed");
    require(!xmem::section_of(f.process, f.block(64), 64), "section_of accepted private memory");
    Child child("child-share");
    const auto token = xmem::export_section(found->first, found->second, 2 * page, child.pid());
    require(token.has_value(), "export_section failed");
    child.send(token_text(*token));
    require(child.line() == "wrote", "child did not map and write");
    require(f.engine.read64(memory + 16) == 0xc41dULL, "the child's store is not visible in the parent");
    require(f.engine.read64(memory + page + 24) == 0xa11a5ULL, "the child's store through its alias is not visible");
    f.engine.write64(memory + 32, 0x9a7e47ULL);
    child.send("check");
    require(child.line() == "saw", "the parent's store is not visible in the child");
    require(child.wait() == 0, "child-share failed");
    require(deallocate(f, memory, 2 * page) == 0, "vm_deallocate of shared memory failed");
}

void readonly(Fixture& f) {
    const auto memory = allocate(f, page);
    f.engine.write64(memory, 0x4ead0ULL);
    const auto found = xmem::section_of(f.process, memory, page);
    require(found.has_value(), "section_of failed");
    Child child("child-readonly");
    const auto token = xmem::export_section(found->first, found->second, page, child.pid(), false);
    require(token.has_value() && !token->writable, "read-only export failed");
    child.send(token_text(*token));
    require(child.line() == "ok", "child-readonly reported a failure");
    require(child.wait() == 0, "child-readonly failed");
    require(deallocate(f, memory, page) == 0, "vm_deallocate failed");
}

void creator_exits(Fixture& f) {
    xmem::Token token;
    {
        Child child("child-create");
        token = parse_token(child.line());
        require(child.wait() == 0, "child-create failed");
    }
    // The creator is gone; the handle it duplicated into this process keeps the section.
    const auto section = xmem::import_token(token);
    const auto address = map(f, section, token.offset, token.size, read | write);
    require(f.read_text(address) == "made by a process that has exited", "the section lost its contents with its creator");
    require(f.engine.read64(address + 3 * page) == 0x3ULL, "the section's last page lost its contents");
    f.engine.write64(address + page, 0x1ULL);
    require(deallocate(f, address, token.size) == 0, "vm_deallocate of an imported section failed");
}

void receiver_exits(Fixture& f) {
    const auto memory = allocate(f, page);
    const auto found = xmem::section_of(f.process, memory, page);
    require(found.has_value(), "section_of failed");
    {
        Child child("child-store");
        const auto token = xmem::export_section(found->first, found->second, page, child.pid());
        require(token.has_value(), "export_section failed");
        child.send(token_text(*token));
        require(child.wait() == 0, "child-store failed");
    }
    require(f.engine.read64(memory + 40) == 0x570eULL, "the exited child's store is lost");
    f.engine.write64(memory + 48, 1);
    require(deallocate(f, memory, page) == 0, "vm_deallocate failed");
}

void ool(Fixture& f) {
    xmem::Token token;
    {
        Child child("child-ool");
        token = parse_token(child.line());
        require(child.wait() == 0, "child-ool failed");
    }
    require(!token.writable && token.size == 40000, "out-of-line token has the wrong shape");
    std::uint32_t result = 0;
    const auto address = xmem::receive_copy(f.process, token, result);
    require(address && result == 0, "receive_copy failed");
    for (std::uint64_t at = 0; at < token.size; at += 997)
        require(f.memory().read8(*address + at) == static_cast<std::uint8_t>(at * 7), "out-of-line bytes differ");
    f.memory().write8(*address, 1);   // a private, writable copy
    require(deallocate(f, *address, token.size) == 0, "vm_deallocate of out-of-line memory failed");
}

void discard(Fixture&) {
    const auto section = xmem::create_section(page);
    require(section.has_value(), "create_section failed");
    const auto token = xmem::export_section(*section, 0, page, GetCurrentProcessId());
    require(token.has_value(), "export to this process failed");
    DWORD flags = 0;
    const auto handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(token->handle));
    require(GetHandleInformation(handle, &flags), "exported handle is not open");
    xmem::discard_token(*token);
    require(!GetHandleInformation(handle, &flags), "discarded token's handle is still open");
    require(!xmem::export_section(*section, 0, page, 0xfffffff0U), "export to a missing process succeeded");
    require(!xmem::export_section(*section, page, page, GetCurrentProcessId()), "export past the section succeeded");
}

void views(Fixture& f) {
    const auto section = xmem::create_section(2 * page);
    require(section.has_value(), "create_section failed");
    const auto first = map(f, *section, 0, 2 * page, read | write);
    const auto second = map(f, *section, page, page, read | write);
    f.engine.write64(first + page + 8, 0x7e1ULL);
    require(f.engine.read64(second + 8) == 0x7e1ULL, "views of one section do not share pages");
    // VM_FLAGS_OVERWRITE: the section replaces vm_allocate pages at a fixed address.
    const auto target = allocate(f, 2 * page);
    f.engine.write64(target, 0xdeadULL);
    std::uint32_t result = 0;
    require(!xmem::map_section(f.process, *section, 0, 2 * page, target, read | write, result) && result == 3,
            "a fixed mapping over Mach pages without overwrite succeeded");
    require(xmem::map_section(f.process, *section, 0, 2 * page, target, read | write, result, read | write, true) == target &&
            result == 0, "overwrite of Mach-owned pages failed");
    require(f.engine.read64(target + page + 8) == 0x7e1ULL && f.engine.read64(target) == 0, "overwrite did not show the section");
    const auto foreign = f.block(2 * page) & ~(page - 1);
    require(!xmem::map_section(f.process, *section, 0, page, foreign + page, read, result, read, true) && result == 3,
            "overwrite replaced memory Mach VM does not own");
    // Maximum protection: read-only stays read-only; current may not exceed it.
    require(!xmem::map_section(f.process, *section, 0, page, 0, read | write, result, read) && result == 2,
            "current protection above the maximum was accepted");
    const auto locked = xmem::map_section(f.process, *section, 0, page, 0, read, result, read);
    require(locked && result == 0, "read-only maximum mapping failed");
    require(protect(f, *locked, page, read | write) != 0, "vm_protect exceeded the maximum protection");
    for (const auto [address, size] : {std::pair{first, 2 * page}, std::pair{second, page}, std::pair{target, 2 * page},
                                       std::pair{*locked, page}})
        require(deallocate(f, address, size) == 0, "vm_deallocate of a view failed");
}

// ---- child side -------------------------------------------------------------------

int child_share(Fixture& f) {
    const auto stdin_handle = GetStdHandle(STD_INPUT_HANDLE), stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    const auto token = parse_token(read_line(stdin_handle));
    const auto section = xmem::import_token(token);
    std::uint32_t result = 0;
    require(!xmem::map_section(f.process, section, 0, token.size, 0, read | execute, result) && result == 2,
            "cross-process memory was mapped executable");
    const auto address = map(f, section, token.offset, token.size, read | write);
    require(f.engine.read64(address + page + 8) == 0x5ec7105ULL, "child does not see the parent's bytes");
    f.engine.write64(address + 16, 0xc41dULL);
    // An alias of the imported memory (mach_vm_map from a memory-entry anchor does this).
    // vm_remap(task, &target, size, mask, VM_FLAGS_ANYWHERE, task, source, copy, &cur, &max, inherit).
    const auto slot = f.block(8), current = f.block(4), maximum = f.block(4);
    alchemy::CallRegs registers{};
    const Address arguments[] = {task_self, slot, page, 0, 1, task_self, address + page, 0};
    for (unsigned index = 0; index < 8; ++index) registers.x[index] = arguments[index];
    registers.push_stack(current);
    registers.push_stack(maximum);
    registers.push_stack(0);   // VM_INHERIT_SHARE
    require(f.regs(sys, "_vm_remap", registers).x0 == 0, "vm_remap of imported memory failed");
    const auto alias = f.engine.read64(slot);
    require(alias && alias != address + page && f.engine.read64(alias + 8) == 0x5ec7105ULL, "alias does not show the section");
    require((f.engine.read32(maximum) & execute) == 0, "alias of cross-process memory may become executable (current " +
            std::to_string(f.engine.read32(current)) + ", maximum " + std::to_string(f.engine.read32(maximum)) + ")");
    require(protect(f, address, page, read | write | execute) != 0, "vm_protect raised cross-process memory to executable");
    f.engine.write64(alias + 24, 0xa11a5ULL);
    require(f.engine.read64(address + page + 24) == 0xa11a5ULL, "store through the alias is not seen at the original");
    require(deallocate(f, alias, page) == 0, "vm_deallocate of the alias failed");
    write_line(stdout_handle, "wrote");
    require(read_line(stdin_handle) == "check", "parent did not ask to check");
    require(f.engine.read64(address + 32) == 0x9a7e47ULL, "child does not see the parent's later store");
    write_line(stdout_handle, "saw");
    require(deallocate(f, address, token.size) == 0, "vm_deallocate failed");
    return 0;
}

int child_readonly(Fixture& f) {
    const auto token = parse_token(read_line(GetStdHandle(STD_INPUT_HANDLE)));
    const auto section = xmem::import_token(token);
    std::uint32_t result = 0;
    require(!xmem::map_section(f.process, section, token.offset, token.size, 0, read | write, result) && result == 2,
            "read-only section mapped writable");
    const auto address = map(f, section, token.offset, token.size, read);
    require(f.engine.read64(address) == 0x4ead0ULL, "read-only view has the wrong bytes");
    require(protect(f, address, page, read | write) != 0, "vm_protect raised a read-only section to writable");
    require(deallocate(f, address, token.size) == 0, "vm_deallocate failed");
    write_line(GetStdHandle(STD_OUTPUT_HANDLE), "ok");
    return 0;
}

int child_create(Fixture& f, std::uint32_t parent) {
    const auto section = xmem::create_section(4 * page);
    require(section.has_value(), "create_section failed");
    const auto address = map(f, *section, 0, 4 * page, read | write);
    const std::string text = "made by a process that has exited";
    f.engine.write_bytes(address, std::vector<std::uint8_t>(text.begin(), text.end() + 1));
    f.engine.write64(address + 3 * page, 0x3ULL);
    const auto token = xmem::export_section(*section, 0, 4 * page, parent);
    require(token.has_value(), "export to the parent failed");
    write_line(GetStdHandle(STD_OUTPUT_HANDLE), token_text(*token));
    return 0;   // exits with the mapping live
}

int child_store(Fixture& f) {
    const auto token = parse_token(read_line(GetStdHandle(STD_INPUT_HANDLE)));
    const auto address = map(f, xmem::import_token(token), token.offset, token.size, read | write);
    f.engine.write64(address + 40, 0x570eULL);
    return 0;   // exits with the mapping live
}

int child_ool(Fixture& f, std::uint32_t parent) {
    const auto buffer = f.block(40000);
    for (std::uint64_t at = 0; at < 40000; ++at) f.memory().write8(buffer + at, static_cast<std::uint8_t>(at * 7));
    const auto token = xmem::export_copy(f.engine, buffer, 40000, parent);
    require(token.has_value(), "export_copy failed");
    f.memory().write8(buffer, 0xff);   // a later store does not reach the copy
    write_line(GetStdHandle(STD_OUTPUT_HANDLE), token_text(*token));
    return 0;
}

int child(std::string_view mode, std::uint32_t parent) {
    try {
        Fixture f;
        if (mode == "child-share") return child_share(f);
        if (mode == "child-readonly") return child_readonly(f);
        if (mode == "child-create") return child_create(f, parent);
        if (mode == "child-store") return child_store(f);
        if (mode == "child-ool") return child_ool(f, parent);
        std::cerr << "unknown mode " << mode << '\n';
        return 3;
    } catch (const std::exception& error) {
        std::cerr << mode << " FAILED: " << error.what() << '\n';
        return 1;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string_view(argv[1]).starts_with("child-"))
        return child(argv[1], static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10)));
    return fill_test::run("chrome xmem", {
        {"share", share},
        {"readonly", readonly},
        {"creator-exits", creator_exits},
        {"receiver-exits", receiver_exits},
        {"ool", ool},
        {"discard", discard},
        {"views", views},
    });
}
