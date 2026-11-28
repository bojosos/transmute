#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// Foundation's Units and Measurement classes (NSUnit, NSUnitConverter,
// NSUnitConverterLinear, NSDimension, NSMeasurement and the 22 standard
// NSDimension subclasses from Apple's public Foundation documentation).
//
// - NSUnit carries an immutable symbol string; NSDimension extends it with an
//   NSUnitConverter to and from its subclass's +baseUnit.
// - NSUnitConverterLinear converts by base = value * coefficient + constant
//   and value = (base - constant) / coefficient. An internal reciprocal
//   converter (_NSUnitConverterReciprocal) handles NSUnitFuelEfficiency's
//   miles-per-gallon units (base = reciprocal / value).
// - NSMeasurement pairs a double value with an NSUnit and supports unit
//   conversion (-canBeConvertedToUnit:, -measurementByConvertingToUnit:) and
//   arithmetic (-measurementByAddingMeasurement:,
//   -measurementBySubtractingMeasurement:), dispatching through Objective-C so
//   guest NSDimension and NSUnitConverter subclasses work identically.
// - All classes implement NSCopying (immutable retain) and NSSecureCoding.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

double arg_double(const CpuState& s, std::size_t index = 0) {
    return std::bit_cast<double>(s.v[index][0]);
}

void return_double(CpuState& s, double value) {
    s.v[0] = {std::bit_cast<std::uint64_t>(value), 0};
}

bool is_kind_of(Support& support, Address object, Address cls) {
    if (!object || !cls) return false;
    CallRegs r;
    r.x[2] = cls;
    return (support.send(object, "isKindOfClass:", r).x0 & 0xff) != 0;
}

bool is_equal(Support& support, Address a, Address b) {
    if (a == b) return true;
    if (!a || !b) return false;
    CallRegs r;
    r.x[2] = b;
    return (support.send(a, "isEqual:", r).x0 & 0xff) != 0;
}

std::uint64_t hash_double(double v) {
    // Normalize -0.0 to +0.0 so equal doubles always produce equal hashes.
    if (v == 0.0) v = 0.0;
    return std::hash<std::uint64_t>{}(std::bit_cast<std::uint64_t>(v));
}

struct UnitState final : ObjectPayload {
    Address symbol{};      // retained NSString
    Address converter{};   // retained NSUnitConverter (NSDimension only)
};

struct LinearConverterState final : ObjectPayload {
    double coefficient{1.0};
    double constant{0.0};
};

struct ReciprocalConverterState final : ObjectPayload {
    double reciprocal{1.0};
};

struct MeasurementState final : ObjectPayload {
    double value{};
    Address unit{};   // retained NSUnit
};

struct UnitSpec {
    const char* selector;
    const char* symbol;
    double coefficient;
    double constant;
    bool reciprocal;
};

struct DimensionSpec {
    const char* class_name;
    const char* base_selector;
    std::vector<UnitSpec> units;
};

constexpr double kPi = 3.14159265358979323846;

double pow1024(int n) {
    double v = 1.0;
    for (int i = 0; i < n; ++i) v *= 1024.0;
    return v;
}

std::vector<DimensionSpec> standard_dimensions() {
    auto lin = [](const char* sel, const char* sym, double coeff, double constant = 0.0) {
        return UnitSpec{sel, sym, coeff, constant, false};
    };
    auto recip = [](const char* sel, const char* sym, double k) {
        return UnitSpec{sel, sym, k, 0.0, true};
    };

    return {
        {"NSUnitAcceleration", "metersPerSecondSquared", {
            lin("metersPerSecondSquared", "m/s\xc2\xb2", 1.0),
            lin("gravity", "g", 9.81),
        }},
        {"NSUnitAngle", "degrees", {
            lin("degrees", "\xc2\xb0", 1.0),
            lin("arcMinutes", "\xe2\x80\xb2", 1.0 / 60.0),
            lin("arcSeconds", "\xe2\x80\xb3", 1.0 / 3600.0),
            lin("radians", "rad", 180.0 / kPi),
            lin("gradians", "grad", 0.9),
            lin("revolutions", "rev", 360.0),
        }},
        {"NSUnitArea", "squareMeters", {
            lin("squareMegameters", "Mm\xc2\xb2", 1e12),
            lin("squareKilometers", "km\xc2\xb2", 1e6),
            lin("squareMeters", "m\xc2\xb2", 1.0),
            lin("squareCentimeters", "cm\xc2\xb2", 1e-4),
            lin("squareMillimeters", "mm\xc2\xb2", 1e-6),
            lin("squareMicrometers", "\xc2\xb5m\xc2\xb2", 1e-12),
            lin("squareNanometers", "nm\xc2\xb2", 1e-18),
            lin("squareInches", "in\xc2\xb2", 0.00064516),
            lin("squareFeet", "ft\xc2\xb2", 0.092903),
            lin("squareYards", "yd\xc2\xb2", 0.836127),
            lin("squareMiles", "mi\xc2\xb2", 2.58999e6),
            lin("acres", "ac", 4046.86),
            lin("ares", "a", 100.0),
            lin("hectares", "ha", 10000.0),
        }},
        {"NSUnitConcentrationMass", "gramsPerLiter", {
            lin("gramsPerLiter", "g/L", 1.0),
            lin("milligramsPerDeciliter", "mg/dL", 0.01),
        }},
        {"NSUnitDispersion", "partsPerMillion", {
            lin("partsPerMillion", "ppm", 1.0),
        }},
        {"NSUnitDuration", "seconds", {
            lin("hours", "hr", 3600.0),
            lin("minutes", "min", 60.0),
            lin("seconds", "s", 1.0),
            lin("milliseconds", "ms", 1e-3),
            lin("microseconds", "\xc2\xb5s", 1e-6),
            lin("nanoseconds", "ns", 1e-9),
            lin("picoseconds", "ps", 1e-12),
        }},
        {"NSUnitElectricCharge", "coulombs", {
            lin("coulombs", "C", 1.0),
            lin("megaampereHours", "MAh", 3.6e9),
            lin("kiloampereHours", "kAh", 3.6e6),
            lin("ampereHours", "Ah", 3600.0),
            lin("milliampereHours", "mAh", 3.6),
            lin("microampereHours", "\xc2\xb5" "Ah", 0.0036),
        }},
        {"NSUnitElectricCurrent", "amperes", {
            lin("megaamperes", "MA", 1e6),
            lin("kiloamperes", "kA", 1e3),
            lin("amperes", "A", 1.0),
            lin("milliamperes", "mA", 1e-3),
            lin("microamperes", "\xc2\xb5" "A", 1e-6),
        }},
        {"NSUnitElectricPotentialDifference", "volts", {
            lin("megavolts", "MV", 1e6),
            lin("kilovolts", "kV", 1e3),
            lin("volts", "V", 1.0),
            lin("millivolts", "mV", 1e-3),
            lin("microvolts", "\xc2\xb5V", 1e-6),
        }},
        {"NSUnitElectricResistance", "ohms", {
            lin("megaohms", "M\xce\xa9", 1e6),
            lin("kiloohms", "k\xce\xa9", 1e3),
            lin("ohms", "\xce\xa9", 1.0),
            lin("milliohms", "m\xce\xa9", 1e-3),
            lin("microohms", "\xc2\xb5\xce\xa9", 1e-6),
        }},
        {"NSUnitEnergy", "joules", {
            lin("kilojoules", "kJ", 1e3),
            lin("joules", "J", 1.0),
            lin("kilocalories", "kCal", 4184.0),
            lin("calories", "cal", 4.184),
            lin("kilowattHours", "kWh", 3600000.0),
        }},
        {"NSUnitFrequency", "hertz", {
            lin("terahertz", "THz", 1e12),
            lin("gigahertz", "GHz", 1e9),
            lin("megahertz", "MHz", 1e6),
            lin("kilohertz", "kHz", 1e3),
            lin("hertz", "Hz", 1.0),
            lin("millihertz", "mHz", 1e-3),
            lin("microhertz", "\xc2\xb5Hz", 1e-6),
            lin("nanohertz", "nHz", 1e-9),
            lin("framesPerSecond", "fps", 1.0),
        }},
        {"NSUnitFuelEfficiency", "litersPer100Kilometers", {
            lin("litersPer100Kilometers", "L/100km", 1.0),
            recip("milesPerImperialGallon", "mpg", 282.481),
            recip("milesPerGallon", "mpg", 235.215),
        }},
        {"NSUnitIlluminance", "lux", {
            lin("lux", "lx", 1.0),
        }},
        {"NSUnitInformationStorage", "bits", {
            lin("bytes", "B", 8.0),
            lin("bits", "bit", 1.0),
            lin("nibbles", "nibble", 4.0),
            lin("yottabytes", "YB", 8.0 * 1e24),
            lin("zettabytes", "ZB", 8.0 * 1e21),
            lin("exabytes", "EB", 8.0 * 1e18),
            lin("petabytes", "PB", 8.0 * 1e15),
            lin("terabytes", "TB", 8.0 * 1e12),
            lin("gigabytes", "GB", 8.0 * 1e9),
            lin("megabytes", "MB", 8.0 * 1e6),
            lin("kilobytes", "kB", 8.0 * 1e3),
            lin("yottabits", "Yb", 1e24),
            lin("zettabits", "Zb", 1e21),
            lin("exabits", "Eb", 1e18),
            lin("petabits", "Pb", 1e15),
            lin("terabits", "Tb", 1e12),
            lin("gigabits", "Gb", 1e9),
            lin("megabits", "Mb", 1e6),
            lin("kilobits", "kb", 1e3),
            lin("yobibytes", "YiB", 8.0 * pow1024(8)),
            lin("zebibytes", "ZiB", 8.0 * pow1024(7)),
            lin("exbibytes", "EiB", 8.0 * pow1024(6)),
            lin("pebibytes", "PiB", 8.0 * pow1024(5)),
            lin("tebibytes", "TiB", 8.0 * pow1024(4)),
            lin("gibibytes", "GiB", 8.0 * pow1024(3)),
            lin("mebibytes", "MiB", 8.0 * pow1024(2)),
            lin("kibibytes", "KiB", 8.0 * pow1024(1)),
            lin("yobibits", "Yib", pow1024(8)),
            lin("zebibits", "Zib", pow1024(7)),
            lin("exbibits", "Eib", pow1024(6)),
            lin("pebibits", "Pib", pow1024(5)),
            lin("tebibits", "Tib", pow1024(4)),
            lin("gibibits", "Gib", pow1024(3)),
            lin("mebibits", "Mib", pow1024(2)),
            lin("kibibits", "Kib", pow1024(1)),
        }},
        {"NSUnitLength", "meters", {
            lin("megameters", "Mm", 1e6),
            lin("kilometers", "km", 1e3),
            lin("hectometers", "hm", 1e2),
            lin("decameters", "dam", 1e1),
            lin("meters", "m", 1.0),
            lin("decimeters", "dm", 1e-1),
            lin("centimeters", "cm", 1e-2),
            lin("millimeters", "mm", 1e-3),
            lin("micrometers", "\xc2\xb5m", 1e-6),
            lin("nanometers", "nm", 1e-9),
            lin("picometers", "pm", 1e-12),
            lin("inches", "in", 0.0254),
            lin("feet", "ft", 0.3048),
            lin("yards", "yd", 0.9144),
            lin("miles", "mi", 1609.34),
            lin("scandinavianMiles", "smi", 10000.0),
            lin("lightyears", "ly", 9.461e15),
            lin("nauticalMiles", "NM", 1852.0),
            lin("fathoms", "ftm", 1.8288),
            lin("furlongs", "fur", 201.168),
            lin("astronomicalUnits", "ua", 1.496e11),
            lin("parsecs", "pc", 3.086e16),
        }},
        {"NSUnitMass", "kilograms", {
            lin("kilograms", "kg", 1.0),
            lin("grams", "g", 1e-3),
            lin("decigrams", "dg", 1e-4),
            lin("centigrams", "cg", 1e-5),
            lin("milligrams", "mg", 1e-6),
            lin("micrograms", "\xc2\xb5g", 1e-9),
            lin("nanograms", "ng", 1e-12),
            lin("picograms", "pg", 1e-15),
            lin("ounces", "oz", 0.0283495),
            lin("pounds", "lb", 0.453592),
            lin("stones", "st", 6.35029),
            lin("metricTons", "t", 1000.0),
            lin("shortTons", "ton", 907.185),
            lin("carats", "ct", 0.0002),
            lin("ouncesTroy", "oz t", 0.03110348),
            lin("slugs", "slug", 14.5939),
        }},
        {"NSUnitPower", "watts", {
            lin("terawatts", "TW", 1e12),
            lin("gigawatts", "GW", 1e9),
            lin("megawatts", "MW", 1e6),
            lin("kilowatts", "kW", 1e3),
            lin("watts", "W", 1.0),
            lin("milliwatts", "mW", 1e-3),
            lin("microwatts", "\xc2\xb5W", 1e-6),
            lin("nanowatts", "nW", 1e-9),
            lin("picowatts", "pW", 1e-12),
            lin("femtowatts", "fW", 1e-15),
            lin("horsepower", "hp", 745.7),
        }},
        {"NSUnitPressure", "newtonsPerMetersSquared", {
            lin("newtonsPerMetersSquared", "N/m\xc2\xb2", 1.0),
            lin("gigapascals", "GPa", 1e9),
            lin("megapascals", "MPa", 1e6),
            lin("kilopascals", "kPa", 1e3),
            lin("hectopascals", "hPa", 100.0),
            lin("inchesOfMercury", "inHg", 3386.39),
            lin("bars", "bar", 1e5),
            lin("millibars", "mbar", 100.0),
            lin("millimetersOfMercury", "mmHg", 133.322),
            lin("poundsForcePerSquareInch", "psi", 6894.76),
        }},
        {"NSUnitSpeed", "metersPerSecond", {
            lin("metersPerSecond", "m/s", 1.0),
            lin("kilometersPerHour", "km/h", 1000.0 / 3600.0),
            lin("milesPerHour", "mph", 1609.34 / 3600.0),
            lin("knots", "kn", 1852.0 / 3600.0),
        }},
        {"NSUnitTemperature", "kelvin", {
            lin("kelvin", "K", 1.0, 0.0),
            lin("celsius", "\xc2\xb0" "C", 1.0, 273.15),
            lin("fahrenheit", "\xc2\xb0" "F", 5.0 / 9.0, 459.67 * 5.0 / 9.0),
        }},
        {"NSUnitVolume", "liters", {
            lin("megaliters", "ML", 1e6),
            lin("kiloliters", "kL", 1e3),
            lin("liters", "L", 1.0),
            lin("deciliters", "dL", 0.1),
            lin("centiliters", "cL", 0.01),
            lin("milliliters", "mL", 1e-3),
            lin("cubicKilometers", "km\xc2\xb3", 1e12),
            lin("cubicMeters", "m\xc2\xb3", 1000.0),
            lin("cubicDecimeters", "dm\xc2\xb3", 1.0),
            lin("cubicCentimeters", "cm\xc2\xb3", 1e-3),
            lin("cubicMillimeters", "mm\xc2\xb3", 1e-6),
            lin("cubicInches", "in\xc2\xb3", 0.0163871),
            lin("cubicFeet", "ft\xc2\xb3", 28.3168),
            lin("cubicYards", "yd\xc2\xb3", 764.555),
            lin("cubicMiles", "mi\xc2\xb3", 4.168e12),
            lin("acreFeet", "af", 1.233e6),
            lin("bushels", "bsh", 35.2391),
            lin("teaspoons", "tsp", 0.00492892),
            lin("tablespoons", "tbsp", 0.0147868),
            lin("fluidOunces", "fl oz", 0.0295735),
            lin("cups", "cup", 0.24),
            lin("pints", "pt", 0.473176),
            lin("quarts", "qt", 0.946353),
            lin("gallons", "gal", 3.78541),
            lin("imperialTeaspoons", "tsp", 0.00591939),
            lin("imperialTablespoons", "tbsp", 0.0177582),
            lin("imperialFluidOunces", "fl oz", 0.0284131),
            lin("imperialPints", "pt", 0.568261),
            lin("imperialQuarts", "qt", 1.13652),
            lin("imperialGallons", "gal", 4.54609),
            lin("metricCups", "metric cup", 0.25),
        }},
    };
}

struct UnitsRegistry {
    explicit UnitsRegistry(Support& owner) : support(owner) {}
    Support& support;
    std::mutex mutex;
    Address unit_cls{};
    Address converter_cls{};
    Address linear_cls{};
    Address reciprocal_cls{};
    Address dimension_cls{};
    Address measurement_cls{};
    std::map<std::pair<Address, std::string>, Address> singletons;

    Address make_linear(double coefficient, double constant) {
        const auto obj = support.objc.allocate(linear_cls);
        auto st = support.objc.host_state<LinearConverterState>(obj);
        st->coefficient = coefficient;
        st->constant = constant;
        return obj;   // +1
    }

    Address make_reciprocal(double reciprocal) {
        const auto obj = support.objc.allocate(reciprocal_cls);
        support.objc.host_state<ReciprocalConverterState>(obj)->reciprocal = reciprocal;
        return obj;   // +1
    }

    Address cached_dimension(Address cls, const UnitSpec& spec) {
        std::lock_guard lock(mutex);
        const auto key = std::make_pair(cls, std::string(spec.selector));
        if (const auto found = singletons.find(key); found != singletons.end()) return found->second;
        const auto conv = spec.reciprocal ? make_reciprocal(spec.coefficient) : make_linear(spec.coefficient, spec.constant);
        const auto sym = support.cf.string(spec.symbol);
        const auto obj = support.objc.allocate(cls);
        auto st = support.objc.host_state<UnitState>(obj);
        st->symbol = sym;
        st->converter = conv;
        if (const auto rec = support.process.objects().find(obj)) rec->immortal = true;
        if (const auto rec = support.process.objects().find(sym)) rec->immortal = true;
        if (const auto rec = support.process.objects().find(conv)) rec->immortal = true;
        singletons[key] = obj;
        return obj;
    }

    // Whether two NSUnit objects are mutually convertible: either equal, or both
    // concrete NSDimension subclasses sharing the same +baseUnit.
    bool can_convert(Address from_unit, Address to_unit) {
        if (!from_unit || !to_unit) return false;
        if (is_equal(support, from_unit, to_unit)) return true;
        if (!is_kind_of(support, from_unit, dimension_cls) || !is_kind_of(support, to_unit, dimension_cls)) return false;
        const auto c1 = support.send_id(from_unit, "class");
        const auto c2 = support.send_id(to_unit, "class");
        if (!c1 || !c2 || c1 == dimension_cls || c2 == dimension_cls) return false;
        if (c1 == c2) return true;
        const auto b1 = support.send_id(c1, "baseUnit");
        const auto b2 = support.send_id(c2, "baseUnit");
        return b1 && b2 && is_equal(support, b1, b2);
    }

    double to_base(Address unit, double value) {
        const auto conv = support.send_id(unit, "converter");
        if (!conv) fail("NSInvalidArgumentException: NSDimension has no converter");
        CallRegs r;
        r.v[0] = {std::bit_cast<std::uint64_t>(value), 0};
        return std::bit_cast<double>(support.send(conv, "baseUnitValueFromValue:", r).v[0][0]);
    }

    double from_base(Address unit, double base_value) {
        const auto conv = support.send_id(unit, "converter");
        if (!conv) fail("NSInvalidArgumentException: NSDimension has no converter");
        CallRegs r;
        r.v[0] = {std::bit_cast<std::uint64_t>(base_value), 0};
        return std::bit_cast<double>(support.send(conv, "valueFromBaseUnitValue:", r).v[0][0]);
    }

    Address make_measurement(Address cls, double value, Address unit) {
        const auto obj = support.objc.allocate(cls ? cls : measurement_cls);
        auto st = support.objc.host_state<MeasurementState>(obj);
        st->value = value;
        st->unit = unit ? support.send_id(unit, "copy") : 0;
        return obj;   // +1
    }
};

} // namespace

void install_units(Support& support) {
    auto reg = std::make_shared<UnitsRegistry>(support);
    const auto key = [&support](const char* name) { return support.cf.constant_string(name); };

    // ---- NSUnit ----
    const auto unit_cls = support.declare_class("NSUnit", "NSObject", foundation_library);
    reg->unit_cls = unit_cls;
    support.method(unit_cls, "init", [&support, unit_cls](CpuState& s, GuestMemory&) {
        const auto self = support.objc.send_super(s.x[0], unit_cls, support.objc.selector("init")).x0;
        if (self) {
            auto st = support.objc.host_state<UnitState>(self);
            if (!st->symbol) st->symbol = support.cf.string("");
        }
        s.x[0] = self;
    }, "@@:");
    support.method(unit_cls, "initWithSymbol:", [&support, unit_cls](CpuState& s, GuestMemory&) {
        const auto sym_arg = s.x[2];
        const auto self = support.objc.send_super(s.x[0], unit_cls, support.objc.selector("init")).x0;
        if (self) {
            auto st = support.objc.host_state<UnitState>(self);
            if (st->symbol) objc_release(support, st->symbol);
            st->symbol = sym_arg ? support.send_id(sym_arg, "copy") : support.cf.string("");
        }
        s.x[0] = self;
    }, "@@:@");
    support.method(unit_cls, "symbol", [&support](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<UnitState>(s.x[0]);
        s.x[0] = st && st->symbol ? st->symbol : support.cf.constant_string("");
    }, "@@:");
    support.method(unit_cls, "isEqual:", [&support, unit_cls](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], other = s.x[2];
        if (self == other) { s.x[0] = 1; return; }
        if (!other || !is_kind_of(support, other, unit_cls)) { s.x[0] = 0; return; }
        if (support.send_id(self, "class") != support.send_id(other, "class")) { s.x[0] = 0; return; }
        s.x[0] = is_equal(support, support.send_id(self, "symbol"), support.send_id(other, "symbol")) ? 1 : 0;
    }, "B@:@");
    support.method(unit_cls, "hash", [&support](CpuState& s, GuestMemory&) {
        const auto sym = support.send_id(s.x[0], "symbol");
        s.x[0] = sym ? support.send_id(sym, "hash") : 0;
    }, "Q@:");
    support.method(unit_cls, "copyWithZone:", [&support](CpuState& s, GuestMemory&) {
        s.x[0] = objc_retain(support, s.x[0]);
    }, "@@:^{_NSZone=}");
    support.method(unit_cls, "description", [&support](CpuState& s, GuestMemory&) {
        s.x[0] = support.send_id(s.x[0], "symbol");
    }, "@@:");
    support.method(unit_cls, "dealloc", [&support, unit_cls](CpuState& s, GuestMemory&) {
        if (const auto st = support.objc.find_host_state<UnitState>(s.x[0]); st && st->symbol) {
            objc_release(support, st->symbol);
            st->symbol = 0;
        }
        static_cast<void>(support.objc.send_super(s.x[0], unit_cls, support.objc.selector("dealloc")));
    }, "v@:");
    support.method(unit_cls, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    support.method(unit_cls, "encodeWithCoder:", [&support, key](CpuState& s, GuestMemory&) {
        const auto sym = support.send_id(s.x[0], "symbol");
        support.send_id(s.x[2], "encodeObject:forKey:", {sym, key("NS.symbol")});
    }, "v@:@");
    support.method(unit_cls, "initWithCoder:", [&support, key](CpuState& s, GuestMemory&) {
        const auto sym = support.send_id(s.x[2], "decodeObjectForKey:", {key("NS.symbol")});
        if (!sym) { objc_release(support, s.x[0]); s.x[0] = 0; return; }
        s.x[0] = support.send_id(s.x[0], "initWithSymbol:", {sym});
    }, "@@:@");

    // ---- NSUnitConverter ----
    const auto converter_cls = support.declare_class("NSUnitConverter", "NSObject", foundation_library);
    reg->converter_cls = converter_cls;
    support.method(converter_cls, "baseUnitValueFromValue:", [](CpuState&, GuestMemory&) {
        fail("NSInvalidArgumentException: -[NSUnitConverter baseUnitValueFromValue:] must be overridden by a subclass");
    }, "d@:d");
    support.method(converter_cls, "valueFromBaseUnitValue:", [](CpuState&, GuestMemory&) {
        fail("NSInvalidArgumentException: -[NSUnitConverter valueFromBaseUnitValue:] must be overridden by a subclass");
    }, "d@:d");
    support.method(converter_cls, "copyWithZone:", [&support](CpuState& s, GuestMemory&) {
        s.x[0] = objc_retain(support, s.x[0]);
    }, "@@:^{_NSZone=}");

    // ---- NSUnitConverterLinear : NSUnitConverter ----
    const auto linear_cls = support.declare_class("NSUnitConverterLinear", "NSUnitConverter", foundation_library);
    reg->linear_cls = linear_cls;
    support.method(linear_cls, "initWithCoefficient:constant:", [&support, linear_cls](CpuState& s, GuestMemory&) {
        const double coeff = arg_double(s, 0), constant = arg_double(s, 1);
        const auto self = support.objc.send_super(s.x[0], linear_cls, support.objc.selector("init")).x0;
        if (self) {
            auto st = support.objc.host_state<LinearConverterState>(self);
            st->coefficient = coeff;
            st->constant = constant;
        }
        s.x[0] = self;
    }, "@@:dd");
    support.method(linear_cls, "initWithCoefficient:", [&support](CpuState& s, GuestMemory&) {
        CallRegs r;
        r.v[0] = s.v[0];
        r.v[1] = {std::bit_cast<std::uint64_t>(0.0), 0};
        s.x[0] = support.send(s.x[0], "initWithCoefficient:constant:", r).x0;
    }, "@@:d");
    support.method(linear_cls, "coefficient", [&support](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<LinearConverterState>(s.x[0]);
        return_double(s, st ? st->coefficient : 1.0);
    }, "d@:");
    support.method(linear_cls, "constant", [&support](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<LinearConverterState>(s.x[0]);
        return_double(s, st ? st->constant : 0.0);
    }, "d@:");
    support.method(linear_cls, "baseUnitValueFromValue:", [&support](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<LinearConverterState>(s.x[0]);
        const double v = arg_double(s, 0);
        return_double(s, st ? v * st->coefficient + st->constant : v);
    }, "d@:d");
    support.method(linear_cls, "valueFromBaseUnitValue:", [&support](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<LinearConverterState>(s.x[0]);
        const double v = arg_double(s, 0);
        return_double(s, st ? (v - st->constant) / st->coefficient : v);
    }, "d@:d");
    support.method(linear_cls, "isEqual:", [&support, linear_cls](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], other = s.x[2];
        if (self == other) { s.x[0] = 1; return; }
        if (!other || !is_kind_of(support, other, linear_cls)) { s.x[0] = 0; return; }
        const auto a = support.objc.find_host_state<LinearConverterState>(self);
        const auto b = support.objc.find_host_state<LinearConverterState>(other);
        s.x[0] = a && b && a->coefficient == b->coefficient && a->constant == b->constant ? 1 : 0;
    }, "B@:@");
    support.method(linear_cls, "hash", [&support](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<LinearConverterState>(s.x[0]);
        const double coeff = st ? st->coefficient : 1.0, constant = st ? st->constant : 0.0;
        s.x[0] = hash_double(coeff) ^ (hash_double(constant) * 0x9e3779b97f4a7c15ULL);
    }, "Q@:");
    support.method(linear_cls, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    support.method(linear_cls, "encodeWithCoder:", [&support, key](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<LinearConverterState>(s.x[0]);
        const double coeff = st ? st->coefficient : 1.0, constant = st ? st->constant : 0.0;
        CallRegs r1; r1.v[0] = {std::bit_cast<std::uint64_t>(coeff), 0}; r1.x[2] = key("NS.coefficient");
        static_cast<void>(support.send(s.x[2], "encodeDouble:forKey:", r1));
        CallRegs r2; r2.v[0] = {std::bit_cast<std::uint64_t>(constant), 0}; r2.x[2] = key("NS.constant");
        static_cast<void>(support.send(s.x[2], "encodeDouble:forKey:", r2));
    }, "v@:@");
    support.method(linear_cls, "initWithCoder:", [&support, key](CpuState& s, GuestMemory&) {
        CallRegs q1; q1.x[2] = key("NS.coefficient");
        const double coeff = std::bit_cast<double>(support.send(s.x[2], "decodeDoubleForKey:", q1).v[0][0]);
        CallRegs q2; q2.x[2] = key("NS.constant");
        const double constant = std::bit_cast<double>(support.send(s.x[2], "decodeDoubleForKey:", q2).v[0][0]);
        CallRegs init;
        init.v[0] = {std::bit_cast<std::uint64_t>(coeff), 0};
        init.v[1] = {std::bit_cast<std::uint64_t>(constant), 0};
        s.x[0] = support.send(s.x[0], "initWithCoefficient:constant:", init).x0;
    }, "@@:@");

    // ---- _NSUnitConverterReciprocal : NSUnitConverter (for NSUnitFuelEfficiency) ----
    const auto reciprocal_cls = support.declare_class("_NSUnitConverterReciprocal", "NSUnitConverter", foundation_library);
    reg->reciprocal_cls = reciprocal_cls;
    support.method(reciprocal_cls, "baseUnitValueFromValue:", [&support](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<ReciprocalConverterState>(s.x[0]);
        const double v = arg_double(s, 0);
        return_double(s, st ? st->reciprocal / v : v);
    }, "d@:d");
    support.method(reciprocal_cls, "valueFromBaseUnitValue:", [&support](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<ReciprocalConverterState>(s.x[0]);
        const double v = arg_double(s, 0);
        return_double(s, st ? st->reciprocal / v : v);
    }, "d@:d");
    support.method(reciprocal_cls, "isEqual:", [&support, reciprocal_cls](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], other = s.x[2];
        if (self == other) { s.x[0] = 1; return; }
        if (!other || !is_kind_of(support, other, reciprocal_cls)) { s.x[0] = 0; return; }
        const auto a = support.objc.find_host_state<ReciprocalConverterState>(self);
        const auto b = support.objc.find_host_state<ReciprocalConverterState>(other);
        s.x[0] = a && b && a->reciprocal == b->reciprocal ? 1 : 0;
    }, "B@:@");
    support.method(reciprocal_cls, "hash", [&support](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<ReciprocalConverterState>(s.x[0]);
        s.x[0] = hash_double(st ? st->reciprocal : 1.0);
    }, "Q@:");
    support.method(reciprocal_cls, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    support.method(reciprocal_cls, "encodeWithCoder:", [&support, key](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<ReciprocalConverterState>(s.x[0]);
        CallRegs r; r.v[0] = {std::bit_cast<std::uint64_t>(st ? st->reciprocal : 1.0), 0}; r.x[2] = key("NS.reciprocal");
        static_cast<void>(support.send(s.x[2], "encodeDouble:forKey:", r));
    }, "v@:@");
    support.method(reciprocal_cls, "initWithCoder:", [&support, reciprocal_cls, key](CpuState& s, GuestMemory&) {
        CallRegs q; q.x[2] = key("NS.reciprocal");
        const double value = std::bit_cast<double>(support.send(s.x[2], "decodeDoubleForKey:", q).v[0][0]);
        const auto self = support.objc.send_super(s.x[0], reciprocal_cls, support.objc.selector("init")).x0;
        if (self) support.objc.host_state<ReciprocalConverterState>(self)->reciprocal = value;
        s.x[0] = self;
    }, "@@:@");

    // ---- NSDimension : NSUnit ----
    const auto dimension_cls = support.declare_class("NSDimension", "NSUnit", foundation_library);
    reg->dimension_cls = dimension_cls;
    support.method(dimension_cls, "initWithSymbol:converter:", [&support, dimension_cls](CpuState& s, GuestMemory&) {
        const auto sym = s.x[2], conv = s.x[3];
        if (!conv) fail("NSInvalidArgumentException: -[NSDimension initWithSymbol:converter:] requires a converter");
        CallRegs r; r.x[2] = sym;
        const auto self = support.objc.send_super(s.x[0], dimension_cls, support.objc.selector("initWithSymbol:"), r).x0;
        if (self) {
            auto st = support.objc.host_state<UnitState>(self);
            if (st->converter) objc_release(support, st->converter);
            st->converter = support.send_id(conv, "copy");
        }
        s.x[0] = self;
    }, "@@:@@");
    support.method(dimension_cls, "converter", [&support](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<UnitState>(s.x[0]);
        s.x[0] = st ? st->converter : 0;
    }, "@@:");
    support.method(dimension_cls, "baseUnit", [](CpuState&, GuestMemory&) {
        fail("NSInvalidArgumentException: +[NSDimension baseUnit] must be overridden by a subclass");
    }, "@@:", true);
    support.method(dimension_cls, "isEqual:", [&support, dimension_cls](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], other = s.x[2];
        if (self == other) { s.x[0] = 1; return; }
        if (!other || !is_kind_of(support, other, dimension_cls)) { s.x[0] = 0; return; }
        if (support.send_id(self, "class") != support.send_id(other, "class")) { s.x[0] = 0; return; }
        const bool sym_eq = is_equal(support, support.send_id(self, "symbol"), support.send_id(other, "symbol"));
        const bool conv_eq = is_equal(support, support.send_id(self, "converter"), support.send_id(other, "converter"));
        s.x[0] = sym_eq && conv_eq ? 1 : 0;
    }, "B@:@");
    support.method(dimension_cls, "hash", [&support](CpuState& s, GuestMemory&) {
        const auto sym = support.send_id(s.x[0], "symbol");
        const auto conv = support.send_id(s.x[0], "converter");
        const auto h1 = sym ? support.send_id(sym, "hash") : 0;
        const auto h2 = conv ? support.send_id(conv, "hash") : 0;
        s.x[0] = h1 ^ (h2 * 0x9e3779b97f4a7c15ULL);
    }, "Q@:");
    support.method(dimension_cls, "dealloc", [&support, dimension_cls](CpuState& s, GuestMemory&) {
        if (const auto st = support.objc.find_host_state<UnitState>(s.x[0]); st && st->converter) {
            objc_release(support, st->converter);
            st->converter = 0;
        }
        static_cast<void>(support.objc.send_super(s.x[0], dimension_cls, support.objc.selector("dealloc")));
    }, "v@:");
    support.method(dimension_cls, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    support.method(dimension_cls, "encodeWithCoder:", [&support, dimension_cls, key](CpuState& s, GuestMemory&) {
        CallRegs r; r.x[2] = s.x[2];
        static_cast<void>(support.objc.send_super(s.x[0], dimension_cls, support.objc.selector("encodeWithCoder:"), r));
        const auto conv = support.send_id(s.x[0], "converter");
        support.send_id(s.x[2], "encodeObject:forKey:", {conv, key("NS.converter")});
    }, "v@:@");
    support.method(dimension_cls, "initWithCoder:", [&support, key](CpuState& s, GuestMemory&) {
        const auto sym = support.send_id(s.x[2], "decodeObjectForKey:", {key("NS.symbol")});
        const auto conv = support.send_id(s.x[2], "decodeObjectForKey:", {key("NS.converter")});
        if (!sym || !conv) { objc_release(support, s.x[0]); s.x[0] = 0; return; }
        s.x[0] = support.send_id(s.x[0], "initWithSymbol:converter:", {sym, conv});
    }, "@@:@");

    // ---- Standard NSDimension subclasses ----
    for (const auto& dim : standard_dimensions()) {
        const auto sub = support.declare_class(dim.class_name, "NSDimension", foundation_library);
        UnitSpec base_spec{};
        bool has_base = false;
        for (const auto& unit : dim.units) {
            if (std::string_view(unit.selector) == dim.base_selector) {
                base_spec = unit;
                has_base = true;
            }
            support.method(sub, unit.selector, [reg, sub, unit](CpuState& s, GuestMemory&) {
                s.x[0] = reg->cached_dimension(sub, unit);
            }, "@@:", true);
        }
        if (has_base) {
            support.method(sub, "baseUnit", [reg, sub, base_spec](CpuState& s, GuestMemory&) {
                s.x[0] = reg->cached_dimension(sub, base_spec);
            }, "@@:", true);
        }
        if (std::string_view(dim.class_name) == "NSUnitConcentrationMass") {
            support.method(sub, "millimolesPerLiterWithGramsPerMole:", [reg, sub](CpuState& s, GuestMemory&) {
                const double grams_per_mole = arg_double(s, 0);
                const auto conv = reg->make_linear(grams_per_mole * 0.001, 0.0);
                const auto sym = reg->support.cf.string("mmol/L");
                const auto obj = reg->support.objc.allocate(sub);
                auto st = reg->support.objc.host_state<UnitState>(obj);
                st->symbol = sym;
                st->converter = conv;
                s.x[0] = autoreleased(reg->support, obj);
            }, "@@:d", true);
        }
    }

    // ---- NSMeasurement ----
    const auto measurement_cls = support.declare_class("NSMeasurement", "NSObject", foundation_library);
    reg->measurement_cls = measurement_cls;
    support.method(measurement_cls, "initWithDoubleValue:unit:", [&support, measurement_cls](CpuState& s, GuestMemory&) {
        const double value = arg_double(s, 0);
        const auto unit = s.x[2];
        if (!unit) fail("NSInvalidArgumentException: -[NSMeasurement initWithDoubleValue:unit:] requires a unit");
        const auto self = support.objc.send_super(s.x[0], measurement_cls, support.objc.selector("init")).x0;
        if (self) {
            auto st = support.objc.host_state<MeasurementState>(self);
            st->value = value;
            if (st->unit) objc_release(support, st->unit);
            st->unit = support.send_id(unit, "copy");
        }
        s.x[0] = self;
    }, "@@:d@");
    support.method(measurement_cls, "doubleValue", [&support](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<MeasurementState>(s.x[0]);
        return_double(s, st ? st->value : 0.0);
    }, "d@:");
    support.method(measurement_cls, "unit", [&support](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<MeasurementState>(s.x[0]);
        s.x[0] = st ? st->unit : 0;
    }, "@@:");
    support.method(measurement_cls, "canBeConvertedToUnit:", [reg](CpuState& s, GuestMemory&) {
        const auto st = reg->support.objc.find_host_state<MeasurementState>(s.x[0]);
        s.x[0] = st && reg->can_convert(st->unit, s.x[2]) ? 1 : 0;
    }, "B@:@");
    support.method(measurement_cls, "measurementByConvertingToUnit:", [reg](CpuState& s, GuestMemory&) {
        const auto st = reg->support.objc.find_host_state<MeasurementState>(s.x[0]);
        const auto target = s.x[2];
        if (!st || !target) fail("NSInvalidArgumentException: -[NSMeasurement measurementByConvertingToUnit:]: nil unit");
        if (is_equal(reg->support, st->unit, target)) {
            s.x[0] = autoreleased(reg->support, reg->make_measurement(0, st->value, target));
            return;
        }
        if (!reg->can_convert(st->unit, target))
            fail("NSInvalidArgumentException: -[NSMeasurement measurementByConvertingToUnit:]: incompatible units");
        const double base_value = reg->to_base(st->unit, st->value);
        const double converted = reg->from_base(target, base_value);
        s.x[0] = autoreleased(reg->support, reg->make_measurement(0, converted, target));
    }, "@@:@");

    const auto combine = [reg](CpuState& s, bool add) {
        const auto a = reg->support.objc.find_host_state<MeasurementState>(s.x[0]);
        const auto b = s.x[2] ? reg->support.objc.find_host_state<MeasurementState>(s.x[2]) : nullptr;
        if (!a || !b) fail("NSInvalidArgumentException: NSMeasurement arithmetic requires an NSMeasurement operand");
        if (is_equal(reg->support, a->unit, b->unit)) {
            const double result = add ? (a->value + b->value) : (a->value - b->value);
            s.x[0] = autoreleased(reg->support, reg->make_measurement(0, result, a->unit));
            return;
        }
        if (!reg->can_convert(a->unit, b->unit))
            fail("NSInvalidArgumentException: NSMeasurement arithmetic with incompatible units");
        const auto cls = reg->support.send_id(a->unit, "class");
        const auto base_unit = reg->support.send_id(cls, "baseUnit");
        const double b1 = reg->to_base(a->unit, a->value);
        const double b2 = reg->to_base(b->unit, b->value);
        const double result = add ? (b1 + b2) : (b1 - b2);
        s.x[0] = autoreleased(reg->support, reg->make_measurement(0, result, base_unit));
    };
    support.method(measurement_cls, "measurementByAddingMeasurement:", [combine](CpuState& s, GuestMemory&) { combine(s, true); }, "@@:@");
    support.method(measurement_cls, "measurementBySubtractingMeasurement:", [combine](CpuState& s, GuestMemory&) { combine(s, false); }, "@@:@");
    support.method(measurement_cls, "isEqual:", [&support, measurement_cls](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], other = s.x[2];
        if (self == other) { s.x[0] = 1; return; }
        if (!other || !is_kind_of(support, other, measurement_cls)) { s.x[0] = 0; return; }
        const auto a = support.objc.find_host_state<MeasurementState>(self);
        const auto b = support.objc.find_host_state<MeasurementState>(other);
        s.x[0] = a && b && a->value == b->value && is_equal(support, a->unit, b->unit) ? 1 : 0;
    }, "B@:@");
    support.method(measurement_cls, "hash", [&support](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<MeasurementState>(s.x[0]);
        const auto h1 = hash_double(st ? st->value : 0.0);
        const auto h2 = st && st->unit ? support.send_id(st->unit, "hash") : 0;
        s.x[0] = h1 ^ (h2 * 0x9e3779b97f4a7c15ULL);
    }, "Q@:");
    support.method(measurement_cls, "copyWithZone:", [&support](CpuState& s, GuestMemory&) {
        s.x[0] = objc_retain(support, s.x[0]);
    }, "@@:^{_NSZone=}");
    support.method(measurement_cls, "description", [&support](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<MeasurementState>(s.x[0]);
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "%g", st ? st->value : 0.0);
        const auto sym = st && st->unit ? text(support, support.send_id(st->unit, "symbol")) : std::string();
        s.x[0] = string(support, sym.empty() ? std::string(buf) : std::string(buf) + " " + sym);
    }, "@@:");
    support.method(measurement_cls, "dealloc", [&support, measurement_cls](CpuState& s, GuestMemory&) {
        if (const auto st = support.objc.find_host_state<MeasurementState>(s.x[0]); st && st->unit) {
            objc_release(support, st->unit);
            st->unit = 0;
        }
        static_cast<void>(support.objc.send_super(s.x[0], measurement_cls, support.objc.selector("dealloc")));
    }, "v@:");
    support.method(measurement_cls, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    support.method(measurement_cls, "encodeWithCoder:", [&support, key](CpuState& s, GuestMemory&) {
        const auto st = support.objc.find_host_state<MeasurementState>(s.x[0]);
        CallRegs r; r.v[0] = {std::bit_cast<std::uint64_t>(st ? st->value : 0.0), 0}; r.x[2] = key("NS.value");
        static_cast<void>(support.send(s.x[2], "encodeDouble:forKey:", r));
        support.send_id(s.x[2], "encodeObject:forKey:", {st ? st->unit : 0, key("NS.unit")});
    }, "v@:@");
    support.method(measurement_cls, "initWithCoder:", [&support, key](CpuState& s, GuestMemory&) {
        CallRegs q; q.x[2] = key("NS.value");
        const double value = std::bit_cast<double>(support.send(s.x[2], "decodeDoubleForKey:", q).v[0][0]);
        const auto unit = support.send_id(s.x[2], "decodeObjectForKey:", {key("NS.unit")});
        if (!unit) { objc_release(support, s.x[0]); s.x[0] = 0; return; }
        CallRegs init;
        init.v[0] = {std::bit_cast<std::uint64_t>(value), 0};
        init.x[2] = unit;
        s.x[0] = support.send(s.x[0], "initWithDoubleValue:unit:", init).x0;
    }, "@@:@");
}

} // namespace alchemy::gui::completion_kit
