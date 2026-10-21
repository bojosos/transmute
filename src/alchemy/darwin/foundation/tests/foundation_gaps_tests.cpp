// Foundation gap tests (property_list_serialization.cpp), split out of the
// fwgaps tests: the HFS type-code helpers, NSKeyValueChangeNewKey and
// NSPropertyListSerialization.
#include "alchemy/gui/tests/fill_fixture.hpp"

#include <array>

namespace {
using namespace fill_test;

constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view foundation = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";

Address string(Fixture& f, std::string_view text) { return f.cf.string(text); }

void foundation_gaps(Fixture& f) {
    require(f.call(foundation, "_NSHFSTypeCodeFromFileType", {string(f, "'TEXT'")}) == 0x54455854, "NSHFSTypeCodeFromFileType");
    require(f.call(foundation, "_NSHFSTypeCodeFromFileType", {string(f, "TEXT")}) == 0, "malformed HFS type");
    require(f.string_constant(foundation, "_NSKeyValueChangeNewKey") == "new", "NSKeyValueChangeNewKey");
    // NSPropertyListSerialization (binary format 200).
    const auto plist = f.cf.array(std::vector<Address>{string(f, "x")});
    const auto data = f.send(f.cls("NSPropertyListSerialization"), "dataWithPropertyList:format:options:error:", {plist, 200, 0, 0});
    require(data && f.call(cf, "_CFDataGetLength", {data}) > 8, "dataWithPropertyList");
    const auto back = f.send(f.cls("NSPropertyListSerialization"), "propertyListWithData:options:format:error:", {data, 0, 0, 0});
    require(back && f.cf.utf8(f.cf.element(back, 0)) == "x", "propertyListWithData");
}

} // namespace

int main() {
    return run("foundation gaps", {{"foundation gaps", foundation_gaps}});
}
