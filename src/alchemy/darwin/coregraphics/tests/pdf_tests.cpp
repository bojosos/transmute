// The PDF reader (darwin/coregraphics/pdf/pdf_parser.cpp) and the CGPDF
// functions over it (darwin/coregraphics/pdf_document.cpp), on PDF files this
// test writes: one with a cross-reference table, one with a cross-reference
// stream and an object stream, and a damaged one the reader reconstructs.
#include "alchemy/darwin/compression/zlib_codec.hpp"
#include "alchemy/darwin/coregraphics/pdf/pdf_parser.hpp"
#include "alchemy/darwin/coregraphics/tests/quartz2d_fixture.hpp"

#include <iostream>
#include <map>
#include <string>

namespace {
namespace pdf = alchemy::gui::quartz2d::pdf;
using transmute::CpuState;
using transmute::GuestMemory;

std::string flate(std::string_view text) {
    const auto packed = alchemy::export_fill::zlib_compress(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), 6);
    return std::string(packed.begin(), packed.end());
}
std::string hex(std::string_view text) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (const auto c : text) {
        out.push_back(digits[static_cast<unsigned char>(c) >> 4]);
        out.push_back(digits[static_cast<unsigned char>(c) & 15]);
    }
    return out + ">";
}

const std::string page_one_content =
    "q 1 0 0 RG 0 0 m 100 100 l S Q\nBT /F1 12 Tf (Hello) Tj ET\nBI /W 2 /H 1 /BPC 8 /CS /G ID \x01\x02 EI\n";
const std::string page_two_content = "0 g 0 0 10 10 re f\n";

// The objects shared by the files (bodies without "n 0 obj").
std::map<int, std::string> objects() {
    std::map<int, std::string> out;
    out[1] = "<< /Type /Catalog /Pages 2 0 R /Version /1.7 >>";
    out[2] = "<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 /MediaBox [0 0 600 800] /Rotate 90 /Resources << /Font << /F1 7 0 R >> >> >>";
    out[3] = "<< /Type /Page /Parent 2 0 R /Contents 4 0 R /CropBox [10 10 590 790] >>";
    const auto packed = flate(page_one_content);
    out[4] = "<< /Length 5 0 R /Filter /FlateDecode >>\nstream\n" + packed + "\nendstream";
    out[5] = std::to_string(packed.size());
    out[6] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Rotate 0 /Contents [8 0 R] /ArtBox [20 20 100 80] >>";
    out[7] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";
    const auto coded = hex(page_two_content);
    out[8] = "<< /Length " + std::to_string(coded.size()) + " /Filter /ASCIIHexDecode >>\nstream\n" + coded + "\nendstream";
    out[9] = "<< /Title (Test \\(1\\)) /Author <FEFF00410042> /CreationDate (D:20240102030405+01'00') >>";
    return out;
}

// A file with a cross-reference table.
std::string classic_file(bool damage_xref) {
    std::string out = "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n";
    std::map<int, std::size_t> offsets;
    for (const auto& [number, body] : objects()) {
        offsets[number] = out.size();
        out += std::to_string(number) + " 0 obj\n" + body + "\nendobj\n";
    }
    const auto xref = out.size();
    out += "xref\n0 10\n0000000000 65535 f \n";
    for (int i = 1; i < 10; ++i) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", offsets[i]);
        out += line;
    }
    out += "trailer\n<< /Size 10 /Root 1 0 R /Info 9 0 R /ID [<0102> <0304>] >>\nstartxref\n" +
           std::to_string(damage_xref ? xref + 7 : xref) + "\n%%EOF\n";
    return out;
}

// A file with a cross-reference stream (PNG "Up" predictor) and an object stream holding the dictionaries.
std::string stream_file() {
    auto bodies = objects();
    const std::vector<int> packed_objects{1, 2, 3, 5, 6, 7, 9};
    std::string header, contents;
    for (const auto number : packed_objects) {
        header += std::to_string(number) + " " + std::to_string(contents.size()) + " ";
        contents += bodies[number] + "\n";
    }
    const auto object_stream = header + contents;
    std::string out = "%PDF-1.5\n";
    std::map<int, std::size_t> offsets;
    for (const auto number : {4, 8}) {
        offsets[number] = out.size();
        out += std::to_string(number) + " 0 obj\n" + bodies[number] + "\nendobj\n";
    }
    const auto packed = flate(object_stream);
    offsets[10] = out.size();
    out += "10 0 obj\n<< /Type /ObjStm /N " + std::to_string(packed_objects.size()) + " /First " + std::to_string(header.size()) +
           " /Length " + std::to_string(packed.size()) + " /Filter /FlateDecode >>\nstream\n" + packed + "\nendstream\nendobj\n";
    // Entries: type (1 byte), offset or object stream (2 bytes), index (1 byte), each row behind a PNG "Up" filter byte.
    offsets[11] = out.size();
    std::vector<std::array<std::uint8_t, 4>> rows(12);
    rows[0] = {0, 0, 0, 0};
    for (int i = 1; i < 12; ++i) {
        const auto inner = std::find(packed_objects.begin(), packed_objects.end(), i);
        if (inner != packed_objects.end()) rows[static_cast<std::size_t>(i)] = {2, 0, 10, static_cast<std::uint8_t>(inner - packed_objects.begin())};
        else rows[static_cast<std::size_t>(i)] = {1, static_cast<std::uint8_t>(offsets[i] >> 8), static_cast<std::uint8_t>(offsets[i] & 0xff), 0};
    }
    std::string predicted;
    std::array<std::uint8_t, 4> previous{};
    for (const auto& row : rows) {
        predicted.push_back(2);   // Up
        for (std::size_t i = 0; i < 4; ++i) predicted.push_back(static_cast<char>(static_cast<std::uint8_t>(row[i] - previous[i])));
        previous = row;
    }
    const auto table = flate(predicted);
    out += "11 0 obj\n<< /Type /XRef /Size 12 /W [1 2 1] /Root 1 0 R /Info 9 0 R /ID [<0102> <0304>] /Filter /FlateDecode "
           "/DecodeParms << /Predictor 12 /Columns 4 >> /Length " + std::to_string(table.size()) + " >>\nstream\n" + table +
           "\nendstream\nendobj\nstartxref\n" + std::to_string(offsets[11]) + "\n%%EOF\n";
    return out;
}

std::shared_ptr<const std::vector<std::uint8_t>> bytes_of(const std::string& text) {
    return std::make_shared<const std::vector<std::uint8_t>>(text.begin(), text.end());
}

void reader(const std::string& name, const std::string& file, bool expect_reconstructed) {
    std::string error;
    const auto document = pdf::Document::open(bytes_of(file), &error);
    require(document != nullptr, name + ": opens: " + error);
    require(document->reconstructed() == expect_reconstructed, name + ": reconstructed " + std::to_string(document->reconstructed()));
    require(document->major_version() == 1 && document->minor_version() == 7, name + ": the catalog's /Version raises the header's");
    require(document->pages().size() == 2, name + ": two pages, not " + std::to_string(document->pages().size()));
    const auto& first = document->pages()[0];
    const auto rotate = document->inherited(first, "Rotate");
    require(rotate && rotate->kind == pdf::Kind::integer && rotate->integer == 90, name + ": /Rotate is inherited");
    const auto contents = document->get(first, "Contents");
    require(contents && contents->kind == pdf::Kind::stream, name + ": a content stream");
    const auto data = document->stream_data(*contents);
    require(data && std::string(data->begin(), data->end()) == page_one_content, name + ": Flate data with an indirect /Length");
    const auto second = document->get(document->get(document->pages()[1], "Contents"), "Filter");
    static_cast<void>(second);
    const auto info = document->info();
    const auto title = document->get(info, "Title");
    require(title && title->kind == pdf::Kind::string && title->text == "Test (1)", name + ": escaped literal strings");
}

void decoders() {
    require(pdf::text_string(std::string_view("\xfe\xff\x00\x41\x00\x42", 6)) == u"AB" &&
                pdf::text_string("caf\xe9 \x80") == std::u16string{u'c', u'a', u'f', 0x00e9, u' ', 0x2022},
            "text strings: UTF-16BE and PDFDocEncoding");
    const auto date = pdf::date_seconds("D:20240102030405+01'00'");
    require(date && *date == 725853845.0, "a date with an offset: " + std::to_string(date.value_or(-1)));
    require(pdf::date_seconds("D:2001") == 0.0, "a year alone is its first moment");
    const std::string a85 = "87cURD_*#4DfTZ)~>";   // "Hello, World" in ASCII85
    const auto decoded = pdf::decode_ascii85(std::span(reinterpret_cast<const std::uint8_t*>(a85.data()), a85.size()));
    require(decoded && std::string(decoded->begin(), decoded->end()) == "Hello, World", "ASCII85");
    const std::string runs = std::string("\x02" "abc" "\xfd" "x" "\x80", 7);
    const auto expanded = pdf::decode_run_length(std::span(reinterpret_cast<const std::uint8_t*>(runs.data()), runs.size()));
    require(expanded && std::string(expanded->begin(), expanded->end()) == "abcxxxx", "RunLength");
    // LZW: the example of the PDF specification (7.4.4.2).
    const std::array<std::uint8_t, 9> lzw{0x80, 0x0b, 0x60, 0x50, 0x22, 0x0c, 0x0c, 0x85, 0x01};
    const auto unpacked = pdf::decode_lzw(lzw, true);
    require(unpacked && *unpacked == std::vector<std::uint8_t>{45, 45, 45, 45, 45, 65, 45, 45, 45, 66}, "LZW");
}

Address guest_bytes(Fixture& f, const std::string& text) {
    const auto at = f.allocate(text.size());
    f.engine.write_bytes(at, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    return at;
}
Address c_string(Fixture& f, const std::string& text) { return guest_bytes(f, text + std::string(1, '\0')); }
std::string read_c_string(Fixture& f, Address at) {
    std::string out;
    for (char c; (c = static_cast<char>(f.read(at + out.size(), 1)[0])) != 0;) out.push_back(c);
    return out;
}

// The scanner's callbacks (host functions the scanner calls).
struct Scan {
    std::vector<double> line_points;
    std::string shown;
    std::size_t image_bytes{}, rectangles{}, lines{};
};
Scan* current_scan{};

void cgpdf(Fixture& f, const std::string& file) {
    const auto data = f.id(cf_library, "_CFDataCreate", {0, guest_bytes(f, file), file.size()});
    const auto provider = f.id(cg, "_CGDataProviderCreateWithCFData", {data});
    const auto document = f.id(cg, "_CGPDFDocumentCreateWithProvider", {provider});
    require(document != 0, "CGPDFDocumentCreateWithProvider");
    const auto slots = f.allocate(64);
    f.call(cg, "_CGPDFDocumentGetVersion", {document, slots, slots + 4});
    require(f.engine.read32(slots) == 1 && f.engine.read32(slots + 4) == 7, "CGPDFDocumentGetVersion");
    require(f.id(cg, "_CGPDFDocumentGetNumberOfPages", {document}) == 2 && (f.id(cg, "_CGPDFDocumentIsEncrypted", {document}) & 0xff) == 0 &&
                (f.id(cg, "_CGPDFDocumentIsUnlocked", {document}) & 0xff) == 1 && (f.id(cg, "_CGPDFDocumentAllowsPrinting", {document}) & 0xff) == 1,
            "pages and locks");
    require(f.id(cg, "_CGPDFDocumentGetPage", {document, 0}) == 0 && f.id(cg, "_CGPDFDocumentGetPage", {document, 3}) == 0, "pages count from 1");
    const auto first = f.id(cg, "_CGPDFDocumentGetPage", {document, 1});
    const auto second = f.id(cg, "_CGPDFDocumentGetPage", {document, 2});
    require(first && f.id(cg, "_CGPDFDocumentGetPage", {document, 1}) == first && f.id(cg, "_CGPDFPageGetPageNumber", {second}) == 2 &&
                f.id(cg, "_CGPDFPageGetDocument", {first}) == document,
            "CGPDFPage");
    // Boxes and rotation.
    const auto box = [&f](Address page, Address which) {
        const auto r = f.call(cg, "_CGPDFPageGetBoxRect", {page, which});
        return std::array<double, 4>{r.d(0), r.d(1), r.d(2), r.d(3)};
    };
    require(box(first, 0) == std::array<double, 4>{0, 0, 600, 800} && box(first, 1) == std::array<double, 4>{10, 10, 580, 780} &&
                box(first, 4) == std::array<double, 4>{10, 10, 580, 780} && box(second, 4) == std::array<double, 4>{20, 20, 80, 60},
            "media (inherited), crop, art boxes");
    require(f.id(cg, "_CGPDFPageGetRotationAngle", {first}) == 90 && f.id(cg, "_CGPDFPageGetRotationAngle", {second}) == 0, "rotation angles");
    // The 200 x 100 page into a 400 x 400 square, aspect kept: scale 2, centred.
    const auto transform = f.allocate(48);
    static_cast<void>(f.call(cg, "_CGPDFPageGetDrawingTransform", {second, 0, 0, 1}, {0, 0, 400, 400}, transform));
    const std::array<double, 6> expected{2, 0, 0, 2, 0, 100};
    for (std::size_t i = 0; i < 6; ++i)
        require(close_to(f.read_double(transform + i * 8), expected[i], 1e-9), "drawing transform element " + std::to_string(i) + ": " +
                                                                                  std::to_string(f.read_double(transform + i * 8)));
    // A quarter turn: the 600 x 800 page (crop 580 x 780) turned to 780 x 580, fit into 780 x 580.
    static_cast<void>(f.call(cg, "_CGPDFPageGetDrawingTransform", {first, 1, 0, 0}, {0, 0, 780, 580}, transform));
    require(close_to(f.read_double(transform), 0, 1e-9) && close_to(f.read_double(transform + 8), -1, 1e-9) &&
                close_to(f.read_double(transform + 16), 1, 1e-9),
            "a rotated page turns clockwise");

    // The catalog and its objects.
    const auto catalog = f.id(cg, "_CGPDFDocumentGetCatalog", {document});
    require((f.id(cg, "_CGPDFDictionaryGetName", {catalog, c_string(f, "Type"), slots}) & 0xff) == 1 &&
                read_c_string(f, f.engine.read64(slots)) == "Catalog",
            "a name");
    require((f.id(cg, "_CGPDFDictionaryGetDictionary", {catalog, c_string(f, "Pages"), slots}) & 0xff) == 1, "the page tree");
    const auto pages = f.engine.read64(slots);
    require((f.id(cg, "_CGPDFDictionaryGetInteger", {pages, c_string(f, "Count"), slots}) & 0xff) == 1 && f.engine.read64(slots) == 2 &&
                (f.id(cg, "_CGPDFDictionaryGetNumber", {pages, c_string(f, "Count"), slots}) & 0xff) == 1 && f.read_double(slots) == 2 &&
                (f.id(cg, "_CGPDFDictionaryGetString", {pages, c_string(f, "Count"), slots}) & 0xff) == 0,
            "integers, numbers, a type mismatch");
    require((f.id(cg, "_CGPDFDictionaryGetArray", {pages, c_string(f, "Kids"), slots}) & 0xff) == 1, "an array");
    const auto kids = f.engine.read64(slots);
    require(f.id(cg, "_CGPDFArrayGetCount", {kids}) == 2 && (f.id(cg, "_CGPDFArrayGetDictionary", {kids, 0, slots}) & 0xff) == 1 &&
                f.engine.read64(slots) == f.id(cg, "_CGPDFPageGetDictionary", {first}),
            "references resolve to the same handles");
    require((f.id(cg, "_CGPDFDictionaryGetObject", {catalog, c_string(f, "Pages"), slots}) & 0xff) == 1 &&
                f.id(cg, "_CGPDFObjectGetType", {f.engine.read64(slots)}) == 8 &&
                (f.id(cg, "_CGPDFObjectGetValue", {f.engine.read64(slots), 8, slots + 8}) & 0xff) == 1 && f.engine.read64(slots + 8) == pages,
            "objects and their values");
    // The information dictionary's strings.
    const auto info = f.id(cg, "_CGPDFDocumentGetInfo", {document});
    const auto text = [&](const char* key) {
        require((f.id(cg, "_CGPDFDictionaryGetString", {info, c_string(f, key), slots}) & 0xff) == 1, std::string("string ") + key);
        return f.engine.read64(slots);
    };
    const auto title = text("Title");
    require(f.cf.utf8(f.id(cg, "_CGPDFStringCopyTextString", {title})) == "Test (1)" && f.id(cg, "_CGPDFStringGetLength", {title}) == 8 &&
                f.read(f.id(cg, "_CGPDFStringGetBytePtr", {title}), 4) == std::vector<std::uint8_t>{'T', 'e', 's', 't'},
            "a literal string");
    require(f.cf.utf8(f.id(cg, "_CGPDFStringCopyTextString", {text("Author")})) == "AB", "a UTF-16 text string");
    const auto date = f.id(cg, "_CGPDFStringCopyDate", {text("CreationDate")});
    require(date && f.call(cf_library, "_CFDateGetAbsoluteTime", {date}).d(0) == 725853845.0, "a date");
    require(f.id(cg, "_CGPDFArrayGetCount", {f.id(cg, "_CGPDFDocumentGetID", {document})}) == 2, "the file identifier");

    // Content streams, resources and the scanner.
    const auto content = f.id(cg, "_CGPDFContentStreamCreateWithPage", {first});
    require(f.id(cf_library, "_CFArrayGetCount", {f.id(cg, "_CGPDFContentStreamGetStreams", {content})}) == 1, "one content stream");
    const auto font = f.id(cg, "_CGPDFContentStreamGetResource", {content, c_string(f, "Font"), c_string(f, "F1")});
    require(font && (f.id(cg, "_CGPDFDictionaryGetName", {font, c_string(f, "BaseFont"), slots}) & 0xff) == 1 &&
                read_c_string(f, f.engine.read64(slots)) == "Helvetica",
            "a resource");
    Scan scan;
    current_scan = &scan;
    const auto pop_number = [&f](Address scanner) {
        const auto out = f.allocate(8);
        require((f.id(cg, "_CGPDFScannerPopNumber", {scanner, out}) & 0xff) == 1, "a number operand");
        return f.read_double(out);
    };
    const auto lib = "/usr/lib/libpdftest.dylib";
    const auto on_line = f.process.install_function("_pdf_test_l", [&f, pop_number](CpuState& s, GuestMemory&) {
        ++current_scan->lines;
        const auto y = pop_number(s.x[0]), x = pop_number(s.x[0]);
        current_scan->line_points = {x, y};
    }, lib);
    const auto on_show = f.process.install_function("_pdf_test_Tj", [&f](CpuState& s, GuestMemory&) {
        const auto out = f.allocate(8);
        if ((f.id(cg, "_CGPDFScannerPopString", {s.x[0], out}) & 0xff) == 1)
            current_scan->shown = f.cf.utf8(f.id(cg, "_CGPDFStringCopyTextString", {f.engine.read64(out)}));
    }, lib);
    const auto on_image = f.process.install_function("_pdf_test_EI", [&f](CpuState& s, GuestMemory&) {
        const auto out = f.allocate(8);
        if ((f.id(cg, "_CGPDFScannerPopStream", {s.x[0], out}) & 0xff) == 1)
            current_scan->image_bytes = f.id(cf_library, "_CFDataGetLength", {f.id(cg, "_CGPDFStreamCopyData", {f.engine.read64(out), 0})});
    }, lib);
    const auto on_rectangle = f.process.install_function("_pdf_test_re", [](CpuState&, GuestMemory&) { ++current_scan->rectangles; }, lib);
    const auto on_move_stop = f.process.install_function("_pdf_test_m_stop", [&f](CpuState& s, GuestMemory&) {
        f.call(cg, "_CGPDFScannerStop", {s.x[0]});
    }, lib);
    const auto table = f.id(cg, "_CGPDFOperatorTableCreate");
    f.call(cg, "_CGPDFOperatorTableSetCallback", {table, c_string(f, "l"), on_line});
    f.call(cg, "_CGPDFOperatorTableSetCallback", {table, c_string(f, "Tj"), on_show});
    f.call(cg, "_CGPDFOperatorTableSetCallback", {table, c_string(f, "EI"), on_image});
    f.call(cg, "_CGPDFOperatorTableSetCallback", {table, c_string(f, "re"), on_rectangle});
    const auto scanner = f.id(cg, "_CGPDFScannerCreate", {content, table, 0});
    require((f.id(cg, "_CGPDFScannerScan", {scanner}) & 0xff) == 1, "the scan completes");
    require(scan.line_points == std::vector<double>{100, 100} && scan.shown == "Hello" && scan.image_bytes == 2,
            "operators with their operands, an inline image: " + scan.shown + " " + std::to_string(scan.image_bytes));
    const auto second_content = f.id(cg, "_CGPDFContentStreamCreateWithPage", {second});
    const auto second_scanner = f.id(cg, "_CGPDFScannerCreate", {second_content, table, 0});
    require((f.id(cg, "_CGPDFScannerScan", {second_scanner}) & 0xff) == 1 && scan.rectangles == 1, "an ASCIIHex content stream");
    // CGPDFScannerStop ends the scan.
    f.call(cg, "_CGPDFOperatorTableSetCallback", {table, c_string(f, "m"), on_move_stop});
    scan.lines = 0;
    const auto stopped = f.id(cg, "_CGPDFScannerCreate", {content, table, 0});
    static_cast<void>(f.id(cg, "_CGPDFScannerScan", {stopped}));
    require(scan.lines == 0, "no operator after a stop");
    current_scan = nullptr;
    for (const auto object : {scanner, second_scanner, stopped}) f.call(cg, "_CGPDFScannerRelease", {object});
    for (const auto object : {content, second_content}) f.call(cg, "_CGPDFContentStreamRelease", {object});
    f.call(cg, "_CGPDFOperatorTableRelease", {table});
    f.call(cg, "_CGPDFDocumentRelease", {document});
    f.call(cg, "_CGDataProviderRelease", {provider});

    // Not a PDF.
    const auto junk = f.id(cg, "_CGDataProviderCreateWithCFData", {f.id(cf_library, "_CFDataCreate", {0, guest_bytes(f, "hello"), 5})});
    require(f.id(cg, "_CGPDFDocumentCreateWithProvider", {junk}) == 0, "not a PDF");
}

// PDF contexts: two pages drawn and written, then read back with the reader.
void writing(Fixture& f) {
    const auto key = [&f](const char* name) { return f.engine.read64(f.symbol(cg, std::string("_kCGPDFContext") + name)); };
    const auto rect = [&f](double x, double y, double w, double h) {
        const auto at = f.allocate(32);
        const std::array<double, 4> v{x, y, w, h};
        for (std::size_t i = 0; i < 4; ++i) f.engine.write64(at + i * 8, std::bit_cast<std::uint64_t>(v[i]));
        return at;
    };
    // A password asks for encryption, which is not provided: no context.
    const auto locked = f.cf.dictionary(std::vector<std::pair<Address, Address>>{{key("UserPassword"), f.str("secret")}});
    const auto sink = f.id(cg, "_CGDataConsumerCreateWithCFData", {f.id(cf_library, "_CFDataCreateMutable", {0, 0})});
    require(f.id(cg, "_CGPDFContextCreate", {sink, 0, locked}) == 0, "an encrypted PDF context is refused");

    const auto data = f.id(cf_library, "_CFDataCreateMutable", {0, 0});
    const auto consumer = f.id(cg, "_CGDataConsumerCreateWithCFData", {data});
    const auto info = f.cf.dictionary(std::vector<std::pair<Address, Address>>{{key("Title"), f.str("Report")}, {key("Author"), f.str("Ann")}});
    const auto context = f.id(cg, "_CGPDFContextCreate", {consumer, rect(0, 0, 200, 100), info});
    require(context != 0, "CGPDFContextCreate");
    f.call(cg, "_CGContextBeginPage", {context, rect(0, 0, 200, 100)});
    f.call(cg, "_CGContextSetRGBFillColor", {context}, {1, 0, 0, 0.5});
    f.call(cg, "_CGContextFillRect", {context}, {10, 10, 50, 30});
    f.call(cg, "_CGContextSetRGBStrokeColor", {context}, {0, 0, 1, 1});
    f.call(cg, "_CGContextSetLineWidth", {context}, {2});
    f.call(cg, "_CGContextStrokeRect", {context}, {5, 5, 190, 90});
    // A gradient whose second stop is half transparent, clipped.
    f.call(cg, "_CGContextSaveGState", {context});
    f.call(cg, "_CGContextClipToRect", {context}, {0, 0, 100, 100});
    const auto space = f.id(cg, "_CGColorSpaceCreateDeviceRGB");
    const auto components = f.allocate(64);
    const std::array<double, 8> colours{1, 0, 0, 1, 0, 0, 1, 0.5};
    for (std::size_t i = 0; i < colours.size(); ++i) f.engine.write64(components + i * 8, std::bit_cast<std::uint64_t>(colours[i]));
    const auto gradient = f.id(cg, "_CGGradientCreateWithColorComponents", {space, components, 0, 2});
    f.call(cg, "_CGContextDrawLinearGradient", {context, gradient, 0}, {0, 0, 100, 0});
    f.call(cg, "_CGContextRestoreGState", {context});
    // A 2 x 2 image.
    Canvas pixels(f, 2, 2);
    pixels.rgb_fill(0, 1, 0, 1);
    pixels.fill(0, 0, 2, 2);
    const auto image = f.id(cg, "_CGBitmapContextCreateImage", {pixels.context});
    f.call(cg, "_CGContextDrawImage", {context, image}, {120, 20, 40, 40});
    // A link and a destination.
    const auto url = f.cf.file_url("/tmp/report.pdf", false);
    require(url != 0, "a file URL");
    f.call(cg, "_CGPDFContextSetURLForRect", {context, url}, {10, 10, 50, 30});
    f.call(cg, "_CGPDFContextAddDestinationAtPoint", {context, f.str("top")}, {0, 100});
    f.call(cg, "_CGContextEndPage", {context});
    // A second page with its own media box.
    const auto box = f.id(cf_library, "_CFDataCreate", {0, rect(0, 0, 300, 300), 32});
    f.call(cg, "_CGPDFContextBeginPage", {context, f.cf.dictionary(std::vector<std::pair<Address, Address>>{{key("MediaBox"), box}})});
    f.call(cg, "_CGContextFillRect", {context}, {0, 0, 10, 10});
    f.call(cg, "_CGPDFContextEndPage", {context});
    f.call(cg, "_CGPDFContextClose", {context});
    f.call(cg, "_CGContextRelease", {context});

    const auto length = f.id(cf_library, "_CFDataGetLength", {data});
    const auto written = f.read(f.id(cf_library, "_CFDataGetBytePtr", {data}), length);
    std::string error;
    const auto document = pdf::Document::open(std::make_shared<const std::vector<std::uint8_t>>(written), &error);
    require(document != nullptr, "the written PDF opens: " + error);
    require(!document->reconstructed() && document->pages().size() == 2, "two pages and a valid cross-reference table");
    const auto media = [&](std::size_t page) {
        std::vector<double> out;
        for (const auto& value : document->get(document->pages()[page], "MediaBox")->array) out.push_back(value->number_value());
        return out;
    };
    require(media(0) == std::vector<double>{0, 0, 200, 100} && media(1) == std::vector<double>{0, 0, 300, 300}, "media boxes");
    const auto title = document->get(document->info(), "Title");
    require(title && title->text == "Report" && document->get(document->info(), "Author")->text == "Ann", "the information dictionary");
    const auto contents = document->get(document->pages()[0], "Contents");
    const auto content = document->stream_data(*contents);
    require(content.has_value(), "page content decodes");
    const std::string text(content->begin(), content->end());
    for (const auto* expected : {"1 0 0 rg", "f\n", "0 0 1 RG", "2 w", "S\n", "W n", " sh\n", " Do\n", " gs\n"})
        require(text.find(expected) != std::string::npos, std::string("page content has \"") + expected + "\":\n" + text);
    // The image: 2 x 2 RGB, green.
    const auto resources = document->get(document->pages()[0], "Resources");
    const auto xobjects = document->get(resources, "XObject");
    require(xobjects && xobjects->dictionary.entries.size() == 1, "one image");
    const auto picture = document->resolve(xobjects->dictionary.entries[0].second);
    const auto samples = document->stream_data(*picture);
    require(samples && samples->size() == 12 && (*samples)[1] == 255 && (*samples)[0] == 0, "the image's samples");
    // The half-transparent stop: a soft mask.
    const auto states = document->get(resources, "ExtGState");
    bool soft_mask = false;
    for (const auto& [name, state] : states->dictionary.entries) soft_mask = soft_mask || document->get(document->resolve(state), "SMask");
    require(soft_mask, "gradient alpha as a soft mask");
    // The link and the destination.
    const auto annotations = document->get(document->pages()[0], "Annots");
    require(annotations && annotations->array.size() == 1, "a link annotation: " + std::string(annotations ? std::to_string(annotations->array.size()) : "none") + " in\n" +
                std::string(written.begin(), written.end()));
    const auto action = document->get(document->resolve(annotations->array[0]), "A");
    const auto uri = document->get(action, "URI")->text;
    require(uri.starts_with("file:") && uri.ends_with("/report.pdf"), "the link's URL: " + uri);
    const auto destinations = document->get(document->catalog(), "Dests");
    require(destinations && document->get(destinations, "top"), "the named destination");
}

} // namespace

int main() {
    try {
        decoders();
        reader("table", classic_file(false), false);
        reader("stream", stream_file(), false);
        reader("damaged", classic_file(true), true);
        Fixture f;
        cgpdf(f, classic_file(false));
        cgpdf(f, stream_file());
        writing(f);
        std::cout << "pdf: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pdf: FAILED: " << error.what() << "\n";
        return 1;
    }
}
