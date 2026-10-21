// CoreServices (coreservices.cpp): Unicode collation, UTType functions and
// Launch Services answers as a renderer plug-in's images call them. Split out of
// the export-fill tests.
#include "alchemy/darwin/kit/export_fill/tests/export_fill_test_kit.hpp"

namespace {

void test_guest_coreservices(Fixture& f) {
    const auto cs = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices";
    // Collation.
    const auto locale_out = f.process.allocate_guest(8), collator_out = f.process.allocate_guest(8);
    require(f.call(cs, "_LocaleRefFromLocaleString", {f.text("en_US"), locale_out}) == 0, "LocaleRefFromLocaleString");
    require(f.call(cs, "_UCCreateCollator", {f.engine.read64(locale_out), 0, 1U << 3, collator_out}) == 0, "UCCreateCollator");
    const auto collator = f.engine.read64(collator_out);
    const auto utf16 = [&](std::string_view text) {
        const auto address = f.process.allocate_guest(text.size() * 2 + 2);
        std::vector<std::uint8_t> units;
        for (const auto c : text) { units.push_back(static_cast<std::uint8_t>(c)); units.push_back(0); }
        f.engine.write_bytes(address, units);
        return address;
    };
    const auto equivalent = f.process.allocate_guest(1), order = f.process.allocate_guest(4);
    require(f.call(cs, "_UCCompareText", {collator, utf16("apple"), 5, utf16("APPLE"), 5, equivalent, order}) == 0 && f.byte(equivalent) == 1,
            "case-insensitive comparison");
    f.call(cs, "_UCCompareText", {collator, utf16("apple"), 5, utf16("banana"), 6, equivalent, order});
    require(static_cast<std::int32_t>(f.engine.read32(order)) < 0, "apple < banana");
    const auto key_a = f.process.allocate_guest(4096), key_b = f.process.allocate_guest(4096), size_a = f.process.allocate_guest(8), size_b = f.process.allocate_guest(8);
    require(f.call(cs, "_UCGetCollationKey", {collator, utf16("pear"), 4, 1024, size_a, key_a}) == 0 &&
            f.call(cs, "_UCGetCollationKey", {collator, utf16("Plum"), 4, 1024, size_b, key_b}) == 0, "UCGetCollationKey");
    f.call(cs, "_UCCompareCollationKeys", {key_a, f.engine.read64(size_a), key_b, f.engine.read64(size_b), equivalent, order});
    require(static_cast<std::int32_t>(f.engine.read32(order)) < 0, "collation keys order pear < Plum");
    require(static_cast<std::int64_t>(f.call(cs, "_UCGetCollationKey", {collator, utf16("pear"), 4, 1, size_a, key_a})) == -29509, "short key buffer");
    require(f.call(cs, "_UCDisposeCollator", {collator_out}) == 0, "UCDisposeCollator");
    // Uniform Type Identifiers.
    const auto extension = f.engine.read64(f.symbol(cs, "_kUTTagClassFilenameExtension"));
    const auto png = f.call(cs, "_UTTypeCreatePreferredIdentifierForTag", {extension, f.cf.string("PNG"), 0});
    require(f.cf.utf8(png) == "public.png", "UTI for .png");
    require(f.call(cs, "_UTTypeConformsTo", {png, f.cf.string("public.image")}) == 1 &&
            f.call(cs, "_UTTypeConformsTo", {png, f.cf.string("public.text")}) == 0, "UTTypeConformsTo");
    require(f.cf.utf8(f.call(cs, "_UTTypeCopyPreferredTagWithClass", {f.cf.string("public.jpeg"), extension})) == "jpg", "preferred extension");
    require(f.cf.utf8(f.call(cs, "_UTTypeCreatePreferredIdentifierForTag", {extension, f.cf.string("vrscene"), 0})).starts_with("dyn."), "dynamic UTI");
    // FSEvents: fsevents.cpp (system_services_tests.cpp).
}

} // namespace

int main() {
    try {
        const auto stage = [](std::string_view name) { std::cout << "[stage] " << name << std::endl; };
        stage("fixture"); Fixture f;
        stage("guest coreservices"); test_guest_coreservices(f);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "coreservices tests passed\n";
    return 0;
}
