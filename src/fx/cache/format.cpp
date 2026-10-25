#include "fx/cache/format.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace transmute::fx::cachefmt {
namespace {

constexpr std::uint64_t P1 = 0x9E3779B185EBCA87ULL;
constexpr std::uint64_t P2 = 0xC2B2AE3D27D4EB4FULL;
constexpr std::uint64_t P3 = 0x165667B19E3779F9ULL;
constexpr std::uint64_t P4 = 0x85EBCA77C2B2AE63ULL;
constexpr std::uint64_t P5 = 0x27D4EB2F165667C5ULL;

inline std::uint64_t rotl(std::uint64_t x, int r) noexcept { return (x << r) | (x >> (64 - r)); }
inline std::uint64_t read64(const std::uint8_t* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
inline std::uint32_t read32(const std::uint8_t* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
inline std::uint64_t round(std::uint64_t acc, std::uint64_t input) noexcept {
    acc += input * P2;
    acc = rotl(acc, 31);
    return acc * P1;
}
inline std::uint64_t merge_round(std::uint64_t acc, std::uint64_t value) noexcept {
    acc ^= round(0, value);
    return acc * P1 + P4;
}

constexpr std::uint64_t align8(std::uint64_t v) noexcept { return (v + 7) & ~std::uint64_t{7}; }

std::uint64_t record_payload_size(std::uint64_t entries, std::uint64_t exits, std::uint64_t fixups,
                                  std::uint64_t faults, std::uint64_t pins, std::uint64_t ranges,
                                  std::uint64_t maps, std::uint64_t map_regs, std::uint64_t code) noexcept {
    return sizeof(RecordHeader) + entries * sizeof(DiskEntry) + exits * sizeof(DiskExit) +
           fixups * sizeof(DiskFixup) + faults * sizeof(DiskFault) + pins * sizeof(DiskPin) +
           ranges * sizeof(DiskRange) + maps * sizeof(DiskFaultMap) + map_regs * sizeof(DiskPin) + code;
}

} // namespace

std::uint64_t hash64(const void* data, std::size_t size, std::uint64_t seed) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    const std::uint8_t* const end = p + size;
    std::uint64_t h;
    if (size >= 32) {
        const std::uint8_t* const limit = end - 32;
        std::uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        do {
            v1 = round(v1, read64(p));
            v2 = round(v2, read64(p + 8));
            v3 = round(v3, read64(p + 16));
            v4 = round(v4, read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = merge_round(h, v1);
        h = merge_round(h, v2);
        h = merge_round(h, v3);
        h = merge_round(h, v4);
    } else {
        h = seed + P5;
    }
    h += size;
    while (p + 8 <= end) {
        h ^= round(0, read64(p));
        h = rotl(h, 27) * P1 + P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= static_cast<std::uint64_t>(read32(p)) * P1;
        h = rotl(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<std::uint64_t>(*p) * P5;
        h = rotl(h, 11) * P1;
        ++p;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

bool encode_record(const TranslatedRegion& region, std::uint64_t base, std::uint64_t span,
                   std::uint64_t guest_hash, std::vector<std::uint8_t>& out, bool pinned) {
    if (span > std::numeric_limits<std::uint32_t>::max()) span = std::numeric_limits<std::uint32_t>::max();
    auto rel = [&](std::uint64_t address, std::uint32_t& rva) {
        if (address < base || address - base > span) return false;
        rva = static_cast<std::uint32_t>(address - base);
        return true;
    };
    const std::size_t code_size = region.code.size();
    if (region.entries.empty() || region.entries[0].guest_pc != region.guest_start) return false;
    if (code_size == 0 || code_size > kMaxCodeSize) return false;
    const std::size_t counts[] = {region.entries.size(), region.exits.size(), region.fixups.size(),
                                  region.faults.size(), region.pins.size(), region.guest_ranges.size(),
                                  region.fault_maps.size(), region.fault_map_regs.size()};
    for (std::size_t c : counts)
        if (c > kMaxItems) return false;
    const std::uint64_t payload = record_payload_size(counts[0], counts[1], counts[2], counts[3], counts[4], counts[5],
                                                      counts[6], counts[7], code_size);
    const std::uint64_t size = align8(payload);
    if (size > std::numeric_limits<std::uint32_t>::max()) return false;

    const std::size_t at = out.size();
    out.resize(at + static_cast<std::size_t>(size), 0);
    std::uint8_t* const record = out.data() + at;
    auto fail = [&] {
        out.resize(at);
        return false;
    };

    RecordHeader header{};
    header.magic = kRecordMagic;
    header.size = static_cast<std::uint32_t>(size);
    if (!rel(region.guest_start, header.start_rva)) return fail();
    header.code_size = static_cast<std::uint32_t>(code_size);
    header.guest_hash = guest_hash;
    header.n_entries = static_cast<std::uint32_t>(counts[0]);
    header.n_exits = static_cast<std::uint32_t>(counts[1]);
    header.n_fixups = static_cast<std::uint32_t>(counts[2]);
    header.n_faults = static_cast<std::uint32_t>(counts[3]);
    header.n_pins = static_cast<std::uint32_t>(counts[4]);
    header.n_ranges = static_cast<std::uint32_t>(counts[5]);
    header.n_maps = static_cast<std::uint32_t>(counts[6]);
    header.n_map_regs = static_cast<std::uint32_t>(counts[7]);
    header.guest_instructions = region.stats.guest_instructions;
    header.interpreted_instructions = region.stats.interpreted_instructions;
    header.blocks = region.stats.blocks;
    header.hot_bytes = region.stats.hot_bytes <= code_size ? region.stats.hot_bytes : 0;
    header.flags = (pinned ? kRecordPinned : 0) | (region.tier == 0 ? kRecordTier0 : 0);
    header.pinned_base = pinned ? base : 0;
    if (region.tier == 0) {
        if (static_cast<std::uint64_t>(region.tier_counter) + 4 > code_size ||
            static_cast<std::uint64_t>(region.tier_counter_slot) + 4 > code_size)
            return fail();
        header.tier_counter = region.tier_counter;
        header.tier_counter_slot = region.tier_counter_slot;
    }

    std::size_t pos = sizeof(RecordHeader);
    for (const RegionEntry& e : region.entries) {
        DiskEntry d{};
        if (!rel(e.guest_pc, d.rva) || e.offset >= code_size) return fail();
        d.offset = e.offset;
        d.flags = e.flags_dead ? kEntryFlagsDead : 0;
        std::memcpy(record + pos, &d, sizeof(d));
        pos += sizeof(d);
    }
    for (const RegionExit& x : region.exits) {
        DiskExit d{};
        if (!rel(x.target_pc, d.target_rva)) return fail();
        if ((x.rel32_offset & 3) != 0 || static_cast<std::uint64_t>(x.rel32_offset) + 4 > code_size) return fail();
        if ((x.skip_rel32_offset & 3) != 0 || static_cast<std::uint64_t>(x.skip_rel32_offset) + 4 > code_size) return fail();
        d.rel32_offset = x.rel32_offset;
        d.skip_rel32_offset = x.skip_rel32_offset;
        std::memcpy(record + pos, &d, sizeof(d));
        pos += sizeof(d);
    }
    const std::size_t fixups_pos = pos;
    for (const CodeFixup& f : region.fixups) {
        DiskFixup d{};
        if (static_cast<std::uint64_t>(f.offset) + 8 > code_size) return fail();
        if (f.kind == FixupKind::guest_address_imm64) {
            std::uint32_t rva = 0;
            if (!rel(f.value, rva)) return fail();
            d.rva = rva;
        } else if (f.kind == FixupKind::literal_imm64) {
            d.rva = f.value;  // a constant: stored and restored as-is
        } else {
            return fail();
        }
        d.offset = f.offset;
        d.kind = static_cast<std::uint32_t>(f.kind);
        std::memcpy(record + pos, &d, sizeof(d));
        pos += sizeof(d);
    }
    for (const FaultSite& f : region.faults) {
        DiskFault d{};
        if (!rel(f.guest_pc, d.guest_rva)) return fail();
        if (f.host_begin > f.host_end || f.host_end > code_size) return fail();
        d.host_begin = f.host_begin;
        d.host_end = f.host_end;
        d.flags = static_cast<std::uint8_t>(f.flags);
        d.flags_register = f.flags_register;
        if (f.map > region.fault_maps.size()) return fail();
        d.map = f.map;
        std::memcpy(record + pos, &d, sizeof(d));
        pos += sizeof(d);
    }
    for (const PinnedRegister& p : region.pins) {
        const DiskPin d{p.guest, p.host, static_cast<std::uint8_t>(p.vector ? 1 : 0), 0};
        std::memcpy(record + pos, &d, sizeof(d));
        pos += sizeof(d);
    }
    for (const auto& [begin, end] : region.guest_ranges) {
        DiskRange d{};
        if (end < begin || !rel(begin, d.begin_rva) || !rel(end, d.end_rva)) return fail();
        std::memcpy(record + pos, &d, sizeof(d));
        pos += sizeof(d);
    }
    for (const FaultMap& m : region.fault_maps) {
        if (m.begin > region.fault_map_regs.size() || m.count > region.fault_map_regs.size() - m.begin) return fail();
        const DiskFaultMap d{m.begin, m.count};
        std::memcpy(record + pos, &d, sizeof(d));
        pos += sizeof(d);
    }
    for (const PinnedRegister& p : region.fault_map_regs) {
        const DiskPin d{p.guest, p.host, static_cast<std::uint8_t>(p.vector ? 1 : 0), 0};
        std::memcpy(record + pos, &d, sizeof(d));
        pos += sizeof(d);
    }
    std::uint8_t* const code = record + pos;
    std::memcpy(code, region.code.data(), code_size);
    // Address slots hold the module-relative value so stored bytes are
    // base-independent; literal slots keep their constant.
    for (std::size_t i = 0; i < region.fixups.size(); ++i) {
        DiskFixup d;
        std::memcpy(&d, record + fixups_pos + i * sizeof(DiskFixup), sizeof(d));
        std::memcpy(code + d.offset, &d.rva, 8);
    }
    std::memcpy(record, &header, sizeof(header));
    header.checksum = hash64(record + 16, static_cast<std::size_t>(size) - 16);
    std::memcpy(record + 8, &header.checksum, 8);
    return true;
}

std::int64_t unrecorded_address(const TranslatedRegion& region, std::uint64_t base, std::uint64_t span) noexcept {
    const std::size_t size = region.code.size();
    if (size < 8) return -1;
    const std::uint8_t* code = region.code.data();
    // Fixup slots sorted by offset; walk them alongside the scan.
    std::uint32_t slots_small[64];
    std::vector<std::uint32_t> slots_large;
    std::uint32_t* slots = slots_small;
    const std::size_t count = region.fixups.size();
    if (count > 64) {
        slots_large.resize(count);
        slots = slots_large.data();
    }
    for (std::size_t i = 0; i < count; ++i) slots[i] = region.fixups[i].offset;
    std::sort(slots, slots + count);
    // Fast path: an address in [base, base + span] has one of at most two
    // high dwords. Find their low byte with memchr (vectorized), then check
    // the window; windows overlapping a fixup slot are never addresses.
    const std::uint64_t last = base + span;
    if (last >= base && (last >> 32) - (base >> 32) <= 1) {
        const auto overlaps_slot = [&](std::size_t at) {
            // Last slot starting before at + 8.
            const std::uint32_t* it = std::upper_bound(slots, slots + count, static_cast<std::uint32_t>(at + 7));
            return it != slots && std::size_t{*(it - 1)} + 8 > at;
        };
        std::int64_t first = -1;
        const std::uint32_t highs[2] = {static_cast<std::uint32_t>(base >> 32), static_cast<std::uint32_t>(last >> 32)};
        for (unsigned h = 0; h < (highs[0] == highs[1] ? 1u : 2u); ++h) {
            const std::uint32_t high = highs[h];
            const int low_byte = static_cast<int>(high & 0xff);
            // High dword of a window at `at` starts at at + 4; windows end by `size`.
            const std::uint8_t* p = code + 4;
            const std::uint8_t* const end = code + size - 3;   // last high-dword start + 1
            while (p < end) {
                p = static_cast<const std::uint8_t*>(std::memchr(p, low_byte, static_cast<std::size_t>(end - p)));
                if (!p) break;
                const auto at = static_cast<std::size_t>(p - code) - 4;
                ++p;
                if (first >= 0 && static_cast<std::int64_t>(at) >= first) break;
                std::uint32_t dword;
                std::memcpy(&dword, code + at + 4, 4);
                if (dword != high) continue;
                std::uint64_t value;
                std::memcpy(&value, code + at, 8);
                if (value - base <= span && !overlaps_slot(at)) {
                    first = static_cast<std::int64_t>(at);
                    break;
                }
            }
        }
        return first;
    }
    std::size_t next = 0;
    for (std::size_t at = 0; at + 8 <= size; ++at) {
        while (next < count && slots[next] + 8 <= at) ++next;
        if (next < count && slots[next] <= at && at < std::size_t{slots[next]} + 8) continue;  // inside a fixup
        std::uint64_t value;
        std::memcpy(&value, code + at, 8);
        if (value - base <= span) {
            // A window overlapping a fixup slot partially is not an embedded address.
            if (next < count && at + 8 > slots[next]) continue;
            return static_cast<std::int64_t>(at);
        }
    }
    return -1;
}

bool decode_record(const std::uint8_t* data, std::size_t avail, bool verify_checksum, RecordView& out) {
    if (avail < sizeof(RecordHeader) || (reinterpret_cast<std::uintptr_t>(data) & 7) != 0) return false;
    const auto* h = reinterpret_cast<const RecordHeader*>(data);
    if (h->magic != kRecordMagic || h->size < sizeof(RecordHeader) || (h->size & 7) != 0 || h->size > avail)
        return false;
    if (h->code_size == 0 || h->code_size > kMaxCodeSize) return false;
    const std::uint32_t counts[] = {h->n_entries, h->n_exits, h->n_fixups, h->n_faults,
                                    h->n_pins, h->n_ranges, h->n_maps, h->n_map_regs};
    for (std::uint32_t c : counts)
        if (c > kMaxItems) return false;
    if (h->n_entries == 0) return false;
    const std::uint64_t payload = record_payload_size(h->n_entries, h->n_exits, h->n_fixups, h->n_faults,
                                                      h->n_pins, h->n_ranges, h->n_maps, h->n_map_regs, h->code_size);
    if (align8(payload) != h->size) return false;
    if (verify_checksum && hash64(data + 16, h->size - 16) != h->checksum) return false;

    std::size_t pos = sizeof(RecordHeader);
    out.header = h;
    out.entries = reinterpret_cast<const DiskEntry*>(data + pos);
    pos += h->n_entries * sizeof(DiskEntry);
    out.exits = reinterpret_cast<const DiskExit*>(data + pos);
    pos += h->n_exits * sizeof(DiskExit);
    out.fixups = reinterpret_cast<const DiskFixup*>(data + pos);
    pos += h->n_fixups * sizeof(DiskFixup);
    out.faults = reinterpret_cast<const DiskFault*>(data + pos);
    pos += h->n_faults * sizeof(DiskFault);
    out.pins = reinterpret_cast<const DiskPin*>(data + pos);
    pos += h->n_pins * sizeof(DiskPin);
    out.ranges = reinterpret_cast<const DiskRange*>(data + pos);
    pos += h->n_ranges * sizeof(DiskRange);
    out.maps = reinterpret_cast<const DiskFaultMap*>(data + pos);
    pos += h->n_maps * sizeof(DiskFaultMap);
    out.map_regs = reinterpret_cast<const DiskPin*>(data + pos);
    pos += h->n_map_regs * sizeof(DiskPin);
    out.code = data + pos;

    // Semantic bounds: nothing below may index outside the code.
    const std::uint32_t code = h->code_size;
    if (out.entries[0].rva != h->start_rva) return false;
    for (std::uint32_t i = 0; i < h->n_entries; ++i)
        if (out.entries[i].offset >= code || (out.entries[i].flags & ~kEntryFlagsDead) != 0 || out.entries[i].reserved != 0)
            return false;
    for (std::uint32_t i = 0; i < h->n_exits; ++i) {
        const DiskExit& x = out.exits[i];
        if ((x.rel32_offset & 3) != 0 || static_cast<std::uint64_t>(x.rel32_offset) + 4 > code) return false;
        if ((x.skip_rel32_offset & 3) != 0 || static_cast<std::uint64_t>(x.skip_rel32_offset) + 4 > code || x.reserved != 0 ||
            (x.skip_rel32_offset != 0 && x.skip_rel32_offset == x.rel32_offset))
            return false;
    }
    for (std::uint32_t i = 0; i < h->n_fixups; ++i)
        if (static_cast<std::uint64_t>(out.fixups[i].offset) + 8 > code || out.fixups[i].kind > 1) return false;
    if ((h->flags & ~(kRecordPinned | kRecordTier0)) != 0 || ((h->flags & kRecordPinned) != 0) != (h->pinned_base != 0))
        return false;
    if (record_tier0(*h) ? (static_cast<std::uint64_t>(h->tier_counter) + 4 > code ||
                            static_cast<std::uint64_t>(h->tier_counter_slot) + 4 > code)
                         : (h->tier_counter != 0 || h->tier_counter_slot != 0))
        return false;
    if (h->hot_bytes > code || h->reserved1 != 0) return false;
    for (std::uint32_t i = 0; i < h->n_faults; ++i)
        if (out.faults[i].host_begin > out.faults[i].host_end || out.faults[i].host_end > code ||
            out.faults[i].map > h->n_maps)
            return false;
    for (std::uint32_t i = 0; i < h->n_maps; ++i)
        if (out.maps[i].begin > h->n_map_regs || out.maps[i].count > h->n_map_regs - out.maps[i].begin) return false;
    for (std::uint32_t i = 0; i < h->n_ranges; ++i)
        if (out.ranges[i].end_rva < out.ranges[i].begin_rva) return false;
    return true;
}

void rebuild_region(const RecordView& record, std::uint64_t base, TranslatedRegion& out) {
    const RecordHeader& h = *record.header;
    out.guest_start = base + h.start_rva;
    out.guest_ranges.resize(h.n_ranges);
    for (std::uint32_t i = 0; i < h.n_ranges; ++i)
        out.guest_ranges[i] = {base + record.ranges[i].begin_rva, base + record.ranges[i].end_rva};
    out.code.assign(record.code, record.code + h.code_size);
    out.entries.resize(h.n_entries);
    for (std::uint32_t i = 0; i < h.n_entries; ++i)
        out.entries[i] = {base + record.entries[i].rva, record.entries[i].offset,
                          (record.entries[i].flags & kEntryFlagsDead) != 0};
    out.exits.resize(h.n_exits);
    for (std::uint32_t i = 0; i < h.n_exits; ++i)
        out.exits[i] = {record.exits[i].rel32_offset, base + record.exits[i].target_rva,
                        record.exits[i].skip_rel32_offset};
    out.fixups.resize(h.n_fixups);
    for (std::uint32_t i = 0; i < h.n_fixups; ++i) {
        const DiskFixup& d = record.fixups[i];
        const bool literal = d.kind == static_cast<std::uint32_t>(FixupKind::literal_imm64);
        const std::uint64_t value = literal ? d.rva : base + d.rva;
        std::memcpy(out.code.data() + d.offset, &value, 8);
        out.fixups[i] = {d.offset, literal ? FixupKind::literal_imm64 : FixupKind::guest_address_imm64, value};
    }
    out.faults.resize(h.n_faults);
    for (std::uint32_t i = 0; i < h.n_faults; ++i) {
        const DiskFault& d = record.faults[i];
        out.faults[i] = {d.host_begin, d.host_end, base + d.guest_rva, static_cast<FlagsLocation>(d.flags),
                         d.flags_register, d.map};
    }
    out.fault_maps.resize(h.n_maps);
    for (std::uint32_t i = 0; i < h.n_maps; ++i) out.fault_maps[i] = {record.maps[i].begin, record.maps[i].count};
    out.fault_map_regs.resize(h.n_map_regs);
    for (std::uint32_t i = 0; i < h.n_map_regs; ++i)
        out.fault_map_regs[i] = {record.map_regs[i].guest, record.map_regs[i].host, record.map_regs[i].vector != 0};
    out.pins.resize(h.n_pins);
    for (std::uint32_t i = 0; i < h.n_pins; ++i)
        out.pins[i] = {record.pins[i].guest, record.pins[i].host, record.pins[i].vector != 0};
    out.stats.guest_instructions = h.guest_instructions;
    out.stats.interpreted_instructions = h.interpreted_instructions;
    out.stats.blocks = h.blocks;
    out.stats.hot_bytes = h.hot_bytes;
    out.tier = record_tier0(h) ? 0 : 1;
    out.tier_counter = h.tier_counter;
    out.tier_counter_slot = h.tier_counter_slot;
}

std::uint64_t header_checksum(const FileHeader& header) noexcept {
    FileHeader copy = header;
    copy.header_checksum = 0;
    return hash64(&copy, sizeof(copy), 0x4844'5221ULL);
}

bool parse_header(const std::uint8_t* data, std::size_t size, std::uint32_t kind, FileHeader& out) {
    if (!data || size < sizeof(FileHeader)) return false;
    std::memcpy(&out, data, sizeof(out));
    if (out.magic != kFileMagic || out.header_size != sizeof(FileHeader) || out.kind != kind) return false;
    return header_checksum(out) == out.header_checksum;
}

bool validate_header(const std::uint8_t* data, std::size_t size, std::uint32_t kind, const Key& key) {
    FileHeader h;
    return parse_header(data, size, kind, h) && h.key == key;
}

bool open_main(const std::uint8_t* data, std::size_t size, const Key& key, MainView& out) {
    if (!validate_header(data, size, kKindMain, key) || (reinterpret_cast<std::uintptr_t>(data) & 7) != 0)
        return false;
    const auto* h = reinterpret_cast<const FileHeader*>(data);
    if (h->total_size != size) return false;  // truncated or extended
    auto in_file = [&](std::uint64_t offset, std::uint64_t bytes) {
        return offset <= size && bytes <= size - offset;
    };
    if (h->records_offset < sizeof(FileHeader) || (h->records_offset & 7) != 0 ||
        !in_file(h->records_offset, h->records_size))
        return false;
    if ((h->index_offset & 3) != 0 || !in_file(h->index_offset, std::uint64_t{h->index_count} * sizeof(IndexEntry)))
        return false;
    if (h->bucket_count == 0 || h->bucket_shift > 31 || (h->bucket_offset & 3) != 0 ||
        !in_file(h->bucket_offset, (std::uint64_t{h->bucket_count} + 1) * 4))
        return false;
    out.data = data;
    out.size = size;
    out.header = h;
    out.index = reinterpret_cast<const IndexEntry*>(data + h->index_offset);
    out.index_count = h->index_count;
    out.buckets = reinterpret_cast<const std::uint32_t*>(data + h->bucket_offset);
    out.bucket_count = h->bucket_count;
    out.bucket_shift = h->bucket_shift;
    out.records_offset = h->records_offset;
    out.records_size = h->records_size;
    return true;
}

unsigned main_find(const MainView& view, std::uint32_t rva, std::uint64_t* offsets, unsigned max) noexcept {
    if (!view.header || max == 0) return 0;
    const std::uint32_t bucket = rva >> view.bucket_shift;
    if (bucket >= view.bucket_count) return 0;
    std::uint32_t lo = view.buckets[bucket];
    std::uint32_t hi = view.buckets[bucket + 1];
    if (hi > view.index_count) hi = view.index_count;
    if (lo > hi) return 0;
    while (lo < hi) {  // lower_bound
        const std::uint32_t mid = lo + (hi - lo) / 2;
        if (view.index[mid].rva < rva) lo = mid + 1;
        else hi = mid;
    }
    unsigned n = 0;
    for (std::uint32_t i = lo; i < view.index_count && view.index[i].rva == rva && n < max; ++i) {
        const std::uint64_t rel = std::uint64_t{view.index[i].record} * 8;
        if (rel + sizeof(RecordHeader) > view.records_size) continue;
        offsets[n++] = view.records_offset + rel;
    }
    return n;
}

bool build_main(const Key& key, std::uint64_t generation, std::span<const RecordRef> records,
                FileSink& sink, BuildResult* result) {
    struct Chosen {
        std::uint32_t start;
        std::uint32_t input;  // position in `records`; later wins (within a tier)
        std::uint32_t order;  // output position: the first input with this start
        bool tier0;
    };
    std::vector<Chosen> chosen;
    chosen.reserve(records.size());
    for (std::uint32_t i = 0; i < records.size(); ++i) {
        const auto* h = reinterpret_cast<const RecordHeader*>(records[i].data);
        chosen.push_back({h->start_rva, i, i, record_tier0(*h)});
    }
    std::stable_sort(chosen.begin(), chosen.end(),
                     [](const Chosen& a, const Chosen& b) { return a.start < b.start; });
    // Keep one input for each start RVA: the last tier-1 one, else the last
    // (a tier-up's record replaces the tier-0 record it grew from), at the
    // position of the first one.
    std::vector<Chosen> unique;
    unique.reserve(chosen.size());
    for (std::size_t i = 0; i < chosen.size();) {
        std::size_t j = i;
        Chosen best = chosen[i];
        for (; j < chosen.size() && chosen[j].start == chosen[i].start; ++j)
            if (!chosen[j].tier0 || best.tier0) best = chosen[j];
        best.order = chosen[i].input;   // stable sort: the first input
        unique.push_back(best);
        i = j;
    }
    // Records keep input order (existing file order, then journal append
    // order, which follows first execution), so regions used together at
    // startup share pages; the index alone is sorted by RVA.
    std::sort(unique.begin(), unique.end(), [](const Chosen& a, const Chosen& b) { return a.order < b.order; });

    struct Indexed {
        std::uint32_t rva;
        std::uint32_t record;
        std::uint32_t rank;  // 0 = region start, then newer inputs first
    };
    std::vector<Indexed> index;
    std::uint64_t offset = 0;
    std::uint64_t span = 1;
    std::uint64_t code_bytes = 0;
    for (const Chosen& c : unique) {
        RecordView view;
        const RecordRef& ref = records[c.input];
        if (!decode_record(ref.data, ref.size, false, view)) return false;
        const RecordHeader& h = *view.header;
        if (offset / 8 > std::numeric_limits<std::uint32_t>::max()) return false;
        for (std::uint32_t e = 0; e < h.n_entries; ++e) {
            const std::uint32_t rva = view.entries[e].rva;
            // Tier-1 records first, then the region starting at rva, then newer inputs.
            const std::uint32_t rank = (c.tier0 ? 0x8000'0000u : 0u) |
                                       (rva == h.start_rva ? 0u : 0x4000'0000u - (c.input & 0x3fff'ffffu));
            index.push_back({rva, static_cast<std::uint32_t>(offset / 8), rank});
            span = std::max<std::uint64_t>(span, std::uint64_t{rva} + 1);
        }
        for (std::uint32_t r = 0; r < h.n_ranges; ++r) span = std::max<std::uint64_t>(span, view.ranges[r].end_rva);
        offset += h.size;
        code_bytes += h.code_size;
    }
    std::sort(index.begin(), index.end(), [](const Indexed& a, const Indexed& b) {
        return a.rva != b.rva ? a.rva < b.rva : a.rank < b.rank;
    });
    std::vector<IndexEntry> disk_index;
    disk_index.reserve(index.size());
    for (std::size_t i = 0; i < index.size(); ++i) {
        if (i > 0 && index[i].rva == index[i - 1].rva) continue;
        disk_index.push_back({index[i].rva, index[i].record});
    }
    const std::uint32_t index_count = static_cast<std::uint32_t>(disk_index.size());
    std::uint32_t shift = 2;
    const std::uint64_t target_buckets = std::max<std::uint64_t>(1, index_count / 2);
    while (shift < 31 && (span >> shift) + 1 > target_buckets) ++shift;
    const std::uint32_t bucket_count = static_cast<std::uint32_t>((span >> shift) + 1);
    std::vector<std::uint32_t> buckets(std::size_t{bucket_count} + 1);
    {
        std::uint32_t i = 0;
        for (std::uint32_t b = 0; b <= bucket_count; ++b) {
            while (i < index_count && (disk_index[i].rva >> shift) < b) ++i;
            buckets[b] = i;
        }
        buckets[bucket_count] = index_count;
    }

    FileHeader header{};
    header.magic = kFileMagic;
    header.header_size = sizeof(FileHeader);
    header.kind = kKindMain;
    header.key = key;
    header.generation = generation;
    header.region_count = static_cast<std::uint32_t>(unique.size());
    header.index_count = index_count;
    header.records_offset = sizeof(FileHeader);
    header.records_size = offset;
    header.index_offset = header.records_offset + offset;
    header.bucket_offset = header.index_offset + std::uint64_t{index_count} * sizeof(IndexEntry);
    header.bucket_count = bucket_count;
    header.bucket_shift = shift;
    header.rva_span = span;
    header.total_size = header.bucket_offset + buckets.size() * sizeof(std::uint32_t);

    const FileHeader placeholder{};
    if (!sink.write(&placeholder, sizeof(placeholder))) return false;
    for (const Chosen& c : unique) {
        const auto* h = reinterpret_cast<const RecordHeader*>(records[c.input].data);
        if (!sink.write(records[c.input].data, h->size)) return false;
    }
    if (!disk_index.empty() && !sink.write(disk_index.data(), disk_index.size() * sizeof(IndexEntry))) return false;
    if (!sink.write(buckets.data(), buckets.size() * sizeof(std::uint32_t))) return false;
    header.header_checksum = header_checksum(header);
    if (!sink.write_at(0, &header, sizeof(header))) return false;
    if (result) {
        result->regions = header.region_count;
        result->index_entries = index_count;
        result->file_size = header.total_size;
        result->code_bytes = code_bytes;
    }
    return true;
}

} // namespace transmute::fx::cachefmt
