#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/foundation/decimal.hpp"

#include <cmath>

// NSDecimalNumber (an NSNumber subclass holding an NSDecimal), the
// NSDecimalNumberHandler behaviors and -[NSNumber decimalValue], over the
// decimal core (decimal.cpp).
//   Arithmetic applies the behavior's scale and rounding mode; calculation
//   errors go to its -exceptionDuringOperation:error:leftOperand:rightOperand:
//   (a handler that raises stops the process with the Foundation exception
//   name: NSDecimalNumberExactnessException, ...OverflowException,
//   ...UnderflowException, ...DivideByZeroException). The default behavior
//   rounds plainly, keeps every digit and raises on overflow, underflow and
//   division by zero.
//   NSNumber accessors are answered from the decimal (integers truncate);
//   -objCType is "d" as in Foundation.
namespace alchemy::gui::completion_kit::data {
namespace {

using decimal::Decimal;

struct DecimalState final : ObjectPayload { Decimal value; };
struct HandlerState final : ObjectPayload {
    decimal::Rounding rounding{decimal::Rounding::plain};
    int scale{decimal::no_scale};
    bool raise_exactness{}, raise_overflow{true}, raise_underflow{true}, raise_divide_by_zero{true};
};

struct Decimals {
    Support& s;
    Guest g{s};
    Address number_class{}, decimal_class{}, handler_class{};
    Address default_behavior{};   // +1 (a handler), or the one set with +setDefaultBehavior:

    Decimal value(Address object) {
        if (const auto st = s.objc.find_host_state<DecimalState>(object)) return st->value;
        if (!object) fail("NSInvalidArgumentException: nil NSDecimalNumber operand");
        const auto kind = s.cf.kind(object);
        if (kind == ObjectKind::number || kind == ObjectKind::boolean) {
            const auto type = s.send(object, "objCType").x0;
            const char code = type ? static_cast<char>(g.memory().read8(type)) : 'q';
            if (code == 'f' || code == 'd') return Decimal::from_double(*s.cf.number_value(object));
            if (code == 'Q') return Decimal::from_unsigned(s.send(object, "unsignedLongLongValue").x0);
            return Decimal::from_integer(static_cast<std::int64_t>(s.send(object, "longLongValue").x0));
        }
        fail("NSInvalidArgumentException: an NSDecimalNumber operand that is not a number");
    }
    Address make(Address cls, Decimal v) {
        const auto self = s.objc.allocate(cls ? cls : decimal_class);
        s.objc.host_state<DecimalState>(self)->value = std::move(v);
        return self;   // +1
    }
    Address behavior(Address explicit_behavior) { return explicit_behavior ? explicit_behavior : default_behavior; }
    void round_with(Address behavior_object, Decimal& v) {
        if (const auto h = s.objc.find_host_state<HandlerState>(behavior_object)) { decimal::round(v, v, h->scale, h->rounding); return; }
        const auto scale = static_cast<std::int16_t>(s.send(behavior_object, "scale").x0);
        const auto mode = static_cast<decimal::Rounding>(s.send(behavior_object, "roundingMode").x0 & 3);
        decimal::round(v, v, scale, mode);
    }
    // The result object (+1) after the behavior has seen the calculation error.
    Address finish(Address behavior_object, Address operation, decimal::Error error, Decimal result, Address left, Address right) {
        if (error != decimal::Error::none) {
            if (const auto h = s.objc.find_host_state<HandlerState>(behavior_object)) {
                const char* name = nullptr;
                switch (error) {
                case decimal::Error::loss_of_precision: if (h->raise_exactness) name = "NSDecimalNumberExactnessException"; break;
                case decimal::Error::overflow: if (h->raise_overflow) name = "NSDecimalNumberOverflowException"; break;
                case decimal::Error::underflow: if (h->raise_underflow) name = "NSDecimalNumberUnderflowException"; break;
                case decimal::Error::divide_by_zero: if (h->raise_divide_by_zero) name = "NSDecimalNumberDivideByZeroException"; break;
                default: break;
                }
                if (name) fail(std::string(name) + ": NSDecimalNumber calculation error " + std::to_string(static_cast<int>(error)));
            } else {
                CallRegs r;
                r.x[2] = operation;
                r.x[3] = static_cast<Address>(error);
                r.x[4] = left;
                r.x[5] = right;
                if (const auto replacement = s.send(behavior_object, "exceptionDuringOperation:error:leftOperand:rightOperand:", r).x0) return g.retain(replacement);
            }
        }
        return make(decimal_class, std::move(result));
    }
    using Op = decimal::Error (*)(Decimal&, const Decimal&, const Decimal&);
    Address binary(Address self, Address other, Address explicit_behavior, Op op, const char* selector) {
        if (!other) fail(std::string("NSInvalidArgumentException: -[NSDecimalNumber ") + selector + "]: nil operand");
        Decimal result;
        const auto error = op(result, value(self), value(other));
        const auto b = behavior(explicit_behavior);
        round_with(b, result);
        return finish(b, s.objc.selector(selector), error, std::move(result), self, other);
    }
    std::u16string separator_of(Address locale) {
        if (!locale) return u".";
        Address value = 0;
        if (s.cf.kind(locale) == ObjectKind::dictionary) value = s.send_id(locale, "objectForKey:", {s.cf.constant_string("NSDecimalSeparator")});
        else if (g.responds(locale, "decimalSeparator")) value = s.send_id(locale, "decimalSeparator");
        const auto text = g.text(value);
        return text.empty() ? std::u16string(u".") : text;
    }
    Decimal parse(Address string, Address locale) {
        const auto parsed = string ? decimal::parse(g.text(string), separator_of(locale)) : std::nullopt;
        return parsed ? *parsed : Decimal::not_a_number();
    }
    void write_decimal(Address at, const Decimal& v) {
        if (!at) fail("an NSDecimal result without a destination");
        const auto bytes = decimal::pack(v);
        g.memory().write_bytes(at, bytes);
    }
    Decimal read_decimal(Address at) {
        decimal::Bytes bytes{};
        g.memory().read_bytes(at, bytes);
        return decimal::unpack(bytes);
    }
};

} // namespace

void install_decimals(Support& support) {
    auto dn = std::make_shared<Decimals>(Decimals{support});
    auto* sp = &support;
    dn->number_class = support.objc.find_class("NSNumber");
    if (!dn->number_class) fail("NSNumber is not registered");
    const auto dc = support.declare_class("NSDecimalNumber", "NSNumber", foundation_library);
    const auto hc = support.declare_class("NSDecimalNumberHandler", "NSObject", foundation_library);
    dn->decimal_class = dc;
    dn->handler_class = hc;
    for (const auto* name : {"NSDecimalNumberExactnessException", "NSDecimalNumberOverflowException", "NSDecimalNumberUnderflowException", "NSDecimalNumberDivideByZeroException"})
        if (!support.process.resolve(foundation_library, std::string("_") + name)) support.string_constant(foundation_library, std::string("_") + name, name);
    {
        const auto handler = support.objc.allocate(hc);
        support.objc.host_state<HandlerState>(handler);
        dn->default_behavior = handler;
    }

    // ---- NSDecimalNumberHandler ----
    const auto configure_handler = [sp](Address self, CpuState& s) {
        auto h = sp->objc.host_state<HandlerState>(self);
        h->rounding = static_cast<decimal::Rounding>(s.x[2] & 3);
        h->scale = static_cast<std::int16_t>(s.x[3]);
        h->raise_exactness = arg_bool(s.x[4]);
        h->raise_overflow = arg_bool(s.x[5]);
        h->raise_underflow = arg_bool(s.x[6]);
        h->raise_divide_by_zero = arg_bool(s.x[7]);
    };
    support.method(hc, "decimalNumberHandlerWithRoundingMode:scale:raiseOnExactness:raiseOnOverflow:raiseOnUnderflow:raiseOnDivideByZero:", [dn, configure_handler](CpuState& s, GuestMemory&) {
        const auto self = dn->s.objc.allocate(s.x[0]);
        configure_handler(self, s);
        s.x[0] = dn->g.autorelease(self);
    }, "@@:QsBBBB", true);
    support.method(hc, "initWithRoundingMode:scale:raiseOnExactness:raiseOnOverflow:raiseOnUnderflow:raiseOnDivideByZero:", [configure_handler](CpuState& s, GuestMemory&) {
        configure_handler(s.x[0], s);
    }, "@@:QsBBBB");
    support.method(hc, "init", [sp](CpuState& s, GuestMemory&) { sp->objc.host_state<HandlerState>(s.x[0]); }, "@@:");
    support.method(hc, "defaultDecimalNumberHandler", [dn](CpuState& s, GuestMemory&) {
        static Address shared = 0;
        if (!shared) { shared = dn->s.objc.allocate(dn->handler_class); dn->s.objc.host_state<HandlerState>(shared); }
        s.x[0] = shared;
    }, "@@:", true);
    support.method(hc, "roundingMode", [sp](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(sp->objc.host_state<HandlerState>(s.x[0])->rounding); }, "Q@:");
    support.method(hc, "scale", [sp](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint16_t>(static_cast<std::int16_t>(sp->objc.host_state<HandlerState>(s.x[0])->scale)); }, "s@:");
    support.method(hc, "exceptionDuringOperation:error:leftOperand:rightOperand:", [dn](CpuState& s, GuestMemory&) {
        // Raising handlers stop in Decimals::finish; here a non-raising answer: use the computed result.
        static_cast<void>(dn);
        s.x[0] = 0;
    }, "@@::Q@@");

    // ---- NSDecimalNumber: creation ----
    for (const auto* name : {"alloc", "allocWithZone:"})
        support.method(dc, name, [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->objc.allocate(s.x[0]); }, name[5] ? "@@:^v" : "@@:", true);
    const auto constant = [dn](Decimal v) {
        auto holder = std::make_shared<Address>(0);
        return [dn, v, holder](CpuState& s, GuestMemory&) {
            if (!*holder) *holder = dn->make(dn->decimal_class, v);
            s.x[0] = *holder;
        };
    };
    support.method(dc, "zero", constant(Decimal{}), "@@:", true);
    support.method(dc, "one", constant(Decimal::from_integer(1)), "@@:", true);
    support.method(dc, "notANumber", constant(Decimal::not_a_number()), "@@:", true);
    {
        Decimal max;
        max.digits = std::string(decimal::max_digits, '9');
        max.exponent = 127;
        Decimal min = max;
        min.negative = true;
        support.method(dc, "maximumDecimalNumber", constant(max), "@@:", true);
        support.method(dc, "minimumDecimalNumber", constant(min), "@@:", true);
    }
    support.method(dc, "decimalNumberWithMantissa:exponent:isNegative:", [dn](CpuState& s, GuestMemory&) {
        s.x[0] = dn->g.autorelease(dn->make(s.x[0], Decimal::from_parts(s.x[2], static_cast<std::int16_t>(s.x[3]), arg_bool(s.x[4]))));
    }, "@@:QsB", true);
    support.method(dc, "initWithMantissa:exponent:isNegative:", [sp](CpuState& s, GuestMemory&) {
        sp->objc.host_state<DecimalState>(s.x[0])->value = Decimal::from_parts(s.x[2], static_cast<std::int16_t>(s.x[3]), arg_bool(s.x[4]));
    }, "@@:QsB");
    support.method(dc, "decimalNumberWithDecimal:", [dn](CpuState& s, GuestMemory&) { s.x[0] = dn->g.autorelease(dn->make(s.x[0], dn->read_decimal(s.x[2]))); }, "@@:{NSDecimal=b8b4b1b1b18[8S]}", true);
    support.method(dc, "initWithDecimal:", [dn](CpuState& s, GuestMemory&) { dn->s.objc.host_state<DecimalState>(s.x[0])->value = dn->read_decimal(s.x[2]); }, "@@:{NSDecimal=b8b4b1b1b18[8S]}");
    support.method(dc, "decimalNumberWithString:", [dn](CpuState& s, GuestMemory&) { s.x[0] = dn->g.autorelease(dn->make(s.x[0], dn->parse(s.x[2], 0))); }, "@@:@", true);
    support.method(dc, "decimalNumberWithString:locale:", [dn](CpuState& s, GuestMemory&) { s.x[0] = dn->g.autorelease(dn->make(s.x[0], dn->parse(s.x[2], s.x[3]))); }, "@@:@@", true);
    support.method(dc, "initWithString:", [dn](CpuState& s, GuestMemory&) { dn->s.objc.host_state<DecimalState>(s.x[0])->value = dn->parse(s.x[2], 0); }, "@@:@");
    support.method(dc, "initWithString:locale:", [dn](CpuState& s, GuestMemory&) { dn->s.objc.host_state<DecimalState>(s.x[0])->value = dn->parse(s.x[2], s.x[3]); }, "@@:@@");
    // NSNumber initializers on NSDecimalNumber (+numberWithInt: and friends inherited from NSNumber).
    for (const auto* name : {"initWithInt:", "initWithInteger:", "initWithLong:", "initWithLongLong:", "initWithShort:", "initWithChar:"})
        support.method(dc, name, [sp, name](CpuState& s, GuestMemory&) {
            std::int64_t v = static_cast<std::int64_t>(s.x[2]);
            const std::string n = name;
            if (n == "initWithInt:") v = static_cast<std::int32_t>(s.x[2]);
            else if (n == "initWithShort:") v = static_cast<std::int16_t>(s.x[2]);
            else if (n == "initWithChar:") v = static_cast<std::int8_t>(s.x[2]);
            sp->objc.host_state<DecimalState>(s.x[0])->value = Decimal::from_integer(v);
        }, "@@:q");
    for (const auto* name : {"initWithUnsignedInt:", "initWithUnsignedInteger:", "initWithUnsignedLong:", "initWithUnsignedLongLong:"})
        support.method(dc, name, [sp, name](CpuState& s, GuestMemory&) {
            const auto v = std::string(name) == "initWithUnsignedInt:" ? static_cast<std::uint32_t>(s.x[2]) : s.x[2];
            sp->objc.host_state<DecimalState>(s.x[0])->value = Decimal::from_unsigned(v);
        }, "@@:Q");
    support.method(dc, "initWithDouble:", [sp](CpuState& s, GuestMemory&) { sp->objc.host_state<DecimalState>(s.x[0])->value = Decimal::from_double(std::bit_cast<double>(s.v[0][0])); }, "@@:d");
    support.method(dc, "initWithFloat:", [sp](CpuState& s, GuestMemory&) {
        sp->objc.host_state<DecimalState>(s.x[0])->value = Decimal::from_double(std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0])));
    }, "@@:f");
    support.method(dc, "initWithBool:", [sp](CpuState& s, GuestMemory&) { sp->objc.host_state<DecimalState>(s.x[0])->value = Decimal::from_integer(arg_bool(s.x[2]) ? 1 : 0); }, "@@:B");
    // +numberWith...: on NSDecimalNumber make decimal numbers ([[self alloc] initWith...:]).
    for (const auto* kind : {"Int", "Integer", "Long", "LongLong", "Short", "Char", "UnsignedInt", "UnsignedInteger", "UnsignedLong", "UnsignedLongLong", "Double", "Float", "Bool"}) {
        const std::string factory = std::string("numberWith") + kind + ":", init = std::string("initWith") + kind + ":";
        support.method(dc, factory, [dn, init](CpuState& s, GuestMemory&) {
            CallRegs r;
            r.x[2] = s.x[2];
            r.v[0] = s.v[0];
            const auto self = dn->s.objc.allocate(s.x[0]);
            s.x[0] = dn->g.autorelease(dn->s.send(self, init, r).x0);
        }, "@@:q", true);
    }
    support.method(dc, "defaultBehavior",[dn](CpuState& s, GuestMemory&) { s.x[0] = dn->default_behavior; }, "@@:", true);
    support.method(dc, "setDefaultBehavior:", [dn](CpuState& s, GuestMemory&) {
        const auto old = std::exchange(dn->default_behavior, s.x[2] ? dn->g.retain(s.x[2]) : dn->g.retain(dn->s.send_id(dn->handler_class, "defaultDecimalNumberHandler")));
        dn->g.release(old);
    }, "v@:@", true);

    // ---- arithmetic ----
    struct Binary { const char* plain; const char* with; Decimals::Op op; };
    for (const auto& b : {Binary{"decimalNumberByAdding:", "decimalNumberByAdding:withBehavior:", decimal::add},
                          Binary{"decimalNumberBySubtracting:", "decimalNumberBySubtracting:withBehavior:", decimal::subtract},
                          Binary{"decimalNumberByMultiplyingBy:", "decimalNumberByMultiplyingBy:withBehavior:", decimal::multiply},
                          Binary{"decimalNumberByDividingBy:", "decimalNumberByDividingBy:withBehavior:", decimal::divide}}) {
        const auto op = b.op;
        const std::string plain = b.plain, with = b.with;
        support.method(dc, plain, [dn, op, plain](CpuState& s, GuestMemory&) { s.x[0] = dn->g.autorelease(dn->binary(s.x[0], s.x[2], 0, op, plain.c_str())); }, "@@:@");
        support.method(dc, with, [dn, op, with](CpuState& s, GuestMemory&) { s.x[0] = dn->g.autorelease(dn->binary(s.x[0], s.x[2], s.x[3], op, with.c_str())); }, "@@:@@");
    }
    const auto raising = [dn](Address self, Address n, Address explicit_behavior) {
        Decimal result;
        const auto error = decimal::power(result, dn->value(self), static_cast<unsigned>(n));
        const auto b = dn->behavior(explicit_behavior);
        dn->round_with(b, result);
        return dn->finish(b, dn->s.objc.selector("decimalNumberByRaisingToPower:"), error, std::move(result), self, 0);
    };
    support.method(dc, "decimalNumberByRaisingToPower:", [dn, raising](CpuState& s, GuestMemory&) { s.x[0] = dn->g.autorelease(raising(s.x[0], s.x[2], 0)); }, "@@:Q");
    support.method(dc, "decimalNumberByRaisingToPower:withBehavior:", [dn, raising](CpuState& s, GuestMemory&) { s.x[0] = dn->g.autorelease(raising(s.x[0], s.x[2], s.x[3])); }, "@@:Q@");
    const auto power10 = [dn](Address self, Address n, Address explicit_behavior) {
        Decimal result;
        const auto error = decimal::multiply_by_power_of_10(result, dn->value(self), static_cast<std::int16_t>(n));
        const auto b = dn->behavior(explicit_behavior);
        dn->round_with(b, result);
        return dn->finish(b, dn->s.objc.selector("decimalNumberByMultiplyingByPowerOf10:"), error, std::move(result), self, 0);
    };
    support.method(dc, "decimalNumberByMultiplyingByPowerOf10:", [dn, power10](CpuState& s, GuestMemory&) { s.x[0] = dn->g.autorelease(power10(s.x[0], s.x[2], 0)); }, "@@:s");
    support.method(dc, "decimalNumberByMultiplyingByPowerOf10:withBehavior:", [dn, power10](CpuState& s, GuestMemory&) { s.x[0] = dn->g.autorelease(power10(s.x[0], s.x[2], s.x[3])); }, "@@:s@");
    support.method(dc, "decimalNumberByRoundingAccordingToBehavior:", [dn](CpuState& s, GuestMemory&) {
        auto v = dn->value(s.x[0]);
        dn->round_with(dn->behavior(s.x[2]), v);
        s.x[0] = dn->g.autorelease(dn->make(dn->decimal_class, std::move(v)));
    }, "@@:@");

    // ---- NSNumber interface ----
    support.method(dc, "decimalValue", [dn](CpuState& s, GuestMemory&) { dn->write_decimal(s.x[8], dn->value(s.x[0])); }, "{NSDecimal=b8b4b1b1b18[8S]}@:");
    support.method(dc, "doubleValue", [dn](CpuState& s, GuestMemory&) { s.v[0] = {std::bit_cast<std::uint64_t>(decimal::to_double(dn->value(s.x[0]))), 0}; }, "d@:");
    support.method(dc, "floatValue", [dn](CpuState& s, GuestMemory&) {
        s.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(decimal::to_double(dn->value(s.x[0])))), 0};
    }, "f@:");
    struct Integer { const char* name; int bits; bool is_signed; const char* types; };
    for (const auto& spec : {Integer{"charValue", 8, true, "c@:"}, Integer{"unsignedCharValue", 8, false, "C@:"}, Integer{"shortValue", 16, true, "s@:"},
                             Integer{"unsignedShortValue", 16, false, "S@:"}, Integer{"intValue", 32, true, "i@:"}, Integer{"unsignedIntValue", 32, false, "I@:"},
                             Integer{"longValue", 64, true, "q@:"}, Integer{"unsignedLongValue", 64, false, "Q@:"}, Integer{"integerValue", 64, true, "q@:"},
                             Integer{"unsignedIntegerValue", 64, false, "Q@:"}, Integer{"longLongValue", 64, true, "q@:"}, Integer{"unsignedLongLongValue", 64, false, "Q@:"}}) {
        const auto bits = spec.bits;
        const bool is_signed = spec.is_signed;
        support.method(dc, spec.name, [dn, bits, is_signed](CpuState& s, GuestMemory&) {
            const auto v = dn->value(s.x[0]);
            // Foundation converts through long long / unsigned long long and truncates.
            const auto raw = is_signed ? static_cast<std::uint64_t>(decimal::to_int64(v))
                                       : (v.negative ? static_cast<std::uint64_t>(decimal::to_int64(v)) : decimal::to_uint64(v));
            if (bits == 64) s.x[0] = raw;
            else if (is_signed) { const auto shift = 64 - bits; s.x[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int64_t>(raw << shift) >> shift)); }
            else s.x[0] = raw & ((1ull << bits) - 1);
        }, spec.types);
    }
    support.method(dc, "boolValue", [dn](CpuState& s, GuestMemory&) { const auto v = dn->value(s.x[0]); return_bool(s, !v.nan && !v.is_zero()); }, "B@:");
    support.method(dc, "objCType", [dn](CpuState& s, GuestMemory&) {
        static Address code = 0;
        if (!code) { code = dn->s.process.allocate_guest(2); dn->g.memory().write8(code, 'd'); dn->g.memory().write8(code + 1, 0); }
        s.x[0] = code;
    }, "r*@:");
    const auto describe = [dn](Address self, Address locale) {
        std::string separator = ".";
        if (locale) { const auto sep = dn->separator_of(locale); separator.clear(); for (const auto c : sep) separator.push_back(c < 0x80 ? static_cast<char>(c) : '.'); }
        return dn->g.auto_string(std::string_view(decimal::to_string(dn->value(self), separator)));
    };
    for (const auto* name : {"description", "stringValue"})
        support.method(dc, name, [describe](CpuState& s, GuestMemory&) { s.x[0] = describe(s.x[0], 0); }, "@@:");
    support.method(dc, "descriptionWithLocale:", [describe](CpuState& s, GuestMemory&) { s.x[0] = describe(s.x[0], s.x[2]); }, "@@:@");
    support.method(dc, "compare:", [dn](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(static_cast<std::int64_t>(decimal::compare(dn->value(s.x[0]), dn->value(s.x[2]))));
    }, "q@:@");
    const auto equal = [dn](Address self, Address other) {
        if (self == other) return true;
        if (!other) return false;
        const auto kind = dn->s.cf.kind(other);
        if (!dn->s.objc.find_host_state<DecimalState>(other) && kind != ObjectKind::number && kind != ObjectKind::boolean) return false;
        return decimal::compare(dn->value(self), dn->value(other)) == 0;
    };
    support.method(dc, "isEqual:", [equal](CpuState& s, GuestMemory&) { return_bool(s, equal(s.x[0], s.x[2])); }, "B@:@");
    support.method(dc, "isEqualToNumber:", [equal](CpuState& s, GuestMemory&) { return_bool(s, equal(s.x[0], s.x[2])); }, "B@:@");
    support.method(dc, "hash", [dn](CpuState& s, GuestMemory&) {
        // Equal NSNumbers hash alike: integral values as integers, others as doubles.
        const auto v = dn->value(s.x[0]);
        Decimal whole;
        decimal::round(whole, v, 0, decimal::Rounding::down);
        const bool integral = decimal::compare(whole, v) == 0 && v.digits.size() + static_cast<std::size_t>(std::max(v.exponent, 0)) <= 18;
        const auto number = integral ? dn->s.cf.integer(decimal::to_int64(v)) : dn->s.cf.real(decimal::to_double(v));
        s.x[0] = dn->s.cf.hash(number);
        dn->s.release(number);
    }, "Q@:");
    support.method(dc, "getValue:", [dn](CpuState& s, GuestMemory& m) { m.write64(s.x[2], std::bit_cast<std::uint64_t>(decimal::to_double(dn->value(s.x[0])))); }, "v@:^v");
    support.method(dc, "copyWithZone:", [dn](CpuState& s, GuestMemory&) { s.x[0] = dn->g.retain(s.x[0]); }, "@@:^v");
    support.method(dc, "copy", [dn](CpuState& s, GuestMemory&) { s.x[0] = dn->g.retain(s.x[0]); }, "@@:");
    support.method(dc, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    support.method(dc, "encodeWithCoder:", [dn](CpuState& s, GuestMemory&) {
        // NSDecimalNumber's keyed coding: the decimal as a string (readable by -initWithCoder: below).
        dn->s.send_id(s.x[2], "encodeObject:forKey:", {dn->s.send_id(s.x[0], "stringValue"), dn->s.cf.constant_string("NS.decimal")});
    }, "v@:@");
    support.method(dc, "initWithCoder:", [dn](CpuState& s, GuestMemory&) {
        const auto text = dn->s.send_id(s.x[2], "decodeObjectForKey:", {dn->s.cf.constant_string("NS.decimal")});
        if (!text || !dn->g.is_string(text)) unsupported("decoding an NSDecimalNumber archived by Foundation (NS.mantissa bytes)");
        dn->s.objc.host_state<DecimalState>(s.x[0])->value = dn->parse(text, 0);
    }, "@@:@");
    static_cast<void>(sp);

    // ---- -[NSNumber decimalValue] ----
    fill_method(support, dn->number_class, "decimalValue", [dn](CpuState& s, GuestMemory&) { dn->write_decimal(s.x[8], dn->value(s.x[0])); }, "{NSDecimal=b8b4b1b1b18[8S]}@:");
}

} // namespace alchemy::gui::completion_kit::data
