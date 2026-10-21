// Function table lookup: .pdata of loaded guest modules plus dynamic tables
// registered by guest-generated code (RtlAddFunctionTable,
// RtlInstallFunctionTableCallback, RtlAddGrowableFunctionTable).

#include "fx/seh/seh.hpp"
#include "fx/seh/dispatch.hpp"

#include <algorithm>
#include <memory>
#include <vector>

namespace transmute::fx::seh {
namespace {

enum class TableKind { table, callback, growable };

struct DynamicTable {
    TableKind kind{};
    std::uint64_t identifier{};   // table address, callback identifier or growable handle
    std::uint64_t base{};         // image base the RVAs are relative to
    std::uint64_t range_begin{};
    std::uint64_t range_end{};
    std::uint64_t entries{};
    std::uint32_t count{};
    std::uint32_t maximum{};
    bool sorted{};
    std::uint64_t callback{};
    std::uint64_t context{};
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<DynamicTable>& tables() {
    static std::vector<DynamicTable> instance;
    return instance;
}
std::uint64_t g_next_growable_handle = 0x4000;

bool read_entry(std::uint64_t table, std::uint32_t index, Arm64RuntimeFunction& entry) {
    return read_guest_value(table + 8ull * index, entry);
}

// Computes [first begin, last end) and whether the entries are sorted.
bool measure_table(std::uint64_t table, std::uint32_t count, std::uint64_t base, DynamicTable& out) {
    std::uint64_t low = ~0ull, high = 0;
    std::uint32_t previous = 0;
    out.sorted = true;
    for (std::uint32_t i = 0; i < count; ++i) {
        Arm64RuntimeFunction entry;
        if (!read_entry(table, i, entry)) return false;
        const std::uint64_t begin = base + entry.BeginAddress;
        const std::uint64_t end = begin + function_length_bytes(base, entry);
        low = std::min(low, begin);
        high = std::max(high, end);
        if (i && entry.BeginAddress < previous) out.sorted = false;
        previous = entry.BeginAddress;
    }
    out.range_begin = count ? low : 0;
    out.range_end = count ? high : 0;
    return true;
}

bool contains(std::uint64_t base, const Arm64RuntimeFunction& entry, std::uint64_t pc) {
    const std::uint64_t begin = base + entry.BeginAddress;
    if (pc < begin) return false;
    return pc - begin < function_length_bytes(base, entry);
}

} // namespace

std::uint64_t search_function_table(std::uint64_t table, std::uint32_t count, std::uint64_t image_base,
                                     std::uint64_t pc, bool sorted) noexcept {
    if (!table || !count || pc < image_base) return 0;
    const std::uint64_t rva64 = pc - image_base;
    if (rva64 > 0xffffffffull) return 0;
    const auto rva = static_cast<std::uint32_t>(rva64);
    Arm64RuntimeFunction entry;
    if (!sorted) {
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!read_entry(table, i, entry)) return 0;
            if (contains(image_base, entry, pc)) return table + 8ull * i;
        }
        return 0;
    }
    // Last entry whose BeginAddress <= rva.
    std::uint32_t low = 0, high = count;
    while (low < high) {
        const std::uint32_t mid = low + (high - low) / 2;
        if (!read_entry(table, mid, entry)) return 0;
        if (entry.BeginAddress <= rva) low = mid + 1;
        else high = mid;
    }
    if (low == 0) return 0;
    if (!read_entry(table, low - 1, entry)) return 0;
    return contains(image_base, entry, pc) ? table + 8ull * (low - 1) : 0;
}

std::uint64_t lookup_function_entry(std::uint64_t pc, std::uint64_t* image_base) {
    if (image_base) *image_base = 0;
    if (pc == 0 || pc == kReturnToHostPc) return 0;
    if (const GuestModule* module = find_guest_module(pc)) {
        if (image_base) *image_base = module->base;
        if (!module->pdata_rva || module->pdata_size < 8) return 0;
        return search_function_table(module->base + module->pdata_rva, module->pdata_size / 8,
                                     module->base, pc, true);
    }
    std::vector<DynamicTable> candidates;
    AcquireSRWLockShared(&g_lock);
    for (const auto& table : tables())
        if (pc >= table.range_begin && pc < table.range_end) candidates.push_back(table);
    ReleaseSRWLockShared(&g_lock);
    for (const auto& table : candidates) {
        std::uint64_t entry = 0;
        if (table.kind == TableKind::callback) {
            entry = invoke_nested(BoundaryKind::callback, nullptr, table.callback, {pc, table.context});
            if (entry) {
                Arm64RuntimeFunction value;
                if (!read_guest_value(entry, value)) entry = 0;
            }
        } else {
            entry = search_function_table(table.entries, table.count, table.base, pc, table.sorted);
        }
        if (entry) {
            if (image_base) *image_base = table.base;
            return entry;
        }
    }
    return 0;
}

bool add_function_table(std::uint64_t table, std::uint32_t count, std::uint64_t base_address) {
    if (!table) return false;
    DynamicTable entry;
    entry.kind = TableKind::table;
    entry.identifier = table;
    entry.base = base_address;
    entry.entries = table;
    entry.count = count;
    entry.maximum = count;
    if (!measure_table(table, count, base_address, entry)) return false;
    AcquireSRWLockExclusive(&g_lock);
    tables().push_back(entry);
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

bool delete_function_table(std::uint64_t table) {
    bool removed = false;
    AcquireSRWLockExclusive(&g_lock);
    auto& list = tables();
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->kind != TableKind::growable && it->identifier == table) {
            list.erase(it);
            removed = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return removed;
}

bool install_function_table_callback(std::uint64_t identifier, std::uint64_t base_address,
                                     std::uint32_t length, std::uint64_t callback, std::uint64_t context) {
    if ((identifier & 3) != 3 || !callback || !length) return false;
    DynamicTable entry;
    entry.kind = TableKind::callback;
    entry.identifier = identifier;
    entry.base = base_address;
    entry.range_begin = base_address;
    entry.range_end = base_address + length;
    entry.callback = callback;
    entry.context = context;
    AcquireSRWLockExclusive(&g_lock);
    tables().push_back(entry);
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

std::uint32_t add_growable_function_table(std::uint64_t* handle, std::uint64_t table, std::uint32_t count,
                                          std::uint32_t maximum, std::uint64_t range_base,
                                          std::uint64_t range_end) {
    if (!handle || !table || count > maximum || range_end <= range_base) return 0xC000000Du;  // STATUS_INVALID_PARAMETER
    DynamicTable entry;
    entry.kind = TableKind::growable;
    entry.base = range_base;
    entry.range_begin = range_base;
    entry.range_end = range_end;
    entry.entries = table;
    entry.count = count;
    entry.maximum = maximum;
    entry.sorted = true;
    AcquireSRWLockExclusive(&g_lock);
    entry.identifier = g_next_growable_handle;
    g_next_growable_handle += 0x10;
    tables().push_back(entry);
    ReleaseSRWLockExclusive(&g_lock);
    *handle = entry.identifier;
    return 0;
}

void grow_function_table(std::uint64_t handle, std::uint32_t count) {
    AcquireSRWLockExclusive(&g_lock);
    for (auto& table : tables())
        if (table.kind == TableKind::growable && table.identifier == handle)
            table.count = std::min(count, table.maximum);
    ReleaseSRWLockExclusive(&g_lock);
}

void delete_growable_function_table(std::uint64_t handle) {
    AcquireSRWLockExclusive(&g_lock);
    auto& list = tables();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const DynamicTable& table) {
                   return table.kind == TableKind::growable && table.identifier == handle;
               }),
               list.end());
    ReleaseSRWLockExclusive(&g_lock);
}

void reset_dynamic_function_tables() {
    AcquireSRWLockExclusive(&g_lock);
    tables().clear();
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace transmute::fx::seh
