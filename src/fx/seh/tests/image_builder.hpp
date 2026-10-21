#pragma once

// Builders for in-memory test images: code, .xdata and .pdata in one arena
// block whose start is the module base (so RVAs are offsets from it).

#include "fx/seh/seh.hpp"
#include "fx/seh/tests/seh_test_util.hpp"

#include <cstring>
#include <vector>

namespace fxseh_test {

struct ImageBuilder {
    std::uint64_t base = 0;
    std::uint64_t cursor = 0;
    std::size_t reserved = 0;

    explicit ImageBuilder(std::size_t reserve = 64 * 1024) : reserved(reserve) {
        base = alloc_guest(reserve, 0x10000);
        cursor = base;
    }
    std::uint64_t put(const void* data, std::size_t size, std::size_t align = 4) {
        cursor = (cursor + align - 1) & ~(std::uint64_t{align} - 1);
        const std::uint64_t address = cursor;
        if (size) std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), data, size);
        cursor += size;
        return address;
    }
    std::uint64_t reserve_bytes(std::size_t size, std::size_t align = 4) {
        cursor = (cursor + align - 1) & ~(std::uint64_t{align} - 1);
        const std::uint64_t address = cursor;
        cursor += size;
        return address;
    }
    std::uint32_t rva(std::uint64_t address) const { return static_cast<std::uint32_t>(address - base); }

    // Registers [base, base+reserved) as a guest module with the given .pdata.
    void register_as_module(std::uint64_t pdata, std::uint32_t entries, const wchar_t* name = L"test.dll") const {
        transmute::fx::GuestModule module{};
        module.base = base;
        module.size = reserved;
        module.pdata_rva = rva(pdata);
        module.pdata_size = entries * 8;
        module.name = name;
        register_module(module);
    }
};

// Builds a version-0 .xdata record and returns its address.
inline std::uint64_t build_xdata(ImageBuilder& image, std::uint32_t function_length_instrs,
                                 const std::vector<std::uint8_t>& codes, bool epilog_in_header,
                                 std::uint32_t epilog_field, const std::vector<std::uint32_t>& epilog_scopes = {},
                                 std::uint32_t handler_rva = 0,
                                 const std::vector<std::uint32_t>& handler_data = {}) {
    std::vector<std::uint32_t> words;
    const std::uint32_t code_words = static_cast<std::uint32_t>((codes.size() + 3) / 4);
    std::uint32_t header = function_length_instrs & 0x3ffff;
    header |= (handler_rva ? 1u : 0u) << 20;
    header |= (epilog_in_header ? 1u : 0u) << 21;
    header |= (epilog_field & 0x1f) << 22;
    header |= (code_words & 0x1f) << 27;
    words.push_back(header);
    if (!epilog_in_header)
        for (std::uint32_t scope : epilog_scopes) words.push_back(scope);
    std::vector<std::uint8_t> padded = codes;
    padded.resize(code_words * 4, 0xe3);
    for (std::uint32_t i = 0; i < code_words; ++i) {
        std::uint32_t word;
        std::memcpy(&word, padded.data() + i * 4, 4);
        words.push_back(word);
    }
    if (handler_rva) {
        words.push_back(handler_rva);
        for (std::uint32_t d : handler_data) words.push_back(d);
    }
    return image.put(words.data(), words.size() * 4, 4);
}

inline std::uint64_t build_pdata(ImageBuilder& image, std::uint32_t begin_rva, std::uint32_t unwind_rva) {
    const std::uint32_t entry[2] = {begin_rva, unwind_rva};
    return image.put(entry, sizeof(entry), 4);
}

inline transmute::fx::seh::Arm64RuntimeFunction read_entry(std::uint64_t address) {
    transmute::fx::seh::Arm64RuntimeFunction entry{};
    std::memcpy(&entry, reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), sizeof(entry));
    return entry;
}

inline void store64(std::uint64_t address, std::uint64_t value) {
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), &value, 8);
}
inline std::uint64_t load64(std::uint64_t address) {
    std::uint64_t value;
    std::memcpy(&value, reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), 8);
    return value;
}

} // namespace fxseh_test
