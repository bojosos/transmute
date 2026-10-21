#pragma once

// Mapping ARM64 PE images into host memory at their guest address (owner:
// loader). Guest VA == host VA. Images are mapped as image sections
// (SEC_IMAGE_NO_EXECUTE: the kernel lays out and relocates the sections and
// shares the pages; they fault in on first touch). Images the kernel will not
// map that way (or with TRANSMUTE_FX_IMAGE_VIEWS=0) become a private
// allocation: headers and sections copied from the file, relocations applied.
// Either way protections are set so that guest code is never host-executable.

#include "fx/loader/pe.hpp"

#include <cstdint>
#include <string>

namespace transmute::fx::loader {

struct MappedImage {
    std::uint8_t* base = nullptr;
    std::uint64_t size = 0;
    PeHeaders headers;
    std::int64_t delta = 0;            // actual base - preferred base
    std::int64_t relocations_applied = 0;
    std::uint64_t file_size = 0;
    std::uint64_t last_write_time = 0;
    std::uint64_t image_hash = 0;
    std::uint64_t text_begin = 0;      // union of executable sections (absolute)
    std::uint64_t text_end = 0;
    bool view = false;                 // image-section view (else a private copy)
};

struct MapOptions {
    // Never use the preferred base (tests exercise relocation with this).
    bool force_relocation = false;
    // Machine required for guest images (0 = any).
    std::uint16_t required_machine = kMachineArm64;
    // Map an image section view when the kernel accepts the file (false: always copy).
    bool image_view = true;
};

// Reads the file at `path` and maps it as an image. On failure `error` is set
// and nothing stays allocated. Protections are NOT applied yet (the loader
// still writes the IAT and TLS index); call protect_image afterwards.
bool map_image(const std::wstring& path, const MapOptions& options, MappedImage& out,
               std::string& error);

// Applies final section protections:
//   executable          -> PAGE_READONLY (translated, never executed directly)
//   executable+writable -> PAGE_READWRITE
//   writable data       -> PAGE_READWRITE, read-only data -> PAGE_READONLY,
//   headers             -> PAGE_READONLY, no-access sections -> PAGE_NOACCESS.
bool protect_image(const MappedImage& image, std::string& error);

// Host protection constant for a section's characteristics.
unsigned long section_protection(std::uint32_t characteristics) noexcept;

void unmap_image(MappedImage& image) noexcept;

// Cheap identity hash of (file size, last-write time, TimeDateStamp,
// SizeOfImage, CheckSum, first 4 KiB of the file).
std::uint64_t compute_image_hash(std::uint64_t file_size, std::uint64_t last_write_time,
                                 const PeHeaders& headers, const std::uint8_t* first_bytes,
                                 std::size_t first_size) noexcept;

// Starts the persistent reader threads used for large images. Call it
// outside the loader lock (launch_guest_process and loader_configure do).
void prepare_image_readers();

// Reads and parses the headers of the PE file at `path` (no mapping).
bool read_image_headers(const std::wstring& path, PeHeaders& out, std::string& error);

// Reads the first bytes of a file and returns its PE machine (0 if the file
// does not exist or is not a PE image).
std::uint16_t probe_pe_file(const std::wstring& path, bool* is_dll = nullptr);

} // namespace transmute::fx::loader
