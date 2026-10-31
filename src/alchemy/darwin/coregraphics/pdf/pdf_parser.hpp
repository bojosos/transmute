#pragma once

// A PDF reader (host-only; the CGPDF functions in ../pdf_document.cpp and
// ../pdf_scanner.cpp stand on it), written from the PDF specification
// (ISO 32000-1).
// * Files: the header version; cross-reference tables and streams
//   (following /Prev and hybrid /XRefStm sections, newest entries first),
//   object streams; a file whose cross-reference data is unusable is
//   reconstructed by scanning for "n g obj" headers and trailers.
// * Objects: the eight basic types, indirect references (resolved on use,
//   cached), streams with their data (/Length direct or indirect; a wrong
//   length falls back to searching for "endstream").
// * Stream filters: FlateDecode (with PNG and TIFF predictors), LZWDecode,
//   ASCIIHexDecode, ASCII85Decode, RunLengthDecode; DCTDecode and JPXDecode
//   are left encoded (the data is JPEG / JPEG 2000). Other filters
//   (CCITTFax, JBIG2, Crypt) give no data.
// * Pages: the page tree flattened in order; inherited attributes
//   (/Resources, /MediaBox, /CropBox, /Rotate) are looked up through /Parent.
// * Encryption: an /Encrypt entry marks the document encrypted; nothing is
//   decrypted (strings and streams of such documents are their encrypted
//   bytes).
// * Content streams: the lexer and object parser also tokenize page content
//   (operators are keywords), with inline images (BI ... ID data EI).

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alchemy::gui::quartz2d::pdf {

struct Object;
using ObjectPtr = std::shared_ptr<const Object>;

enum class Kind : std::uint8_t { null, boolean, integer, real, name, string, array, dictionary, stream, reference, keyword };

struct Dictionary {
    std::vector<std::pair<std::string, ObjectPtr>> entries;   // file order; later duplicates replace earlier ones
    [[nodiscard]] ObjectPtr find(std::string_view key) const;
};

struct Object {
    Kind kind{Kind::null};
    bool boolean{};
    std::int64_t integer{};
    double real{};
    std::string text;                  // name (decoded, without '/'), string bytes, keyword
    std::vector<ObjectPtr> array;
    Dictionary dictionary;             // a dictionary, or a stream's dictionary
    // Stream data: a range of the file, or bytes of their own (inline images).
    std::size_t data_offset{}, data_length{};
    std::shared_ptr<const std::vector<std::uint8_t>> own_data;
    std::uint32_t number{}, generation{};   // a reference (and the indirect object a stream came from)

    [[nodiscard]] bool is_number() const { return kind == Kind::integer || kind == Kind::real; }
    [[nodiscard]] double number_value() const { return kind == Kind::integer ? static_cast<double>(integer) : real; }
};

[[nodiscard]] ObjectPtr make_null();
[[nodiscard]] ObjectPtr make_integer(std::int64_t value);
[[nodiscard]] ObjectPtr make_name(std::string name);

// The lexer and object parser over a byte range (a file, or content data).
class Lexer {
public:
    explicit Lexer(std::span<const std::uint8_t> bytes, std::size_t position = 0) : bytes_(bytes), position_(position) {}
    [[nodiscard]] std::size_t position() const { return position_; }
    void seek(std::size_t position) { position_ = position; }
    [[nodiscard]] bool at_end();
    void skip_space();
    // The next object: numbers, names, strings, arrays, dictionaries, true /
    // false / null, "n g R" references (when `references`), and other
    // keywords (operators, "obj", "stream", ...) as Kind::keyword. Arrays and
    // dictionaries nest. Nothing (nullopt) at the end or on a syntax error.
    [[nodiscard]] std::optional<ObjectPtr> next(bool references = true);
    [[nodiscard]] std::span<const std::uint8_t> bytes() const { return bytes_; }

private:
    [[nodiscard]] std::optional<ObjectPtr> token(bool references, int depth);
    std::span<const std::uint8_t> bytes_;
    std::size_t position_{};
};

enum class DataFormat { raw = 0, jpeg = 1, jpeg2000 = 2 };

class Document {
public:
    // A document over `bytes` (a PDF file), or nullopt with `error` set.
    static std::shared_ptr<Document> open(std::shared_ptr<const std::vector<std::uint8_t>> bytes, std::string* error = nullptr);

    [[nodiscard]] int major_version() const { return major_; }
    [[nodiscard]] int minor_version() const { return minor_; }
    [[nodiscard]] bool encrypted() const { return encrypted_; }
    [[nodiscard]] const ObjectPtr& trailer() const { return trailer_; }
    [[nodiscard]] ObjectPtr catalog();
    [[nodiscard]] ObjectPtr info();
    [[nodiscard]] ObjectPtr file_id();
    [[nodiscard]] const std::vector<ObjectPtr>& pages() const { return pages_; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const { return *bytes_; }
    [[nodiscard]] bool reconstructed() const { return reconstructed_; }

    // `object` with references followed (the null object for a missing one).
    [[nodiscard]] ObjectPtr resolve(const ObjectPtr& object);
    [[nodiscard]] ObjectPtr indirect(std::uint32_t number);
    // A dictionary's (or stream's) value for `key`, resolved; nullptr when absent.
    [[nodiscard]] ObjectPtr get(const ObjectPtr& dictionary, std::string_view key);
    // A page attribute, inherited through /Parent.
    [[nodiscard]] ObjectPtr inherited(const ObjectPtr& page, std::string_view key);
    // A stream's data after its filters (to `format`: DCT / JPX data stays encoded); nullopt for unsupported filters.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> stream_data(const Object& stream, DataFormat* format = nullptr);

private:
    struct Entry {
        enum class Type { free, offset, compressed } type{Type::free};
        std::uint64_t value{};       // file offset, or the object stream's number
        std::uint32_t index{};       // index in the object stream
    };
    bool load();
    bool read_xref_chain(std::size_t start);
    bool read_xref_table(std::size_t offset, ObjectPtr& trailer_out);
    bool read_xref_stream(std::size_t offset, ObjectPtr& trailer_out);
    void reconstruct();
    void collect_pages();
    ObjectPtr parse_indirect_at(std::size_t offset, std::uint32_t expected);
    ObjectPtr from_object_stream(std::uint32_t stream_number, std::uint32_t index, std::uint32_t number);
    std::size_t stream_length(const Dictionary& dictionary, std::size_t data_start);

    std::shared_ptr<const std::vector<std::uint8_t>> bytes_;
    int major_{1}, minor_{4};
    bool encrypted_{}, reconstructed_{};
    ObjectPtr trailer_;
    std::map<std::uint32_t, Entry> entries_;
    std::map<std::uint32_t, ObjectPtr> cache_;
    std::set<std::uint32_t> loading_;          // indirect objects being parsed (cycles)
    std::map<std::uint32_t, std::vector<ObjectPtr>> object_streams_;
    std::vector<ObjectPtr> pages_;
};

// Decoders (exposed for tests).
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode_flate(std::span<const std::uint8_t> input);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode_lzw(std::span<const std::uint8_t> input, bool early_change);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode_ascii_hex(std::span<const std::uint8_t> input);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode_ascii85(std::span<const std::uint8_t> input);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode_run_length(std::span<const std::uint8_t> input);
// PNG (predictor >= 10) and TIFF (2) predictors.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> unpredict(std::vector<std::uint8_t> data, int predictor, int colors,
                                                                 int bits, int columns);

// PDF text strings: UTF-16BE (with a byte order mark), UTF-8 (with one), else
// PDFDocEncoding; as UTF-16 code units.
[[nodiscard]] std::u16string text_string(std::string_view bytes);
// A PDF date ("D:YYYYMMDDHHmmSSOHH'mm'", fields after the year optional) as
// seconds since 2001-01-01 00:00 UTC (CFAbsoluteTime).
[[nodiscard]] std::optional<double> date_seconds(std::string_view bytes);

} // namespace alchemy::gui::quartz2d::pdf
