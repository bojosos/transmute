#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"
#include "alchemy/darwin_frameworks.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

// libiconv (the GNU interface as shipped in /usr/lib/libiconv.2.dylib):
// iconv_open/iconv/iconv_close (and the lib-prefixed names) between UTF-8,
// UTF-16, UCS-2, UTF-32, ASCII and Latin-1, with //TRANSLIT and //IGNORE;
// iconvctl (TRIVIALP and the transliterate / discard-ilseq switches),
// iconvlist and iconv_canonicalize over the same names.
// Moved from darwin/identities/frameworks.cpp (the headless framework set);
// registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

constexpr std::string_view iconv_library = darwin_framework_libraries[15];
constexpr std::string_view charset_library = darwin_framework_libraries[52];
static_assert(charset_library == "/usr/lib/libcharset.1.dylib");

// <sys/errno.h>
constexpr std::uint32_t e2big = 7, ebadf = 9, einval = 22, eilseq = 92;
// <iconv.h> iconvctl requests.
constexpr std::uint64_t iconv_trivialp = 0, iconv_get_transliterate = 1, iconv_set_transliterate = 2,
    iconv_get_discard_ilseq = 3, iconv_set_discard_ilseq = 4;

// ---- iconv (GNU libiconv interface as shipped in /usr/lib/libiconv.2.dylib) ----
enum class Form { utf8, utf16, ucs2, utf32, ascii, latin1 };
struct Charset { Form form{Form::utf8}; bool big{true}, detect{}, mark{}; };
bool operator==(const Charset& a, const Charset& b) {
    return a.form == b.form && a.big == b.big && a.detect == b.detect && a.mark == b.mark;
}
// The charsets and their names (upper case), the canonical one first: iconv_open, iconvlist and iconv_canonicalize.
struct CharsetNames { Charset value; std::vector<std::string_view> names; };
const std::vector<CharsetNames>& charsets() {
    static const std::vector<CharsetNames> table{
        {{Form::utf8}, {"UTF-8", "UTF8"}},
        {{Form::utf16, true, true, true}, {"UTF-16"}},
        {{Form::utf16, true}, {"UTF-16BE"}},
        {{Form::utf16, false}, {"UTF-16LE"}},
        {{Form::ucs2, true, true, false}, {"UCS-2", "ISO-10646-UCS-2", "CSUNICODE"}},
        {{Form::ucs2, true}, {"UCS-2BE", "UNICODEBIG", "UNICODE-1-1", "CSUNICODE11"}},
        {{Form::ucs2, false}, {"UCS-2LE", "UNICODELITTLE", "UCS-2-INTERNAL"}},
        {{Form::utf32, true, true, true}, {"UTF-32"}},
        {{Form::utf32, true}, {"UTF-32BE"}},
        // WCHAR_T is native 32-bit.
        {{Form::utf32, false}, {"UTF-32LE", "UCS-4-INTERNAL", "WCHAR_T"}},
        {{Form::utf32, true, true, false}, {"UCS-4", "ISO-10646-UCS-4", "CSUCS4"}},
        {{Form::utf32, true}, {"UCS-4BE"}},
        {{Form::utf32, false}, {"UCS-4LE"}},
        {{Form::ascii}, {"ASCII", "US-ASCII", "ANSI_X3.4-1968", "ANSI_X3.4-1986", "ISO646-US", "ISO_646.IRV:1991",
                         "US", "IBM367", "CP367", "CSASCII", "ISO-IR-6"}},
        {{Form::latin1}, {"ISO-8859-1", "ISO8859-1", "ISO_8859-1", "ISO_8859-1:1987", "LATIN1", "L1", "IBM819", "CP819",
                          "CSISOLATIN1", "ISO-IR-100"}},
    };
    return table;
}
const CharsetNames* charset_names(std::string name) {
    for (auto& character : name) if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
    // Darwin's locale_charset() is always UTF-8.
    if (name.empty() || name == "CHAR") name = "UTF-8";
    for (const auto& entry : charsets())
        if (std::find(entry.names.begin(), entry.names.end(), name) != entry.names.end()) return &entry;
    return nullptr;
}
std::optional<Charset> charset(std::string name) {
    const auto found = charset_names(std::move(name));
    return found ? std::optional<Charset>(found->value) : std::nullopt;
}
struct Converter {
    Charset from, to;
    bool transliterate{}, ignore{}, input_big{true}, input_started{}, output_started{};
};
// Returns the charset part and records //TRANSLIT and //IGNORE suffixes.
std::string split_flags(std::string_view code, bool* transliterate, bool* ignore) {
    const auto at = code.find("//");
    std::string name(code.substr(0, at));
    if (at == std::string_view::npos) return name;
    auto flags = std::string(code.substr(at));
    for (auto& character : flags) if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
    if (transliterate && flags.find("//TRANSLIT") != std::string::npos) *transliterate = true;
    if (ignore && flags.find("//IGNORE") != std::string::npos) *ignore = true;
    return name;
}
unsigned unit_size(Form form) { return form == Form::utf16 || form == Form::ucs2 ? 2 : form == Form::utf32 ? 4 : 1; }
std::uint32_t load(std::span<const std::uint8_t> bytes, unsigned size, bool big) {
    std::uint32_t value{};
    for (unsigned index = 0; index < size; ++index)
        value |= static_cast<std::uint32_t>(bytes[index]) << (8 * (big ? size - 1 - index : index));
    return value;
}
void store(std::vector<std::uint8_t>& out, std::uint32_t value, unsigned size, bool big) {
    for (unsigned index = 0; index < size; ++index)
        out.push_back(static_cast<std::uint8_t>(value >> (8 * (big ? size - 1 - index : index))));
}
enum class Decoded { ok, invalid, incomplete };
Decoded decode(std::span<const std::uint8_t> in, Form form, bool big, std::uint32_t& scalar, std::size_t& used) {
    switch (form) {
    case Form::ascii: used = 1; scalar = in[0]; return scalar < 0x80 ? Decoded::ok : Decoded::invalid;
    case Form::latin1: used = 1; scalar = in[0]; return Decoded::ok;
    case Form::utf8: {
        const auto first = in[0];
        unsigned more; std::uint32_t minimum;
        if (first < 0x80) { used = 1; scalar = first; return Decoded::ok; }
        if (first >= 0xc2 && first <= 0xdf) { more = 1; scalar = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { more = 2; scalar = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { more = 3; scalar = first & 0x07; minimum = 0x10000; }
        else return Decoded::invalid;
        for (unsigned index = 1; index <= more; ++index) {
            if (index >= in.size()) return Decoded::incomplete;
            if ((in[index] & 0xc0) != 0x80) return Decoded::invalid;
            scalar = (scalar << 6) | (in[index] & 0x3f);
        }
        used = more + 1;
        return scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff) ? Decoded::invalid : Decoded::ok;
    }
    case Form::utf16: case Form::ucs2: {
        if (in.size() < 2) return Decoded::incomplete;
        const auto first = load(in, 2, big);
        if (first >= 0xdc00 && first <= 0xdfff) return Decoded::invalid;
        if (first < 0xd800 || first > 0xdbff) { used = 2; scalar = first; return Decoded::ok; }
        if (form == Form::ucs2) return Decoded::invalid;
        if (in.size() < 4) return Decoded::incomplete;
        const auto second = load(in.subspan(2), 2, big);
        if (second < 0xdc00 || second > 0xdfff) return Decoded::invalid;
        used = 4; scalar = 0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00);
        return Decoded::ok;
    }
    case Form::utf32:
        if (in.size() < 4) return Decoded::incomplete;
        used = 4; scalar = load(in, 4, big);
        return scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff) ? Decoded::invalid : Decoded::ok;
    }
    return Decoded::invalid;
}
bool encode(std::uint32_t scalar, const Charset& to, std::vector<std::uint8_t>& out) {
    switch (to.form) {
    case Form::ascii: if (scalar > 0x7f) return false; out.push_back(static_cast<std::uint8_t>(scalar)); return true;
    case Form::latin1: if (scalar > 0xff) return false; out.push_back(static_cast<std::uint8_t>(scalar)); return true;
    case Form::utf8:
        if (scalar < 0x80) out.push_back(static_cast<std::uint8_t>(scalar));
        else if (scalar < 0x800) { out.push_back(static_cast<std::uint8_t>(0xc0 | scalar >> 6)); out.push_back(static_cast<std::uint8_t>(0x80 | (scalar & 0x3f))); }
        else if (scalar < 0x10000) { out.push_back(static_cast<std::uint8_t>(0xe0 | scalar >> 12)); out.push_back(static_cast<std::uint8_t>(0x80 | ((scalar >> 6) & 0x3f))); out.push_back(static_cast<std::uint8_t>(0x80 | (scalar & 0x3f))); }
        else { out.push_back(static_cast<std::uint8_t>(0xf0 | scalar >> 18)); out.push_back(static_cast<std::uint8_t>(0x80 | ((scalar >> 12) & 0x3f))); out.push_back(static_cast<std::uint8_t>(0x80 | ((scalar >> 6) & 0x3f))); out.push_back(static_cast<std::uint8_t>(0x80 | (scalar & 0x3f))); }
        return true;
    case Form::ucs2: if (scalar > 0xffff) return false; store(out, scalar, 2, to.big); return true;
    case Form::utf16:
        if (scalar < 0x10000) store(out, scalar, 2, to.big);
        else { store(out, 0xd800 + ((scalar - 0x10000) >> 10), 2, to.big); store(out, 0xdc00 + ((scalar - 0x10000) & 0x3ff), 2, to.big); }
        return true;
    case Form::utf32: store(out, scalar, 4, to.big); return true;
    }
    return false;
}
struct IconvResult { std::size_t consumed{}, irreversible{}; std::vector<std::uint8_t> output; std::uint32_t error{}; };
// One call's conversion. Each character is decoded and encoded whole, so an
// error leaves the input at the start of the failing character (POSIX iconv).
IconvResult convert(Converter& state, std::span<const std::uint8_t> in, std::uint64_t room) {
    IconvResult result;
    std::vector<std::uint8_t> piece;
    while (result.consumed < in.size()) {
        const auto rest = in.subspan(result.consumed);
        const auto width = unit_size(state.from.form);
        if (!state.input_started) {
            state.input_big = state.from.big;
            if (state.from.detect) {
                if (rest.size() < width) { result.error = einval; break; }
                const auto mark = load(rest, width, true);
                const bool big = width == 2 ? mark == 0xfeff : mark == 0x0000feffU;
                const bool little = width == 2 ? mark == 0xfffe : mark == 0xfffe0000U;
                state.input_started = true;
                if (big || little) { state.input_big = big; result.consumed += width; continue; }
            }
            state.input_started = true;
        }
        std::uint32_t scalar{};
        std::size_t used{};
        const auto decoded = decode(rest, state.from.form, state.input_big, scalar, used);
        if (decoded == Decoded::incomplete) { result.error = einval; break; }
        if (decoded == Decoded::invalid) { result.error = eilseq; break; }
        piece.clear();
        if (state.to.mark && !state.output_started) store(piece, 0xfeff, unit_size(state.to.form), true);
        const auto mark_size = piece.size();
        if (!encode(scalar, state.to, piece)) {
            if (state.transliterate) { piece.resize(mark_size); encode('?', state.to, piece); ++result.irreversible; }
            else if (state.ignore) { ++result.irreversible; result.consumed += used; continue; }
            else { result.error = eilseq; break; }
        }
        if (piece.size() > room - result.output.size()) { result.error = e2big; break; }
        result.output.insert(result.output.end(), piece.begin(), piece.end());
        state.output_started = true;
        result.consumed += used;
    }
    return result;
}

class Iconv final : public Provider {
public:
    using Provider::Provider;
    std::mutex iconv_mutex;
    std::map<Address, std::shared_ptr<Converter>> converters;
    // Charset names as guest strings, made once and kept (iconv_canonicalize returns them).
    std::map<std::string_view, Address> name_texts;
    Address name_text(std::string_view name) {
        std::lock_guard lock(iconv_mutex);
        auto& text = name_texts[name];
        if (!text) {
            text = context.allocate_guest(name.size() + 1);
            memory().write_bytes(text, std::span(reinterpret_cast<const std::uint8_t*>(name.data()), name.size()));
            memory().write8(text + name.size(), 0);
        }
        return text;
    }
    std::shared_ptr<Converter> converter(Address handle) {
        std::lock_guard lock(iconv_mutex);
        const auto found = converters.find(handle);
        return found == converters.end() ? nullptr : found->second;
    }

    void install() {
        const auto open = [this](CpuState& s, GuestMemory& m) {
            if (!s.x[0] || !s.x[1]) { context.set_errno(einval); s.x[0] = UINT64_MAX; return; }
            auto state = std::make_shared<Converter>();
            const auto to = charset(split_flags(guest_text(m, s.x[0], 256), &state->transliterate, &state->ignore));
            const auto from = charset(split_flags(guest_text(m, s.x[1], 256), nullptr, nullptr));
            if (!to || !from) { context.set_errno(einval); s.x[0] = UINT64_MAX; return; }
            state->from = *from; state->to = *to;
            const auto handle = context.allocate_guest(16);
            std::lock_guard lock(iconv_mutex);
            converters.emplace(handle, std::move(state));
            s.x[0] = handle;
        };
        const auto close = [this](CpuState& s, GuestMemory&) {
            {
                std::lock_guard lock(iconv_mutex);
                if (converters.erase(s.x[0]) == 0) { context.set_errno(ebadf); s.x[0] = UINT64_MAX; return; }
            }
            context.release_guest(s.x[0]);
            s.x[0] = 0;
        };
        const auto run = [this](CpuState& s, GuestMemory& m) {
            const auto state = converter(s.x[0]);
            if (!state) { context.set_errno(ebadf); s.x[0] = UINT64_MAX; return; }
            const auto inbuf = s.x[1], inleft = s.x[2], outbuf = s.x[3], outleft = s.x[4];
            if (!inbuf || !m.read64(inbuf)) {
                // Return to the initial shift state. No charset here has a
                // reset sequence; byte-order marks are written again.
                state->input_started = false; state->output_started = false;
                s.x[0] = 0; return;
            }
            if (!inleft || !outbuf || !outleft) { context.set_errno(einval); s.x[0] = UINT64_MAX; return; }
            const auto input = m.read64(inbuf), size = m.read64(inleft);
            const auto output = m.read64(outbuf), room = m.read64(outleft);
            if (size > (256ULL << 20)) throw std::runtime_error("Alchemy iconv: input exceeds 256 MiB");
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            if (size) m.read_bytes(input, bytes);
            const auto result = convert(*state, bytes, room);
            if (!result.output.empty()) {
                if (!output) { context.set_errno(einval); s.x[0] = UINT64_MAX; return; }
                m.write_bytes(output, result.output);
            }
            m.write64(inbuf, input + result.consumed); m.write64(inleft, size - result.consumed);
            m.write64(outbuf, output + result.output.size()); m.write64(outleft, room - result.output.size());
            if (result.error) { context.set_errno(result.error); s.x[0] = UINT64_MAX; return; }
            s.x[0] = result.irreversible;
        };
        // int iconvctl(iconv_t, int request, void* argument): ICONV_TRIVIALP (the conversion copies bytes as
        // they are) and the //TRANSLIT and //IGNORE switches, through an int. ICONV_SET_HOOKS / _FALLBACKS (and
        // anything else): EINVAL, as this converter never calls back.
        const auto control = [this](CpuState& s, GuestMemory& m) {
            const auto state = converter(s.x[0]);
            if (!state) { context.set_errno(ebadf); s.x[0] = UINT64_MAX; return; }
            const auto request = s.x[1], argument = s.x[2];
            if (request > iconv_set_discard_ilseq || !argument) { context.set_errno(einval); s.x[0] = UINT64_MAX; return; }
            switch (request) {
            case iconv_trivialp: m.write32(argument, state->from == state->to && !state->from.detect && !state->from.mark ? 1 : 0); break;
            case iconv_get_transliterate: m.write32(argument, state->transliterate ? 1 : 0); break;
            case iconv_set_transliterate: state->transliterate = m.read32(argument) != 0; break;
            case iconv_get_discard_ilseq: m.write32(argument, state->ignore ? 1 : 0); break;
            default: state->ignore = m.read32(argument) != 0; break;
            }
            s.x[0] = 0;
        };
        // const char* iconv_canonicalize(const char* name): the charset's canonical name (as iconvlist gives it
        // first); an unknown name comes back as it is.
        const auto canonicalize = [this](CpuState& s, GuestMemory& m) {
            if (!s.x[0]) return;
            const auto found = charset_names(guest_text(m, s.x[0], 256));
            if (!found) return;
            s.x[0] = name_text(found->names.front());
        };
        // void iconvlist(int (*do_one)(unsigned count, const char* const* names, void* data), void* data): each
        // charset's names, until do_one returns non-zero.
        const auto list = [this](CpuState& s, GuestMemory& m) {
            const auto callback = s.x[0], data = s.x[1];
            s.x[0] = 0;
            if (!callback) return;
            for (const auto& entry : charsets()) {
                const auto names = context.allocate_guest(entry.names.size() * 8);
                for (std::size_t index = 0; index < entry.names.size(); ++index) m.write64(names + index * 8, name_text(entry.names[index]));
                Address stop{};
                try {
                    const std::array<Address, 3> arguments{entry.names.size(), names, data};
                    stop = EngineAdapter(context.calling_engine()).call(callback, arguments) & 0xffffffffU;
                } catch (...) { context.release_guest(names); throw; }
                context.release_guest(names);
                if (stop) return;
            }
        };
        for (const auto* prefix : {"_", "_lib"}) {
            function(iconv_library, std::string(prefix) + "iconv_open", open);
            function(iconv_library, std::string(prefix) + "iconv_close", close);
            function(iconv_library, std::string(prefix) + "iconv", run);
            function(iconv_library, std::string(prefix) + "iconvctl", control);
            function(iconv_library, std::string(prefix) + "iconvlist", list);
        }
        function(iconv_library, "_iconv_canonicalize", canonicalize);
        // libcharset: const char* locale_charset(void), the locale's character encoding, which GNU
        // libcharset answers "UTF-8" on Darwin whatever the locale. The relocation prefixes
        // (libcharset_set_relocation_prefix, libiconv_set_relocation_prefix) move where the
        // libraries look for their data files; these implementations read none, so they change nothing.
        const auto charset = name_text("UTF-8");
        function(charset_library, "_locale_charset", [charset](CpuState& s, GuestMemory&) { s.x[0] = charset; });
        function(charset_library, "_libcharset_set_relocation_prefix", [](CpuState&, GuestMemory&) {});
        function(iconv_library, "_libiconv_set_relocation_prefix", [](CpuState&, GuestMemory&) {});
    }
};

} // namespace

void install_libiconv(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<Iconv>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
