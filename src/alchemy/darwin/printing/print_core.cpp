#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"

#include <bit>
#include <cstdio>
#include <tuple>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// PrintCore (ApplicationServices' PM* functions), both builds. There are no
// printers, but the printing objects are real:
// * PMPrintSettings: copies, collation, first / last page, the page range,
//   duplex mode, job name, the destination (type, document format, location
//   URL) and any other key's value (PMPrintSettingsSetValue / GetValue).
// * PMPageFormat: its paper, orientation and scale; the adjusted and
//   unadjusted page and paper rectangles follow from them.
// * PMPaper: an identifier, a name, width, height and margins (the imageable
//   area). The default paper is US Letter (612 x 792 points) with 18-point
//   margins *(Alchemy's choice: there is no printer to ask)*.
// * PMPrintSession: defaults, validation (nothing to change), the last error,
//   and drawing. A document whose destination is a file is drawn into a PDF
//   at the destination URL through CoreGraphics' CGPDFContext: pages are
//   media boxes of the page format's paper. Any other destination (a printer,
//   the default; a preview; a fax) fails with kPMNoDefaultPrinter: no job is
//   ever reported that did not happen. Without CoreGraphics (the headless
//   build) drawing fails with kPMNotImplemented.
// * Data representations are property lists of these values (XML); they
//   round-trip between Alchemy processes, not with macOS.
// * Printers: none exist. PMServerCreatePrinterList gives an empty list,
//   PMPrinterCreateFromPrinterID gives NULL, sessions have no current printer
//   (kPMNoDefaultPrinter), and calls on a printer (necessarily NULL) answer
//   kPMInvalidParameter with their outputs cleared, or NULL / false.
// PMRetain / PMRelease count references on these objects (and release the
// CFArrays PMServerCreatePrinterList makes).
// Replaces no_printers.cpp, and the PM names of printcore.cpp (fill) and
// print_core_gaps.cpp (values): this file is the only owner of the PM names.
namespace alchemy::darwin_headless {
namespace {

constexpr std::int32_t no_err = 0, param_err = -50;
constexpr std::int32_t pm_no_default_printer = -30872, pm_not_implemented = -30873, pm_invalid_print_settings = -30875,
                       pm_invalid_page_format = -30876, pm_invalid_print_session = -30879, pm_invalid_printer = -30880;
constexpr std::uint32_t print_all_pages = 0xffffffffU;   // kPMPrintAllPages
constexpr std::uint32_t destination_printer = 1, destination_file = 2;
constexpr std::string_view core_foundation_library = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view default_format = "com.apple.documentformat.default", pdf_format = "application/pdf";

struct Margins { double top{18}, left{18}, bottom{18}, right{18}; };
struct PaperState final : ObjectPayload {
    std::string id{"na-letter"}, name{"US Letter"};
    double width{612}, height{792};
    Margins margins;
};
struct FormatState final : ObjectPayload {
    Address paper{};       // +1
    std::uint32_t orientation{1};   // kPMPortrait 1, kPMLandscape 2, reverse portrait 3, reverse landscape 4
    double scale{100};     // percent
};
struct SettingsState final : ObjectPayload {
    std::uint32_t copies{1}, first{1}, last{print_all_pages}, range_min{1}, range_max{print_all_pages}, duplex{1};
    bool collate{true};
    std::string job_name;
    std::uint32_t destination{destination_printer};
    std::string format{default_format};
    Address location{};    // +1 CFURL
    std::map<std::string, Address> values;   // +1 each, by key
};
struct SessionState final : ObjectPayload {
    std::int32_t error{no_err};
    Address context{};     // +1 CGContext while a document is open
    bool in_page{};
};

class PrintCore final : public Provider {
public:
    using Provider::Provider;

    Address paper_class{}, format_class{}, settings_class{}, session_class{};
    Address scratch{};     // 32 bytes for a CGRect

    std::uint64_t ret(std::int32_t code) { return status(code); }
    bool is(Address object, Address cls) { return object && objc.object_class(object) == cls; }
    template <class T> std::shared_ptr<T> state(Address object) { return objc.host_state<T>(object); }
    void put32(Address at, std::uint32_t value) { if (at) memory().write32(at, value); }
    void put64(Address at, std::uint64_t value) { if (at) memory().write64(at, value); }
    void put_double(Address at, double value) { if (at) memory().write64(at, std::bit_cast<std::uint64_t>(value)); }
    double get_double(Address at) { return std::bit_cast<double>(memory().read64(at)); }
    void put_rect(Address at, double top, double left, double bottom, double right) {   // PMRect / PMPaperMargins
        put_double(at, top); put_double(at + 8, left); put_double(at + 16, bottom); put_double(at + 24, right);
    }
    void cf_release(Address object) { if (object) static_cast<void>(guest_call(core_foundation_library, "_CFRelease", {object})); }
    Address cf_retain(Address object) { return object ? guest_call(core_foundation_library, "_CFRetain", {object}) : 0; }
    std::string text(Address string) { return string ? cf.utf8(string) : std::string(); }

    Address make(Address cls) { return objc.allocate(cls); }   // +1
    Address make_paper(const PaperState& from) {
        const auto made = make(paper_class);
        *state<PaperState>(made) = from;
        return made;
    }
    Address make_format(Address paper) {
        const auto made = make(format_class);
        state<FormatState>(made)->paper = paper ? retain(paper) : make_paper({});
        return made;
    }
    void copy_settings(const SettingsState& from, SettingsState& to) {
        cf_release(std::exchange(to.location, 0));
        for (const auto& [key, value] : std::exchange(to.values, {})) { static_cast<void>(key); cf_release(value); }
        to = from;
        to.location = cf_retain(from.location);
        for (auto& [key, value] : to.values) { static_cast<void>(key); value = cf_retain(value); }
    }

    // The page format's rectangles (points, PMRect top / left / bottom / right). The paper
    // rectangle is relative to the page's imageable origin; landscape swaps the axes; the
    // scale shrinks or grows both.
    struct Rects { double page_w, page_h, top, left, paper_w, paper_h; };
    Rects rects_of(Address format) {
        const auto f = state<FormatState>(format);
        const auto p = state<PaperState>(f->paper);
        const bool landscape = f->orientation == 2 || f->orientation == 4;
        Margins m = p->margins;
        double w = p->width, h = p->height;
        if (landscape) { std::swap(w, h); m = {p->margins.left, p->margins.bottom, p->margins.right, p->margins.top}; }
        return {w - m.left - m.right, h - m.top - m.bottom, m.top, m.left, w, h};
    }

    // ---- data representations: property lists of the values ----
    Address number(double value) { return cf.real(value); }
    Address settings_plist(const SettingsState& s) {
        std::vector<std::pair<Address, Address>> entries;
        const auto add = [&](std::string_view key, Address value) { if (value) entries.emplace_back(cf.string(key), value); };
        add("AlchemyPMCopies", cf.integer(s.copies));
        add("AlchemyPMFirstPage", cf.integer(s.first));
        add("AlchemyPMLastPage", cf.integer(s.last));
        add("AlchemyPMRangeMin", cf.integer(s.range_min));
        add("AlchemyPMRangeMax", cf.integer(s.range_max));
        add("AlchemyPMDuplex", cf.integer(s.duplex));
        add("AlchemyPMCollate", cf.integer(s.collate ? 1 : 0));
        add("AlchemyPMJobName", cf.string(s.job_name));
        add("AlchemyPMDestination", cf.integer(s.destination));
        add("AlchemyPMDestinationFormat", cf.string(s.format));
        if (s.location) add("AlchemyPMDestinationURL", cf.string(cf.url_path(s.location)));
        std::vector<std::pair<Address, Address>> values;
        for (const auto& [key, value] : s.values) values.emplace_back(cf.string(key), cf_retain(value));
        add("AlchemyPMValues", cf.dictionary(values));
        for (const auto& [key, value] : values) { cf_release(key); cf_release(value); }
        const auto dictionary = cf.dictionary(entries);
        for (const auto& [key, value] : entries) { cf_release(key); cf_release(value); }
        return dictionary;
    }
    Address plist_data(Address dictionary) {
        const auto data = guest_call(core_foundation_library, "_CFPropertyListCreateData", {0, dictionary, 100 /* XML */, 0, 0});
        cf_release(dictionary);
        return data;
    }
    Address plist_from(Address data) {
        if (!data) return 0;
        const auto value = guest_call(core_foundation_library, "_CFPropertyListCreateWithData", {0, data, 0, 0, 0});
        if (value && cf.kind(value) != ObjectKind::dictionary) { cf_release(value); return 0; }
        return value;
    }
    double plist_number(Address dictionary, std::string_view key, double fallback) {
        const auto name = cf.string(key);
        const auto value = cf.dictionary_value(dictionary, name);
        cf_release(name);
        return value ? cf.number_value(value).value_or(fallback) : fallback;
    }
    std::string plist_text(Address dictionary, std::string_view key) {
        const auto name = cf.string(key);
        const auto value = cf.dictionary_value(dictionary, name);
        cf_release(name);
        return value && cf.kind(value) == ObjectKind::string ? cf.utf8(value) : std::string();
    }

    // ---- drawing ----
    bool can_draw() { return context.resolve(core_graphics, "_CGPDFContextCreateWithURL").has_value(); }
    Address media_box(double width, double height) {
        if (!scratch) scratch = context.allocate_guest(32, 8);
        put_double(scratch, 0); put_double(scratch + 8, 0); put_double(scratch + 16, width); put_double(scratch + 24, height);
        return scratch;
    }

    void install();
};

void PrintCore::install() {
    const auto root = objc.find_class("NSObject");
    if (!root) throw std::runtime_error("Alchemy PrintCore: NSObject is missing");
    const auto declare = [&](const char* name) {
        auto cls = objc.find_class(name);
        if (!cls) cls = objc.register_native_class(name, root, 8);
        return cls;
    };
    paper_class = declare("_AlchemyPMPaper");
    format_class = declare("_AlchemyPMPageFormat");
    settings_class = declare("_AlchemyPMPrintSettings");
    session_class = declare("_AlchemyPMPrintSession");
    // Deallocation releases what each object holds.
    method("_AlchemyPMPageFormat", "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto f = objc.find_host_state<FormatState>(s.x[0])) release(std::exchange(f->paper, 0));
        static_cast<void>(objc.send_super(s.x[0], format_class, objc.selector("dealloc")));
    }, false, "v@:");
    method("_AlchemyPMPrintSettings", "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto p = objc.find_host_state<SettingsState>(s.x[0])) copy_settings({}, *p);
        static_cast<void>(objc.send_super(s.x[0], settings_class, objc.selector("dealloc")));
    }, false, "v@:");
    method("_AlchemyPMPrintSession", "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto p = objc.find_host_state<SessionState>(s.x[0]); p && p->context) {
            static_cast<void>(guest_call(core_graphics, "_CGPDFContextClose", {p->context}));
            static_cast<void>(guest_call(core_graphics, "_CGContextRelease", {std::exchange(p->context, 0)}));
        }
        static_cast<void>(objc.send_super(s.x[0], session_class, objc.selector("dealloc")));
    }, false, "v@:");

    const auto out_object = [this](CpuState& s, unsigned index, Address value) {
        if (!s.x[index]) { release(value); s.x[0] = ret(param_err); return; }
        put64(s.x[index], value);
        s.x[0] = ret(no_err);
    };
    // ---- creating, retaining, copying ----
    function(application_services, "_PMCreateSession", [this, out_object](CpuState& s, GuestMemory&) { out_object(s, 0, make(session_class)); });
    function(application_services, "_PMCreatePrintSettings", [this, out_object](CpuState& s, GuestMemory&) { out_object(s, 0, make(settings_class)); });
    function(application_services, "_PMCreatePageFormat", [this, out_object](CpuState& s, GuestMemory&) { out_object(s, 0, make_format(0)); });
    function(application_services, "_PMCreatePageFormatWithPMPaper", [this, out_object](CpuState& s, GuestMemory&) {
        if (!is(s.x[1], paper_class)) { put64(s.x[0], 0); s.x[0] = ret(param_err); return; }
        out_object(s, 0, make_format(s.x[1]));
    });
    function(application_services, "_PMRetain", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0]) { s.x[0] = ret(param_err); return; }
        static_cast<void>(retain(s.x[0]));
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMRelease", [this](CpuState& s, GuestMemory&) {
        const auto object = s.x[0];
        if (!object) { s.x[0] = ret(param_err); return; }
        const auto cls = objc.object_class(object);
        if (cls == paper_class || cls == format_class || cls == settings_class || cls == session_class) release(object);
        else context.objects().release(object);   // the CFArray of PMServerCreatePrinterList
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMCopyPrintSettings", [this](CpuState& s, GuestMemory&) {
        if (!is(s.x[0], settings_class) || !is(s.x[1], settings_class)) { s.x[0] = ret(pm_invalid_print_settings); return; }
        if (s.x[0] != s.x[1]) copy_settings(*state<SettingsState>(s.x[0]), *state<SettingsState>(s.x[1]));
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMCopyPageFormat", [this](CpuState& s, GuestMemory&) {
        if (!is(s.x[0], format_class) || !is(s.x[1], format_class)) { s.x[0] = ret(pm_invalid_page_format); return; }
        if (s.x[0] != s.x[1]) {
            const auto from = state<FormatState>(s.x[0]);
            const auto to = state<FormatState>(s.x[1]);
            const auto paper = make_paper(*state<PaperState>(from->paper));
            release(std::exchange(to->paper, paper));
            to->orientation = from->orientation;
            to->scale = from->scale;
        }
        s.x[0] = ret(no_err);
    });

    // ---- PMPaper ----
    // PMPaperCreateCustom(printer, id, name, width (d0), height (d1), margins, &paper)
    function(application_services, "_PMPaperCreateCustom", [this, out_object](CpuState& s, GuestMemory&) {
        const double width = real(s, 0), height = real(s, 1);
        if (!(width > 0) || !(height > 0) || !s.x[4]) { put64(s.x[4], 0); s.x[0] = ret(param_err); return; }
        PaperState paper;
        paper.id = s.x[1] ? text(s.x[1]) : "custom";
        paper.name = s.x[2] ? text(s.x[2]) : paper.id;
        paper.width = width;
        paper.height = height;
        paper.margins = s.x[3] ? Margins{get_double(s.x[3]), get_double(s.x[3] + 8), get_double(s.x[3] + 16), get_double(s.x[3] + 24)} : Margins{0, 0, 0, 0};
        out_object(s, 4, make_paper(paper));
    });
    const auto paper_double = [this](const char* symbol, double PaperState::*member) {
        function(application_services, symbol, [this, member](CpuState& s, GuestMemory&) {
            if (!is(s.x[0], paper_class) || !s.x[1]) { s.x[0] = ret(param_err); return; }
            put_double(s.x[1], (*state<PaperState>(s.x[0])).*member);
            s.x[0] = ret(no_err);
        });
    };
    paper_double("_PMPaperGetWidth", &PaperState::width);
    paper_double("_PMPaperGetHeight", &PaperState::height);
    function(application_services, "_PMPaperGetMargins", [this](CpuState& s, GuestMemory&) {
        if (!is(s.x[0], paper_class) || !s.x[1]) { s.x[0] = ret(param_err); return; }
        const auto& m = state<PaperState>(s.x[0])->margins;
        put_rect(s.x[1], m.top, m.left, m.bottom, m.right);
        s.x[0] = ret(no_err);
    });
    // The paper's identifier (not retained: a string the paper owns for its lifetime).
    const auto paper_string = [this](const char* symbol, bool name, bool copy) {
        function(application_services, symbol, [this, name, copy](CpuState& s, GuestMemory&) {
            const auto out = copy ? s.x[2] : s.x[1];
            if (!is(s.x[0], paper_class) || !out) { put64(out, 0); s.x[0] = ret(param_err); return; }
            const auto p = state<PaperState>(s.x[0]);
            put64(out, copy ? cf.string(name ? p->name : p->id) : constant_string(name ? p->name : p->id));
            s.x[0] = ret(no_err);
        });
    };
    paper_string("_PMPaperGetPPDPaperName", false, false);
    paper_string("_PMPaperGetID", false, false);
    paper_string("_PMPaperCreateLocalizedName", true, true);   // (paper, printer, &name): +1

    // ---- PMPageFormat ----
    function(application_services, "_PMGetPageFormatPaper", [this](CpuState& s, GuestMemory&) {
        if (!is(s.x[0], format_class) || !s.x[1]) { put64(s.x[1], 0); s.x[0] = ret(param_err); return; }
        put64(s.x[1], state<FormatState>(s.x[0])->paper);
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMGetOrientation", [this](CpuState& s, GuestMemory&) {
        if (!is(s.x[0], format_class) || !s.x[1]) { s.x[0] = ret(param_err); return; }
        put32(s.x[1], state<FormatState>(s.x[0])->orientation);
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMSetOrientation", [this](CpuState& s, GuestMemory&) {
        const auto value = static_cast<std::uint32_t>(s.x[1]);
        if (!is(s.x[0], format_class) || value < 1 || value > 4) { s.x[0] = ret(param_err); return; }
        state<FormatState>(s.x[0])->orientation = value;
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMGetScale", [this](CpuState& s, GuestMemory&) {
        if (!is(s.x[0], format_class) || !s.x[1]) { s.x[0] = ret(param_err); return; }
        put_double(s.x[1], state<FormatState>(s.x[0])->scale);
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMSetScale", [this](CpuState& s, GuestMemory&) {
        if (!is(s.x[0], format_class) || !(real(s, 0) > 0)) { s.x[0] = ret(param_err); return; }
        state<FormatState>(s.x[0])->scale = real(s, 0);
        s.x[0] = ret(no_err);
    });
    // Adjusted rectangles include the scale; unadjusted ones do not.
    for (const auto& [symbol, paper, adjusted] : std::initializer_list<std::tuple<const char*, bool, bool>>{
             {"_PMGetAdjustedPageRect", false, true}, {"_PMGetAdjustedPaperRect", true, true},
             {"_PMGetUnadjustedPageRect", false, false}, {"_PMGetUnadjustedPaperRect", true, false}}) {
        function(application_services, symbol, [this, paper = paper, adjusted = adjusted](CpuState& s, GuestMemory&) {
            if (!is(s.x[0], format_class) || !s.x[1]) { s.x[0] = ret(param_err); return; }
            const auto r = rects_of(s.x[0]);
            const double k = adjusted ? 100.0 / state<FormatState>(s.x[0])->scale : 1.0;
            if (paper) put_rect(s.x[1], -r.top * k, -r.left * k, (r.paper_h - r.top) * k, (r.paper_w - r.left) * k);
            else put_rect(s.x[1], 0, 0, r.page_h * k, r.page_w * k);
            s.x[0] = ret(no_err);
        });
    }

    // ---- PMPrintSettings ----
    const auto settings_u32 = [this](const char* getter, const char* setter, std::uint32_t SettingsState::*member, std::uint32_t lowest) {
        function(application_services, getter, [this, member](CpuState& s, GuestMemory&) {
            if (!is(s.x[0], settings_class) || !s.x[1]) { s.x[0] = ret(param_err); return; }
            put32(s.x[1], (*state<SettingsState>(s.x[0])).*member);
            s.x[0] = ret(no_err);
        });
        function(application_services, setter, [this, member, lowest](CpuState& s, GuestMemory&) {
            const auto value = static_cast<std::uint32_t>(s.x[1]);
            if (!is(s.x[0], settings_class) || value < lowest) { s.x[0] = ret(param_err); return; }
            (*state<SettingsState>(s.x[0])).*member = value;
            s.x[0] = ret(no_err);
        });
    };
    settings_u32("_PMGetCopies", "_PMSetCopies", &SettingsState::copies, 1);
    settings_u32("_PMGetFirstPage", "_PMSetFirstPage", &SettingsState::first, 1);
    settings_u32("_PMGetLastPage", "_PMSetLastPage", &SettingsState::last, 1);
    settings_u32("_PMGetDuplex", "_PMSetDuplex", &SettingsState::duplex, 1);
    function(application_services, "_PMGetCollate", [this](CpuState& s, GuestMemory& m) {
        if (!is(s.x[0], settings_class) || !s.x[1]) { s.x[0] = ret(param_err); return; }
        m.write8(s.x[1], state<SettingsState>(s.x[0])->collate ? 1 : 0);
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMSetCollate", [this](CpuState& s, GuestMemory&) {
        if (!is(s.x[0], settings_class)) { s.x[0] = ret(param_err); return; }
        state<SettingsState>(s.x[0])->collate = (s.x[1] & 0xff) != 0;
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMGetPageRange", [this](CpuState& s, GuestMemory&) {
        if (!is(s.x[0], settings_class) || !s.x[1] || !s.x[2]) { s.x[0] = ret(param_err); return; }
        const auto p = state<SettingsState>(s.x[0]);
        put32(s.x[1], p->range_min);
        put32(s.x[2], p->range_max);
        s.x[0] = ret(no_err);
    });
    // Setting the range also brings the first and last pages inside it.
    function(application_services, "_PMSetPageRange", [this](CpuState& s, GuestMemory&) {
        const auto low = static_cast<std::uint32_t>(s.x[1]), high = static_cast<std::uint32_t>(s.x[2]);
        if (!is(s.x[0], settings_class) || low < 1 || high < low) { s.x[0] = ret(param_err); return; }
        const auto p = state<SettingsState>(s.x[0]);
        p->range_min = low;
        p->range_max = high;
        p->first = std::clamp(p->first, low, high);
        p->last = std::clamp(p->last, low, high);
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMPrintSettingsGetJobName", [this](CpuState& s, GuestMemory&) {
        if (!is(s.x[0], settings_class) || !s.x[1]) { put64(s.x[1], 0); s.x[0] = ret(param_err); return; }
        const auto p = state<SettingsState>(s.x[0]);
        put64(s.x[1], p->job_name.empty() ? 0 : constant_string(p->job_name));
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMPrintSettingsSetJobName", [this](CpuState& s, GuestMemory&) {
        if (!is(s.x[0], settings_class)) { s.x[0] = ret(param_err); return; }
        state<SettingsState>(s.x[0])->job_name = text(s.x[1]);
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMPrintSettingsGetValue", [this](CpuState& s, GuestMemory&) {
        if (!is(s.x[0], settings_class) || !s.x[1] || !s.x[2]) { put64(s.x[2], 0); s.x[0] = ret(param_err); return; }
        const auto p = state<SettingsState>(s.x[0]);
        const auto found = p->values.find(cf.utf8(s.x[1]));
        put64(s.x[2], found == p->values.end() ? 0 : found->second);
        s.x[0] = ret(no_err);
    });
    // PMPrintSettingsSetValue(settings, key, value, locked): NULL removes the key.
    function(application_services, "_PMPrintSettingsSetValue", [this](CpuState& s, GuestMemory&) {
        if (!is(s.x[0], settings_class) || !s.x[1]) { s.x[0] = ret(param_err); return; }
        const auto p = state<SettingsState>(s.x[0]);
        auto& slot = p->values[cf.utf8(s.x[1])];
        const auto old = std::exchange(slot, cf_retain(s.x[2]));
        if (!slot) p->values.erase(cf.utf8(s.x[1]));
        cf_release(old);
        s.x[0] = ret(no_err);
    });

    // ---- data representations ----
    function(application_services, "_PMPrintSettingsCreateDataRepresentation", [this](CpuState& s, GuestMemory&) {
        if (!is(s.x[0], settings_class) || !s.x[1]) { put64(s.x[1], 0); s.x[0] = ret(param_err); return; }
        put64(s.x[1], plist_data(settings_plist(*state<SettingsState>(s.x[0]))));
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMPrintSettingsCreateWithDataRepresentation", [this](CpuState& s, GuestMemory&) {
        if (!s.x[1]) { s.x[0] = ret(param_err); return; }
        const auto plist = plist_from(s.x[0]);
        if (!plist) { put64(s.x[1], 0); s.x[0] = ret(pm_invalid_print_settings); return; }
        const auto made = make(settings_class);
        auto p = state<SettingsState>(made);
        p->copies = static_cast<std::uint32_t>(plist_number(plist, "AlchemyPMCopies", 1));
        p->first = static_cast<std::uint32_t>(plist_number(plist, "AlchemyPMFirstPage", 1));
        p->last = static_cast<std::uint32_t>(plist_number(plist, "AlchemyPMLastPage", print_all_pages));
        p->range_min = static_cast<std::uint32_t>(plist_number(plist, "AlchemyPMRangeMin", 1));
        p->range_max = static_cast<std::uint32_t>(plist_number(plist, "AlchemyPMRangeMax", print_all_pages));
        p->duplex = static_cast<std::uint32_t>(plist_number(plist, "AlchemyPMDuplex", 1));
        p->collate = plist_number(plist, "AlchemyPMCollate", 1) != 0;
        p->job_name = plist_text(plist, "AlchemyPMJobName");
        p->destination = static_cast<std::uint32_t>(plist_number(plist, "AlchemyPMDestination", destination_printer));
        p->format = plist_text(plist, "AlchemyPMDestinationFormat");
        if (const auto path = plist_text(plist, "AlchemyPMDestinationURL"); !path.empty()) p->location = cf.file_url(path, false);
        const auto key = cf.string("AlchemyPMValues");
        if (const auto values = cf.dictionary_value(plist, key))
            for (const auto& [k, v] : cf.dictionary_entries(values)) p->values[cf.utf8(k)] = cf_retain(v);
        cf_release(key);
        cf_release(plist);
        put64(s.x[1], made);
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMPageFormatCreateDataRepresentation", [this](CpuState& s, GuestMemory&) {
        if (!is(s.x[0], format_class) || !s.x[1]) { put64(s.x[1], 0); s.x[0] = ret(param_err); return; }
        const auto f = state<FormatState>(s.x[0]);
        const auto p = state<PaperState>(f->paper);
        std::vector<std::pair<Address, Address>> entries;
        const auto add = [&](std::string_view key, Address value) { entries.emplace_back(cf.string(key), value); };
        add("AlchemyPMOrientation", cf.integer(f->orientation));
        add("AlchemyPMScale", number(f->scale));
        add("AlchemyPMPaperID", cf.string(p->id));
        add("AlchemyPMPaperName", cf.string(p->name));
        add("AlchemyPMPaperWidth", number(p->width));
        add("AlchemyPMPaperHeight", number(p->height));
        add("AlchemyPMMarginTop", number(p->margins.top));
        add("AlchemyPMMarginLeft", number(p->margins.left));
        add("AlchemyPMMarginBottom", number(p->margins.bottom));
        add("AlchemyPMMarginRight", number(p->margins.right));
        const auto dictionary = cf.dictionary(entries);
        for (const auto& [key, value] : entries) { cf_release(key); cf_release(value); }
        put64(s.x[1], plist_data(dictionary));
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMPageFormatCreateWithDataRepresentation", [this](CpuState& s, GuestMemory&) {
        if (!s.x[1]) { s.x[0] = ret(param_err); return; }
        const auto plist = plist_from(s.x[0]);
        if (!plist) { put64(s.x[1], 0); s.x[0] = ret(pm_invalid_page_format); return; }
        PaperState paper;
        paper.id = plist_text(plist, "AlchemyPMPaperID");
        paper.name = plist_text(plist, "AlchemyPMPaperName");
        paper.width = plist_number(plist, "AlchemyPMPaperWidth", 612);
        paper.height = plist_number(plist, "AlchemyPMPaperHeight", 792);
        paper.margins = {plist_number(plist, "AlchemyPMMarginTop", 18), plist_number(plist, "AlchemyPMMarginLeft", 18),
                         plist_number(plist, "AlchemyPMMarginBottom", 18), plist_number(plist, "AlchemyPMMarginRight", 18)};
        const auto paper_object = make_paper(paper);
        const auto made = make_format(paper_object);
        release(paper_object);
        auto f = state<FormatState>(made);
        f->orientation = static_cast<std::uint32_t>(plist_number(plist, "AlchemyPMOrientation", 1));
        f->scale = plist_number(plist, "AlchemyPMScale", 100);
        cf_release(plist);
        put64(s.x[1], made);
        s.x[0] = ret(no_err);
    });

    // ---- PMPrintSession ----
    const auto session_ok = [this](Address session) { return is(session, session_class); };
    function(application_services, "_PMSessionError", [this, session_ok](CpuState& s, GuestMemory&) {
        s.x[0] = ret(session_ok(s.x[0]) ? state<SessionState>(s.x[0])->error : param_err);
    });
    function(application_services, "_PMSessionDefaultPrintSettings", [this, session_ok](CpuState& s, GuestMemory&) {
        if (!session_ok(s.x[0]) || !is(s.x[1], settings_class)) { s.x[0] = ret(param_err); return; }
        copy_settings({}, *state<SettingsState>(s.x[1]));
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMSessionDefaultPageFormat", [this, session_ok](CpuState& s, GuestMemory&) {
        if (!session_ok(s.x[0]) || !is(s.x[1], format_class)) { s.x[0] = ret(param_err); return; }
        const auto f = state<FormatState>(s.x[1]);
        release(std::exchange(f->paper, make_paper({})));
        f->orientation = 1;
        f->scale = 100;
        s.x[0] = ret(no_err);
    });
    // Validation: there is no printer whose limits could change anything.
    for (const auto& [symbol, cls] : std::initializer_list<std::pair<const char*, Address*>>{
             {"_PMSessionValidatePrintSettings", &settings_class}, {"_PMSessionValidatePageFormat", &format_class}})
        function(application_services, symbol, [this, session_ok, cls = cls](CpuState& s, GuestMemory& m) {
            if (!session_ok(s.x[0]) || !is(s.x[1], *cls)) { s.x[0] = ret(param_err); return; }
            if (s.x[2]) m.write8(s.x[2], 0);
            s.x[0] = ret(no_err);
        });
    function(application_services, "_PMSessionGetCurrentPrinter", [this](CpuState& s, GuestMemory&) {
        put64(s.x[1], 0);
        s.x[0] = ret(pm_no_default_printer);
    });
    function(application_services, "_PMSessionSetCurrentPMPrinter", [this](CpuState& s, GuestMemory&) { s.x[0] = ret(pm_invalid_printer); });
    // There are no printers to print a file with.
    function(application_services, "_PMPrinterPrintWithFile", [this](CpuState& s, GuestMemory&) { s.x[0] = ret(pm_invalid_printer); });
    // PMSessionSetDestination(session, settings, type, format, location)
    function(application_services, "_PMSessionSetDestination", [this, session_ok](CpuState& s, GuestMemory&) {
        const auto type = static_cast<std::uint32_t>(s.x[2]);
        if (!session_ok(s.x[0]) || !is(s.x[1], settings_class) || type < 1 || type > 5) { s.x[0] = ret(param_err); return; }
        const auto p = state<SettingsState>(s.x[1]);
        p->destination = type;
        p->format = s.x[3] ? text(s.x[3]) : std::string(type == destination_file ? pdf_format : default_format);
        cf_release(std::exchange(p->location, cf_retain(s.x[4])));
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMSessionGetDestinationType", [this, session_ok](CpuState& s, GuestMemory&) {
        if (!session_ok(s.x[0]) || !is(s.x[1], settings_class) || !s.x[2]) { s.x[0] = ret(param_err); return; }
        put32(s.x[2], state<SettingsState>(s.x[1])->destination);
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMSessionCopyDestinationFormat", [this, session_ok](CpuState& s, GuestMemory&) {
        if (!session_ok(s.x[0]) || !is(s.x[1], settings_class) || !s.x[2]) { put64(s.x[2], 0); s.x[0] = ret(param_err); return; }
        put64(s.x[2], cf.string(state<SettingsState>(s.x[1])->format));
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMSessionCopyDestinationLocation", [this, session_ok](CpuState& s, GuestMemory&) {
        if (!session_ok(s.x[0]) || !is(s.x[1], settings_class) || !s.x[2]) { put64(s.x[2], 0); s.x[0] = ret(param_err); return; }
        put64(s.x[2], cf_retain(state<SettingsState>(s.x[1])->location));
        s.x[0] = ret(no_err);
    });
    // Drawing a document: only into a file (a PDF at the destination URL).
    function(application_services, "_PMSessionBeginCGDocumentNoDialog", [this, session_ok](CpuState& s, GuestMemory&) {
        if (!session_ok(s.x[0]) || !is(s.x[1], settings_class) || !is(s.x[2], format_class)) { s.x[0] = ret(param_err); return; }
        const auto session = state<SessionState>(s.x[0]);
        const auto settings = state<SettingsState>(s.x[1]);
        const auto fail_with = [&](std::int32_t code, std::string_view why) {
            session->error = code;
            std::fprintf(stderr, "Alchemy: PMSessionBeginCGDocumentNoDialog: %.*s\n", static_cast<int>(why.size()), why.data());
            s.x[0] = ret(code);
        };
        if (session->context) { fail_with(pm_invalid_print_session, "a document is already open"); return; }
        if (settings->destination != destination_file || !settings->location)
            return fail_with(pm_no_default_printer, "no printer exists; only a file destination is printed");
        if (!can_draw()) return fail_with(pm_not_implemented, "drawing needs CoreGraphics (not in this build)");
        const auto r = rects_of(s.x[2]);
        const auto made = guest_call(core_graphics, "_CGPDFContextCreateWithURL", {settings->location, media_box(r.paper_w, r.paper_h), 0});
        if (!made) return fail_with(pm_not_implemented, "the destination file could not be created");
        session->context = made;
        session->error = no_err;
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMSessionBeginPageNoDialog", [this, session_ok](CpuState& s, GuestMemory&) {
        if (!session_ok(s.x[0])) { s.x[0] = ret(param_err); return; }
        const auto session = state<SessionState>(s.x[0]);
        if (!session->context || session->in_page) { s.x[0] = ret(pm_invalid_print_session); return; }
        double width = 612, height = 792;
        if (is(s.x[1], format_class)) { const auto r = rects_of(s.x[1]); width = r.paper_w; height = r.paper_h; }
        static_cast<void>(guest_call(core_graphics, "_CGContextBeginPage", {session->context, media_box(width, height)}));
        session->in_page = true;
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMSessionEndPageNoDialog", [this, session_ok](CpuState& s, GuestMemory&) {
        if (!session_ok(s.x[0])) { s.x[0] = ret(param_err); return; }
        const auto session = state<SessionState>(s.x[0]);
        if (!session->context || !session->in_page) { s.x[0] = ret(pm_invalid_print_session); return; }
        static_cast<void>(guest_call(core_graphics, "_CGContextEndPage", {session->context}));
        session->in_page = false;
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMSessionEndDocumentNoDialog", [this, session_ok](CpuState& s, GuestMemory&) {
        if (!session_ok(s.x[0])) { s.x[0] = ret(param_err); return; }
        const auto session = state<SessionState>(s.x[0]);
        if (!session->context) { s.x[0] = ret(pm_invalid_print_session); return; }
        if (session->in_page) static_cast<void>(guest_call(core_graphics, "_CGContextEndPage", {session->context}));
        static_cast<void>(guest_call(core_graphics, "_CGPDFContextClose", {session->context}));
        static_cast<void>(guest_call(core_graphics, "_CGContextRelease", {std::exchange(session->context, 0)}));
        session->in_page = false;
        s.x[0] = ret(no_err);
    });
    function(application_services, "_PMSessionGetCGGraphicsContext", [this, session_ok](CpuState& s, GuestMemory&) {
        if (!session_ok(s.x[0]) || !s.x[1]) { put64(s.x[1], 0); s.x[0] = ret(param_err); return; }
        const auto session = state<SessionState>(s.x[0]);
        put64(s.x[1], session->in_page ? session->context : 0);
        s.x[0] = ret(session->in_page ? no_err : pm_invalid_print_session);
    });

    // ---- printers: none exist ----
    function(application_services, "_PMServerCreatePrinterList", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = ret(param_err); return; }
        m.write64(s.x[1], cf.array({}));
        s.x[0] = ret(no_err);
    });
    for (const auto* name : {"_PMPrinterCreateFromPrinterID", "_PMPrinterGetName", "_PMPrinterGetID", "_PMPrinterGetLocation", "_PMPrinterIsDefault"})
        function(application_services, name, [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    // (printer, out...) and (printer, settings, out...): the outputs cleared.
    for (const auto& [name, out] : std::initializer_list<std::pair<const char*, unsigned>>{
             {"_PMPrinterGetMakeAndModelName", 1}, {"_PMPrinterGetState", 1}, {"_PMPrinterIsRemote", 1}, {"_PMPrinterGetPaperList", 1},
             {"_PMPrinterGetPrinterResolutionCount", 1}, {"_PMPrinterCopyDescriptionURL", 2}, {"_PMPrinterGetMimeTypes", 2},
             {"_PMPrinterGetIndexedPrinterResolution", 2}, {"_PMPrinterGetOutputResolution", 2}, {"_PMPrinterSetOutputResolution", 0}})
        function(application_services, name, [this, out = out](CpuState& s, GuestMemory& m) {
            if (out && s.x[out]) m.write64(s.x[out], 0);
            s.x[0] = ret(param_err);
        });
}

} // namespace

// (The entry point keeps the name headless_additions.hpp declares.)
void install_printing_no_printers(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<PrintCore>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
