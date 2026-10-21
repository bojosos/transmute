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
});

} // namespace
