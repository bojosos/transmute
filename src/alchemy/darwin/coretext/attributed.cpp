#include "alchemy/darwin/coretext/ct_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

// Attributed strings with real attribute runs (NSAttributedString,
// NSMutableAttributedString and the CFAttributedString functions, which are
// the same objects), NSParagraphStyle / NSMutableParagraphStyle and
// CTParagraphStyle. They replace the attribute-less NSAttributedString of
// foundation_extra.cpp by adding methods to the same class.
namespace alchemy::gui::quartz2d {
namespace {

Coretext coretext_state;

[[noreturn]] void range_error(std::string_view what) { fail("NSRangeException: " + std::string(what) + " out of bounds"); }

std::shared_ptr<AttributedState> state_of(Quartz& q, Address object) { return q.objc.host_state<AttributedState>(object); }

std::vector<std::uint16_t> units_of(Quartz& q, Address string) {
    if (!string) return {};
    if (q.cf.kind(string) != ObjectKind::string) fail("attributed string text is not an NSString");
    return q.cf.units(string);
}

// Run boundary at `index`; returns the index of the run starting there.
std::size_t split(Quartz& q, AttributedState& s, std::size_t index) {
    std::size_t at = 0;
    for (std::size_t run = 0; run < s.runs.size(); ++run) {
        if (at == index) return run;
        if (index < at + s.runs[run].length) {
            const auto first = index - at;
            const AttributeRun second{s.runs[run].length - first, q.retain(s.runs[run].attributes)};
            s.runs[run].length = first;
            s.runs.insert(s.runs.begin() + static_cast<std::ptrdiff_t>(run + 1), second);
            return run + 1;
        }
        at += s.runs[run].length;
    }
    return s.runs.size();
}
void coalesce(Quartz& q, AttributedState& s) {
    std::vector<AttributeRun> runs;
    for (auto& run : s.runs) {
        if (run.length == 0) { q.release(run.attributes); continue; }
        if (!runs.empty() && (runs.back().attributes == run.attributes || q.cf.equal(runs.back().attributes, run.attributes))) {
            runs.back().length += run.length;
            q.release(run.attributes);
            continue;
        }
        runs.push_back(run);
    }
    s.runs = std::move(runs);
}
void invalidate(Quartz& q, AttributedState& s) {
    // -string results are not retained by callers; keep them alive until the pool drains.
    if (const auto old = std::exchange(s.string_cache, 0)) static_cast<void>(q.autorelease(old));
}
void check_range(const AttributedState& s, std::uint64_t location, std::uint64_t length, std::string_view what) {
    if (location > s.text.size() || length > s.text.size() - location) range_error(what);
}
// The dictionary at `index` (index < size), or the run before an insertion point.
Address attributes_at(const AttributedState& s, std::size_t index, std::size_t* start = nullptr, std::size_t* length = nullptr) {
    std::size_t at = 0;
    for (const auto& run : s.runs) {
        if (index < at + run.length) {
            if (start) *start = at;
            if (length) *length = run.length;
            return run.attributes;
        }
        at += run.length;
    }
    return 0;
}
void set_runs(Quartz& q, AttributedState& s, std::size_t location, std::size_t length, Address attributes) {
    const auto first = split(q, s, location);
    const auto last = split(q, s, location + length);
    for (auto run = first; run < last; ++run) {
        q.release(s.runs[run].attributes);
        s.runs[run].attributes = q.retain(attributes);
    }
    coalesce(q, s);
}
// Rewrites the dictionaries of [location, location + length).
void modify_runs(Quartz& q, AttributedState& s, std::size_t location, std::size_t length,
                 const std::function<void(std::vector<std::pair<Address, Address>>&)>& change) {
    const auto first = split(q, s, location);
    const auto last = split(q, s, location + length);
    for (auto run = first; run < last; ++run) {
        auto entries = q.cf.dictionary_entries(s.runs[run].attributes);
        change(entries);
        const auto replacement = q.cf.dictionary(entries);
        q.release(s.runs[run].attributes);
        s.runs[run].attributes = replacement;
    }
    coalesce(q, s);
}
void erase_key(Quartz& q, std::vector<std::pair<Address, Address>>& entries, Address key) {
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) { return q.cf.equal(entry.first, key); }), entries.end());
}
// Replaces [location, location + length) with `units`; `runs` (adopted)
// gives their attributes, otherwise they take the attributes at `location`.
void replace(Quartz& q, AttributedState& s, std::size_t location, std::size_t length, std::span<const std::uint16_t> units,
             std::optional<std::vector<AttributeRun>> runs) {
    Address inherited{};
    if (!runs && !units.empty()) {
        if (location < s.text.size()) inherited = attributes_at(s, location);
        else if (location > 0) inherited = attributes_at(s, location - 1);
        if (!inherited) inherited = coretext_state.empty_attributes;
        inherited = q.retain(inherited);
    }
    const auto first = split(q, s, location);
    const auto last = split(q, s, location + length);
    for (auto run = first; run < last; ++run) q.release(s.runs[run].attributes);
    s.runs.erase(s.runs.begin() + static_cast<std::ptrdiff_t>(first), s.runs.begin() + static_cast<std::ptrdiff_t>(last));
    std::vector<AttributeRun> inserted;
    if (runs) inserted = std::move(*runs);
    else if (!units.empty()) inserted.push_back({units.size(), inherited});
    s.runs.insert(s.runs.begin() + static_cast<std::ptrdiff_t>(first), inserted.begin(), inserted.end());
    s.text.erase(s.text.begin() + static_cast<std::ptrdiff_t>(location), s.text.begin() + static_cast<std::ptrdiff_t>(location + length));
    s.text.insert(s.text.begin() + static_cast<std::ptrdiff_t>(location), units.begin(), units.end());
    coalesce(q, s);
    invalidate(q, s);
}
void initialize(Quartz& q, Address object, std::vector<std::uint16_t> units, Address attributes) {
    const auto s = state_of(q, object);
    std::lock_guard lock(s->mutex);
    if (!s->text.empty() || !s->runs.empty()) fail("NSAttributedString initialized twice");
    s->text = std::move(units);
    if (!s->text.empty()) {
        const auto copy = attributes ? q.cf.copy(attributes) : q.retain(coretext_state.empty_attributes);
        s->runs.push_back({s->text.size(), copy});
    }
}
void release_runs(Quartz& q, std::vector<AttributeRun>& runs) {
    for (auto& run : runs) q.release(std::exchange(run.attributes, 0));
    runs.clear();
}
std::vector<AttributeRun> sub_runs(Quartz& q, const AttributedState& s, std::size_t location, std::size_t length) {
    std::vector<AttributeRun> result;
    std::size_t at = 0;
    for (const auto& run : s.runs) {
        const auto begin = std::max(at, location), end = std::min(at + run.length, location + length);
        if (begin < end) result.push_back({end - begin, q.retain(run.attributes)});
        at += run.length;
    }
    return result;
}

// NSTextAlignment (arm64 macOS) <-> CTTextAlignment: center and right swap.
std::int32_t ns_to_ct(std::int64_t value) { return value == 1 ? alignment::center : value == 2 ? alignment::right : static_cast<std::int32_t>(value); }
std::int64_t ct_to_ns(std::int32_t value) { return value == alignment::center ? 1 : value == alignment::right ? 2 : value; }

[[nodiscard]] float float_arg(const CpuState& s, unsigned index) { return std::bit_cast<float>(static_cast<std::uint32_t>(s.v[index][0])); }
void set_float(CpuState& s, unsigned index, float value) { s.v[index] = {std::bit_cast<std::uint32_t>(value), 0}; }

// ---- NSTextTab / CTTextTab (toll-free bridged) ------------------------------------------------
// One tab stop: its location, its alignment (CTTextAlignment) and options
// (+1 dictionary; NSTabColumnTerminatorsAttributeName for decimal tabs).
struct TextTabState final : ObjectPayload {
    double location{};
    std::int32_t align{alignment::left};
    Address options{};
};
Address text_tab_class{};
constexpr std::size_t default_tab_count = 12;
constexpr double default_tab_spacing = 28;

std::shared_ptr<TextTabState> tab_state(Quartz& q, Address tab) {
    const auto state = q.find<TextTabState>(tab);
    if (!state) fail("tab stop is not an NSTextTab");
    return state;
}
Address make_tab(Quartz& q, double location, std::int32_t align, Address options) {
    const auto tab = q.objc.allocate(text_tab_class);
    const auto state = q.objc.host_state<TextTabState>(tab);
    state->location = location;
    state->align = align;
    state->options = options ? q.cf.copy(options) : q.cf.dictionary({});
    return tab;
}
// The paragraph's tab stops: its own, or AppKit's default ones (autoreleased objects).
std::vector<Address> tabs_of(Quartz& q, Address paragraph) {
    const auto state = q.find<ParagraphState>(paragraph);
    if (state && state->tabs_set) return state->tabs;
    std::vector<Address> defaults;
    for (std::size_t i = 1; i <= default_tab_count; ++i)
        defaults.push_back(q.autorelease(make_tab(q, default_tab_spacing * static_cast<double>(i), alignment::left, 0)));
    return defaults;
}
// Sets the stops (retained, sorted by location) and the locations the style reports.
void set_tabs(Quartz& q, Address paragraph, std::vector<Address> tabs) {
    const auto state = q.objc.host_state<ParagraphState>(paragraph);
    tabs.erase(std::remove(tabs.begin(), tabs.end(), Address{}), tabs.end());
    for (const auto tab : tabs) static_cast<void>(tab_state(q, tab));
    std::stable_sort(tabs.begin(), tabs.end(), [&](Address a, Address b) { return tab_state(q, a)->location < tab_state(q, b)->location; });
    for (const auto tab : tabs) static_cast<void>(q.retain(tab));
    for (const auto old : std::exchange(state->tabs, tabs)) q.release(old);
    state->tabs_set = true;
    state->style.tab_stops.clear();
    for (const auto tab : state->tabs) state->style.tab_stops.push_back(tab_state(q, tab)->location);
}
void copy_tabs(Quartz& q, Address from, Address to) {
    const auto source = q.find<ParagraphState>(from);
    if (!source) return;
    // The text lists travel with the tabs.
    if (const auto target = q.find<ParagraphState>(to); target && target != source) {
        for (const auto list : source->lists) q.retain(list);
        for (const auto list : std::exchange(target->lists, source->lists)) q.release(list);
    }
    if (!source->tabs_set) return;
    set_tabs(q, to, source->tabs);
}

// NSTextTabType (legacy) <-> alignment and options.
Address column_terminators(Quartz& q) {
    const auto set_class = q.objc.find_class("NSCharacterSet");
    if (!set_class) unsupported("decimal tab stops (no NSCharacterSet)");
    return q.send_id(set_class, "characterSetWithCharactersInString:", {q.autorelease(q.cf.string(".,"))});
}

void install_text_tabs(Quartz& q) {
    text_tab_class = q.declare_class("NSTextTab", "NSObject", appkit_library);
    const auto t = text_tab_class;
    Quartz* const quartz = &q;
    const auto method = [&q](Address cls, const char* name, Handler handler, const char* types = "@@:", bool class_method = false,
                                   const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        static_cast<void>(q.objc.add_native_method(cls, name, scoped(std::move(handler)), class_method, types));
    };
    q.string_constant(appkit_library, "_NSTabColumnTerminatorsAttributeName", "NSTabColumnTerminatorsAttributeName");
    const auto init = [quartz](Address self, double location, std::int32_t align, Address options) {
        const auto state = quartz->objc.host_state<TextTabState>(self);
        state->location = location;
        state->align = align;
        quartz->release(std::exchange(state->options, options ? quartz->cf.copy(options) : quartz->cf.dictionary({})));
    };
    method(t, "init", [init](CpuState& s, GuestMemory&) { init(s.x[0], 0, alignment::left, 0); });
    // (alignment, location d0, options)
    method(t, "initWithTextAlignment:location:options:", [init](CpuState& s, GuestMemory&) {
        init(s.x[0], d(s, 0), ns_to_ct(static_cast<std::int64_t>(s.x[2])), s.x[3]);
    }, "@@:qd@");
    // Legacy types: left, right, center, decimal (right-aligned on the decimal separator).
    method(t, "initWithType:location:", [quartz, init](CpuState& s, GuestMemory&) {
        const auto type = s.x[2];
        if (type > 3) unsupported("NSTextTab type " + std::to_string(type));
        Address options{};
        if (type == 3) {
            const auto key = quartz->autorelease(quartz->cf.string("NSTabColumnTerminatorsAttributeName"));
            const std::pair<Address, Address> entry{key, column_terminators(*quartz)};
            options = quartz->autorelease(quartz->cf.dictionary(std::span(&entry, 1)));
        }
        const std::int32_t align = type == 0 ? alignment::left : type == 2 ? alignment::center : alignment::right;
        init(s.x[0], d(s, 0), align, options);
    }, "@@:Qd");
    method(t, "location", [quartz](CpuState& s, GuestMemory&) { set_d(s, 0, tab_state(*quartz, s.x[0])->location); }, "d@:");
    method(t, "alignment", [quartz](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(ct_to_ns(tab_state(*quartz, s.x[0])->align)); }, "q@:");
    method(t, "options", [quartz](CpuState& s, GuestMemory&) { s.x[0] = tab_state(*quartz, s.x[0])->options; });
    method(t, "tabStopType", [quartz](CpuState& s, GuestMemory&) {
        const auto state = tab_state(*quartz, s.x[0]);
        const bool decimal = state->options && quartz->dictionary_get(state->options, "NSTabColumnTerminatorsAttributeName") != 0;
        s.x[0] = state->align == alignment::center ? 2 : state->align == alignment::right ? (decimal ? 3 : 1) : 0;
    }, "Q@:");
    method(t, "copyWithZone:", [quartz](CpuState& s, GuestMemory&) { s.x[0] = quartz->retain(s.x[0]); }, "@@:^v");
    method(t, "isEqual:", [quartz](CpuState& s, GuestMemory&) {
        const auto other = quartz->find<TextTabState>(s.x[2]);
        const auto self = tab_state(*quartz, s.x[0]);
        return_bool(s, other && other->location == self->location && other->align == self->align &&
                           (other->options == self->options || quartz->cf.equal(other->options, self->options)));
    }, "B@:@");
    method(t, "hash", [quartz](CpuState& s, GuestMemory&) {
        const auto state = tab_state(*quartz, s.x[0]);
        s.x[0] = std::bit_cast<std::uint64_t>(state->location) ^ static_cast<std::uint64_t>(state->align);
    }, "Q@:");
    method(t, "description", [quartz](CpuState& s, GuestMemory&) {
        const auto state = tab_state(*quartz, s.x[0]);
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%gL", state->location);   // AppKit prints "28L" for a left tab
        if (state->align == alignment::right) buffer[std::strlen(buffer) - 1] = 'R';
        if (state->align == alignment::center) buffer[std::strlen(buffer) - 1] = 'C';
        s.x[0] = quartz->autorelease(quartz->cf.string(buffer));
    });
    method(t, "dealloc", [quartz, t](CpuState& s, GuestMemory&) {
        if (const auto state = quartz->find<TextTabState>(s.x[0])) quartz->release(std::exchange(state->options, 0));
        static_cast<void>(quartz->objc.send_super(s.x[0], t, quartz->objc.selector("dealloc")));
    }, "v@:");
    // Archives: NSLocation, NSTabOptions, NSAlignment (archived numbering: right 1, center 2).
    method(t, "initWithCoder:", [quartz, init](CpuState& s, GuestMemory&) {
        const auto coder = s.x[2];
        const auto key = [&](const char* name) { return quartz->autorelease(quartz->cf.string(name)); };
        const auto has = [&](const char* name) { return (quartz->send_id(coder, "containsValueForKey:", {key(name)}) & 0xff) != 0; };
        CallRegs registers;
        registers.x[2] = key("NSLocation");
        const double location = has("NSLocation") ? quartz->send(coder, "decodeDoubleForKey:", registers).d(0) : 0;
        const auto archived = has("NSAlignment") ? static_cast<std::int64_t>(quartz->send_id(coder, "decodeIntegerForKey:", {key("NSAlignment")})) : 0;
        const std::int32_t align = archived == 1 ? alignment::right : archived == 2 ? alignment::center : static_cast<std::int32_t>(archived);
        const auto options = has("NSTabOptions") ? quartz->send_id(coder, "decodeObjectForKey:", {key("NSTabOptions")}) : 0;
        init(s.x[0], location, align, options);
    });
    method(t, "columnTerminatorsForLocale:", [quartz](CpuState& s, GuestMemory&) { s.x[0] = column_terminators(*quartz); }, "@@:@", true);

    // CTTextTab (the same objects).
    q.function(core_text_library, "_CTTextTabCreate", [quartz](CpuState& s, GuestMemory&) {
        // (CTTextAlignment, location d0, options)
        s.x[0] = make_tab(*quartz, d(s, 0), static_cast<std::int32_t>(s.x[0] & 0xff), s.x[1]);
    });
    q.function(core_text_library, "_CTTextTabGetAlignment", [quartz](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(tab_state(*quartz, s.x[0])->align); });
    q.function(core_text_library, "_CTTextTabGetLocation", [quartz](CpuState& s, GuestMemory&) { set_d(s, 0, tab_state(*quartz, s.x[0])->location); });
    q.function(core_text_library, "_CTTextTabGetOptions", [quartz](CpuState& s, GuestMemory&) { s.x[0] = tab_state(*quartz, s.x[0])->options; });
    q.function(core_text_library, "_CTTextTabGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x101; });
}

} // namespace

Coretext& coretext() { return coretext_state; }

std::optional<Rgba> object_color(Quartz& q, Address color) {
    if (!color) return std::nullopt;
    if (q.find<ColorInfo>(color)) return q.color_rgba(color);
    if (!q.objc.responds(color, q.objc.selector("getRed:green:blue:alpha:"))) return std::nullopt;
    Quartz::Scratch buffer(q, 32);
    const auto base = buffer.address();
    // Colors in other spaces (pattern, catalog) convert to RGB first.
    auto rgb = color;
    if (q.objc.responds(color, q.objc.selector("colorUsingColorSpaceName:"))) {
        const auto converted = q.send_id(color, "colorUsingColorSpaceName:", {q.cf.constant_string("NSCalibratedRGBColorSpace")});
        if (converted) rgb = converted;
    }
    static_cast<void>(q.send_id(rgb, "getRed:green:blue:alpha:", {base, base + 8, base + 16, base + 24}));
    return Rgba{q.read_double(base), q.read_double(base + 8), q.read_double(base + 16), q.read_double(base + 24)};
}

ParagraphStyle paragraph_of(Quartz& q, Address paragraph) {
    if (const auto state = q.find<ParagraphState>(paragraph)) return state->style;
    return {};
}

TextStyle resolve_style(Quartz& q, Address attributes) {
    TextStyle style;
    const auto value = [&](std::string_view key) { return q.dictionary_get(attributes, key); };
    if (const auto font = value("NSFont"); font && font_state(q, font)) style.font = q.retain(font);
    else style.font = q.retain(default_font(q));
    style.font_state = font_state(q, style.font);
    if (const auto from_context = q.number(value("CTForegroundColorFromContext")); from_context && *from_context != 0) style.color = std::nullopt;
    else if (const auto color = object_color(q, value("CTForegroundColor"))) style.color = color;
    else if (const auto ns_color = object_color(q, value("NSColor"))) style.color = ns_color;
    else style.color = Rgba{0, 0, 0, 1};
    style.background = object_color(q, value("NSBackgroundColor"));
    style.kern = q.number(value("NSKern")).value_or(0);
    style.underline = static_cast<std::int64_t>(q.number(value("NSUnderline")).value_or(0));
    style.underline_color = object_color(q, value("NSUnderlineColor"));
    style.strikethrough = static_cast<std::int64_t>(q.number(value("NSStrikethrough")).value_or(0));
    style.strikethrough_color = object_color(q, value("NSStrikethroughColor"));
    style.baseline_offset = q.number(value("NSBaselineOffset")).value_or(0);
    style.stroke_width = q.number(value("NSStrokeWidth")).value_or(0);
    style.stroke_color = object_color(q, value("NSStrokeColor"));
    if (const auto paragraph = value("NSParagraphStyle"); paragraph && q.find<ParagraphState>(paragraph)) {
        style.paragraph = paragraph_of(q, paragraph);
        style.has_paragraph = true;
    }
    return style;
}
void release_style(Quartz& q, TextStyle& style) { q.release(std::exchange(style.font, 0)); }

AttributedText attributed_text(Quartz& q, Address string) {
    AttributedText text;
    if (!string) return text;
    if (q.cf.kind(string) == ObjectKind::string) return plain_text(q, string, coretext_state.empty_attributes);
    if (const auto s = q.find<AttributedState>(string)) {
        std::lock_guard lock(s->mutex);
        text.text = s->text;
        for (const auto& run : s->runs) text.runs.push_back({run.length, q.retain(run.attributes)});
        return text;
    }
    // Another NSAttributedString implementation: through its primitives.
    text.text = units_of(q, q.send_id(string, "string"));
    Quartz::Scratch range(q, 16);
    for (std::size_t at = 0; at < text.text.size();) {
        const auto attributes = q.send_id(string, "attributesAtIndex:effectiveRange:", {at, range.address()});
        const auto start = q.memory().read64(range.address()), length = q.memory().read64(range.address() + 8);
        const auto end = std::min<std::size_t>(text.text.size(), std::max<std::size_t>(at + 1, static_cast<std::size_t>(start + length)));
        text.runs.push_back({end - at, q.retain(attributes ? attributes : coretext_state.empty_attributes)});
        at = end;
    }
    return text;
}
AttributedText plain_text(Quartz& q, Address string, Address attributes) {
    AttributedText text;
    text.text = units_of(q, string);
    if (!text.text.empty()) text.runs.push_back({text.text.size(), q.retain(attributes ? attributes : coretext_state.empty_attributes)});
    return text;
}
void release_text(Quartz& q, AttributedText& text) { release_runs(q, text.runs); }
AttributedText copy_text(Quartz& q, const AttributedText& text) {
    AttributedText copy;
    copy.text = text.text;
    for (const auto& run : text.runs) copy.runs.push_back({run.length, q.retain(run.attributes)});
    return copy;
}
Address make_attributed(Quartz& q, AttributedText text, bool mutable_string) {
    const auto object = q.objc.allocate(mutable_string ? coretext_state.mutable_attributed_class : coretext_state.attributed_class);
    const auto s = state_of(q, object);
    s->text = std::move(text.text);
    s->runs = std::move(text.runs);
    s->mutable_string = mutable_string;
    coalesce(q, *s);
    return object;
}

void install_attributed_strings(Quartz& q) {
    auto& ct = coretext_state;
    const auto foundation = std::string_view(foundation_library);
    ct.attributed_class = q.objc.find_class("NSAttributedString");
    if (!ct.attributed_class) ct.attributed_class = q.declare_class("NSAttributedString", "NSObject", foundation);
    ct.mutable_attributed_class = q.declare_class("NSMutableAttributedString", "NSAttributedString", foundation);
    ct.paragraph_class = q.declare_class("_AlchemyCTParagraphStyle");
    ct.ns_paragraph_class = q.declare_class("NSParagraphStyle", "NSObject", appkit_library);
    ct.ns_mutable_paragraph_class = q.declare_class("NSMutableParagraphStyle", "NSParagraphStyle", appkit_library);
    if (!ct.empty_attributes) {
        ct.empty_attributes = q.cf.dictionary({});
        if (const auto record = q.process.objects().find(ct.empty_attributes)) record->immortal = true;
    }

    const auto method = [&q](Address cls, const char* name, Handler handler, const char* types = "@@:",
                                   const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        static_cast<void>(q.objc.add_native_method(cls, name, scoped(std::move(handler)), false, types));
    };
    const auto class_method = [&q](Address cls, const char* name, Handler handler, const char* types = "@@:",
                                   const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        static_cast<void>(q.objc.add_native_method(cls, name, scoped(std::move(handler)), true, types));
    };
    const auto a = ct.attributed_class, ma = ct.mutable_attributed_class;
    Quartz* const quartz = &q;

    // ---- NSAttributedString ----
    method(a, "init", [quartz](CpuState& s, GuestMemory&) { static_cast<void>(state_of(*quartz, s.x[0])); });
    method(a, "initWithString:", [quartz](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("-[NSAttributedString initWithString:] with a nil string");
        initialize(*quartz, s.x[0], units_of(*quartz, s.x[2]), 0);
    });
    method(a, "initWithString:attributes:", [quartz](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("-[NSAttributedString initWithString:attributes:] with a nil string");
        initialize(*quartz, s.x[0], units_of(*quartz, s.x[2]), s.x[3]);
    });
    method(a, "initWithAttributedString:", [quartz](CpuState& s, GuestMemory&) {
        auto text = attributed_text(*quartz, s.x[2]);
        const auto state = state_of(*quartz, s.x[0]);
        std::lock_guard lock(state->mutex);
        state->text = std::move(text.text);
        state->runs = std::move(text.runs);
    });
    method(a, "string", [quartz](CpuState& s, GuestMemory&) {
        const auto state = state_of(*quartz, s.x[0]);
        std::lock_guard lock(state->mutex);
        if (!state->string_cache) state->string_cache = quartz->cf.string_utf16(state->text);
        s.x[0] = state->string_cache;
    });
    method(a, "length", [quartz](CpuState& s, GuestMemory&) {
        const auto state = state_of(*quartz, s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->text.size();
    }, "Q@:");
    const auto attributes_method = [quartz](CpuState& s, GuestMemory& m, Address range_pointer) {
        const auto state = state_of(*quartz, s.x[0]);
        std::lock_guard lock(state->mutex);
        if (s.x[2] >= state->text.size()) range_error("-[NSAttributedString attributesAtIndex:effectiveRange:] index");
        std::size_t start{}, length{};
        const auto value = attributes_at(*state, static_cast<std::size_t>(s.x[2]), &start, &length);
        if (range_pointer) { m.write64(range_pointer, start); m.write64(range_pointer + 8, length); }
        s.x[0] = value;
    };
    method(a, "attributesAtIndex:effectiveRange:", [attributes_method](CpuState& s, GuestMemory& m) { attributes_method(s, m, s.x[3]); },
           "@@:Q^{_NSRange=QQ}");
    method(a, "attributesAtIndex:longestEffectiveRange:inRange:", [quartz, attributes_method](CpuState& s, GuestMemory& m) {
        // (index, range*, inRange location x4, length x5): runs are coalesced, so the run is the longest range.
        const auto pointer = s.x[3];
        const auto location = s.x[4], length = s.x[5];
        attributes_method(s, m, pointer);
        if (pointer) {
            const auto start = std::max<std::uint64_t>(m.read64(pointer), location);
            const auto end = std::min<std::uint64_t>(m.read64(pointer) + m.read64(pointer + 8), location + length);
            m.write64(pointer, start);
            m.write64(pointer + 8, end > start ? end - start : 0);
        }
        static_cast<void>(quartz);
    }, "@@:Q^{_NSRange=QQ}{_NSRange=QQ}");
    const auto attribute_method = [quartz](CpuState& s, GuestMemory& m, Address range_pointer) {
        // (name, index)
        const auto state = state_of(*quartz, s.x[0]);
        std::lock_guard lock(state->mutex);
        if (s.x[3] >= state->text.size()) range_error("-[NSAttributedString attribute:atIndex:effectiveRange:] index");
        std::size_t start{}, length{};
        const auto dictionary = attributes_at(*state, static_cast<std::size_t>(s.x[3]), &start, &length);
        Address found{};
        for (const auto& [key, value] : quartz->cf.dictionary_entries(dictionary)) if (quartz->cf.equal(key, s.x[2])) found = value;
        // The effective range of one attribute spans neighbouring runs with the same value.
        if (range_pointer) {
            std::size_t at = 0, begin = start, end = start + length;
            std::vector<std::pair<std::size_t, Address>> values;
            for (const auto& run : state->runs) {
                Address value{};
                for (const auto& [key, item] : quartz->cf.dictionary_entries(run.attributes)) if (quartz->cf.equal(key, s.x[2])) value = item;
                values.emplace_back(at, value);
                at += run.length;
            }
            const auto same = [&](Address value) { return value == found || (value && found && quartz->cf.equal(value, found)); };
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (values[index].first != start) continue;
                for (auto back = index; back-- > 0 && same(values[back].second);) begin = values[back].first;
                for (auto next = index + 1; next < values.size() && same(values[next].second); ++next)
                    end = next + 1 < values.size() ? values[next + 1].first : state->text.size();
                break;
            }
            m.write64(range_pointer, begin);
            m.write64(range_pointer + 8, end - begin);
        }
        s.x[0] = found;
    };
    method(a, "attribute:atIndex:effectiveRange:", [attribute_method](CpuState& s, GuestMemory& m) { attribute_method(s, m, s.x[4]); },
           "@@:@Q^{_NSRange=QQ}");
    method(a, "attribute:atIndex:longestEffectiveRange:inRange:", [attribute_method](CpuState& s, GuestMemory& m) {
        const auto pointer = s.x[4];
        const auto location = s.x[5], length = s.x[6];
        attribute_method(s, m, pointer);
        if (pointer) {
            const auto start = std::max<std::uint64_t>(m.read64(pointer), location);
            const auto end = std::min<std::uint64_t>(m.read64(pointer) + m.read64(pointer + 8), location + length);
            m.write64(pointer, start);
            m.write64(pointer + 8, end > start ? end - start : 0);
        }
    }, "@@:@Q^{_NSRange=QQ}{_NSRange=QQ}");
    method(a, "attributedSubstringFromRange:", [quartz](CpuState& s, GuestMemory&) {
        const auto state = state_of(*quartz, s.x[0]);
        AttributedText text;
        {
            std::lock_guard lock(state->mutex);
            check_range(*state, s.x[2], s.x[3], "-[NSAttributedString attributedSubstringFromRange:] range");
            text.text.assign(state->text.begin() + static_cast<std::ptrdiff_t>(s.x[2]), state->text.begin() + static_cast<std::ptrdiff_t>(s.x[2] + s.x[3]));
            text.runs = sub_runs(*quartz, *state, static_cast<std::size_t>(s.x[2]), static_cast<std::size_t>(s.x[3]));
        }
        s.x[0] = quartz->autorelease(make_attributed(*quartz, std::move(text), false));
    }, "@@:{_NSRange=QQ}");
    const auto equal_strings = [quartz](Address left, Address right) {
        if (left == right) return true;
        if (!left || !right || !quartz->find<AttributedState>(right)) return false;
        auto a_text = attributed_text(*quartz, left), b_text = attributed_text(*quartz, right);
        bool same = a_text.text == b_text.text && a_text.runs.size() == b_text.runs.size();
        for (std::size_t index = 0; same && index < a_text.runs.size(); ++index)
            same = a_text.runs[index].length == b_text.runs[index].length && quartz->cf.equal(a_text.runs[index].attributes, b_text.runs[index].attributes);
        release_text(*quartz, a_text);
        release_text(*quartz, b_text);
        return same;
    };
    method(a, "isEqualToAttributedString:", [equal_strings](CpuState& s, GuestMemory&) { return_bool(s, equal_strings(s.x[0], s.x[2])); }, "B@:@");
    method(a, "isEqual:", [equal_strings](CpuState& s, GuestMemory&) { return_bool(s, equal_strings(s.x[0], s.x[2])); }, "B@:@");
    method(a, "hash", [quartz](CpuState& s, GuestMemory&) {
        const auto state = state_of(*quartz, s.x[0]);
        std::lock_guard lock(state->mutex);
        std::uint64_t value = 1469598103934665603ULL;
        for (const auto unit : state->text) value = (value ^ unit) * 1099511628211ULL;
        s.x[0] = value;
    }, "Q@:");
    method(a, "copyWithZone:", [quartz](CpuState& s, GuestMemory&) {
        const auto state = state_of(*quartz, s.x[0]);
        if (!state->mutable_string && quartz->objc.object_class(s.x[0]) == coretext_state.attributed_class) { s.x[0] = quartz->retain(s.x[0]); return; }
        s.x[0] = make_attributed(*quartz, attributed_text(*quartz, s.x[0]), false);
    }, "@@:^v");
    method(a, "mutableCopyWithZone:", [quartz](CpuState& s, GuestMemory&) { s.x[0] = make_attributed(*quartz, attributed_text(*quartz, s.x[0]), true); }, "@@:^v");
    method(a, "description", [quartz](CpuState& s, GuestMemory&) {
        const auto state = state_of(*quartz, s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = quartz->autorelease(quartz->cf.string_utf16(state->text));
    });
    // Enumeration: block(attributes or value, NSRange (2 registers), BOOL *stop).
    const auto enumerate = [quartz](Address self, std::uint64_t location, std::uint64_t length, std::uint64_t options, Address block,
                                    std::optional<Address> key) {
        if (!block) return;
        AttributedText text = attributed_text(*quartz, self);
        if (location > text.text.size() || length > text.text.size() - location) { release_text(*quartz, text); range_error("enumeration range"); }
        struct Piece { std::size_t start, length; Address value; };
        std::vector<Piece> pieces;
        std::size_t at = 0;
        for (const auto& run : text.runs) {
            const auto begin = std::max<std::size_t>(at, location), end = std::min<std::size_t>(at + run.length, location + length);
            if (begin < end) {
                Address value = run.attributes;
                if (key) {
                    value = 0;
                    for (const auto& [name, item] : quartz->cf.dictionary_entries(run.attributes)) if (quartz->cf.equal(name, *key)) value = item;
                    if (!pieces.empty() && (pieces.back().value == value || (value && pieces.back().value && quartz->cf.equal(value, pieces.back().value)))) {
                        pieces.back().length += end - begin;
                        at += run.length;
                        continue;
                    }
                }
                pieces.push_back({begin, end - begin, value});
            }
            at += run.length;
        }
        if (options & 2) std::reverse(pieces.begin(), pieces.end());   // NSAttributedStringEnumerationReverse
        Quartz::Scratch stop(*quartz, 16);
        const auto invoke = quartz->memory().read64(block + 16);
        for (const auto& piece : pieces) {
            quartz->memory().write8(stop.address(), 0);
            static_cast<void>(quartz->call_guest(invoke, {block, piece.value, piece.start, piece.length, stop.address()}));
            if (quartz->memory().read8(stop.address())) break;
        }
        release_text(*quartz, text);
    };
    method(a, "enumerateAttributesInRange:options:usingBlock:", [enumerate](CpuState& s, GuestMemory&) {
        enumerate(s.x[0], s.x[2], s.x[3], s.x[4], s.x[5], std::nullopt);
    }, "v@:{_NSRange=QQ}Q@?");
    method(a, "enumerateAttribute:inRange:options:usingBlock:", [enumerate](CpuState& s, GuestMemory&) {
        enumerate(s.x[0], s.x[3], s.x[4], s.x[5], s.x[6], s.x[2]);
    }, "v@:@{_NSRange=QQ}Q@?");
    method(a, "dealloc", [quartz, a](CpuState& s, GuestMemory&) {
        if (const auto state = quartz->find<AttributedState>(s.x[0])) {
            std::lock_guard lock(state->mutex);
            release_runs(*quartz, state->runs);
            quartz->release(std::exchange(state->string_cache, 0));
        }
        static_cast<void>(quartz->objc.send_super(s.x[0], a, quartz->objc.selector("dealloc")));
    }, "v@:");

    // ---- NSMutableAttributedString ----
    const auto mutate = [quartz](Address self, const std::function<void(AttributedState&)>& body) {
        const auto state = state_of(*quartz, self);
        std::lock_guard lock(state->mutex);
        state->mutable_string = true;
        body(*state);
    };
    method(ma, "init", [quartz](CpuState& s, GuestMemory&) { state_of(*quartz, s.x[0])->mutable_string = true; });
    method(ma, "replaceCharactersInRange:withString:", [quartz, mutate](CpuState& s, GuestMemory&) {
        const auto units = units_of(*quartz, s.x[4]);
        mutate(s.x[0], [&](AttributedState& state) {
            check_range(state, s.x[2], s.x[3], "-replaceCharactersInRange:withString: range");
            replace(*quartz, state, static_cast<std::size_t>(s.x[2]), static_cast<std::size_t>(s.x[3]), units, std::nullopt);
        });
    }, "v@:{_NSRange=QQ}@");
    method(ma, "deleteCharactersInRange:", [quartz, mutate](CpuState& s, GuestMemory&) {
        mutate(s.x[0], [&](AttributedState& state) {
            check_range(state, s.x[2], s.x[3], "-deleteCharactersInRange: range");
            replace(*quartz, state, static_cast<std::size_t>(s.x[2]), static_cast<std::size_t>(s.x[3]), {}, std::vector<AttributeRun>{});
        });
    }, "v@:{_NSRange=QQ}");
    const auto replace_attributed = [quartz, mutate](Address self, std::uint64_t location, std::uint64_t length, Address string) {
        auto text = attributed_text(*quartz, string);
        mutate(self, [&](AttributedState& state) {
            if (location > state.text.size() || length > state.text.size() - location) { release_text(*quartz, text); range_error("replacement range"); }
            replace(*quartz, state, static_cast<std::size_t>(location), static_cast<std::size_t>(length), text.text, std::move(text.runs));
        });
    };
    method(ma, "replaceCharactersInRange:withAttributedString:", [replace_attributed](CpuState& s, GuestMemory&) {
        replace_attributed(s.x[0], s.x[2], s.x[3], s.x[4]);
    }, "v@:{_NSRange=QQ}@");
    method(ma, "insertAttributedString:atIndex:", [replace_attributed](CpuState& s, GuestMemory&) { replace_attributed(s.x[0], s.x[3], 0, s.x[2]); },
           "v@:@Q");
    method(ma, "appendAttributedString:", [quartz, replace_attributed](CpuState& s, GuestMemory&) {
        const auto state = state_of(*quartz, s.x[0]);
        std::size_t end{};
        { std::lock_guard lock(state->mutex); end = state->text.size(); }
        replace_attributed(s.x[0], end, 0, s.x[2]);
    }, "v@:@");
    method(ma, "setAttributedString:", [quartz, replace_attributed](CpuState& s, GuestMemory&) {
        const auto state = state_of(*quartz, s.x[0]);
        std::size_t end{};
        { std::lock_guard lock(state->mutex); end = state->text.size(); }
        replace_attributed(s.x[0], 0, end, s.x[2]);
    }, "v@:@");
    method(ma, "setAttributes:range:", [quartz, mutate](CpuState& s, GuestMemory&) {
        // (attributes, location, length)
        const auto attributes = s.x[2] ? quartz->cf.copy(s.x[2]) : quartz->retain(coretext_state.empty_attributes);
        mutate(s.x[0], [&](AttributedState& state) {
            if (s.x[3] > state.text.size() || s.x[4] > state.text.size() - s.x[3]) { quartz->release(attributes); range_error("-setAttributes:range: range"); }
            set_runs(*quartz, state, static_cast<std::size_t>(s.x[3]), static_cast<std::size_t>(s.x[4]), attributes);
        });
        quartz->release(attributes);
    }, "v@:@{_NSRange=QQ}");
    method(ma, "addAttribute:value:range:", [quartz, mutate](CpuState& s, GuestMemory&) {
        // (name, value, location, length)
        if (!s.x[2] || !s.x[3]) fail("-[NSMutableAttributedString addAttribute:value:range:] with a nil name or value");
        mutate(s.x[0], [&](AttributedState& state) {
            check_range(state, s.x[4], s.x[5], "-addAttribute:value:range: range");
            modify_runs(*quartz, state, static_cast<std::size_t>(s.x[4]), static_cast<std::size_t>(s.x[5]), [&](auto& entries) {
                erase_key(*quartz, entries, s.x[2]);
                entries.emplace_back(s.x[2], s.x[3]);
            });
        });
    }, "v@:@@{_NSRange=QQ}");
    method(ma, "addAttributes:range:", [quartz, mutate](CpuState& s, GuestMemory&) {
        // (attributes, location, length)
        const auto added = quartz->cf.dictionary_entries(s.x[2]);
        mutate(s.x[0], [&](AttributedState& state) {
            check_range(state, s.x[3], s.x[4], "-addAttributes:range: range");
            modify_runs(*quartz, state, static_cast<std::size_t>(s.x[3]), static_cast<std::size_t>(s.x[4]), [&](auto& entries) {
                for (const auto& [key, value] : added) { erase_key(*quartz, entries, key); entries.emplace_back(key, value); }
            });
        });
    }, "v@:@{_NSRange=QQ}");
    method(ma, "removeAttribute:range:", [quartz, mutate](CpuState& s, GuestMemory&) {
        mutate(s.x[0], [&](AttributedState& state) {
            check_range(state, s.x[3], s.x[4], "-removeAttribute:range: range");
            modify_runs(*quartz, state, static_cast<std::size_t>(s.x[3]), static_cast<std::size_t>(s.x[4]),
                        [&](auto& entries) { erase_key(*quartz, entries, s.x[2]); });
        });
    }, "v@:@{_NSRange=QQ}");
    method(ma, "beginEditing", [quartz](CpuState& s, GuestMemory&) { ++state_of(*quartz, s.x[0])->editing; }, "v@:");
    method(ma, "endEditing", [quartz](CpuState& s, GuestMemory&) { --state_of(*quartz, s.x[0])->editing; }, "v@:");
    method(ma, "mutableString", [](CpuState&, GuestMemory&) {
        unsupported("-[NSMutableAttributedString mutableString] (live mutable string proxy)");
    });
    method(ma, "copyWithZone:", [quartz](CpuState& s, GuestMemory&) { s.x[0] = make_attributed(*quartz, attributed_text(*quartz, s.x[0]), false); }, "@@:^v");

    // ---- CFAttributedString (CoreFoundation, toll-free bridged) ----
    const auto cf_library = core_foundation_library;
    const auto cf_function = [&q, cf_library](const char* name, Handler handler) { q.function(cf_library, name, std::move(handler)); };
    cf_function("_CFAttributedStringCreate", [quartz](CpuState& s, GuestMemory&) {
        // (allocator, string, attributes)
        if (!s.x[1]) { s.x[0] = 0; return; }
        s.x[0] = make_attributed(*quartz, plain_text(*quartz, s.x[1], 0), false);
        if (s.x[2]) {
            const auto state = state_of(*quartz, s.x[0]);
            const auto copy = quartz->cf.copy(s.x[2]);
            for (auto& run : state->runs) { quartz->release(run.attributes); run.attributes = quartz->retain(copy); }
            quartz->release(copy);
        }
    });
    cf_function("_CFAttributedStringCreateWithSubstring", [quartz](CpuState& s, GuestMemory&) {
        // (allocator, string, CFRange x2 x3)
        auto text = attributed_text(*quartz, s.x[1]);
        if (s.x[2] > text.text.size() || s.x[3] > text.text.size() - s.x[2]) { release_text(*quartz, text); range_error("CFAttributedStringCreateWithSubstring range"); }
        const auto object = make_attributed(*quartz, std::move(text), false);
        const auto state = state_of(*quartz, object);
        AttributedText sub;
        sub.text.assign(state->text.begin() + static_cast<std::ptrdiff_t>(s.x[2]), state->text.begin() + static_cast<std::ptrdiff_t>(s.x[2] + s.x[3]));
        sub.runs = sub_runs(*quartz, *state, static_cast<std::size_t>(s.x[2]), static_cast<std::size_t>(s.x[3]));
        quartz->release(object);
        s.x[0] = make_attributed(*quartz, std::move(sub), false);
    });
    cf_function("_CFAttributedStringCreateCopy", [quartz](CpuState& s, GuestMemory&) { s.x[0] = s.x[1] ? make_attributed(*quartz, attributed_text(*quartz, s.x[1]), false) : 0; });
    cf_function("_CFAttributedStringCreateMutableCopy", [quartz](CpuState& s, GuestMemory&) { s.x[0] = make_attributed(*quartz, attributed_text(*quartz, s.x[2]), true); });
    cf_function("_CFAttributedStringCreateMutable", [quartz](CpuState& s, GuestMemory&) { s.x[0] = make_attributed(*quartz, {}, true); });
    cf_function("_CFAttributedStringGetString", [quartz](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? quartz->send_id(s.x[0], "string") : 0; });
    cf_function("_CFAttributedStringGetLength", [quartz](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? quartz->send(s.x[0], "length").x0 : 0; });
    cf_function("_CFAttributedStringGetAttributes", [quartz](CpuState& s, GuestMemory&) {
        s.x[0] = quartz->send_id(s.x[0], "attributesAtIndex:effectiveRange:", {s.x[1], s.x[2]});
    });
    cf_function("_CFAttributedStringGetAttribute", [quartz](CpuState& s, GuestMemory&) {
        // (string, location, name, effectiveRange*)
        s.x[0] = quartz->send_id(s.x[0], "attribute:atIndex:effectiveRange:", {s.x[2], s.x[1], s.x[3]});
    });
    cf_function("_CFAttributedStringGetAttributesAndLongestEffectiveRange", [quartz](CpuState& s, GuestMemory&) {
        // (string, location, inRange x2 x3, longest*)
        s.x[0] = quartz->send_id(s.x[0], "attributesAtIndex:longestEffectiveRange:inRange:", {s.x[1], s.x[4], s.x[2], s.x[3]});
    });
    cf_function("_CFAttributedStringGetAttributeAndLongestEffectiveRange", [quartz](CpuState& s, GuestMemory&) {
        // (string, location, name, inRange x3 x4, longest*)
        s.x[0] = quartz->send_id(s.x[0], "attribute:atIndex:longestEffectiveRange:inRange:", {s.x[2], s.x[1], s.x[5], s.x[3], s.x[4]});
    });
    cf_function("_CFAttributedStringReplaceString", [quartz](CpuState& s, GuestMemory&) {
        static_cast<void>(quartz->send_id(s.x[0], "replaceCharactersInRange:withString:", {s.x[1], s.x[2], s.x[3]}));
    });
    cf_function("_CFAttributedStringReplaceAttributedString", [quartz](CpuState& s, GuestMemory&) {
        static_cast<void>(quartz->send_id(s.x[0], "replaceCharactersInRange:withAttributedString:", {s.x[1], s.x[2], s.x[3]}));
    });
    cf_function("_CFAttributedStringSetAttributes", [quartz](CpuState& s, GuestMemory&) {
        // (string, range x1 x2, replacement, clearOtherAttributes)
        if (bool_arg(s, 4)) static_cast<void>(quartz->send_id(s.x[0], "setAttributes:range:", {s.x[3], s.x[1], s.x[2]}));
        else static_cast<void>(quartz->send_id(s.x[0], "addAttributes:range:", {s.x[3], s.x[1], s.x[2]}));
    });
    cf_function("_CFAttributedStringSetAttribute", [quartz](CpuState& s, GuestMemory&) {
        // (string, range x1 x2, name, value)
        static_cast<void>(quartz->send_id(s.x[0], "addAttribute:value:range:", {s.x[3], s.x[4], s.x[1], s.x[2]}));
    });
    cf_function("_CFAttributedStringRemoveAttribute", [quartz](CpuState& s, GuestMemory&) {
        static_cast<void>(quartz->send_id(s.x[0], "removeAttribute:range:", {s.x[3], s.x[1], s.x[2]}));
    });
    cf_function("_CFAttributedStringBeginEditing", [quartz](CpuState& s, GuestMemory&) { static_cast<void>(quartz->send_id(s.x[0], "beginEditing")); });
    cf_function("_CFAttributedStringEndEditing", [quartz](CpuState& s, GuestMemory&) { static_cast<void>(quartz->send_id(s.x[0], "endEditing")); });
    cf_function("_CFAttributedStringGetMutableString", [](CpuState&, GuestMemory&) {
        unsupported("CFAttributedStringGetMutableString (live mutable string proxy)");
    });
    cf_function("_CFAttributedStringGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });

    // ---- paragraph styles ----
    const auto paragraph_state = [quartz](Address object) { return quartz->objc.host_state<ParagraphState>(object); };
    const auto make_paragraph = [quartz](Address cls, const ParagraphStyle& style) {
        const auto object = quartz->objc.allocate(cls);
        quartz->objc.host_state<ParagraphState>(object)->style = style;
        return object;
    };
    q.function(core_text_library, "_CTParagraphStyleCreate", [quartz, make_paragraph](CpuState& s, GuestMemory& m) {
        // (settings {u32 spec; size_t size; const void* value}[count], count)
        ParagraphStyle style;
        std::vector<Address> tab_objects;
        bool tabs_given{};
        for (Address index = 0; index < s.x[1] && s.x[0]; ++index) {
            const auto entry = s.x[0] + index * 24;
            const auto spec = m.read32(entry);
            const auto size = m.read64(entry + 8), value = m.read64(entry + 16);
            if (!value) continue;
            const auto real = [&]() { return size >= 8 ? quartz->read_double(value) : 0.0; };
            switch (spec) {
            case 0: style.alignment = m.read8(value); break;
            case 1: style.first_head_indent = real(); break;
            case 2: style.head_indent = real(); break;
            case 3: style.tail_indent = real(); break;
            case 4: {   // tab stops: CFArray of CTTextTab (NSTextTab)
                const auto tabs = m.read64(value);
                tab_objects = tabs ? quartz->cf.array_values(tabs) : std::vector<Address>{};
                tabs_given = true;
                break;
            }
            case 5: style.default_tab_interval = real(); break;
            case 6: style.line_break = m.read8(value); break;
            case 7: style.line_height_multiple = real(); break;
            case 8: style.maximum_line_height = real(); break;
            case 9: style.minimum_line_height = real(); break;
            case 10: case 16: style.line_spacing = real(); break;
            case 11: style.paragraph_spacing = real(); break;
            case 12: style.paragraph_spacing_before = real(); break;
            case 13: style.writing_direction = static_cast<std::int8_t>(m.read8(value)); break;
            default: break;   // maximum/minimum line spacing, line bounds options: defaults
            }
        }
        s.x[0] = make_paragraph(coretext_state.paragraph_class, style);
        if (tabs_given) set_tabs(*quartz, s.x[0], tab_objects);
    });
    q.function(core_text_library, "_CTParagraphStyleCreateCopy", [quartz, make_paragraph](CpuState& s, GuestMemory&) {
        const auto copy = make_paragraph(coretext_state.paragraph_class, paragraph_of(*quartz, s.x[0]));
        copy_tabs(*quartz, s.x[0], copy);
        s.x[0] = copy;
    });
    q.function(core_text_library, "_CTParagraphStyleGetValueForSpecifier", [quartz](CpuState& s, GuestMemory& m) {
        // (style, spec, bufferSize, buffer)
        const auto style = paragraph_of(*quartz, s.x[0]);
        const auto spec = static_cast<std::uint32_t>(s.x[1]);
        const auto size = s.x[2], buffer = s.x[3];
        if (!buffer) { return_bool(s, false); return; }
        const auto write_real = [&](double value) { if (size < 8) return false; quartz->write_double(buffer, value); return true; };
        bool ok = true;
        switch (spec) {
        case 0: if (size >= 1) m.write8(buffer, static_cast<std::uint8_t>(style.alignment)); else ok = false; break;
        case 1: ok = write_real(style.first_head_indent); break;
        case 2: ok = write_real(style.head_indent); break;
        case 3: ok = write_real(style.tail_indent); break;
        case 5: ok = write_real(style.default_tab_interval); break;
        case 6: if (size >= 1) m.write8(buffer, static_cast<std::uint8_t>(style.line_break)); else ok = false; break;
        case 7: ok = write_real(style.line_height_multiple); break;
        case 8: ok = write_real(style.maximum_line_height); break;
        case 9: ok = write_real(style.minimum_line_height); break;
        case 10: case 16: ok = write_real(style.line_spacing); break;
        case 11: ok = write_real(style.paragraph_spacing); break;
        case 12: ok = write_real(style.paragraph_spacing_before); break;
        case 13: if (size >= 1) m.write8(buffer, static_cast<std::uint8_t>(style.writing_direction)); else ok = false; break;
        default: ok = false;
        }
        return_bool(s, ok);
    });
    q.function(core_text_library, "_CTParagraphStyleGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });

    // NSParagraphStyle / NSMutableParagraphStyle (AppKit).
    const auto ps = ct.ns_paragraph_class, mps = ct.ns_mutable_paragraph_class;
    if (!ct.default_paragraph) {
        ct.default_paragraph = make_paragraph(ps, ParagraphStyle{});
        if (const auto record = q.process.objects().find(ct.default_paragraph)) record->immortal = true;
    }
    class_method(ps, "defaultParagraphStyle", [](CpuState& s, GuestMemory&) { s.x[0] = coretext_state.default_paragraph; });
    class_method(ps, "defaultWritingDirectionForLanguage:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:@");   // left to right
    method(ps, "init", [paragraph_state](CpuState& s, GuestMemory&) { static_cast<void>(paragraph_state(s.x[0])); });
    method(ps, "copyWithZone:", [quartz, make_paragraph, paragraph_state](CpuState& s, GuestMemory&) {
        if (quartz->objc.object_class(s.x[0]) == coretext_state.ns_paragraph_class) { s.x[0] = quartz->retain(s.x[0]); return; }
        const auto copy = make_paragraph(coretext_state.ns_paragraph_class, paragraph_state(s.x[0])->style);
        copy_tabs(*quartz, s.x[0], copy);
        s.x[0] = copy;
    }, "@@:^v");
    method(ps, "mutableCopyWithZone:", [quartz, make_paragraph, paragraph_state](CpuState& s, GuestMemory&) {
        const auto copy = make_paragraph(coretext_state.ns_mutable_paragraph_class, paragraph_state(s.x[0])->style);
        copy_tabs(*quartz, s.x[0], copy);
        s.x[0] = copy;
    }, "@@:^v");
    for (const auto cls : {ps, coretext_state.paragraph_class})
        method(cls, "dealloc", [quartz, cls](CpuState& s, GuestMemory&) {
            if (const auto state = quartz->find<ParagraphState>(s.x[0])) {
                for (const auto tab : std::exchange(state->tabs, {})) quartz->release(tab);
                for (const auto list : std::exchange(state->lists, {})) quartz->release(list);
            }
            static_cast<void>(quartz->objc.send_super(s.x[0], cls, quartz->objc.selector("dealloc")));
        }, "v@:");
    // Archived paragraph styles (keyed archives; nibs make them values).
    method(ps, "initWithCoder:", [quartz, paragraph_state](CpuState& s, GuestMemory&) {
        const auto coder = s.x[2];
        const auto has = [&](const char* key) { return (quartz->send_id(coder, "containsValueForKey:", {quartz->autorelease(quartz->cf.string(key))}) & 0xff) != 0; };
        const auto real = [&](const char* key, double fallback) {
            if (!has(key)) return fallback;
            CallRegs registers;
            registers.x[2] = quartz->autorelease(quartz->cf.string(key));
            return quartz->send(coder, "decodeDoubleForKey:", registers).d(0);
        };
        const auto integer = [&](const char* key, std::int64_t fallback) {
            return has(key) ? static_cast<std::int64_t>(quartz->send_id(coder, "decodeIntegerForKey:", {quartz->autorelease(quartz->cf.string(key))})) : fallback;
        };
        auto& style = paragraph_state(s.x[0])->style;
        // Archives number alignments the original AppKit way (right 1, center 2), as the cell flags do.
        const auto archived = integer("NSAlignment", 4);
        style.alignment = archived == 1 ? alignment::right : archived == 2 ? alignment::center : static_cast<std::int32_t>(archived);
        style.line_break = static_cast<std::int32_t>(integer("NSLineBreakMode", 0));
        style.first_head_indent = real("NSFirstLineHeadIndent", 0);
        style.head_indent = real("NSHeadIndent", 0);
        style.tail_indent = real("NSTailIndent", 0);
        style.line_spacing = real("NSLineSpacing", 0);
        style.paragraph_spacing = real("NSParagraphSpacing", 0);
        style.paragraph_spacing_before = real("NSParagraphSpacingBefore", 0);
        style.minimum_line_height = real("NSMinLineHeight", 0);
        style.maximum_line_height = real("NSMaxLineHeight", 0);
        style.line_height_multiple = real("NSLineHeightMultiple", 0);
        style.default_tab_interval = real("NSDefaultTabInterval", 0);
        style.writing_direction = static_cast<std::int32_t>(integer("NSWritingDirection", -1));
        style.allows_tightening = integer("NSAllowsTighteningForTruncation", 0) != 0;
        if (has("NSTabStops")) {
            const auto tabs = quartz->send_id(coder, "decodeObjectForKey:", {quartz->autorelease(quartz->cf.string("NSTabStops"))});
            set_tabs(*quartz, s.x[0], tabs ? quartz->cf.array_values(tabs) : std::vector<Address>{});
        }
    });
    method(ps, "alignment", [paragraph_state](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(ct_to_ns(paragraph_state(s.x[0])->style.alignment)); }, "q@:");
    method(ps, "lineBreakMode", [paragraph_state](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(paragraph_state(s.x[0])->style.line_break); }, "Q@:");
    method(ps, "baseWritingDirection", [paragraph_state](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(static_cast<std::int64_t>(paragraph_state(s.x[0])->style.writing_direction));
    }, "q@:");
    const auto real_getter = [&method, paragraph_state, ps](const char* name, double ParagraphStyle::*field) {
        method(ps, name, [paragraph_state, field](CpuState& s, GuestMemory&) { set_d(s, 0, paragraph_state(s.x[0])->style.*field); }, "d@:");
    };
    const auto real_setter = [&method, paragraph_state, mps](const char* name, double ParagraphStyle::*field) {
        method(mps, name, [paragraph_state, field](CpuState& s, GuestMemory&) { paragraph_state(s.x[0])->style.*field = d(s, 0); }, "v@:d");
    };
    for (const auto& [getter, setter, field] : std::initializer_list<std::tuple<const char*, const char*, double ParagraphStyle::*>>{
             {"lineSpacing", "setLineSpacing:", &ParagraphStyle::line_spacing},
             {"paragraphSpacing", "setParagraphSpacing:", &ParagraphStyle::paragraph_spacing},
             {"paragraphSpacingBefore", "setParagraphSpacingBefore:", &ParagraphStyle::paragraph_spacing_before},
             {"headIndent", "setHeadIndent:", &ParagraphStyle::head_indent},
             {"tailIndent", "setTailIndent:", &ParagraphStyle::tail_indent},
             {"firstLineHeadIndent", "setFirstLineHeadIndent:", &ParagraphStyle::first_head_indent},
             {"minimumLineHeight", "setMinimumLineHeight:", &ParagraphStyle::minimum_line_height},
             {"maximumLineHeight", "setMaximumLineHeight:", &ParagraphStyle::maximum_line_height},
             {"lineHeightMultiple", "setLineHeightMultiple:", &ParagraphStyle::line_height_multiple},
             {"defaultTabInterval", "setDefaultTabInterval:", &ParagraphStyle::default_tab_interval}}) {
        real_getter(getter, field);
        real_setter(setter, field);
    }
    method(ps, "tabStops", [quartz](CpuState& s, GuestMemory&) { s.x[0] = quartz->autorelease(quartz->cf.array(tabs_of(*quartz, s.x[0]))); });
    method(ps, "hyphenationFactor", [paragraph_state](CpuState& s, GuestMemory&) { set_float(s, 0, paragraph_state(s.x[0])->style.hyphenation_factor); }, "f@:");
    method(ps, "allowsDefaultTighteningForTruncation", [paragraph_state](CpuState& s, GuestMemory&) {
        return_bool(s, paragraph_state(s.x[0])->style.allows_tightening);
    }, "B@:");
    method(ps, "isEqual:", [quartz, paragraph_state](CpuState& s, GuestMemory&) {
        const auto other = quartz->find<ParagraphState>(s.x[2]);
        if (!other) { return_bool(s, false); return; }
        const auto& a_style = paragraph_state(s.x[0])->style;
        const auto& b_style = other->style;
        const auto a_tabs = tabs_of(*quartz, s.x[0]), b_tabs = tabs_of(*quartz, s.x[2]);
        bool same_tabs = a_tabs.size() == b_tabs.size();
        for (std::size_t i = 0; same_tabs && i < a_tabs.size(); ++i)
            same_tabs = (quartz->send_id(a_tabs[i], "isEqual:", {b_tabs[i]}) & 0xff) != 0;
        if (!same_tabs) { return_bool(s, false); return; }
        return_bool(s, a_style.alignment == b_style.alignment && a_style.line_break == b_style.line_break &&
                           a_style.line_spacing == b_style.line_spacing && a_style.paragraph_spacing == b_style.paragraph_spacing &&
                           a_style.head_indent == b_style.head_indent && a_style.tail_indent == b_style.tail_indent &&
                           a_style.first_head_indent == b_style.first_head_indent &&
                           a_style.minimum_line_height == b_style.minimum_line_height &&
                           a_style.maximum_line_height == b_style.maximum_line_height &&
                           a_style.line_height_multiple == b_style.line_height_multiple &&
                           a_style.paragraph_spacing_before == b_style.paragraph_spacing_before &&
                           a_style.header_level == b_style.header_level && a_style.line_break_strategy == b_style.line_break_strategy &&
                           paragraph_state(s.x[0])->lists.size() == other->lists.size());
    }, "B@:@");
    method(ps, "headerLevel", [paragraph_state](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(paragraph_state(s.x[0])->style.header_level); }, "q@:");
    method(ps, "lineBreakStrategy", [paragraph_state](CpuState& s, GuestMemory&) { s.x[0] = paragraph_state(s.x[0])->style.line_break_strategy; }, "Q@:");
    method(ps, "tighteningFactorForTruncation", [paragraph_state](CpuState& s, GuestMemory&) {
        set_float(s, 0, paragraph_state(s.x[0])->style.tightening_factor);
    }, "f@:");
    method(ps, "usesDefaultHyphenation", [paragraph_state](CpuState& s, GuestMemory&) {
        return_bool(s, paragraph_state(s.x[0])->style.uses_default_hyphenation);
    }, "B@:");
    method(ps, "textLists", [quartz, paragraph_state](CpuState& s, GuestMemory&) {
        s.x[0] = quartz->autorelease(quartz->cf.array(paragraph_state(s.x[0])->lists));
    });
    // Text blocks (tables) are not laid out, so none can be set.
    method(ps, "textBlocks", [quartz](CpuState& s, GuestMemory&) { s.x[0] = quartz->autorelease(quartz->cf.array({})); });
    method(mps, "setAlignment:", [paragraph_state](CpuState& s, GuestMemory&) { paragraph_state(s.x[0])->style.alignment = ns_to_ct(static_cast<std::int64_t>(s.x[2])); }, "v@:q");
    method(mps, "setLineBreakMode:", [paragraph_state](CpuState& s, GuestMemory&) { paragraph_state(s.x[0])->style.line_break = static_cast<std::int32_t>(s.x[2]); }, "v@:Q");
    method(mps, "setBaseWritingDirection:", [paragraph_state](CpuState& s, GuestMemory&) {
        paragraph_state(s.x[0])->style.writing_direction = static_cast<std::int32_t>(static_cast<std::int64_t>(s.x[2]));
    }, "v@:q");
    method(mps, "setParagraphStyle:", [quartz, paragraph_state](CpuState& s, GuestMemory&) {
        paragraph_state(s.x[0])->style = s.x[2] ? paragraph_of(*quartz, s.x[2]) : ParagraphStyle{};
        if (s.x[2]) {
            copy_tabs(*quartz, s.x[2], s.x[0]);
        } else {
            const auto state = paragraph_state(s.x[0]);
            for (const auto tab : std::exchange(state->tabs, {})) quartz->release(tab);
            for (const auto list : std::exchange(state->lists, {})) quartz->release(list);
            state->tabs_set = false;
        }
    }, "v@:@");
    // Tab stops are kept (NSTextTab objects, sorted by location); line
    // layout tabs to the default interval (typesetter.cpp).
    method(mps, "setTabStops:", [quartz](CpuState& s, GuestMemory&) {
        set_tabs(*quartz, s.x[0], s.x[2] ? quartz->cf.array_values(s.x[2]) : std::vector<Address>{});
    }, "v@:@");
    method(mps, "addTabStop:", [quartz](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        auto list = tabs_of(*quartz, s.x[0]);
        list.push_back(s.x[2]);
        set_tabs(*quartz, s.x[0], list);
    }, "v@:@");
    method(mps, "removeTabStop:", [quartz](CpuState& s, GuestMemory&) {
        auto list = tabs_of(*quartz, s.x[0]);
        list.erase(std::remove_if(list.begin(), list.end(), [&](Address tab) {
            return tab == s.x[2] || (quartz->send_id(tab, "isEqual:", {s.x[2]}) & 0xff) != 0;
        }), list.end());
        set_tabs(*quartz, s.x[0], list);
    }, "v@:@");
    method(mps, "setHyphenationFactor:", [paragraph_state](CpuState& s, GuestMemory&) {
        paragraph_state(s.x[0])->style.hyphenation_factor = std::clamp(float_arg(s, 0), 0.0f, 1.0f);
    }, "v@:f");
    method(mps, "setAllowsDefaultTighteningForTruncation:", [paragraph_state](CpuState& s, GuestMemory&) {
        paragraph_state(s.x[0])->style.allows_tightening = bool_arg(s, 2);
    }, "v@:B");
    method(mps, "setHeaderLevel:", [paragraph_state](CpuState& s, GuestMemory&) {
        paragraph_state(s.x[0])->style.header_level = std::clamp<std::int64_t>(static_cast<std::int64_t>(s.x[2]), 0, 6);
    }, "v@:q");
    method(mps, "setLineBreakStrategy:", [paragraph_state](CpuState& s, GuestMemory&) { paragraph_state(s.x[0])->style.line_break_strategy = s.x[2]; }, "v@:Q");
    method(mps, "setTighteningFactorForTruncation:", [paragraph_state](CpuState& s, GuestMemory&) {
        paragraph_state(s.x[0])->style.tightening_factor = float_arg(s, 0);
    }, "v@:f");
    method(mps, "setUsesDefaultHyphenation:", [paragraph_state](CpuState& s, GuestMemory&) {
        paragraph_state(s.x[0])->style.uses_default_hyphenation = bool_arg(s, 2);
    }, "v@:B");
    method(mps, "setTextLists:", [quartz, paragraph_state](CpuState& s, GuestMemory&) {
        const auto lists = s.x[2] ? quartz->cf.array_values(s.x[2]) : std::vector<Address>{};
        for (const auto list : lists) quartz->retain(list);
        for (const auto list : std::exchange(paragraph_state(s.x[0])->lists, lists)) quartz->release(list);
    }, "v@:@");
    method(mps, "setTextBlocks:", [quartz](CpuState& s, GuestMemory&) {
        if (s.x[2] && !quartz->cf.array_values(s.x[2]).empty()) unsupported("-[NSMutableParagraphStyle setTextBlocks:] (text blocks and tables are not laid out)");
    }, "v@:@");

    install_text_tabs(q);
}

} // namespace alchemy::gui::quartz2d
