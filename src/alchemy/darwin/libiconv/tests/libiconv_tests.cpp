// libiconv (src/alchemy/darwin/libiconv/): UTF-8 to UTF-16LE, and an unknown
// character set.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view iconv_library = "/usr/lib/libiconv.2.dylib";

const Case libiconv_case("libiconv", [](Fixture& f) {
    const auto handle = f.call(iconv_library, "_iconv_open", {f.text("UTF-16LE"), f.text("UTF-8")});
    require(handle != UINT64_MAX, "iconv_open UTF-8 -> UTF-16LE");
    const auto input = f.text("A"), output = f.block(8);
    const auto in_pointer = f.block(8), in_left = f.block(8), out_pointer = f.block(8), out_left = f.block(8);
    f.engine.write64(in_pointer, input);
    f.engine.write64(in_left, 1);
    f.engine.write64(out_pointer, output);
    f.engine.write64(out_left, 8);
    require(f.call(iconv_library, "_iconv", {handle, in_pointer, in_left, out_pointer, out_left}) == 0, "iconv converts");
    require(f.memory().read8(output) == 'A' && f.memory().read8(output + 1) == 0 && f.engine.read64(out_left) == 6, "UTF-16LE 'A'");
    require(f.call(iconv_library, "_iconv_close", {handle}) == 0, "iconv_close");
    require(f.call(iconv_library, "_iconv_open", {f.text("EBCDIC-US"), f.text("UTF-8")}) == UINT64_MAX, "unknown charsets fail");

    // iconvctl: TRIVIALP, the transliterate and discard switches; hooks are refused.
    const auto flag = f.block(4);
    const auto same = f.call(iconv_library, "_iconv_open", {f.text("utf8"), f.text("UTF-8")});
    require(f.call(iconv_library, "_iconvctl", {same, 0, flag}) == 0 && f.memory().read32(flag) == 1, "ICONV_TRIVIALP for UTF-8 to UTF-8");
    const auto latin = f.call(iconv_library, "_iconv_open", {f.text("LATIN1//TRANSLIT"), f.text("UTF-8")});
    require(f.call(iconv_library, "_iconvctl", {latin, 0, flag}) == 0 && f.memory().read32(flag) == 0 &&
            f.call(iconv_library, "_iconvctl", {latin, 1, flag}) == 0 && f.memory().read32(flag) == 1, "TRIVIALP no; //TRANSLIT is on");
    f.memory().write32(flag, 0);
    require(f.call(iconv_library, "_iconvctl", {latin, 2, flag}) == 0 && f.call(iconv_library, "_iconvctl", {latin, 1, flag}) == 0 &&
            f.memory().read32(flag) == 0, "ICONV_SET_TRANSLITERATE off");
    f.memory().write32(flag, 1);
    require(f.call(iconv_library, "_iconvctl", {latin, 4, flag}) == 0 && f.call(iconv_library, "_iconvctl", {latin, 3, flag}) == 0 &&
            f.memory().read32(flag) == 1, "ICONV_SET_DISCARD_ILSEQ on");
    require(static_cast<std::int32_t>(f.call(iconv_library, "_iconvctl", {latin, 5, flag})) == -1, "ICONV_SET_HOOKS is refused");
    // With discard on and transliteration off, a character Latin-1 lacks is dropped.
    const auto euro = f.text("a\xe2\x82\xac" "b"), latin_out = f.block(8);
    f.engine.write64(in_pointer, euro); f.engine.write64(in_left, 5);
    f.engine.write64(out_pointer, latin_out); f.engine.write64(out_left, 8);
    require(f.call(iconv_library, "_iconv", {latin, in_pointer, in_left, out_pointer, out_left}) == 1 &&
            f.memory().read8(latin_out) == 'a' && f.memory().read8(latin_out + 1) == 'b' && f.engine.read64(out_left) == 6, "discarded");
    f.call(iconv_library, "_iconv_close", {same});
    f.call(iconv_library, "_iconv_close", {latin});

    // iconv_canonicalize and iconvlist.
    require(f.read_text(f.call(iconv_library, "_iconv_canonicalize", {f.text("latin1")})) == "ISO-8859-1" &&
            f.read_text(f.call(iconv_library, "_iconv_canonicalize", {f.text("utf8")})) == "UTF-8", "iconv_canonicalize");
    const auto unknown = f.text("EBCDIC-US");
    require(f.call(iconv_library, "_iconv_canonicalize", {unknown}) == unknown, "an unknown name comes back");
    static std::vector<std::string> firsts;
    static unsigned calls{};
    firsts.clear(); calls = 0;
    static Fixture* fixture = &f;
    const auto each = f.process.install_function("__fill_test_iconvlist", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        firsts.push_back(fixture->read_text(m.read64(s.x[1])));
        ++calls;
        s.x[0] = s.x[2] == 1 && calls == 3 ? 1 : 0;   // data 1: stop after three
    });
    f.call(iconv_library, "_iconvlist", {each, 0});
    require(calls == 15 && firsts.front() == "UTF-8" && firsts.back() == "ISO-8859-1", "iconvlist visits every charset");
    calls = 0;
    f.call(iconv_library, "_iconvlist", {each, 1});
    require(calls == 3, "iconvlist stops when do_one returns non-zero");
});

} // namespace
