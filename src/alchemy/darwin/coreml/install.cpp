#include "alchemy/darwin/coreml/install.hpp"
#include "alchemy/gui/value_provider.hpp"

#include "alchemy/darwin/security/bridge.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// CoreML: no model can be loaded or compiled (nil plus an NSError in the
// com.apple.CoreML domain, or the completion handler gets the error); neither
// can a compute plan, so no compute device is ever handed out. The data
// classes are real: MLMultiArray (Double, Float32, Float16 and Int32 elements
// in guest memory, any strides), MLFeatureValue and MLPredictionOptions.
// Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
namespace alchemy::coreml {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view core_ml = "/System/Library/Frameworks/CoreML.framework/Versions/A/CoreML";
constexpr std::int64_t ml_model_error_generic = 0;

// MLMultiArrayDataType: the kind in the high half, the element's bits in the low.
constexpr std::int64_t ml_double = 0x10000 | 64, ml_float32 = 0x10000 | 32, ml_float16 = 0x10000 | 16, ml_int32 = 0x20000 | 32;
// MLFeatureType
enum : std::int64_t { feature_invalid, feature_int64, feature_double, feature_string, feature_image, feature_multi_array, feature_dictionary };

struct MultiArray final : ObjectPayload {
    std::vector<std::int64_t> shape, strides;   // strides in elements
    std::int64_t type{};
    Address data{};
    bool owned{};                               // allocated here (released in dealloc)
    Address deallocator{};                      // copied block, called with `data` in dealloc
    [[nodiscard]] std::int64_t element_size() const { return (type & 0xffff) / 8; }
    [[nodiscard]] std::int64_t count() const {
        std::int64_t n = 1;
        for (const auto d : shape) n *= d;
        return n;
    }
    // Elements from the first to one past the last addressable one.
    [[nodiscard]] std::int64_t span() const {
        std::int64_t last = 0;
        for (std::size_t i = 0; i < shape.size(); ++i) last += (shape[i] - 1) * strides[i];
        return count() ? last + 1 : 0;
    }
    // The element at the row-major position `linear`.
    [[nodiscard]] std::int64_t offset_of_linear(std::int64_t linear) const {
        std::int64_t offset = 0;
        for (std::size_t i = shape.size(); i-- > 0;) {
            offset += (linear % shape[i]) * strides[i];
            linear /= shape[i];
        }
        return offset;
    }
};

float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    if (exponent == 0) return std::bit_cast<float>(sign) + (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa), -24);
    if (exponent == 31) return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13));
    return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

std::uint16_t float_to_half(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000);
    if (std::isnan(value)) return static_cast<std::uint16_t>(sign | 0x7e00);
    const float magnitude = std::fabs(value);
    if (magnitude >= 65520.0f) return static_cast<std::uint16_t>(sign | 0x7c00);
    if (magnitude < std::ldexp(1.0f, -14)) {   // subnormal (or zero): round to nearest even
        return static_cast<std::uint16_t>(sign | static_cast<std::uint16_t>(std::nearbyint(magnitude * std::ldexp(1.0f, 24))));
    }
    std::uint32_t rest = bits & 0x7fffffffu;
    const std::uint32_t round = 0xfff + ((rest >> 13) & 1);   // to nearest even
    rest += round;
    return static_cast<std::uint16_t>(sign | ((rest - (112u << 23)) >> 13));
}

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();

private:
    Address multi_array_class{}, feature_class{};
    std::shared_ptr<MultiArray> multi_array(Address object) { return object ? objc.find_host_state<MultiArray>(object) : nullptr; }
    void install_multi_array();
    void install_feature_value();
    double element(GuestMemory& m, const MultiArray& a, std::int64_t offset);
    void set_element(GuestMemory& m, const MultiArray& a, std::int64_t offset, double value);
    Address numbers(const std::vector<std::int64_t>& values);
    std::optional<std::vector<std::int64_t>> integers(Address array);
    void fail(Address out, std::string_view description) {
        bridge->store_error(out, bridge->error("com.apple.CoreML", ml_model_error_generic, description), true);
    }
};

double Provider::element(GuestMemory& m, const MultiArray& a, std::int64_t offset) {
    const auto address = a.data + static_cast<Address>(offset * a.element_size());
    switch (a.type) {
    case ml_double: return std::bit_cast<double>(m.read64(address));
    case ml_float32: return std::bit_cast<float>(m.read32(address));
    case ml_float16: return half_to_float(static_cast<std::uint16_t>(m.read8(address) | (m.read8(address + 1) << 8)));
    default: return static_cast<std::int32_t>(m.read32(address));
    }
}

void Provider::set_element(GuestMemory& m, const MultiArray& a, std::int64_t offset, double value) {
    const auto address = a.data + static_cast<Address>(offset * a.element_size());
    switch (a.type) {
    case ml_double: m.write64(address, std::bit_cast<std::uint64_t>(value)); break;
    case ml_float32: m.write32(address, std::bit_cast<std::uint32_t>(static_cast<float>(value))); break;
    case ml_float16: {
        const auto half = float_to_half(static_cast<float>(value));
        m.write8(address, static_cast<std::uint8_t>(half));
        m.write8(address + 1, static_cast<std::uint8_t>(half >> 8));
        break;
    }
    default: m.write32(address, static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
    }
}

// An autoreleased NSArray of NSNumbers.
Address Provider::numbers(const std::vector<std::int64_t>& values) {
    std::vector<Address> items;
    for (const auto v : values) items.push_back(cf.integer(v));
    const auto array = cf.array(items);
    for (const auto item : items) release(item);
    return bridge->autorelease(array);
}

// The integers of an NSArray of NSNumbers (nullopt when it is not one).
std::optional<std::vector<std::int64_t>> Provider::integers(Address array) {
    if (!array) return std::nullopt;
    std::vector<std::int64_t> values;
    for (const auto item : cf.array_values(array)) {
        const auto n = cf.number_value(item);
        if (!n) return std::nullopt;
        values.push_back(static_cast<std::int64_t>(*n));
    }
    return values;
}

void Provider::install_multi_array() {
    multi_array_class = cls(core_ml, "MLMultiArray", "NSObject");
    const auto valid_type = [](std::int64_t type) { return type == ml_double || type == ml_float32 || type == ml_float16 || type == ml_int32; };
    method(multi_array_class, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto a = multi_array(s.x[0])) {
            if (a->owned) process.release_guest(std::exchange(a->data, 0));
            if (a->deallocator) {
                static_cast<void>(bridge->invoke_block(a->deallocator, {a->data}));
                bridge->block_release(std::exchange(a->deallocator, 0));
            }
        }
        static_cast<void>(objc.send_super(s.x[0], multi_array_class, objc.selector("dealloc")));
    }, false, "v@:");
    // -initWithShape:(NSArray<NSNumber *> *)shape dataType:(MLMultiArrayDataType)dataType error:(NSError **)
    method(multi_array_class, "initWithShape:dataType:error:", [this, valid_type](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto shape = integers(s.x[2]);
        const auto type = static_cast<std::int64_t>(s.x[3]);
        s.x[0] = 0;
        if (!shape || std::any_of(shape->begin(), shape->end(), [](std::int64_t d) { return d < 0; }) || !valid_type(type)) {
            fail(s.x[4], "MLMultiArray: the shape must be non-negative numbers and the data type Double, Float32, Float16 or Int32");
            release(self);
            return;
        }
        const auto a = objc.host_state<MultiArray>(self);
        a->shape = *shape;
        a->type = type;
        a->strides.assign(shape->size(), 1);
        for (std::size_t i = shape->size(); i-- > 1;) a->strides[i - 1] = a->strides[i] * std::max<std::int64_t>((*shape)[i], 1);
        const auto bytes = std::max<std::int64_t>(a->count() * a->element_size(), 1);
        a->data = process.allocate_guest(static_cast<std::uint64_t>(bytes), 64);
        a->owned = true;
        s.x[0] = self;
    }, false, "@@:@q^@");
    // -initWithDataPointer:(void *)dataPointer shape:(NSArray *)shape dataType:(MLMultiArrayDataType)dataType
    //   strides:(NSArray *)strides deallocator:(void (^)(void *bytes))deallocator error:(NSError **)error
    method(multi_array_class, "initWithDataPointer:shape:dataType:strides:deallocator:error:", [this, valid_type](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto shape = integers(s.x[3]), strides = integers(s.x[5]);
        const auto type = static_cast<std::int64_t>(s.x[4]);
        s.x[0] = 0;
        if (!s.x[2] || !shape || !strides || shape->size() != strides->size() || !valid_type(type)) {
            fail(s.x[7], "MLMultiArray: a data pointer, a shape and strides of one length, and a supported data type are needed");
            release(self);
            return;
        }
        const auto a = objc.host_state<MultiArray>(self);
        a->shape = *shape;
        a->strides = *strides;
        a->type = type;
        a->data = s.x[2];
        a->deallocator = s.x[6] ? bridge->block_copy(s.x[6]) : 0;
        s.x[0] = self;
    }, false, "@@:^v@q@@?^@");
    const auto get = [this](Address self) {
        const auto a = multi_array(self);
        if (!a) throw std::runtime_error("Alchemy: MLMultiArray was not initialized with a shape");
        return a;
    };
    method(multi_array_class, "shape", [this, get](CpuState& s, GuestMemory&) { s.x[0] = numbers(get(s.x[0])->shape); });
    method(multi_array_class, "strides", [this, get](CpuState& s, GuestMemory&) { s.x[0] = numbers(get(s.x[0])->strides); });
    method(multi_array_class, "dataType", [get](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint64_t>(get(s.x[0])->type); }, false, "q@:");
    method(multi_array_class, "count", [get](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint64_t>(get(s.x[0])->count()); }, false, "q@:");
    method(multi_array_class, "dataPointer", [get](CpuState& s, GuestMemory&) { s.x[0] = get(s.x[0])->data; }, false, "^v@:");
    // -getBytesWithHandler:(void (^)(const void *bytes, NSInteger size)), and the mutable form
    // whose handler also gets the strides.
    method(multi_array_class, "getBytesWithHandler:", [this, get](CpuState& s, GuestMemory&) {
        const auto a = get(s.x[0]);
        static_cast<void>(bridge->invoke_block(s.x[2], {a->data, static_cast<Address>(a->span() * a->element_size())}));
    }, false, "v@:@?");
    method(multi_array_class, "getMutableBytesWithHandler:", [this, get](CpuState& s, GuestMemory&) {
        const auto a = get(s.x[0]);
        static_cast<void>(bridge->invoke_block(s.x[2], {a->data, static_cast<Address>(a->span() * a->element_size()), numbers(a->strides)}));
    }, false, "v@:@?");
    // Subscripts: a row-major index, or an NSArray of one index per dimension.
    const auto checked_linear = [](const MultiArray& a, std::int64_t index) {
        if (index < 0 || index >= a.count()) throw std::runtime_error("Alchemy: NSRangeException: MLMultiArray index " + std::to_string(index) + " is out of range");
        return a.offset_of_linear(index);
    };
    const auto checked_key = [this](const MultiArray& a, Address key) {
        const auto indices = integers(key);
        if (!indices || indices->size() != a.shape.size()) throw std::runtime_error("Alchemy: NSInvalidArgumentException: an MLMultiArray key needs one index per dimension");
        std::int64_t offset = 0;
        for (std::size_t i = 0; i < indices->size(); ++i) {
            if ((*indices)[i] < 0 || (*indices)[i] >= a.shape[i]) throw std::runtime_error("Alchemy: NSRangeException: MLMultiArray key is out of range");
            offset += (*indices)[i] * a.strides[i];
        }
        return offset;
    };
    const auto number_of = [this](const MultiArray& a, double value) {
        return bridge->autorelease(a.type == ml_int32 ? cf.integer(static_cast<std::int64_t>(value), 3) : cf.real(value));
    };
    method(multi_array_class, "objectAtIndexedSubscript:", [this, get, checked_linear, number_of](CpuState& s, GuestMemory& m) {
        const auto a = get(s.x[0]);
        s.x[0] = number_of(*a, element(m, *a, checked_linear(*a, static_cast<std::int64_t>(s.x[2]))));
    }, false, "@@:q");
    method(multi_array_class, "objectForKeyedSubscript:", [this, get, checked_key, number_of](CpuState& s, GuestMemory& m) {
        const auto a = get(s.x[0]);
        s.x[0] = number_of(*a, element(m, *a, checked_key(*a, s.x[2])));
    });
    const auto value_of = [this](Address number) {
        const auto value = number ? cf.number_value(number) : std::nullopt;
        if (!value) throw std::runtime_error("Alchemy: NSInvalidArgumentException: MLMultiArray elements are NSNumbers");
        return *value;
    };
    method(multi_array_class, "setObject:atIndexedSubscript:", [this, get, checked_linear, value_of](CpuState& s, GuestMemory& m) {
        const auto a = get(s.x[0]);
        set_element(m, *a, checked_linear(*a, static_cast<std::int64_t>(s.x[3])), value_of(s.x[2]));
    }, false, "v@:@q");
    method(multi_array_class, "setObject:forKeyedSubscript:", [this, get, checked_key, value_of](CpuState& s, GuestMemory& m) {
        const auto a = get(s.x[0]);
        set_element(m, *a, checked_key(*a, s.x[3]), value_of(s.x[2]));
    }, false, "v@:@@");
}

void Provider::install_feature_value() {
    feature_class = cls(core_ml, "MLFeatureValue", "NSObject");
    integer_properties(feature_class, {"type", "int64Value", "isUndefined"});
    object_properties(feature_class, {"stringValue", "multiArrayValue", "dictionaryValue", "imageBufferValue"});
    // +featureValueWith...: a new value of one type, holding `object` under `key` (or an integer).
    const auto made = [this](std::int64_t type) {
        const auto value = create(feature_class);
        store_integer(value, "type", type);
        return value;
    };
    const auto with_object = [this, made](std::int64_t type, const char* key) {
        return [this, made, type, key](CpuState& s, GuestMemory&) {
            const auto value = made(type);
            store(value, key, s.x[2]);
            s.x[0] = bridge->autorelease(value);
        };
    };
    method(feature_class, "featureValueWithString:", with_object(feature_string, "stringValue"), true, "@@:@");
    method(feature_class, "featureValueWithMultiArray:", with_object(feature_multi_array, "multiArrayValue"), true, "@@:@");
    method(feature_class, "featureValueWithPixelBuffer:", with_object(feature_image, "imageBufferValue"), true, "@@:^v");
    method(feature_class, "featureValueWithInt64:", [this, made](CpuState& s, GuestMemory&) {
        const auto value = made(feature_int64);
        store_integer(value, "int64Value", static_cast<std::int64_t>(s.x[2]));
        s.x[0] = bridge->autorelease(value);
    }, true, "@@:q");
    method(feature_class, "featureValueWithDouble:", [this, made](CpuState& s, GuestMemory&) {
        const auto value = made(feature_double);
        store_integer(value, "doubleBits", std::bit_cast<std::int64_t>(fill::real(s, 0)));
        s.x[0] = bridge->autorelease(value);
    }, true, "@@:d");
    method(feature_class, "doubleValue", [this](CpuState& s, GuestMemory&) {
        fill::return_real(s, std::bit_cast<double>(stored_integer(s.x[0], "doubleBits")));
    }, false, "d@:");
    // +featureValueWithDictionary:(NSDictionary<id, NSNumber *> *) error:: keys are NSStrings or
    // NSNumbers (integers), values NSNumbers.
    method(feature_class, "featureValueWithDictionary:error:", [this, made](CpuState& s, GuestMemory&) {
        bool valid = s.x[2] != 0;
        if (valid)
            for (const auto& [key, value] : cf.dictionary_entries(s.x[2])) {
                const auto key_kind = cf.kind(key);
                valid = valid && (key_kind == ObjectKind::string || key_kind == ObjectKind::number) && cf.kind(value) == ObjectKind::number;
            }
        if (!valid) {
            fail(s.x[3], "MLFeatureValue: a dictionary feature maps NSString or NSNumber keys to NSNumbers");
            s.x[0] = 0;
            return;
        }
        const auto value = made(feature_dictionary);
        store(value, "dictionaryValue", s.x[2]);
        s.x[0] = bridge->autorelease(value);
    }, true, "@@:@^@");
    method(feature_class, "undefinedFeatureValueWithType:", [this, made](CpuState& s, GuestMemory&) {
        const auto value = made(static_cast<std::int64_t>(s.x[2]));
        store_integer(value, "isUndefined", 1);
        s.x[0] = bridge->autorelease(value);
    }, true, "@@:q");
    method(feature_class, "isEqualToFeatureValue:", [this](CpuState& s, GuestMemory&) {
        const auto a = s.x[0], b = s.x[2];
        bool equal = b && stored_integer(a, "type") == stored_integer(b, "type") && stored_integer(a, "isUndefined") == stored_integer(b, "isUndefined");
        if (equal) {
            switch (stored_integer(a, "type")) {
            case feature_int64: equal = stored_integer(a, "int64Value") == stored_integer(b, "int64Value"); break;
            case feature_double:
                equal = std::bit_cast<double>(stored_integer(a, "doubleBits")) == std::bit_cast<double>(stored_integer(b, "doubleBits"));
                break;
            default:
                for (const auto* key : {"stringValue", "multiArrayValue", "dictionaryValue", "imageBufferValue"}) {
                    const auto left = stored(a, key), right = stored(b, key);
                    equal = equal && (left == right || (left && right && cf.equal(left, right)));
                }
            }
        }
        s.x[0] = equal;
    }, false, "B@:@");
}

void Provider::install() {
    ProcessContext::add_provider_library(std::string(core_ml));
    install_multi_array();
    install_feature_value();
    const auto options = cls(core_ml, "MLPredictionOptions", "NSObject");
    integer_properties(options, {"usesCPUOnly"});
    object_properties(options, {"outputBackings"});
    // The compute devices a compute plan or MLAllComputeDevices would hand out; no plan loads.
    for (const auto* name : {"MLCPUComputeDevice", "MLGPUComputeDevice", "MLNeuralEngineComputeDevice"})
        static_cast<void>(cls(core_ml, name, "NSObject"));
    const auto configuration = cls(core_ml, "MLModelConfiguration", "NSObject");
    integer_properties(configuration, {"computeUnits", "allowLowPrecisionAccumulationOnGPU"});
    object_properties(configuration, {"modelDisplayName", "preferredMetalDevice"});
    const auto model = cls(core_ml, "MLModel", "NSObject");
    const auto model_error = [this](Address out) {
        bridge->store_error(out, bridge->error("com.apple.CoreML", ml_model_error_generic, "Core ML is not available"), true);
    };
    method(model, "modelWithContentsOfURL:error:", [model_error](CpuState& s, GuestMemory&) { model_error(s.x[3]); s.x[0] = 0; }, true);
    method(model, "modelWithContentsOfURL:configuration:error:", [model_error](CpuState& s, GuestMemory&) { model_error(s.x[4]); s.x[0] = 0; }, true);
    method(model, "compileModelAtURL:error:", [model_error](CpuState& s, GuestMemory&) { model_error(s.x[3]); s.x[0] = 0; }, true);
    method(model, "loadContentsOfURL:configuration:completionHandler:", [this](CpuState& s, GuestMemory&) {
        complete_with_error(s.x[4], "com.apple.CoreML", ml_model_error_generic, "Core ML is not available");
    }, true, "v@:@@@?");
    method(model, "compileModelAtURL:completionHandler:", [this](CpuState& s, GuestMemory&) {
        complete_with_error(s.x[3], "com.apple.CoreML", ml_model_error_generic, "Core ML is not available");
    }, true, "v@:@@?");
    // +[MLComputePlan loadContentsOfURL:configuration:completionHandler:] and
    // +loadModelAsset:configuration:completionHandler:: the plan needs a model.
    const auto plan = cls(core_ml, "MLComputePlan", "NSObject");
    for (const auto* selector : {"loadContentsOfURL:configuration:completionHandler:", "loadModelAsset:configuration:completionHandler:"})
        method(plan, selector, [this](CpuState& s, GuestMemory&) {
            complete_with_error(s.x[4], "com.apple.CoreML", ml_model_error_generic, "Core ML is not available");
        }, true, "v@:@@@?");
}

} // namespace

void install_coreml(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::coreml
