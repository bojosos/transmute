#pragma once

// On-disk translation cache format (owner: cache). Internal to src/fx/cache.
//
// Main file  <dir>\<module>.<key16>.fxc
//   FileHeader | records ... | IndexEntry[index_count] | u32 buckets[bucket_count + 1]
// Journal    <dir>\<module>.<key16>.j<pid>-<n>.fxj   (one per process and module)
//   FileHeader (kind = journal) | records ... (append-only; tail validated)
//
// A record is a self-checksummed, module-relative TranslatedRegion. Every
// guest address is stored as an RVA; code bytes hold the RVA in each fixup
// slot (rebased on load). The index maps every entry RVA to its record and is
// searched through a bucket table, so a lookup touches O(1) index pages and
// nothing is parsed up front.

#include "fx/translator/translator.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace transmute::fx::cachefmt {

inline constexpr std::uint64_t kFileMagic = 0x3145'4843'4143'5846ULL;  // "FXCACHE1"
inline constexpr std::uint32_t kRecordMagic = 0x4752'5846u;             // "FXRG"
// 2: literal_imm64 fixups and base-pinned (unrelocatable) records.
// 3: fault maps (FaultSite::map, TranslatedRegion::fault_maps/fault_map_regs).
// 4: tiers (kRecordTier0, RecordHeader::tier_counter).
// 5: code layout (RecordHeader::hot_bytes = TranslationStats::hot_bytes).
// 6: flag stores skipped at links (DiskEntry::flags = RegionEntry::flags_dead,
//    DiskExit::skip_rel32_offset = RegionExit::skip_rel32_offset).
inline constexpr std::uint32_t kFormatVersion = 6;
// RecordHeader::flags
inline constexpr std::uint32_t kRecordPinned = 1;  // usable only with the module at pinned_base
// A tier-0 (provisional, counted) region (TranslatedRegion::tier == 0). Lookups
// and merges prefer tier-1 records: a tier-1 record for a pc is never shadowed
// by a tier-0 one.
inline constexpr std::uint32_t kRecordTier0 = 2;
inline constexpr std::uint32_t kKindMain = 1;
inline constexpr std::uint32_t kKindJournal = 2;
// Sanity limits applied when decoding (corrupt files must never allocate wildly).
inline constexpr std::uint32_t kMaxCodeSize = 64u << 20;
inline constexpr std::uint32_t kMaxItems = 1u << 22;

// 64-bit hash (xxHash64 algorithm).
std::uint64_t hash64(const void* data, std::size_t size, std::uint64_t seed = 0) noexcept;
inline std::uint64_t mix64(std::uint64_t h, std::uint64_t v) noexcept {
    return hash64(&v, sizeof(v), h);
}

struct Key {
    std::uint32_t format_version{};
    std::uint32_t abi_version{};
    std::uint64_t image_hash{};
    std::uint64_t file_size{};
    std::uint64_t cpu_features{};
    std::uint64_t options_hash{};
    bool operator==(const Key&) const = default;
    [[nodiscard]] std::uint64_t digest() const noexcept { return hash64(this, sizeof(*this), 0x6678'6361'6368'65ULL); }
};
static_assert(sizeof(Key) == 40);

struct FileHeader {
    std::uint64_t magic;
    std::uint32_t header_size;
    std::uint32_t kind;
    Key key;
    std::uint64_t total_size;       // main: exact file size (truncation check); journal: 0
    std::uint64_t generation;       // main: merge generation
    std::uint32_t region_count;
    std::uint32_t index_count;
    std::uint64_t records_offset;
    std::uint64_t records_size;
    std::uint64_t index_offset;
    std::uint64_t bucket_offset;
    std::uint32_t bucket_count;
    std::uint32_t bucket_shift;
    std::uint64_t rva_span;         // all indexed RVAs are < rva_span
    std::uint64_t created_filetime;
    std::uint32_t creator_pid;
    std::uint32_t reserved0;
    std::uint64_t reserved[13];
    std::uint64_t header_checksum;  // hash64 of the header with this field zero
};
static_assert(sizeof(FileHeader) == 256);

struct IndexEntry {
    std::uint32_t rva;
    std::uint32_t record;  // (record offset - records_offset) / 8
};

struct RecordHeader {
    std::uint32_t magic;
    std::uint32_t size;             // whole record, multiple of 8
    std::uint64_t checksum;         // hash64 of bytes [16, size)
    std::uint32_t start_rva;
    std::uint32_t code_size;
    std::uint64_t guest_hash;       // hash of guest bytes over `ranges`
    std::uint32_t n_entries, n_exits, n_fixups, n_faults, n_pins, n_ranges;
    std::uint32_t guest_instructions, interpreted_instructions, blocks;
    std::uint32_t flags;            // kRecordPinned
    std::uint64_t pinned_base;      // module base the code was translated at (pinned records)
    std::uint32_t n_maps, n_map_regs;   // fault maps (format 3)
    std::uint32_t tier_counter;         // TranslatedRegion::tier_counter (format 4; tier 0 only)
    std::uint32_t tier_counter_slot;
    std::uint32_t hot_bytes;            // TranslationStats::hot_bytes (format 5; <= code_size)
    std::uint32_t reserved1;
};
static_assert(sizeof(RecordHeader) == 104);

inline bool record_tier0(const RecordHeader& h) noexcept { return (h.flags & kRecordTier0) != 0; }

inline constexpr std::uint32_t kEntryFlagsDead = 1;   // DiskEntry::flags (format 6)
struct DiskEntry { std::uint32_t rva; std::uint32_t offset; std::uint32_t flags; std::uint32_t reserved; };
// skip_rel32_offset: 0, or a second patchable rel32 (format 6).
struct DiskExit { std::uint32_t rel32_offset; std::uint32_t target_rva; std::uint32_t skip_rel32_offset; std::uint32_t reserved; };
// kind 0 (guest_address_imm64): `rva` is module-relative and rebased on load.
// kind 1 (literal_imm64): `rva` holds the constant itself, never rebased.
struct DiskFixup { std::uint32_t offset; std::uint32_t kind; std::uint64_t rva; };
struct DiskFault {
    std::uint32_t host_begin, host_end, guest_rva;
    std::uint8_t flags, flags_register;
    std::uint16_t map;   // FaultSite::map (format 3; was padding)
};
struct DiskFaultMap { std::uint32_t begin, count; };   // into the record's map registers (DiskPin)
struct DiskPin { std::uint8_t guest, host, vector, pad; };
struct DiskRange { std::uint32_t begin_rva, end_rva; };
static_assert(sizeof(DiskFixup) == 16 && sizeof(DiskFault) == 16 && sizeof(DiskPin) == 4);
static_assert(sizeof(DiskEntry) == 16 && sizeof(DiskExit) == 16);

// Decoded (validated) view over a record in a mapped file or buffer.
struct RecordView {
    const RecordHeader* header{};
    const DiskEntry* entries{};
    const DiskExit* exits{};
    const DiskFixup* fixups{};
    const DiskFault* faults{};
    const DiskPin* pins{};
    const DiskRange* ranges{};
    const DiskFaultMap* maps{};
    const DiskPin* map_regs{};
    const std::uint8_t* code{};
    [[nodiscard]] std::span<const std::uint8_t> bytes() const {
        return {reinterpret_cast<const std::uint8_t*>(header), header->size};
    }
};

// Converts `region` (absolute addresses, module at `base`) into a record
// appended to `out`. Fails (appending nothing) if an address lies outside
// [base, base + span] or the region is malformed. `pinned` marks code that
// embeds addresses of this base without fixups: it is reused only at `base`.
bool encode_record(const TranslatedRegion& region, std::uint64_t base, std::uint64_t span,
                   std::uint64_t guest_hash, std::vector<std::uint8_t>& out, bool pinned = false);

// True if the record can be used with its module mapped at `base`.
inline bool usable_at(const RecordHeader& h, std::uint64_t base) noexcept {
    return (h.flags & kRecordPinned) == 0 || h.pinned_base == base;
}

// Relocation audit: offset of the first 8-byte value in `region.code` that is
// an address inside [base, base + span] but not a recorded fixup slot (code
// that would silently keep pointing at this base after a rebase), or -1.
std::int64_t unrecorded_address(const TranslatedRegion& region, std::uint64_t base, std::uint64_t span) noexcept;

// Validates structure and bounds of the record at `data` (`avail` readable
// bytes). With `verify_checksum` also checks the record checksum. Never
// reads outside [data, data + avail).
bool decode_record(const std::uint8_t* data, std::size_t avail, bool verify_checksum, RecordView& out);

// Rebuilds an absolute TranslatedRegion for a module at `base`.
void rebuild_region(const RecordView& record, std::uint64_t base, TranslatedRegion& out);

// Guest-code hash over a record's ranges: init(count), then step() per range
// in record order with the bytes at [begin_rva, end_rva).
inline std::uint64_t guest_hash_init(std::size_t ranges) noexcept { return 0x9e37'79b9'7f4a'7c15ULL ^ ranges; }
inline std::uint64_t guest_hash_step(std::uint64_t h, std::uint32_t begin_rva, const void* bytes,
                                     std::uint32_t size) noexcept {
    return hash64(bytes, size, h ^ (static_cast<std::uint64_t>(begin_rva) << 32 | size));
}

// Parses and checks a header (magic, size, kind, checksum) without a key.
bool parse_header(const std::uint8_t* data, std::size_t size, std::uint32_t kind, FileHeader& out);

// ---- main file ------------------------------------------------------------

std::uint64_t header_checksum(const FileHeader& header) noexcept;
// Validates a header against `key` and the actual file size (`size`).
bool validate_header(const std::uint8_t* data, std::size_t size, std::uint32_t kind, const Key& key);

struct MainView {
    const std::uint8_t* data{};
    std::size_t size{};
    const FileHeader* header{};
    const IndexEntry* index{};
    std::uint32_t index_count{};
    const std::uint32_t* buckets{};
    std::uint32_t bucket_count{};
    std::uint32_t bucket_shift{};
    std::uint64_t records_offset{};
    std::uint64_t records_size{};
};

// Header-only validation (O(1)); everything else is checked lazily.
bool open_main(const std::uint8_t* data, std::size_t size, const Key& key, MainView& out);
// Offsets (relative to data) of records indexed under `rva`, best first.
// Returns the number of candidates written (0..max).
unsigned main_find(const MainView& view, std::uint32_t rva, std::uint64_t* offsets, unsigned max) noexcept;

// Records from several sources, merged into one main file. Later inputs win
// for the same start RVA. Every input must already be checksum-validated.
struct RecordRef {
    const std::uint8_t* data;
    std::uint32_t size;
};

class FileSink {
public:
    virtual ~FileSink() = default;
    virtual bool write(const void* data, std::size_t size) = 0;
    virtual bool write_at(std::uint64_t offset, const void* data, std::size_t size) = 0;
};

struct BuildResult {
    std::uint32_t regions{};
    std::uint32_t index_entries{};
    std::uint64_t file_size{};
    std::uint64_t code_bytes{};
};

bool build_main(const Key& key, std::uint64_t generation, std::span<const RecordRef> records,
                FileSink& sink, BuildResult* result);

// Walks records of a journal (after its header) or the record area of a main
// file. Stops at the first record that fails validation (a torn tail).
template <typename Fn>
void walk_records(const std::uint8_t* data, std::size_t begin, std::size_t end, bool verify, Fn&& fn) {
    std::size_t offset = begin;
    while (offset + sizeof(RecordHeader) <= end) {
        RecordView view;
        if (!decode_record(data + offset, end - offset, verify, view)) break;
        fn(offset, view);
        offset += view.header->size;
    }
}

} // namespace transmute::fx::cachefmt
