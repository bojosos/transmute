#include "alchemy/cross_process_memory.hpp"

#include "alchemy/darwin_mach_vm.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/process_context.hpp"
#include "alchemy/host/sections.hpp"
#include "alchemy/host/vm.hpp"

#include <cstring>
#include <new>
#include <stdexcept>
#include <vector>

namespace alchemy::xmem {
namespace {

// <mach/kern_return.h>, <mach/vm_prot.h>
constexpr std::uint32_t kern_protection_failure = 2, kern_invalid_argument = 4, kern_not_supported = 46;
constexpr std::uint32_t prot_read = 1, prot_write = 2, prot_execute = 4;
constexpr std::uint64_t page = 16384;

std::uint64_t round_page(std::uint64_t value) { return (value + page - 1) & ~(page - 1); }

// A section's shared handle: the host section handle value, closed with the last copy.
std::shared_ptr<void> owned(host::Handle handle) {
    return std::shared_ptr<void>(reinterpret_cast<void*>(handle.value), [](void* value) {
        if (value) host::close(host::Handle{reinterpret_cast<std::uintptr_t>(value)});
    });
}
host::Handle handle_of(const Section& section) { return host::Handle{reinterpret_cast<std::uintptr_t>(section.handle.get())}; }
host::Handle handle_of(void* engine_section) { return host::Handle{reinterpret_cast<std::uintptr_t>(engine_section)}; }

// A host-only view of the section (never guest memory) for copying bytes.
struct HostView {
    std::uintptr_t base{};
    std::uint64_t skip{};
    HostView(host::Handle section, std::uint64_t offset, std::uint64_t size, bool writable) {
        const auto granularity = static_cast<std::uint64_t>(host::vm::page_info().allocation_granularity);
        const auto start = offset & ~(granularity - 1);
        skip = offset - start;
        if (const auto view = host::sections::map(section, start, static_cast<std::size_t>(skip + size), writable)) base = *view;
    }
    HostView(const HostView&) = delete;
    HostView& operator=(const HostView&) = delete;
    ~HostView() { if (base) static_cast<void>(host::sections::unmap(base)); }
    [[nodiscard]] std::uint8_t* bytes() const { return reinterpret_cast<std::uint8_t*>(base) + skip; }
};

} // namespace

std::optional<Section> create_section(std::uint64_t size) {
    if (!size) return std::nullopt;
    const auto length = round_page(size);
    const auto handle = host::sections::create_section(length);
    if (!handle) return std::nullopt;
    return Section{owned(*handle), length, true};
}

std::optional<std::pair<Section, std::uint64_t>> section_of(ProcessContext& context, std::uint64_t address,
                                                            std::uint64_t size) {
    if (!size || address > UINT64_MAX - size) return std::nullopt;
    auto& engine = context.calling_engine();
    std::uint64_t offset{}, extent{}, last_offset{}, last_extent{};
    bool writable{}, last_writable{};
    // The engine hands out duplicates of its host section handles.
    const auto first = handle_of(engine_duplicate_section(engine, address, offset, extent, writable));
    if (!first) return std::nullopt;
    auto section = owned(first);
    // The whole range must be one contiguous run of the same section.
    const auto last = handle_of(engine_duplicate_section(engine, address + size - 1, last_offset, last_extent, last_writable));
    if (!last) return std::nullopt;
    const auto last_owned = owned(last);
    if (!host::sections::same(first, last) || last_offset - offset != size - 1) return std::nullopt;
    for (auto at = (address & ~(page - 1)) + page; at < address + size; at += page) {
        std::uint64_t middle_offset{}, middle_extent{};
        bool middle_writable{};
        const auto middle = handle_of(engine_duplicate_section(engine, at, middle_offset, middle_extent, middle_writable));
        if (!middle) return std::nullopt;
        const auto middle_owned = owned(middle);
        if (!host::sections::same(first, middle) || middle_offset - offset != at - address) return std::nullopt;
    }
    return std::pair{Section{section, extent, writable}, offset};
}

std::optional<Token> export_section(const Section& section, std::uint64_t offset, std::uint64_t size,
                                    std::uint32_t host_pid, bool writable) {
    if (!section.handle || !size || offset > section.size || size > section.size - offset) return std::nullopt;
    writable = writable && section.writable;
    // A read-only export cannot be raised to writable by the receiver.
    const auto remote = host::sections::duplicate_into(handle_of(section), host_pid, writable);
    if (!remote) return std::nullopt;
    return Token{*remote, offset, size, writable};
}

std::optional<Token> export_copy(transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t size,
                                 std::uint32_t host_pid) {
    const auto section = create_section(size);
    if (!section) return std::nullopt;
    {
        HostView view(handle_of(*section), 0, size, true);
        if (!view.base) return std::nullopt;
        memory.read_bytes(address, std::span(view.bytes(), static_cast<std::size_t>(size)));
    }
    return export_section(*section, 0, size, host_pid, false);
}

Section import_token(const Token& token) {
    return Section{owned(host::Handle{static_cast<std::uintptr_t>(token.handle)}), token.offset + token.size, token.writable};
}

void discard_token(const Token& token) {
    if (token.handle) host::close(host::Handle{static_cast<std::uintptr_t>(token.handle)});
}

std::optional<std::uint64_t> map_section(ProcessContext& context, const Section& section, std::uint64_t offset,
                                         std::uint64_t size, std::uint64_t address, std::uint32_t protection,
                                         std::uint32_t& kern_result, std::uint32_t maximum, bool overwrite) {
    if (!section.handle || !size || offset % page || offset > section.size || size > section.size - offset ||
        (protection & ~(prot_read | prot_write | prot_execute))) {
        kern_result = kern_invalid_argument;
        return std::nullopt;
    }
    if ((protection & prot_execute) || ((protection & prot_write) && !section.writable) || (protection & ~maximum)) {
        kern_result = kern_protection_failure;
        return std::nullopt;
    }
    kern_result = map_darwin_section(context, section.handle.get(), offset, size, section.writable, address, protection,
                                     maximum, overwrite);
    if (kern_result) return std::nullopt;
    return address;
}

std::optional<std::uint64_t> receive_copy(ProcessContext& context, const Token& token, std::uint32_t& kern_result) {
    const auto section = import_token(token);
    if (!token.size) {
        kern_result = kern_invalid_argument;
        return std::nullopt;
    }
    HostView view(handle_of(section), token.offset, token.size, false);
    if (!view.base) {
        kern_result = kern_invalid_argument;
        return std::nullopt;
    }
    std::uint64_t address{};
    kern_result = allocate_darwin_vm(context, token.size, address);
    if (kern_result) return std::nullopt;
    context.calling_engine().write_bytes(address, std::span<const std::uint8_t>(view.bytes(), static_cast<std::size_t>(token.size)));
    return address;
}

} // namespace alchemy::xmem
