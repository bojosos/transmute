// CoreML (src/alchemy/darwin/coreml/): the identity, the documented
// "unavailable" answers, and the data classes (MLMultiArray, MLFeatureValue).
#include "alchemy/call_regs.hpp"

#include <bit>
#include <vector>
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/CoreML.framework/Versions/A/CoreML"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case coreml_case("coreml", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "CoreML identity");
    const auto error = f.block(8);
    require(f.send(f.cls("MLModel"), "modelWithContentsOfURL:error:", {f.cf.string("x"), error}) == 0 && f.engine.read64(error), "MLModel fails");

    // An NSArray of NSNumbers.
    const auto list = [&f](std::initializer_list<std::int64_t> values) {
        std::vector<std::uint64_t> items;
        for (const auto v : values) items.push_back(f.cf.integer(v));
        return f.cf.array(items);
    };
    // A 2x3 Float32 array: row-major strides, subscripts by position and by key.
    const auto shape = list({2, 3});
    const auto array = f.send(f.send(f.cls("MLMultiArray"), "alloc"), "initWithShape:dataType:error:", {shape, 0x10000 | 32, 0});
    require(array && f.send(array, "count") == 6 && f.send(array, "dataType") == (0x10000 | 32), "a Float32 multi-array");
    const auto strides = f.send(array, "strides");
    require(f.cf.number_value(f.cf.element(strides, 0)) == 3.0 && f.cf.number_value(f.cf.element(strides, 1)) == 1.0, "row-major strides");
    f.send(array, "setObject:atIndexedSubscript:", {f.cf.real(2.5), 5});
    const auto last = f.send(array, "objectForKeyedSubscript:", {list({1, 2})});
    require(f.cf.number_value(last) == 2.5, "the element at [1, 2]");
    require(std::bit_cast<float>(f.engine.read32(f.send(array, "dataPointer") + 20)) == 2.5f, "the element in memory");
    // Float16 and Int32 elements convert through NSNumber.
    const auto half = f.send(f.send(f.cls("MLMultiArray"), "alloc"), "initWithShape:dataType:error:", {list({4}), 0x10000 | 16, 0});
    f.send(half, "setObject:atIndexedSubscript:", {f.cf.real(-1.5), 3});
    require(f.cf.number_value(f.send(half, "objectAtIndexedSubscript:", {3})) == -1.5 &&
            (f.engine.read32(f.send(half, "dataPointer") + 4) >> 16) == 0xbe00, "a Float16 element");
    const auto ints = f.send(f.send(f.cls("MLMultiArray"), "alloc"), "initWithShape:dataType:error:", {list({2}), 0x20000 | 32, 0});
    f.send(ints, "setObject:atIndexedSubscript:", {f.cf.integer(-7), 1});
    require(f.cf.number_value(f.send(ints, "objectAtIndexedSubscript:", {1})) == -7.0, "an Int32 element");
    const auto bad = f.block(8);
    require(f.send(f.send(f.cls("MLMultiArray"), "alloc"), "initWithShape:dataType:error:", {shape, 12345, bad}) == 0 && f.engine.read64(bad),
            "an unknown data type fails with an error");

    // Feature values.
    const auto integer = f.send(f.cls("MLFeatureValue"), "featureValueWithInt64:", {7});
    require(f.send(integer, "type") == 1 && f.send(integer, "int64Value") == 7, "an Int64 feature");
    alchemy::CallRegs with_double{};
    with_double.set_d(0, 0.25);
    const auto real = f.objc.send(f.cls("MLFeatureValue"), f.objc.selector("featureValueWithDouble:"), with_double).x0;
    require(f.send(real, "type") == 2 && f.objc.send(real, f.objc.selector("doubleValue")).d(0) == 0.25, "a Double feature");
    const auto wrapped = f.send(f.cls("MLFeatureValue"), "featureValueWithMultiArray:", {array});
    require(f.send(wrapped, "type") == 5 && f.send(wrapped, "multiArrayValue") == array, "a multi-array feature");
    require((f.send(integer, "isEqualToFeatureValue:", {f.send(f.cls("MLFeatureValue"), "featureValueWithInt64:", {7})}) & 0xff) == 1 &&
            (f.send(integer, "isEqualToFeatureValue:", {real}) & 0xff) == 0, "feature equality");
});

#undef LIBRARY
} // namespace
