// NSUnit, NSUnitConverter, NSUnitConverterLinear, NSDimension, NSMeasurement
// and the standard NSDimension subclasses (units.cpp).
#include "alchemy/gui/tests/framework_case.hpp"

#include <bit>
#include <cmath>
#include <string>

namespace {
using namespace fill_test;

double d_result(const alchemy::CallResult& r) { return std::bit_cast<double>(r.v[0][0]); }

bool close(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

const Case foundation_units_case("foundation_units", [](Fixture& f) {
    const auto send_d = [&](std::uint64_t target, std::string_view sel, double a) {
        alchemy::CallRegs r;
        r.v[0] = {std::bit_cast<std::uint64_t>(a), 0};
        return f.objc.send(target, sel, r);
    };
    const auto send_dd = [&](std::uint64_t target, std::string_view sel, double a, double b) {
        alchemy::CallRegs r;
        r.v[0] = {std::bit_cast<std::uint64_t>(a), 0};
        r.v[1] = {std::bit_cast<std::uint64_t>(b), 0};
        return f.objc.send(target, sel, r);
    };
    const auto make_measurement = [&](double value, std::uint64_t unit) {
        alchemy::CallRegs r;
        r.v[0] = {std::bit_cast<std::uint64_t>(value), 0};
        r.x[2] = unit;
        const auto obj = f.send(f.objc.find_class("NSMeasurement"), "alloc");
        return f.objc.send(obj, "initWithDoubleValue:unit:", r).x0;
    };
    const auto double_val = [&](std::uint64_t obj, std::string_view sel) {
        return d_result(f.objc.send(obj, sel));
    };

    // NSUnit
    const auto unit_cls = f.objc.find_class("NSUnit");
    require(unit_cls != 0, "NSUnit class");
    const auto u1 = f.send(f.send(unit_cls, "alloc"), "initWithSymbol:", {f.cf.string("widgets")});
    const auto u2 = f.send(f.send(unit_cls, "alloc"), "initWithSymbol:", {f.cf.string("widgets")});
    const auto u3 = f.send(f.send(unit_cls, "alloc"), "initWithSymbol:", {f.cf.string("gadgets")});
    require(f.cf.utf8(f.send(u1, "symbol")) == "widgets", "NSUnit symbol");
    require((f.send(u1, "isEqual:", {u2}) & 0xff) == 1 && (f.send(u1, "isEqual:", {u3}) & 0xff) == 0, "NSUnit isEqual:");
    require(f.send(u1, "hash") == f.send(u2, "hash"), "NSUnit hash");
    require(f.send(u1, "copy") == u1, "NSUnit copy is immutable retain");

    // NSUnitConverterLinear
    const auto linear_cls = f.objc.find_class("NSUnitConverterLinear");
    require(linear_cls != 0, "NSUnitConverterLinear class");
    const auto c1 = send_dd(f.send(linear_cls, "alloc"), "initWithCoefficient:constant:", 2.0, 10.0).x0;
    const auto c2 = send_dd(f.send(linear_cls, "alloc"), "initWithCoefficient:constant:", 2.0, 10.0).x0;
    const auto c3 = send_d(f.send(linear_cls, "alloc"), "initWithCoefficient:", 5.0).x0;
    require(close(double_val(c1, "coefficient"), 2.0) && close(double_val(c1, "constant"), 10.0), "linear accessors");
    require(close(double_val(c3, "coefficient"), 5.0) && close(double_val(c3, "constant"), 0.0), "linear default constant");
    require(close(d_result(send_d(c1, "baseUnitValueFromValue:", 3.0)), 16.0), "linear baseUnitValueFromValue:");
    require(close(d_result(send_d(c1, "valueFromBaseUnitValue:", 16.0)), 3.0), "linear valueFromBaseUnitValue:");
    require((f.send(c1, "isEqual:", {c2}) & 0xff) == 1 && (f.send(c1, "isEqual:", {c3}) & 0xff) == 0, "linear isEqual:");
    require(f.send(c1, "hash") == f.send(c2, "hash"), "linear hash");

    // NSUnitInformationStorage (imported by Ice 0.11.12)
    const auto info_cls = f.objc.find_class("NSUnitInformationStorage");
    require(info_cls != 0, "NSUnitInformationStorage class");
    const auto bits = f.send(info_cls, "bits");
    const auto bytes = f.send(info_cls, "bytes");
    const auto mib = f.send(info_cls, "mebibytes");
    const auto mb = f.send(info_cls, "megabytes");
    require(f.send(info_cls, "baseUnit") == bits && f.cf.utf8(f.send(bits, "symbol")) == "bit", "info baseUnit is bits");
    require(f.cf.utf8(f.send(bytes, "symbol")) == "B" && f.cf.utf8(f.send(mib, "symbol")) == "MiB", "info symbols");
    const auto one_mib = make_measurement(1.0, mib);
    const auto in_bytes = f.send(one_mib, "measurementByConvertingToUnit:", {bytes});
    const auto in_bits = f.send(one_mib, "measurementByConvertingToUnit:", {bits});
    require(close(double_val(in_bytes, "doubleValue"), 1048576.0, 1e-3), "1 MiB == 1048576 B");
    require(close(double_val(in_bits, "doubleValue"), 8388608.0, 1e-3), "1 MiB == 8388608 bits");
    const auto one_mb = make_measurement(1.0, mb);
    const auto mb_in_bytes = f.send(one_mb, "measurementByConvertingToUnit:", {bytes});
    require(close(double_val(mb_in_bytes, "doubleValue"), 1000000.0, 1e-3), "1 MB == 1000000 B");

    // NSUnitTemperature (affine linear converter)
    const auto temp_cls = f.objc.find_class("NSUnitTemperature");
    const auto kelvin = f.send(temp_cls, "kelvin");
    const auto celsius = f.send(temp_cls, "celsius");
    const auto fahrenheit = f.send(temp_cls, "fahrenheit");
    require(f.send(temp_cls, "baseUnit") == kelvin, "temperature baseUnit is kelvin");
    const auto freezing = make_measurement(0.0, celsius);
    const auto freezing_f = f.send(freezing, "measurementByConvertingToUnit:", {fahrenheit});
    const auto freezing_k = f.send(freezing, "measurementByConvertingToUnit:", {kelvin});
    require(close(double_val(freezing_f, "doubleValue"), 32.0, 1e-4), "0 C == 32 F");
    require(close(double_val(freezing_k, "doubleValue"), 273.15, 1e-4), "0 C == 273.15 K");
    const auto boiling_f = make_measurement(212.0, fahrenheit);
    const auto boiling_c = f.send(boiling_f, "measurementByConvertingToUnit:", {celsius});
    require(close(double_val(boiling_c, "doubleValue"), 100.0, 1e-4), "212 F == 100 C");

    // NSUnitLength & NSMeasurement arithmetic / equality / compatibility
    const auto len_cls = f.objc.find_class("NSUnitLength");
    const auto meters = f.send(len_cls, "meters");
    const auto km = f.send(len_cls, "kilometers");
    const auto cm = f.send(len_cls, "centimeters");
    const auto m_km = make_measurement(1.0, km);
    const auto m_m = make_measurement(1000.0, meters);
    const auto m_m2 = make_measurement(1000.0, meters);
    require((f.send(m_km, "canBeConvertedToUnit:", {cm}) & 0xff) == 1, "km convertible to cm");
    require((f.send(m_km, "canBeConvertedToUnit:", {celsius}) & 0xff) == 0, "km not convertible to celsius");
    require((f.send(m_km, "isEqual:", {m_m}) & 0xff) == 0, "NSMeasurement isEqual: requires same unit");
    require((f.send(m_m, "isEqual:", {m_m2}) & 0xff) == 1 && f.send(m_m, "hash") == f.send(m_m2, "hash"), "NSMeasurement equal + hash");
    // Adding same units keeps the unit; different units converts to baseUnit.
    const auto sum_same = f.send(m_km, "measurementByAddingMeasurement:", {make_measurement(2.5, km)});
    require(close(double_val(sum_same, "doubleValue"), 3.5) && f.send(sum_same, "unit") == km, "add same unit");
    const auto sum_mixed = f.send(m_km, "measurementByAddingMeasurement:", {make_measurement(500.0, meters)});
    require(close(double_val(sum_mixed, "doubleValue"), 1500.0) && f.send(sum_mixed, "unit") == meters, "add mixed units -> baseUnit");
    const auto diff_mixed = f.send(m_km, "measurementBySubtractingMeasurement:", {make_measurement(25000.0, cm)});
    require(close(double_val(diff_mixed, "doubleValue"), 750.0) && f.send(diff_mixed, "unit") == meters, "sub mixed units -> baseUnit");

    // Plain NSUnit measurement: same unit converts/adds, different unit refuses.
    const auto w1 = make_measurement(4.0, u1);
    const auto w2 = make_measurement(3.0, u2);
    require((f.send(w1, "canBeConvertedToUnit:", {u2}) & 0xff) == 1 && (f.send(w1, "canBeConvertedToUnit:", {u3}) & 0xff) == 0, "plain NSUnit compatibility");
    const auto w_sum = f.send(w1, "measurementByAddingMeasurement:", {w2});
    require(close(double_val(w_sum, "doubleValue"), 7.0), "plain NSUnit add");

    // NSUnitFuelEfficiency (reciprocal converter)
    const auto fuel_cls = f.objc.find_class("NSUnitFuelEfficiency");
    const auto l100 = f.send(fuel_cls, "litersPer100Kilometers");
    const auto mpg = f.send(fuel_cls, "milesPerGallon");
    const auto eff = make_measurement(23.5215, mpg);
    const auto eff_l = f.send(eff, "measurementByConvertingToUnit:", {l100});
    require(close(double_val(eff_l, "doubleValue"), 10.0, 1e-4), "fuel efficiency reciprocal mpg -> L/100km");
    const auto eff_back = f.send(eff_l, "measurementByConvertingToUnit:", {mpg});
    require(close(double_val(eff_back, "doubleValue"), 23.5215, 1e-4), "fuel efficiency reciprocal L/100km -> mpg");

    // NSUnitConcentrationMass dynamic unit
    const auto conc_cls = f.objc.find_class("NSUnitConcentrationMass");
    const auto mmol = send_d(conc_cls, "millimolesPerLiterWithGramsPerMole:", 180.16).x0;
    const auto glucose = make_measurement(5.0, mmol);
    const auto glucose_mgdl = f.send(glucose, "measurementByConvertingToUnit:", {f.send(conc_cls, "milligramsPerDeciliter")});
    require(close(double_val(glucose_mgdl, "doubleValue"), 90.08, 1e-3), "millimolesPerLiterWithGramsPerMole:");

    // NSUnitDuration & NSUnitSpeed spot checks + all standard classes exist
    const auto dur_cls = f.objc.find_class("NSUnitDuration");
    const auto one_hr = make_measurement(1.0, f.send(dur_cls, "hours"));
    require(close(double_val(f.send(one_hr, "measurementByConvertingToUnit:", {f.send(dur_cls, "milliseconds")}), "doubleValue"), 3600000.0), "1 hr == 3.6e6 ms");
    for (const auto* name : {"NSUnitAcceleration", "NSUnitAngle", "NSUnitArea", "NSUnitConcentrationMass", "NSUnitDispersion",
                             "NSUnitDuration", "NSUnitElectricCharge", "NSUnitElectricCurrent", "NSUnitElectricPotentialDifference",
                             "NSUnitElectricResistance", "NSUnitEnergy", "NSUnitFrequency", "NSUnitFuelEfficiency", "NSUnitIlluminance",
                             "NSUnitInformationStorage", "NSUnitLength", "NSUnitMass", "NSUnitPower", "NSUnitPressure",
                             "NSUnitSpeed", "NSUnitTemperature", "NSUnitVolume"}) {
        const auto cls = f.objc.find_class(name);
        require(cls != 0 && f.send(cls, "baseUnit") != 0, std::string(name) + " registered with +baseUnit");
    }

    // NSSecureCoding round trip via NSKeyedArchiver / NSKeyedUnarchiver
    const auto archiver = f.objc.find_class("NSKeyedArchiver");
    const auto unarchiver = f.objc.find_class("NSKeyedUnarchiver");
    const auto archived = f.send(archiver, "archivedDataWithRootObject:", {m_km});
    require(archived != 0, "archived NSMeasurement");
    const auto restored = f.send(unarchiver, "unarchiveObjectWithData:", {archived});
    require(restored != 0 && (f.send(restored, "isEqual:", {m_km}) & 0xff) == 1, "unarchived NSMeasurement equals original");

    // Incompatible conversion throws NSInvalidArgumentException
    bool threw = false;
    try {
        static_cast<void>(f.send(m_km, "measurementByConvertingToUnit:", {celsius}));
    } catch (const std::exception&) {
        threw = true;
    }
    require(threw, "incompatible unit conversion raises");
});

} // namespace
