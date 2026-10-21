#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/foundation/byte_count.hpp"

#include <cctype>
#include <map>
#include <mutex>
#include <tuple>

// NSByteCountFormatter (byte_count.cpp), NSListFormatter (en_US: "a, b,
// and c"), -[NSFormatter formattingContext] (kept per formatter; the English
// strings these formatters make are already capitalized for any context)
// and -[NSLocale localizedStringForLanguageCode:] (English language names for
// ISO 639-1 codes; nil for codes outside the table, as for unknown codes).
namespace alchemy::gui::completion_kit::data {
namespace {

struct ByteCountState final : ObjectPayload {
    byte_count::Options options;
    std::int64_t context{};
};
struct ListState final : ObjectPayload {
    Address locale{};          // +1
    Address item_formatter{};  // +1
};

std::u16string join(const std::vector<std::u16string>& items) {
    if (items.empty()) return {};
    if (items.size() == 1) return items[0];
    if (items.size() == 2) return items[0] + u" and " + items[1];
    std::u16string out;
    for (std::size_t i = 0; i + 1 < items.size(); ++i) out += items[i] + u", ";
    return out + u"and " + items.back();
}

const std::map<std::string, const char*>& language_names() {
    static const std::map<std::string, const char*> names = {
        {"af", "Afrikaans"}, {"am", "Amharic"}, {"ar", "Arabic"}, {"az", "Azerbaijani"}, {"be", "Belarusian"}, {"bg", "Bulgarian"}, {"bn", "Bangla"},
        {"bs", "Bosnian"}, {"ca", "Catalan"}, {"cs", "Czech"}, {"cy", "Welsh"}, {"da", "Danish"}, {"de", "German"}, {"el", "Greek"}, {"en", "English"},
        {"eo", "Esperanto"}, {"es", "Spanish"}, {"et", "Estonian"}, {"eu", "Basque"}, {"fa", "Persian"}, {"fi", "Finnish"}, {"fil", "Filipino"},
        {"fr", "French"}, {"ga", "Irish"}, {"gl", "Galician"}, {"gu", "Gujarati"}, {"he", "Hebrew"}, {"hi", "Hindi"}, {"hr", "Croatian"},
        {"hu", "Hungarian"}, {"hy", "Armenian"}, {"id", "Indonesian"}, {"is", "Icelandic"}, {"it", "Italian"}, {"ja", "Japanese"}, {"ka", "Georgian"},
        {"kk", "Kazakh"}, {"km", "Khmer"}, {"kn", "Kannada"}, {"ko", "Korean"}, {"lo", "Lao"}, {"lt", "Lithuanian"}, {"lv", "Latvian"},
        {"mk", "Macedonian"}, {"ml", "Malayalam"}, {"mn", "Mongolian"}, {"mr", "Marathi"}, {"ms", "Malay"}, {"mt", "Maltese"}, {"my", "Burmese"},
        {"nb", "Norwegian Bokmal"}, {"ne", "Nepali"}, {"nl", "Dutch"}, {"nn", "Norwegian Nynorsk"}, {"no", "Norwegian"}, {"pa", "Punjabi"},
        {"pl", "Polish"}, {"pt", "Portuguese"}, {"ro", "Romanian"}, {"ru", "Russian"}, {"si", "Sinhala"}, {"sk", "Slovak"}, {"sl", "Slovenian"},
        {"sq", "Albanian"}, {"sr", "Serbian"}, {"sv", "Swedish"}, {"sw", "Swahili"}, {"ta", "Tamil"}, {"te", "Telugu"}, {"th", "Thai"},
        {"tr", "Turkish"}, {"uk", "Ukrainian"}, {"ur", "Urdu"}, {"uz", "Uzbek"}, {"vi", "Vietnamese"}, {"zh", "Chinese"}, {"zu", "Zulu"},
    };
    return names;
}

} // namespace

void install_formatters_extra(Support& support) {
    auto g = std::make_shared<Guest>(Guest{support});
    auto* sp = &support;
    const auto formatter = support.objc.find_class("NSFormatter") ? support.objc.find_class("NSFormatter") : support.declare_class("NSFormatter", "NSObject", foundation_library);

    // ---- -[NSFormatter formattingContext] ----
    struct Contexts { std::mutex mutex; std::map<Address, std::int64_t> values; };
    auto contexts = std::make_shared<Contexts>();
    fill_method(support, formatter, "formattingContext", [contexts](CpuState& s, GuestMemory&) {
        std::lock_guard lock(contexts->mutex);
        const auto found = contexts->values.find(s.x[0]);
        s.x[0] = found == contexts->values.end() ? 0 : static_cast<Address>(found->second);
    }, "q@:");
    fill_method(support, formatter, "setFormattingContext:", [contexts](CpuState& s, GuestMemory&) {
        if (s.x[2] > 5) fail("NSInvalidArgumentException: NSFormattingContext " + std::to_string(s.x[2]));
        std::lock_guard lock(contexts->mutex);
        contexts->values[s.x[0]] = static_cast<std::int64_t>(s.x[2]);
    }, "v@:q");

    // ---- NSByteCountFormatter ----
    const auto bc = support.declare_class("NSByteCountFormatter", "NSFormatter", foundation_library);
    const auto options = [sp](Address self) { return sp->objc.host_state<ByteCountState>(self); };
    support.method(bc, "init", [options](CpuState& s, GuestMemory&) { options(s.x[0]); }, "@@:");
    support.method(bc, "stringFromByteCount:", [g, options](CpuState& s, GuestMemory&) {
        s.x[0] = g->auto_string(byte_count::format(static_cast<std::int64_t>(s.x[2]), options(s.x[0])->options));
    }, "@@:q");
    support.method(bc, "stringFromByteCount:countStyle:", [g](CpuState& s, GuestMemory&) {
        if (s.x[3] > 3) fail("NSInvalidArgumentException: NSByteCountFormatterCountStyle " + std::to_string(s.x[3]));
        byte_count::Options o;
        o.style = static_cast<byte_count::Style>(s.x[3]);
        s.x[0] = g->auto_string(byte_count::format(static_cast<std::int64_t>(s.x[2]), o));
    }, "@@:qq", true);
    support.method(bc, "stringForObjectValue:", [g, options](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !g->s.cf.number_value(s.x[2])) { s.x[0] = 0; return; }
        s.x[0] = g->auto_string(byte_count::format(static_cast<std::int64_t>(g->s.send(s.x[2], "longLongValue").x0), options(s.x[0])->options));
    }, "@@:@");
    support.method(bc, "countStyle", [options](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(options(s.x[0])->options.style); }, "q@:");
    support.method(bc, "setCountStyle:", [options](CpuState& s, GuestMemory&) {
        if (s.x[2] > 3) fail("NSInvalidArgumentException: NSByteCountFormatterCountStyle " + std::to_string(s.x[2]));
        options(s.x[0])->options.style = static_cast<byte_count::Style>(s.x[2]);
    }, "v@:q");
    support.method(bc, "allowedUnits", [options](CpuState& s, GuestMemory&) { s.x[0] = options(s.x[0])->options.allowed; }, "Q@:");
    support.method(bc, "setAllowedUnits:", [options](CpuState& s, GuestMemory&) { options(s.x[0])->options.allowed = s.x[2]; }, "v@:Q");
    struct Flag { const char* getter; const char* setter; bool byte_count::Options::*field; };
    for (const auto& [getter, setter, field] : {Flag{"includesUnit", "setIncludesUnit:", &byte_count::Options::includes_unit},
                                               Flag{"includesCount", "setIncludesCount:", &byte_count::Options::includes_count},
                                               Flag{"includesActualByteCount", "setIncludesActualByteCount:", &byte_count::Options::includes_actual},
                                               Flag{"isAdaptive", "setAdaptive:", &byte_count::Options::adaptive},
                                               Flag{"allowsNonnumericFormatting", "setAllowsNonnumericFormatting:", &byte_count::Options::nonnumeric},
                                               Flag{"zeroPadsFractionDigits", "setZeroPadsFractionDigits:", &byte_count::Options::zero_pads}}) {
        const auto member = field;
        support.method(bc, getter, [options, member](CpuState& s, GuestMemory&) { s.x[0] = options(s.x[0])->options.*member; }, "B@:");
        support.method(bc, setter, [options, member](CpuState& s, GuestMemory&) { options(s.x[0])->options.*member = arg_bool(s.x[2]); }, "v@:B");
    }
    support.method(bc, "formattingContext", [options](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(options(s.x[0])->context); }, "q@:");
    support.method(bc, "setFormattingContext:", [options](CpuState& s, GuestMemory&) { options(s.x[0])->context = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    support.method(bc, "copyWithZone:", [sp, options](CpuState& s, GuestMemory&) {
        const auto copy = sp->objc.allocate(sp->objc.object_class(s.x[0]));
        *options(copy) = *options(s.x[0]);
        s.x[0] = copy;
    }, "@@:^v");

    // ---- NSListFormatter ----
    const auto lf = support.declare_class("NSListFormatter", "NSFormatter", foundation_library);
    const auto strings_of = [g](Address list, Address item_formatter) {
        std::vector<std::u16string> items;
        for (const auto item : g->elements(list)) {
            Address text = item;
            if (item_formatter) text = g->s.send_id(item_formatter, "stringForObjectValue:", {item});
            else if (!g->is_string(item)) text = g->s.send_id(item, "description");
            if (text) items.push_back(g->text(text));
        }
        return items;
    };
    support.method(lf, "localizedStringByJoiningStrings:", [g, strings_of](CpuState& s, GuestMemory&) { s.x[0] = g->auto_string(join(strings_of(s.x[2], 0))); }, "@@:@", true);
    support.method(lf, "init", [sp](CpuState& s, GuestMemory&) { sp->objc.host_state<ListState>(s.x[0]); }, "@@:");
    support.method(lf, "stringFromItems:", [g, sp, strings_of](CpuState& s, GuestMemory&) {
        const auto st = sp->objc.host_state<ListState>(s.x[0]);
        s.x[0] = g->auto_string(join(strings_of(s.x[2], st->item_formatter)));
    }, "@@:@");
    support.method(lf, "stringForObjectValue:", [g, sp, strings_of](CpuState& s, GuestMemory&) {
        if (!s.x[2] || g->s.cf.kind(s.x[2]) != ObjectKind::array) { s.x[0] = 0; return; }
        const auto st = sp->objc.host_state<ListState>(s.x[0]);
        s.x[0] = g->auto_string(join(strings_of(s.x[2], st->item_formatter)));
    }, "@@:@");
    for (const auto& [getter, setter, field] : {std::tuple{"locale", "setLocale:", &ListState::locale}, std::tuple{"itemFormatter", "setItemFormatter:", &ListState::item_formatter}}) {
        const auto member = field;
        support.method(lf, getter, [sp, member](CpuState& s, GuestMemory&) { s.x[0] = (*sp->objc.host_state<ListState>(s.x[0])).*member; }, "@@:");
        support.method(lf, setter, [sp, g, member](CpuState& s, GuestMemory&) {
            auto& slot = (*sp->objc.host_state<ListState>(s.x[0])).*member;
            g->release(std::exchange(slot, g->retain(s.x[2])));
        }, "v@:@");
    }
    support.method(lf, "dealloc", [sp, lf](CpuState& s, GuestMemory&) {
        if (const auto st = sp->objc.find_host_state<ListState>(s.x[0]))
            for (auto* held : {&st->locale, &st->item_formatter}) if (*held) sp->send_id(std::exchange(*held, 0), "release");
        static_cast<void>(sp->objc.send_super(s.x[0], lf, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- -[NSLocale localizedStringForLanguageCode:] ----
    if (const auto locale = support.objc.find_class("NSLocale")) {
        fill_method(support, locale, "localizedStringForLanguageCode:", [g](CpuState& s, GuestMemory&) {
            auto code = g->utf8(s.x[2]);
            for (auto& c : code) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const auto cut = code.find_first_of("-_");
            if (cut != std::string::npos) code.resize(cut);
            const auto& names = language_names();
            const auto found = names.find(code);
            s.x[0] = found == names.end() ? 0 : g->auto_string(std::string_view(found->second));
        }, "@@:@");
    }
}

} // namespace alchemy::gui::completion_kit::data
