// CoreMediaIO (src/alchemy/darwin/coremediaio/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/CoreMediaIO.framework/Versions/A/CoreMediaIO"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case coremediaio_case("coremediaio", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "CoreMediaIO identity");
    require(f.status(LIBRARY, "_CMIOObjectGetPropertyData", {1, 0, 0, 0, 0, 0, 0}) == static_cast<std::int32_t>(0x216f626a), "no DAL objects");
    require(f.call(LIBRARY, "_CMIOObjectHasProperty", {1, 0}) == 0, "no properties");
    // The DAL plug-in and camera extension side: CoreMediaIO's documented errors; the classes exist.
    const auto out = f.block(8, 0xff);
    require(f.status(LIBRARY, "_CMIOObjectCreate", {0, 1, 0, out}) == static_cast<std::int32_t>(0x216f626a) && f.engine.read32(out) == 0, "no plug-in objects");
    require(f.status(LIBRARY, "_CMIODeviceStartStream", {2, 3}) == static_cast<std::int32_t>(0x21646576), "no devices ('!dev')");
    f.engine.write64(out, ~0ULL);
    require(f.status(LIBRARY, "_CMIOStreamCopyBufferQueue", {3, 0, 0, out}) == static_cast<std::int32_t>(0x21737472) && f.engine.read64(out) == 0,
            "no streams ('!str')");
    f.engine.write64(out, ~0ULL);
    require(f.status(LIBRARY, "_CMIOStreamClockCreate", {0, 0, 0, f.block(24), 10, 10, out}) == static_cast<std::int32_t>(0x756e6f70) &&
            f.engine.read64(out) == 0, "no stream clock ('unop')");
    require(f.string_constant(LIBRARY, "_CMIOExtensionPropertyProviderName") == "CMIOExtensionPropertyProviderName", "an extension property key");
    require(f.cls("CMIOExtensionProvider") != 0 && f.cls("CMIOExtensionStreamProperties") != 0, "the extension classes");
    // The properties the Swift overlay sets: kept as given.
    const auto properties = f.send(f.send(f.cls("CMIOExtensionStreamProperties"), "alloc"), "init");
    const auto index = f.cf.integer(2);
    f.send(properties, "setActiveFormatIndex:", {index});
    require(f.cf.number_value(f.send(properties, "activeFormatIndex")) == 2.0, "activeFormatIndex kept");
    const auto max = f.block(24), min = f.block(24), time = f.block(24);
    f.engine.write64(max, 1); f.engine.write32(max + 8, 30); f.engine.write32(max + 12, 1);
    f.engine.write64(min, 1); f.engine.write32(min + 8, 60); f.engine.write32(min + 12, 1);
    const auto format = f.send(f.send(f.cls("CMIOExtensionStreamFormat"), "alloc"), "initWithFormatDescription:maxFrameDuration:minFrameDuration:validFrameDurations:",
                               {0, max, min, 0});
    alchemy::CallRegs r{};
    r.indirect_result = time;
    static_cast<void>(f.objc.send(format, "maxFrameDuration", r));
    require(format != 0 && f.engine.read64(time) == 1 && f.engine.read32(time + 8) == 30, "a stream format's maximum frame duration (1/30)");
});

#undef LIBRARY
} // namespace
