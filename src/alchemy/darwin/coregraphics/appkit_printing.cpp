#include "alchemy/darwin/coregraphics/appkit_printing.hpp"
#include "alchemy/darwin/coregraphics/context.hpp"
#include "alchemy/darwin/coregraphics/pdf/pdf_parser.hpp"
#include "alchemy/darwin/coregraphics/pdf/pdf_render.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Printing (AppKit over the Quartz layer): NSPrintOperation, NSPrinter,
// NSPDFImageRep and NSView's printing methods.
// * Printed pages are images: each page is the view's rectangle drawn
//   (cacheDisplayInRect:toBitmapImageRep:) at 300 dpi (150 for pages that
//   would be too large) and placed on a CGPDFContext page; text in printed
//   output is not selectable. Vector printing needs AppKit's own drawing to
//   reach contexts other than bitmaps (canvas_over_cg), later work.
// * Pagination follows NSPrintInfo: paper size, margins, scaling factor,
//   horizontal / vertical pagination (automatic tiles, fit scales, clip
//   keeps one row / column), centring; a view that knows its page range
//   gives its pages through -rectForPage:. Pages go top to bottom, left to
//   right; NSPrintFirstPage / NSPrintLastPage limit them when
//   NSPrintAllPages is NO.
// * Destinations: PDF operations (to data or a file) and NSPrintSaveJob (to
//   NSPrintJobSavingURL) write the PDF. There are no printers: spool jobs,
//   previews and print panels cannot happen, so -runOperation answers NO
//   and says why; it never reports a job that did not happen. EPS
//   operations stop explicitly.
// * NSPrinter lists no printers and makes none.
// * NSPDFImageRep draws pages through CGContextDrawPDFPage's renderer; an
//   NSImage made from PDF data holds one.
namespace alchemy::gui::quartz2d {
namespace {

struct OperationState {
    Address view{}, print_info{};               // +1
    Address data{};                             // +1 NSMutableData (PDF to data)
    std::string path;                           // PDF to a file
    Rect inside{};
    bool copying{}, shows_print_panel{true}, shows_progress_panel{true}, can_spawn{};
    Address job_title{}, print_panel{}, pdf_panel{};   // +1
    std::int64_t current_page{}, first_page{1}, page_count{}, page_order{2};
    Address context{};                          // +1 NSGraphicsContext while running
};
struct PdfRep {
    std::shared_ptr<pdf::Document> document;
    Address data{};                             // +1 NSData
    std::int64_t page{};
};

std::mutex printing_mutex;
std::map<Address, OperationState> operations;
std::map<Address, PdfRep> pdf_reps;
Address current_operation{};

Address symbol(Quartz& q, std::string_view name) {
    const auto found = q.process.resolve(core_graphics_library, name);
    if (!found) fail("missing " + std::string(name));
    return found->address;
}

std::optional<Rect> page_box(pdf::Document& document, const pdf::ObjectPtr& page) {
    std::vector<double> values;
    const auto read = [&](std::string_view key) {
        values.clear();
        const auto array = document.inherited(page, key);
        if (!array || array->kind != pdf::Kind::array) return;
        for (const auto& item : array->array) {
            const auto value = document.resolve(item);
            values.push_back(value && value->is_number() ? value->number_value() : 0);
        }
    };
    read("MediaBox");
    if (values.size() != 4) return std::nullopt;
    const auto media = standardize({{values[0], values[1]}, {values[2] - values[0], values[3] - values[1]}});
    read("CropBox");
    if (values.size() != 4) return media;
    return rect_intersection(standardize({{values[0], values[1]}, {values[2] - values[0], values[3] - values[1]}}), media);
}

// The view's rectangle drawn into a bitmap `scale` pixels per point: a +1 CGImage.
Address render_view_rect(Quartz& q, Address view, Rect rect, double scale) {
    const auto width = static_cast<std::uint64_t>(std::max(1.0, std::ceil(rect.size.width * scale)));
    const auto height = static_cast<std::uint64_t>(std::max(1.0, std::ceil(rect.size.height * scale)));
    CallRegs registers;
    registers.x[2] = 0; registers.x[3] = width; registers.x[4] = height; registers.x[5] = 8; registers.x[6] = 4; registers.x[7] = 1;
    registers.push_stack(std::uint8_t{0});
    registers.push_stack(q.cf.constant_string("NSCalibratedRGBColorSpace"));
    registers.push_stack(std::int64_t{0});
    registers.push_stack(std::int64_t{0});
    const auto rep = q.send(q.objc.allocate(q.objc.find_class("NSBitmapImageRep")),
                            "initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bytesPerRow:bitsPerPixel:",
                            registers).x0;
    if (!rep) fail("printing: no bitmap for a page");
    Address image{};
    try {
        CallRegs cache;
        put_rect(cache, 0, rect);
        cache.x[2] = rep;
        static_cast<void>(q.send(view, "cacheDisplayInRect:toBitmapImageRep:", cache));
        image = q.retain(q.send(rep, "CGImage").x0);
    } catch (...) { q.release(rep); throw; }
    q.release(rep);
    return image;
}

struct Page {
    Rect rect;              // view coordinates
    double scale{1};        // view points -> paper points
};

double number_value(Quartz& q, Address object, double fallback) {
    if (!object) return fallback;
    return q.send(object, "doubleValue").d(0);
}

// Runs a print operation: true when its PDF was written.
bool run_operation(Quartz& q, Address self) {
    OperationState state;
    {
        std::lock_guard lock(printing_mutex);
        const auto found = operations.find(self);
        if (found == operations.end()) fail("not an NSPrintOperation");
        state = found->second;
    }
    if (!state.view) return false;
    const auto info = state.print_info;
    const auto dictionary = info ? q.send_id(info, "dictionary") : 0;
    const auto entry = [&](std::string_view key) { return dictionary ? q.send_id(dictionary, "objectForKey:", {q.autorelease(q.cf.string(key))}) : 0; };
    Address url{};
    if (!state.copying) {
        const auto disposition = q.string_text(entry("NSJobDisposition"));
        if (disposition == "NSPrintCancelJob") return false;
        if (disposition != "NSPrintSaveJob") {
            diagnostic(disposition == "NSPrintPreviewJob" ? "NSPrintOperation: print previews are not available (no print system); the job did not run"
                                                          : "NSPrintOperation: there are no printers; the job was not printed");
            return false;
        }
        url = entry("NSJobSavingURL");
        if (!url) { diagnostic("NSPrintOperation: a save job without NSPrintJobSavingURL; nothing was written"); return false; }
    }
    // Paper, margins and pages.
    const auto paper = info ? result_size(q.send(info, "paperSize")) : Size{612, 792};
    const auto margin = [&](const char* name, double fallback) { return info ? q.send(info, name).d(0) : fallback; };
    const double left = margin("leftMargin", 72), right = margin("rightMargin", 72), top = margin("topMargin", 90), bottom = margin("bottomMargin", 90);
    const double factor = std::max(0.01, info ? q.send(info, "scalingFactor").d(0) : 1.0);
    const bool centre_x = info && (q.send(info, "isHorizontallyCentered").x0 & 0xff) != 0;
    const bool centre_y = info && (q.send(info, "isVerticallyCentered").x0 & 0xff) != 0;
    const auto horizontal = info ? static_cast<std::int64_t>(q.send(info, "horizontalPagination").x0) : 2;
    const auto vertical = info ? static_cast<std::int64_t>(q.send(info, "verticalPagination").x0) : 0;
    const Size printable{std::max(1.0, paper.width - left - right), std::max(1.0, paper.height - top - bottom)};
    const auto bounds = result_rect(q.send(state.view, "bounds"));
    const bool flipped = (q.send(state.view, "isFlipped").x0 & 0xff) != 0;
    std::vector<Page> pages;
    Size media = paper;
    if (state.copying) {
        // One page the size of the rectangle.
        media = state.inside.size;
        pages.push_back({state.inside, 1});
    } else if (q.objc.responds(state.view, q.objc.selector("knowsPageRange:")) &&
               [&] {
                   Quartz::Scratch range(q, 16);
                   const bool knows = (q.send_id(state.view, "knowsPageRange:", {range.address()}) & 0xff) != 0;
                   if (knows) {
                       const auto first = static_cast<std::int64_t>(q.memory().read64(range.address()));
                       const auto count = static_cast<std::int64_t>(q.memory().read64(range.address() + 8));
                       for (std::int64_t n = first; n < first + std::min<std::int64_t>(count, 10000); ++n) {
                           CallRegs page;
                           page.x[2] = static_cast<Address>(n);
                           pages.push_back({result_rect(q.send(state.view, "rectForPage:", page)), factor});
                       }
                       state.first_page = first;
                   }
                   return knows;
               }()) {
    } else {
        // Automatic (0), fit (1) and clip (2) pagination.
        double scale = factor;
        if (horizontal == 1 && bounds.size.width > 0) scale = printable.width / bounds.size.width;
        if (vertical == 1 && bounds.size.height > 0) scale = std::min(scale, printable.height / bounds.size.height);
        const double page_width = printable.width / scale, page_height = printable.height / scale;
        const auto columns = horizontal == 0 ? std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(bounds.size.width / page_width - 1e-9))) : 1;
        const auto rows = vertical == 0 ? std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(bounds.size.height / page_height - 1e-9))) : 1;
        for (std::int64_t r = 0; r < rows && pages.size() < 10000; ++r)
            for (std::int64_t c = 0; c < columns; ++c) {
                const double x = bounds.origin.x + static_cast<double>(c) * page_width;
                const double w = std::min(page_width, bounds.origin.x + bounds.size.width - x);
                const double h = vertical == 0 ? std::min(page_height, bounds.size.height - static_cast<double>(r) * page_height)
                                               : std::min(page_height, bounds.size.height);
                const double y = flipped ? bounds.origin.y + static_cast<double>(r) * page_height
                                         : bounds.origin.y + bounds.size.height - static_cast<double>(r) * page_height - h;
                pages.push_back({{{x, y}, {std::max(0.0, w), std::max(0.0, h)}}, scale});
            }
    }
    // A page range from the dictionary (NSPrintAllPages NO).
    if (!state.copying) {
        const auto all = entry("NSPrintAllPages");
        if (all && number_value(q, all, 1) == 0) {
            const auto first = static_cast<std::int64_t>(number_value(q, entry("NSFirstPage"), 1));
            const auto last = static_cast<std::int64_t>(number_value(q, entry("NSLastPage"), static_cast<double>(pages.size())));
            std::vector<Page> kept;
            for (std::size_t i = 0; i < pages.size(); ++i) {
                const auto number = state.first_page + static_cast<std::int64_t>(i);
                if (number >= first && number <= last) kept.push_back(pages[i]);
            }
            pages = std::move(kept);
        }
    }
    {
        std::lock_guard lock(printing_mutex);
        operations[self].page_count = static_cast<std::int64_t>(pages.size());
        operations[self].first_page = state.first_page;
    }
    // The PDF context.
    Quartz::Scratch box(q, 32);
    q.write_double(box.address(), 0);
    q.write_double(box.address() + 8, 0);
    q.write_double(box.address() + 16, media.width);
    q.write_double(box.address() + 24, media.height);
    Address consumer{}, context{};
    if (state.copying && state.data) {
        consumer = q.call_guest(symbol(q, "_CGDataConsumerCreateWithCFData"), {state.data});
        context = q.call_guest(symbol(q, "_CGPDFContextCreate"), {consumer, box.address(), 0});
    } else if (state.copying) {
        const auto file = q.cf.file_url(state.path, false);
        context = q.call_guest(symbol(q, "_CGPDFContextCreateWithURL"), {file, box.address(), 0});
        q.release(file);
    } else {
        context = q.call_guest(symbol(q, "_CGPDFContextCreateWithURL"), {url, box.address(), 0});
    }
    if (!context) { q.release(consumer); return false; }
    const auto previous = std::exchange(current_operation, self);
    bool written = false;
    try {
        if (q.objc.responds(state.view, q.objc.selector("beginDocument"))) static_cast<void>(q.send(state.view, "beginDocument"));
        for (std::size_t i = 0; i < pages.size(); ++i) {
            const auto& page = pages[i];
            {
                std::lock_guard lock(printing_mutex);
                operations[self].current_page = state.first_page + static_cast<std::int64_t>(i);
            }
            const Size placed{page.rect.size.width * page.scale, page.rect.size.height * page.scale};
            Point origin{state.copying ? 0 : left, state.copying ? 0 : paper.height - top - placed.height};
            if (!state.copying && centre_x) origin.x = left + (printable.width - placed.width) / 2;
            if (!state.copying && centre_y) origin.y = bottom + (printable.height - placed.height) / 2;
            // 300 dpi, or 150 for pages over about 36 million pixels.
            double dpi = 300;
            if (placed.width * placed.height * (dpi / 72) * (dpi / 72) > 36e6) dpi = 150;
            Address image{};
            if (page.rect.size.width > 0 && page.rect.size.height > 0) image = render_view_rect(q, state.view, page.rect, page.scale * dpi / 72);
            const auto c = context_state(q, context);
            std::lock_guard lock(c->mutex);
            try {
                pdf_begin_page(q, *c, Rect{{0, 0}, media});
                if (image) draw_image(q, *c, image, {origin, placed});
                pdf_end_page(*c);
            } catch (...) { q.release(image); throw; }
            q.release(image);
        }
        if (q.objc.responds(state.view, q.objc.selector("endDocument"))) static_cast<void>(q.send(state.view, "endDocument"));
        static_cast<void>(q.call_guest(symbol(q, "_CGPDFContextClose"), {context}));
        written = true;
    } catch (...) {
        current_operation = previous;
        q.release(context);
        q.release(consumer);
        throw;
    }
    current_operation = previous;
    q.release(context);
    q.release(consumer);
    return written;
}

} // namespace

// A +1 CGImage of an NSImage's NSPDFImageRep (its current page at the image's size, `scale` pixels per point), or 0.
Address render_pdf_rep_image(Quartz& q, Address image, double scale) {
    const auto pdf_class = q.objc.find_class("NSPDFImageRep");
    if (!pdf_class) return 0;
    Address rep{};
    const auto reps = q.send_id(image, "representations");
    const auto count = reps ? q.send(reps, "count").x0 : 0;
    for (Address i = 0; i < count && !rep; ++i) {
        const auto candidate = q.send_id(reps, "objectAtIndex:", {i});
        if (q.is_kind(candidate, pdf_class)) rep = candidate;
    }
    if (!rep) return 0;
    PdfRep info;
    {
        std::lock_guard lock(printing_mutex);
        const auto found = pdf_reps.find(rep);
        if (found == pdf_reps.end()) return 0;
        info = found->second;
    }
    const auto& pages = info.document->pages();
    if (pages.empty()) return 0;
    const auto page = pages[static_cast<std::size_t>(std::clamp<std::int64_t>(info.page, 0, static_cast<std::int64_t>(pages.size()) - 1))];
    const auto box = page_box(*info.document, page).value_or(Rect{{0, 0}, {612, 792}});
    const auto size = result_size(q.send(image, "size"));
    const Size drawn = size.width > 0 && size.height > 0 ? size : box.size;
    scale = std::clamp(std::isfinite(scale) ? scale : 1.0, 1.0, 4.0);
    PixelLayout layout;
    layout.width = static_cast<std::uint32_t>(std::max(1.0, std::ceil(drawn.width * scale)));
    layout.height = static_cast<std::uint32_t>(std::max(1.0, std::ceil(drawn.height * scale)));
    layout.bytes_per_row = static_cast<std::uint64_t>(layout.width) * 4;
    layout.bitmap_info = alpha_info::premultiplied_last;
    const auto context = create_context(q, layout, 0, q.device_rgb());
    if (!context) return 0;
    Image pixels;
    try {
        const auto c = context_state(q, context);
        std::lock_guard lock(c->mutex);
        c->g.ctm = Affine::translation(-box.origin.x, -box.origin.y)
                       .then(Affine::scaling(layout.width / std::max(1.0, box.size.width), layout.height / std::max(1.0, box.size.height)))
                       .then(c->g.ctm);
        render_pdf_page(q, *c, *info.document, page);
        pixels = snapshot(q, *c);
    } catch (...) { q.release(context); throw; }
    q.release(context);
    PixelLayout straight;
    straight.width = pixels.width;
    straight.height = pixels.height;
    straight.bytes_per_row = static_cast<std::uint64_t>(pixels.width) * 4;
    straight.bitmap_info = alpha_info::last;
    straight.straight_alpha = true;
    return q.make_image(straight, q.srgb(), std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels.rgba)));
}

void Quartz::install_appkit_printing() {
    const auto view_class = objc.find_class("NSView");
    const auto image_rep_class = objc.find_class("NSImageRep");
    if (!view_class || !image_rep_class) return;   // no AppKit (headless)
    Quartz* const q = this;
    const auto method = [this](Address cls, const char* name, Handler handler, const char* types = "@@:", bool class_side = false,
                               const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        static_cast<void>(objc.add_native_method(cls, name, scoped(std::move(handler)), class_side, types));
    };
    const auto op_state = [](Address self) -> OperationState& {
        const auto found = operations.find(self);
        if (found == operations.end()) fail("not an NSPrintOperation");
        return found->second;
    };

    // ---- NSPrintOperation ----
    const auto op = declare_class("NSPrintOperation", "NSObject", appkit_library);
    const auto make = [q, op](Address view, Address info, bool copying, Rect inside, Address data, std::string path) {
        const auto self = q->send(q->objc.allocate(op), "init").x0;
        OperationState state;
        state.view = q->retain(view);
        state.print_info = q->retain(info ? info : q->send(q->objc.find_class("NSPrintInfo"), "sharedPrintInfo").x0);
        state.copying = copying;
        state.inside = standardize(inside);
        state.data = q->retain(data);
        state.path = std::move(path);
        state.shows_print_panel = !copying;
        state.shows_progress_panel = !copying;
        std::lock_guard lock(printing_mutex);
        operations[self] = state;
        return q->autorelease(self);
    };
    method(op, "printOperationWithView:", [make](CpuState& s, GuestMemory&) { s.x[0] = make(s.x[2], 0, false, {}, 0, {}); }, "@@:@", true);
    method(op, "printOperationWithView:printInfo:", [make](CpuState& s, GuestMemory&) { s.x[0] = make(s.x[2], s.x[3], false, {}, 0, {}); }, "@@:@@", true);
    method(op, "PDFOperationWithView:insideRect:toData:", [make](CpuState& s, GuestMemory&) {
        s.x[0] = make(s.x[2], 0, true, rect_arg(s), s.x[3], {});
    }, "@@:@{CGRect={CGPoint=dd}{CGSize=dd}}@", true);
    method(op, "PDFOperationWithView:insideRect:toData:printInfo:", [make](CpuState& s, GuestMemory&) {
        s.x[0] = make(s.x[2], s.x[4], true, rect_arg(s), s.x[3], {});
    }, "@@:@{CGRect={CGPoint=dd}{CGSize=dd}}@@", true);
    method(op, "PDFOperationWithView:insideRect:toPath:printInfo:", [q, make](CpuState& s, GuestMemory&) {
        s.x[0] = make(s.x[2], s.x[4], true, rect_arg(s), 0, q->string_text(s.x[3]));
    }, "@@:@{CGRect={CGPoint=dd}{CGSize=dd}}@@", true);
    for (const auto* name : {"EPSOperationWithView:insideRect:toData:", "EPSOperationWithView:insideRect:toData:printInfo:",
                             "EPSOperationWithView:insideRect:toPath:printInfo:"})
        method(op, name, [selector = std::string(name)](CpuState&, GuestMemory&) { unsupported("+[NSPrintOperation " + selector + "] (EPS output)"); }, "@@:@", true);
    method(op, "currentOperation", [](CpuState& s, GuestMemory&) { s.x[0] = current_operation; }, "@@:", true);
    method(op, "setCurrentOperation:", [](CpuState& s, GuestMemory&) { current_operation = s.x[2]; }, "v@:@", true);
    method(op, "dealloc", [q, op](CpuState& s, GuestMemory&) {
        std::optional<OperationState> state;
        {
            std::lock_guard lock(printing_mutex);
            if (const auto found = operations.find(s.x[0]); found != operations.end()) { state = found->second; operations.erase(found); }
        }
        if (state) for (const auto object : {state->view, state->print_info, state->data, state->job_title, state->print_panel, state->pdf_panel, state->context}) q->release(object);
        if (current_operation == s.x[0]) current_operation = 0;
        static_cast<void>(q->objc.send_super(s.x[0], op, q->objc.selector("dealloc")));
    }, "v@:");
    method(op, "runOperation", [q](CpuState& s, GuestMemory&) { return_bool(s, run_operation(*q, s.x[0])); }, "B@:");
    method(op, "runOperationModalForWindow:delegate:didRunSelector:contextInfo:", [q](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], delegate = s.x[3], selector = s.x[4], context = s.x[5];
        const bool success = run_operation(*q, self);
        if (delegate && selector) {
            CallRegs call;
            call.x[2] = self;
            call.x[3] = success ? 1 : 0;
            call.x[4] = context;
            static_cast<void>(q->objc.send(delegate, selector, call));
        }
    }, "v@:@@:^v");
    method(op, "cleanUpOperation", [](CpuState& s, GuestMemory&) { if (current_operation == s.x[0]) current_operation = 0; }, "v@:");
    method(op, "deliverResult", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(op, "isCopyingOperation", [op_state](CpuState& s, GuestMemory&) { std::lock_guard lock(printing_mutex); return_bool(s, op_state(s.x[0]).copying); }, "B@:");
    method(op, "view", [op_state](CpuState& s, GuestMemory&) { std::lock_guard lock(printing_mutex); s.x[0] = op_state(s.x[0]).view; });
    const auto object_property = [method, op, op_state, q](const char* getter, const char* setter, Address OperationState::*member) {
        method(op, getter, [op_state, member](CpuState& s, GuestMemory&) { std::lock_guard lock(printing_mutex); s.x[0] = op_state(s.x[0]).*member; }, "@@:", false);
        method(op, setter, [op_state, member, q](CpuState& s, GuestMemory&) {
            Address old{};
            {
                std::lock_guard lock(printing_mutex);
                auto& slot = op_state(s.x[0]).*member;
                old = std::exchange(slot, q->retain(s.x[2]));
            }
            q->release(old);
        }, "v@:@", false);
    };
    object_property("printInfo", "setPrintInfo:", &OperationState::print_info);
    object_property("jobTitle", "setJobTitle:", &OperationState::job_title);
    object_property("printPanel", "setPrintPanel:", &OperationState::print_panel);
    object_property("PDFPanel", "setPDFPanel:", &OperationState::pdf_panel);
    const auto flag_property = [method, op, op_state](const char* getter, const char* setter, bool OperationState::*member) {
        method(op, getter, [op_state, member](CpuState& s, GuestMemory&) { std::lock_guard lock(printing_mutex); return_bool(s, op_state(s.x[0]).*member); }, "B@:", false);
        method(op, setter, [op_state, member](CpuState& s, GuestMemory&) { std::lock_guard lock(printing_mutex); op_state(s.x[0]).*member = bool_arg(s, 2); }, "v@:B", false);
    };
    flag_property("showsPrintPanel", "setShowsPrintPanel:", &OperationState::shows_print_panel);
    flag_property("showsProgressPanel", "setShowsProgressPanel:", &OperationState::shows_progress_panel);
    flag_property("canSpawnSeparateThread", "setCanSpawnSeparateThread:", &OperationState::can_spawn);
    method(op, "pageOrder", [op_state](CpuState& s, GuestMemory&) { std::lock_guard lock(printing_mutex); s.x[0] = static_cast<Address>(op_state(s.x[0]).page_order); }, "q@:");
    method(op, "setPageOrder:", [op_state](CpuState& s, GuestMemory&) { std::lock_guard lock(printing_mutex); op_state(s.x[0]).page_order = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    method(op, "currentPage", [op_state](CpuState& s, GuestMemory&) { std::lock_guard lock(printing_mutex); s.x[0] = static_cast<Address>(op_state(s.x[0]).current_page); }, "q@:");
    method(op, "pageRange", [op_state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(printing_mutex);
        const auto& state = op_state(s.x[0]);
        s.x[0] = static_cast<Address>(state.first_page);
        s.x[1] = static_cast<Address>(state.page_count);
    }, "{_NSRange=QQ}@:");
    method(op, "preferredRenderingQuality", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");   // NSPrintRenderingQualityBest
    method(op, "context", [op_state](CpuState& s, GuestMemory&) { std::lock_guard lock(printing_mutex); s.x[0] = op_state(s.x[0]).context; });
    method(op, "createContext", [op_state](CpuState& s, GuestMemory&) { std::lock_guard lock(printing_mutex); s.x[0] = op_state(s.x[0]).context; });
    method(op, "destroyContext", [](CpuState&, GuestMemory&) {}, "v@:");

    // ---- NSPrinter: there are no printers ----
    const auto printer = declare_class("NSPrinter", "NSObject", appkit_library);
    method(printer, "printerNames", [q](CpuState& s, GuestMemory&) { s.x[0] = q->autorelease(q->cf.array({})); }, "@@:", true);
    method(printer, "printerTypes", [q](CpuState& s, GuestMemory&) { s.x[0] = q->autorelease(q->cf.array({})); }, "@@:", true);
    method(printer, "printerWithName:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:@", true);
    method(printer, "printerWithType:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:@", true);

    // ---- NSEPSImageRep ----
    // EPS (PostScript) is not interpreted: no data initializes one (nil, as
    // for data a rep cannot use), +canInitWithData: is NO and no types are
    // offered, so NSImage never picks it.
    const auto eps_rep = declare_class("NSEPSImageRep", "NSImageRep", appkit_library);
    method(eps_rep, "initWithData:", [q](CpuState& s, GuestMemory&) { q->release(s.x[0]); s.x[0] = 0; }, "@@:@");
    method(eps_rep, "imageRepWithData:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:@", true);
    method(eps_rep, "canInitWithData:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:@", true);
    for (const auto* types : {"imageUnfilteredTypes", "imageTypes"})
        method(eps_rep, types, [q](CpuState& s, GuestMemory&) { s.x[0] = q->autorelease(q->cf.array(std::vector<Address>{})); }, "@@:", true);

    // ---- NSPDFImageRep ----
    const auto pdf_rep = declare_class("NSPDFImageRep", "NSImageRep", appkit_library);
    const auto rep_info = [](Address self) -> PdfRep& {
        const auto found = pdf_reps.find(self);
        if (found == pdf_reps.end()) fail("not an NSPDFImageRep");
        return found->second;
    };
    const auto current_box = [rep_info](Address self) {
        std::lock_guard lock(printing_mutex);
        const auto& info = rep_info(self);
        const auto& pages = info.document->pages();
        if (pages.empty()) return Rect{};
        return page_box(*info.document, pages[static_cast<std::size_t>(std::clamp<std::int64_t>(info.page, 0, static_cast<std::int64_t>(pages.size()) - 1))])
            .value_or(Rect{{0, 0}, {612, 792}});
    };
    method(pdf_rep, "initWithData:", [q, pdf_rep, current_box](CpuState& s, GuestMemory&) {
        const auto self = q->objc.send_super(s.x[0], pdf_rep, q->objc.selector("init")).x0;
        s.x[0] = self;
        if (!self) return;
        auto bytes = s.x[2] ? std::make_shared<const std::vector<std::uint8_t>>(q->data_bytes(s.x[2])) : nullptr;
        auto document = bytes ? pdf::Document::open(bytes) : nullptr;
        if (!document || document->pages().empty()) { q->release(self); s.x[0] = 0; return; }
        {
            std::lock_guard lock(printing_mutex);
            pdf_reps[self] = {std::move(document), q->send_id(s.x[2], "copy"), 0};
        }
        const auto box = current_box(self);
        CallRegs size;
        size.set_d(0, box.size.width);
        size.set_d(1, box.size.height);
        static_cast<void>(q->send(self, "setSize:", size));
    }, "@@:@");
    method(pdf_rep, "imageRepWithData:", [q](CpuState& s, GuestMemory&) {
        s.x[0] = q->autorelease(q->send_id(q->objc.allocate(s.x[0]), "initWithData:", {s.x[2]}));
    }, "@@:@", true);
    method(pdf_rep, "canInitWithData:", [q](CpuState& s, GuestMemory&) {
        const auto bytes = s.x[2] ? q->data_bytes(s.x[2]) : std::vector<std::uint8_t>{};
        const std::string head(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(bytes.size(), 1024)));
        return_bool(s, head.find("%PDF-") != std::string::npos);
    }, "B@:@", true);
    method(pdf_rep, "imageUnfilteredTypes", [q](CpuState& s, GuestMemory&) {
        const std::array<Address, 1> types{q->cf.string("com.adobe.pdf")};
        s.x[0] = q->autorelease(q->cf.array(types));
        q->release(types[0]);
    }, "@@:", true);
    method(pdf_rep, "dealloc", [q, pdf_rep](CpuState& s, GuestMemory&) {
        Address data{};
        {
            std::lock_guard lock(printing_mutex);
            if (const auto found = pdf_reps.find(s.x[0]); found != pdf_reps.end()) { data = found->second.data; pdf_reps.erase(found); }
        }
        q->release(data);
        static_cast<void>(q->objc.send_super(s.x[0], pdf_rep, q->objc.selector("dealloc")));
    }, "v@:");
    method(pdf_rep, "PDFRepresentation", [rep_info](CpuState& s, GuestMemory&) { std::lock_guard lock(printing_mutex); s.x[0] = rep_info(s.x[0]).data; });
    method(pdf_rep, "pageCount", [rep_info](CpuState& s, GuestMemory&) {
        std::lock_guard lock(printing_mutex);
        s.x[0] = rep_info(s.x[0]).document->pages().size();
    }, "q@:");
    method(pdf_rep, "currentPage", [rep_info](CpuState& s, GuestMemory&) { std::lock_guard lock(printing_mutex); s.x[0] = static_cast<Address>(rep_info(s.x[0]).page); }, "q@:");
    method(pdf_rep, "setCurrentPage:", [q, rep_info, current_box](CpuState& s, GuestMemory&) {
        {
            std::lock_guard lock(printing_mutex);
            auto& info = rep_info(s.x[0]);
            info.page = std::clamp<std::int64_t>(static_cast<std::int64_t>(s.x[2]), 0, static_cast<std::int64_t>(info.document->pages().size()) - 1);
        }
        const auto box = current_box(s.x[0]);
        CallRegs size;
        size.set_d(0, box.size.width);
        size.set_d(1, box.size.height);
        static_cast<void>(q->send(s.x[0], "setSize:", size));
    }, "v@:q");
    method(pdf_rep, "bounds", [current_box](CpuState& s, GuestMemory&) { return_rect(s, current_box(s.x[0])); }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    // Drawing the current page into the current graphics context (vector: through the page renderer).
    const auto draw_page = [q, rep_info, current_box](Address self, std::optional<Rect> target) {
        const auto graphics = q->send(q->objc.find_class("NSGraphicsContext"), "currentContext").x0;
        const auto cg = graphics ? q->send(graphics, "CGContext").x0 : 0;
        if (!cg) return false;
        PdfRep info;
        {
            std::lock_guard lock(printing_mutex);
            info = rep_info(self);
        }
        const auto& pages = info.document->pages();
        const auto page = pages[static_cast<std::size_t>(std::clamp<std::int64_t>(info.page, 0, static_cast<std::int64_t>(pages.size()) - 1))];
        const auto box = current_box(self);
        const auto drawn = target.value_or(Rect{{0, 0}, box.size});
        const bool flipped = (q->send(graphics, "isFlipped").x0 & 0xff) != 0;
        const auto c = context_state(*q, cg);
        std::lock_guard lock(c->mutex);
        save_state(*c);
        try {
            auto m = Affine::translation(-box.origin.x, -box.origin.y)
                         .then(Affine::scaling(drawn.size.width / std::max(1e-9, box.size.width), drawn.size.height / std::max(1e-9, box.size.height)));
            if (flipped) m = m.then(Affine::translation(0, -drawn.size.height)).then(Affine::scaling(1, -1));
            c->g.ctm = m.then(Affine::translation(drawn.origin.x, drawn.origin.y)).then(c->g.ctm);
            render_pdf_page(*q, *c, *info.document, page);
        } catch (...) { restore_state(*q, *c); throw; }
        restore_state(*q, *c);
        return true;
    };
    method(pdf_rep, "draw", [draw_page](CpuState& s, GuestMemory&) { return_bool(s, draw_page(s.x[0], std::nullopt)); }, "B@:");
    method(pdf_rep, "drawInRect:", [draw_page](CpuState& s, GuestMemory&) { return_bool(s, draw_page(s.x[0], rect_arg(s))); }, "B@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(pdf_rep, "drawAtPoint:", [q, draw_page](CpuState& s, GuestMemory&) {
        return_bool(s, draw_page(s.x[0], Rect{point_arg(s), result_size(q->send(s.x[0], "size"))}));
    }, "B@:{CGPoint=dd}");

    // ---- NSView printing ----
    method(view_class, "print:", [q](CpuState& s, GuestMemory&) {
        const auto operation = q->send_id(q->objc.find_class("NSPrintOperation"), "printOperationWithView:", {s.x[0]});
        static_cast<void>(q->send(operation, "runOperation"));
    }, "v@:@");
    method(view_class, "dataWithPDFInsideRect:", [q](CpuState& s, GuestMemory&) {
        const auto data = q->send(q->send(q->objc.find_class("NSMutableData"), "alloc").x0, "init").x0;
        CallRegs call;
        call.x[2] = s.x[0];
        put_rect(call, 0, rect_arg(s));
        call.x[3] = data;
        const auto operation = q->send(q->objc.find_class("NSPrintOperation"), "PDFOperationWithView:insideRect:toData:", call).x0;
        const bool ok = (q->send(operation, "runOperation").x0 & 0xff) != 0;
        if (!ok) { q->release(data); s.x[0] = 0; return; }
        s.x[0] = q->autorelease(data);
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(view_class, "writePDFInsideRect:toPasteboard:", [q](CpuState& s, GuestMemory&) {
        CallRegs call;
        put_rect(call, 0, rect_arg(s));
        const auto data = q->send(s.x[0], "dataWithPDFInsideRect:", call).x0;
        if (data && s.x[2]) {
            const auto type = q->autorelease(q->cf.string("com.adobe.pdf"));
            static_cast<void>(q->send_id(s.x[2], "declareTypes:owner:", {q->autorelease(q->cf.array(std::vector<Address>{type})), 0}));
            static_cast<void>(q->send_id(s.x[2], "setData:forType:", {data, type}));
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    method(view_class, "dataWithEPSInsideRect:", [](CpuState&, GuestMemory&) { unsupported("-[NSView dataWithEPSInsideRect:] (EPS output)"); },
           "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    // The pagination hooks views override: AppKit's defaults.
    method(view_class, "knowsPageRange:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:^{_NSRange=QQ}");
    method(view_class, "rectForPage:", [](CpuState& s, GuestMemory&) { return_rect(s, {}); }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:q");
    method(view_class, "locationOfPrintRect:", [](CpuState& s, GuestMemory&) { return_point(s, {}); }, "{CGPoint=dd}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    for (const auto* name : {"beginDocument", "endDocument", "endPage"}) method(view_class, name, [](CpuState&, GuestMemory&) {}, "v@:");
    method(view_class, "beginPageInRect:atPlacement:", [](CpuState&, GuestMemory&) {}, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}{CGPoint=dd}");
    method(view_class, "drawPageBorderWithSize:", [](CpuState&, GuestMemory&) {}, "v@:{CGSize=dd}");
    method(view_class, "printJobTitle", [q](CpuState& s, GuestMemory&) {
        const auto window = q->send(s.x[0], "window").x0;
        s.x[0] = window ? q->send(window, "title").x0 : q->autorelease(q->cf.string(""));
    });
    for (const auto* name : {"heightAdjustLimit", "widthAdjustLimit"}) method(view_class, name, [](CpuState& s, GuestMemory&) { set_d(s, 0, 0.2); }, "d@:");
    method(view_class, "adjustPageHeightNew:top:bottom:limit:", [q](CpuState& s, GuestMemory&) {
        if (s.x[2]) q->write_double(s.x[2], d(s, 1));   // the proposed bottom
    }, "v@:^dddd");
    method(view_class, "adjustPageWidthNew:left:right:limit:", [q](CpuState& s, GuestMemory&) {
        if (s.x[2]) q->write_double(s.x[2], d(s, 1));   // the proposed right edge
    }, "v@:^dddd");
}

} // namespace alchemy::gui::quartz2d
