#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/foundation/predicate.hpp"
#include "alchemy/darwin/foundation/regex.hpp"
#include "alchemy/darwin/kit/completion/data/text.hpp"

// NSPredicate, NSComparisonPredicate and NSCompoundPredicate over the format
// parser (predicate.cpp), and the collection filters
// (-filteredArrayUsingPredicate:, -[NSMutableArray filterUsingPredicate:],
// -filteredSetUsingPredicate:, -[NSMutableSet filterUsingPredicate:],
// -filteredOrderedSetUsingPredicate:, -[NSMutableOrderedSet filterUsingPredicate:]).
// Evaluation reads key paths with -valueForKeyPath:. Values compare as
// Foundation's comparison predicates do: NSNull is nil; numbers numerically
// (NSNumber and NSDecimalNumber through -doubleValue); strings with the
// [c]/[d] options (CFString order, no locale collation); anything else
// with -isEqual: and -compare:. MATCHES is a whole-string regular expression.
// NSExpression objects (-leftExpression, +predicateWithLeftExpression:...)
// are not provided and fail explicitly.
namespace alchemy::gui::completion_kit::data {
namespace {

using predicate::Expression;
using predicate::Node;
using predicate::Operator;

struct PredicateState final : ObjectPayload {
    std::shared_ptr<Node> node;
    std::vector<Address> retained;   // +1: %@ objects, foreign subpredicates
    Address block{};                 // +1 block copy (predicateWithBlock:)
    bool initialized{};
};

struct Predicates {
    Support& s;
    Guest g{s};
    Address predicate_class{}, comparison_class{}, compound_class{}, number_class{}, set_class{}, ordered_class{}, null_class{};

    std::shared_ptr<PredicateState> state(Address self) {
        auto st = s.objc.find_host_state<PredicateState>(self);
        if (!st || !st->initialized) fail("NSPredicate used before initialization");
        return st;
    }
    Address make(Address cls, std::shared_ptr<Node> node, std::vector<Address> retained, Address block = 0) {
        const auto self = s.objc.allocate(cls);
        auto st = s.objc.host_state<PredicateState>(self);
        st->node = std::move(node);
        st->retained = std::move(retained);
        st->block = block;
        st->initialized = true;
        return self;   // +1
    }
    Address class_for(const Node& n) {
        if (n.kind == Node::Kind::comparison) return comparison_class;
        if (n.kind == Node::Kind::conjunction || n.kind == Node::Kind::disjunction || n.kind == Node::Kind::negation) return compound_class;
        return predicate_class;
    }

    // ---- parsing with format arguments ----
    Address parse(Address format, const predicate::Arguments& arguments, std::vector<Address>& retained) {
        predicate::Status status;
        std::string message;
        auto node = predicate::parse(g.text(format), arguments, status, message);
        if (!node) {
            for (const auto r : retained) g.release(r);
            if (status == predicate::Status::unsupported) unsupported("NSPredicate format \"" + g.utf8(format) + "\": " + message);
            fail("NSInvalidArgumentException: Unable to parse the format string \"" + g.utf8(format) + "\": " + message);
        }
        auto shared = std::make_shared<Node>(std::move(*node));
        return make(class_for(*shared), shared, std::move(retained));
    }
    Expression object_argument(Address object, std::vector<Address>& retained, char conversion) {
        Expression e;
        if (conversion == 'K') {
            if (!object || !g.is_string(object)) fail("NSInvalidArgumentException: NSPredicate %K needs a string key path");
            e.kind = Expression::Kind::key_path;
            e.text = g.text(object);
            return e;
        }
        if (!object) return e;
        e.kind = Expression::Kind::object;
        e.object = g.retain(object);
        retained.push_back(e.object);
        return e;
    }
    // Variadic slots (Darwin arm64 va_list: 8-byte slots).
    predicate::Arguments va_arguments(Address cursor, std::vector<Address>& retained) {
        auto at = std::make_shared<Address>(cursor);
        return [this, at, &retained](char conversion, std::string_view length) {
            const auto raw = g.memory().read64(*at);
            *at += 8;
            Expression e;
            switch (conversion) {
            case '@': case 'K': return object_argument(raw, retained, conversion);
            case 'f': case 'e': case 'E': case 'g': case 'G':
                e.kind = Expression::Kind::real;
                e.real = std::bit_cast<double>(raw);
                return e;
            case 'd': case 'i': case 'D': case 'c': case 'C':
                e.kind = Expression::Kind::integer;
                e.integer = length.empty() || length == "h" ? static_cast<std::int64_t>(static_cast<std::int32_t>(raw)) : static_cast<std::int64_t>(raw);
                return e;
            default:
                e.kind = Expression::Kind::integer;
                e.integer = length.empty() || length == "h" ? static_cast<std::int64_t>(static_cast<std::uint32_t>(raw)) : static_cast<std::int64_t>(raw);
                return e;
            }
        };
    }
    predicate::Arguments array_arguments(Address array, std::vector<Address>& retained) {
        auto items = std::make_shared<std::vector<Address>>(g.elements(array));
        auto next = std::make_shared<std::size_t>(0);
        return [this, items, next, &retained](char conversion, std::string_view) {
            if (*next >= items->size()) fail("NSInvalidArgumentException: NSPredicate format needs more arguments than the argument array has");
            const auto object = (*items)[(*next)++];
            if (conversion == '@' || conversion == 'K') return object_argument(object, retained, conversion);
            Expression e;
            const auto number = s.cf.number_value(object);
            if (!number) fail("NSInvalidArgumentException: NSPredicate numeric format argument is not a number");
            if (conversion == 'f' || conversion == 'e' || conversion == 'E' || conversion == 'g' || conversion == 'G') { e.kind = Expression::Kind::real; e.real = *number; }
            else { e.kind = Expression::Kind::integer; e.integer = static_cast<std::int64_t>(s.send(object, "longLongValue").x0); }
            return e;
        };
    }

    // ---- evaluation ----
    bool is_null(Address v) { return !v || (null_class && s.objc.object_class(v) == null_class); }
    bool is_number(Address v) {
        if (!v) return false;
        const auto k = s.cf.kind(v);
        if (k == ObjectKind::number || k == ObjectKind::boolean) return true;
        return number_class && g.is_kind(v, number_class);
    }
    double number(Address v) {
        if (const auto n = s.cf.number_value(v)) return *n;
        return std::bit_cast<double>(s.send(v, "doubleValue").v[0][0]);
    }
    bool is_collection(Address v) {
        if (!v) return false;
        const auto k = s.cf.kind(v);
        if (k == ObjectKind::array || k == ObjectKind::dictionary) return true;
        return (set_class && g.is_kind(v, set_class)) || (ordered_class && g.is_kind(v, ordered_class));
    }
    std::vector<Address> members(Address v) {
        if (s.cf.kind(v) == ObjectKind::dictionary) {
            std::vector<Address> values;
            for (const auto& [key, value] : s.cf.dictionary_entries(v)) values.push_back(value);
            return values;
        }
        return g.elements(v);
    }
    Address value(const Expression& e, Address object, Address variables) {
        switch (e.kind) {
        case Expression::Kind::null: return 0;
        case Expression::Kind::boolean: return s.cf.boolean(e.boolean);
        case Expression::Kind::integer: return g.autorelease(s.cf.integer(e.integer));
        case Expression::Kind::real: return g.autorelease(s.cf.real(e.real));
        case Expression::Kind::string: return g.auto_string(e.text);
        case Expression::Kind::object: return e.object;
        case Expression::Kind::self: return object;
        case Expression::Kind::key_path: return object ? g.value_for_key_path(object, g.auto_string(e.text)) : 0;
        case Expression::Kind::variable: {
            const auto bound = variables ? s.send_id(variables, "objectForKey:", {g.auto_string(e.text)}) : 0;
            if (!bound) fail("NSInvalidArgumentException: Can't get value for '" + text_of(e.text) + "' in bindings");
            return bound;
        }
        case Expression::Kind::aggregate: {
            std::vector<Address> items;
            for (const auto& item : e.items) {
                const auto v = value(item, object, variables);
                items.push_back(v ? v : s.send_id(null_class, "null"));
            }
            return g.auto_array(items);
        }
        }
        return 0;
    }
    static std::string text_of(const std::u16string& v) { std::string out; for (const auto c : v) out.push_back(c < 0x80 ? static_cast<char>(c) : '?'); return out; }
    std::uint64_t string_options(unsigned options) {
        std::uint64_t o = 0;
        if (options & predicate::case_insensitive) o |= text::case_insensitive_search;
        if (options & predicate::diacritic_insensitive) o |= text::diacritic_insensitive_search;
        return o;
    }
    bool equal(Address l, Address r, unsigned options) {
        if (is_null(l) || is_null(r)) return is_null(l) && is_null(r);
        if (is_number(l) && is_number(r)) return number(l) == number(r);
        if (g.is_string(l) && g.is_string(r)) return text::compare(g.text(l), g.text(r), string_options(options)) == 0;
        return g.is_equal(l, r);
    }
    std::optional<int> order(Address l, Address r, unsigned options) {
        if (is_null(l) || is_null(r)) return std::nullopt;
        if (is_number(l) && is_number(r)) { const auto a = number(l), b = number(r); return a < b ? -1 : a > b ? 1 : 0; }
        if (g.is_string(l) && g.is_string(r)) return text::compare(g.text(l), g.text(r), string_options(options));
        if (!g.responds(l, "compare:")) return std::nullopt;
        const auto c = g.compare(l, r, "compare:");
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    bool strings(Address l, Address r) { return g.is_string(l) && g.is_string(r); }
    bool compare(Operator op, unsigned options, Address l, Address r) {
        switch (op) {
        case Operator::equal: return equal(l, r, options);
        case Operator::not_equal: return !equal(l, r, options);
        case Operator::less: { const auto o = order(l, r, options); return o && *o < 0; }
        case Operator::less_or_equal: { const auto o = order(l, r, options); return o && *o <= 0; }
        case Operator::greater: { const auto o = order(l, r, options); return o && *o > 0; }
        case Operator::greater_or_equal: { const auto o = order(l, r, options); return o && *o >= 0; }
        case Operator::begins_with: case Operator::ends_with: {
            if (!strings(l, r)) return false;
            const auto a = g.text(l), b = g.text(r);
            if (b.empty() || b.size() > a.size()) return b.empty();
            const auto at = op == Operator::begins_with ? 0 : a.size() - b.size();
            return text::compare(std::u16string_view(a).substr(at, b.size()), b, string_options(options)) == 0;
        }
        case Operator::like: return strings(l, r) && predicate::like(g.text(l), g.text(r), options);
        case Operator::matches: {
            if (!strings(l, r)) return false;
            regex::Status status;
            std::string message;
            // The whole string must match: (?:pattern)\z anchored at the start.
            const auto pattern = regex::Pattern::compile(u"(?:" + g.text(r) + u")\\z", (options & predicate::case_insensitive) ? regex::case_insensitive : 0, status, message);
            if (!pattern) {
                if (status == regex::Status::unsupported) unsupported("NSPredicate MATCHES pattern: " + message);
                fail("NSInvalidArgumentException: Can't do regex matching, reason: " + message);
            }
            const auto subject = g.text(l);
            return pattern->search(subject, {0, static_cast<std::int64_t>(subject.size())}, 0, regex::anchored).has_value();
        }
        case Operator::contains:
            if (is_collection(l)) { for (const auto m : members(l)) if (equal(m, r, options)) return true; return false; }
            if (strings(l, r)) { const auto a = g.text(l), b = g.text(r); return !b.empty() && text::find(a, b, string_options(options), 0, static_cast<std::int64_t>(a.size())).location >= 0; }
            return false;
        case Operator::in:
            if (is_collection(r)) { for (const auto m : members(r)) if (equal(l, m, options)) return true; return false; }
            if (strings(l, r)) { const auto a = g.text(r), b = g.text(l); return !b.empty() && text::find(a, b, string_options(options), 0, static_cast<std::int64_t>(a.size())).location >= 0; }
            return false;
        case Operator::between: {
            const auto bounds = is_collection(r) ? members(r) : std::vector<Address>{};
            if (bounds.size() != 2) fail("NSInvalidArgumentException: BETWEEN needs a two-element array");
            const auto lo = order(l, bounds[0], options), hi = order(l, bounds[1], options);
            return lo && hi && *lo >= 0 && *hi <= 0;
        }
        }
        return false;
    }
    bool evaluate(const Node& n, Address object, Address variables) {
        switch (n.kind) {
        case Node::Kind::truth: return true;
        case Node::Kind::falsity: return false;
        case Node::Kind::negation: return !evaluate(n.children.at(0), object, variables);
        case Node::Kind::conjunction: for (const auto& c : n.children) if (!evaluate(c, object, variables)) return false; return true;
        case Node::Kind::disjunction: for (const auto& c : n.children) if (evaluate(c, object, variables)) return true; return false;
        case Node::Kind::foreign: return evaluate_object(n.object, object, variables);
        case Node::Kind::comparison: {
            const auto l = value(n.left, object, variables);
            const auto r = value(n.right, object, variables);
            if (n.modifier == predicate::Modifier::direct) return compare(n.op, n.options, l, r);
            if (!is_null(l) && !is_collection(l)) fail("NSInvalidArgumentException: The left hand side for an ALL or ANY operator must be either an NSArray or an NSSet");
            const auto items = is_null(l) ? std::vector<Address>{} : members(l);
            if (n.modifier == predicate::Modifier::all) { for (const auto m : items) if (!compare(n.op, n.options, m, r)) return false; return true; }
            bool any = false;
            for (const auto m : items) if (compare(n.op, n.options, m, r)) { any = true; break; }
            return n.none ? !any : any;
        }
        }
        return false;
    }
    bool evaluate_object(Address predicate_object, Address object, Address variables) {
        if (const auto st = s.objc.find_host_state<PredicateState>(predicate_object); st && st->initialized) {
            if (st->block) return (g.invoke_block(st->block, {object, variables}) & 0xff) != 0;
            return evaluate(*st->node, object, variables);
        }
        CallRegs r;
        r.x[2] = object;
        if (variables) { r.x[3] = variables; return (s.send(predicate_object, "evaluateWithObject:substitutionVariables:", r).x0 & 0xff) != 0; }
        return (s.send(predicate_object, "evaluateWithObject:", r).x0 & 0xff) != 0;
    }
    std::u16string describe(Address handle) {
        if (const auto st = s.objc.find_host_state<PredicateState>(handle); st && st->initialized && !st->block) {
            const auto inner = predicate::format(*st->node, [this](std::uint64_t h) { return describe(h); });
            const bool compound = st->node->kind == Node::Kind::conjunction || st->node->kind == Node::Kind::disjunction;
            return compound ? u"(" + inner + u")" : inner;
        }
        if (g.is_string(handle)) return predicate::format(Expression{Expression::Kind::string, false, 0, 0.0, g.text(handle), 0, {}}, {});
        if (g.responds(handle, "predicateFormat")) return g.text(s.send_id(handle, "predicateFormat"));
        return g.text(s.send_id(handle, "description"));
    }
    std::vector<Address> filtered(Address collection, Address predicate_object) {
        std::vector<Address> kept;
        for (const auto v : g.elements(collection)) if (evaluate_object(predicate_object, v, 0)) kept.push_back(v);
        return kept;
    }
};

} // namespace

void install_predicates(Support& support) {
    auto pr = std::make_shared<Predicates>(Predicates{support});
    auto* sp = &support;
    const auto np = support.declare_class("NSPredicate", "NSObject", foundation_library);
    const auto cmp = support.declare_class("NSComparisonPredicate", "NSPredicate", foundation_library);
    const auto cpd = support.declare_class("NSCompoundPredicate", "NSPredicate", foundation_library);
    pr->predicate_class = np;
    pr->comparison_class = cmp;
    pr->compound_class = cpd;
    pr->number_class = support.objc.find_class("NSNumber");
    pr->set_class = support.objc.find_class("NSSet");
    pr->ordered_class = support.objc.find_class("NSOrderedSet");
    pr->null_class = support.objc.find_class("NSNull");

    // ---- NSPredicate ----
    support.method(np, "predicateWithFormat:", [pr](CpuState& s, GuestMemory&) {
        std::vector<Address> retained;
        s.x[0] = pr->g.autorelease(pr->parse(s.x[2], pr->va_arguments(s.sp, retained), retained));
    }, "@@:@", true);
    support.method(np, "predicateWithFormat:arguments:", [pr](CpuState& s, GuestMemory&) {
        std::vector<Address> retained;
        s.x[0] = pr->g.autorelease(pr->parse(s.x[2], pr->va_arguments(s.x[3], retained), retained));
    }, "@@:@*", true);
    support.method(np, "predicateWithFormat:argumentArray:", [pr](CpuState& s, GuestMemory&) {
        std::vector<Address> retained;
        s.x[0] = pr->g.autorelease(pr->parse(s.x[2], pr->array_arguments(s.x[3], retained), retained));
    }, "@@:@@", true);
    support.method(np, "predicateWithValue:", [pr, np](CpuState& s, GuestMemory&) {
        auto node = std::make_shared<Node>();
        node->kind = arg_bool(s.x[2]) ? Node::Kind::truth : Node::Kind::falsity;
        s.x[0] = pr->g.autorelease(pr->make(np, node, {}));
    }, "@@:B", true);
    support.method(np, "predicateWithBlock:", [pr, np](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("NSInvalidArgumentException: +[NSPredicate predicateWithBlock:]: nil block");
        const auto block = pr->s.call(libsystem_library, "__Block_copy", {s.x[2]});
        s.x[0] = pr->g.autorelease(pr->make(np, std::make_shared<Node>(), {}, block));
    }, "@@:@?", true);
    support.method(np, "evaluateWithObject:", [pr](CpuState& s, GuestMemory&) { return_bool(s, pr->evaluate_object(s.x[0], s.x[2], 0)); }, "B@:@");
    support.method(np, "evaluateWithObject:substitutionVariables:", [pr](CpuState& s, GuestMemory&) {
        return_bool(s, pr->evaluate_object(s.x[0], s.x[2], s.x[3]));
    }, "B@:@@");
    support.method(np, "predicateFormat", [pr](CpuState& s, GuestMemory&) {
        const auto st = pr->state(s.x[0]);
        if (st->block) { s.x[0] = pr->g.auto_string(std::string_view("BLOCKPREDICATE")); return; }
        s.x[0] = pr->g.auto_string(predicate::format(*st->node, [pr](std::uint64_t h) { return pr->describe(h); }));
    }, "@@:");
    support.method(np, "description", [pr](CpuState& s, GuestMemory&) { s.x[0] = pr->s.send_id(s.x[0], "predicateFormat"); }, "@@:");
    support.method(np, "predicateWithSubstitutionVariables:", [pr](CpuState& s, GuestMemory&) {
        const auto st = pr->state(s.x[0]);
        if (st->block) { s.x[0] = s.x[0]; return; }
        auto node = std::make_shared<Node>(*st->node);
        std::vector<Address> retained;
        for (const auto r : st->retained) retained.push_back(pr->g.retain(r));
        const auto variables = s.x[2];
        predicate::substitute(*node, [&](std::u16string_view name) -> std::optional<Expression> {
            const auto bound = variables ? pr->s.send_id(variables, "objectForKey:", {pr->g.auto_string(name)}) : 0;
            if (!bound) return std::nullopt;
            Expression e;
            e.kind = Expression::Kind::object;
            e.object = pr->g.retain(bound);
            retained.push_back(e.object);
            return e;
        });
        s.x[0] = pr->g.autorelease(pr->make(pr->s.objc.object_class(s.x[0]), node, std::move(retained)));
    }, "@@:@");
    support.method(np, "allowEvaluation", [](CpuState&, GuestMemory&) {}, "v@:");
    support.method(np, "copyWithZone:", [pr](CpuState& s, GuestMemory&) { s.x[0] = pr->g.retain(s.x[0]); }, "@@:^v");
    support.method(np, "isEqual:", [pr](CpuState& s, GuestMemory&) {
        if (s.x[0] == s.x[2]) { s.x[0] = 1; return; }
        if (!s.x[2] || !pr->s.objc.find_host_state<PredicateState>(s.x[2])) { s.x[0] = 0; return; }
        return_bool(s, pr->g.is_equal(pr->s.send_id(s.x[0], "predicateFormat"), pr->s.send_id(s.x[2], "predicateFormat")));
    }, "B@:@");
    support.method(np, "hash", [pr](CpuState& s, GuestMemory&) { s.x[0] = pr->g.hash(pr->s.send_id(s.x[0], "predicateFormat")); }, "Q@:");
    support.method(np, "dealloc", [sp, np](CpuState& s, GuestMemory&) {
        if (const auto st = sp->objc.find_host_state<PredicateState>(s.x[0])) {
            for (const auto r : std::exchange(st->retained, {})) sp->send_id(r, "release");
            if (st->block) static_cast<void>(sp->call(libsystem_library, "__Block_release", {std::exchange(st->block, 0)}));
        }
        static_cast<void>(sp->objc.send_super(s.x[0], np, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- NSComparisonPredicate ----
    const auto comparison_node = [pr](Address self) -> const Node& {
        const auto st = pr->state(self);
        if (st->node->kind != Node::Kind::comparison) fail("NSComparisonPredicate without a comparison");
        return *st->node;
    };
    support.method(cmp, "predicateOperatorType", [comparison_node](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(comparison_node(s.x[0]).op); }, "Q@:");
    support.method(cmp, "comparisonPredicateModifier", [comparison_node](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(comparison_node(s.x[0]).modifier); }, "Q@:");
    support.method(cmp, "options", [comparison_node](CpuState& s, GuestMemory&) { s.x[0] = comparison_node(s.x[0]).options; }, "Q@:");
    for (const auto* name : {"leftExpression", "rightExpression", "customSelector"})
        support.method(cmp, name, [name](CpuState&, GuestMemory&) { unsupported(std::string("-[NSComparisonPredicate ") + name + "] (NSExpression objects are not provided)"); }, "@@:");

    // ---- NSCompoundPredicate ----
    const auto compound = [pr, cpd](Address self, Address type, Address subpredicates) {
        auto node = std::make_shared<Node>();
        if (type == 0) node->kind = Node::Kind::negation;
        else if (type == 1) node->kind = Node::Kind::conjunction;
        else if (type == 2) node->kind = Node::Kind::disjunction;
        else fail("NSInvalidArgumentException: unknown NSCompoundPredicateType " + std::to_string(type));
        std::vector<Address> retained;
        for (const auto sub : pr->g.elements(subpredicates)) {
            Node child;
            child.kind = Node::Kind::foreign;
            child.object = pr->g.retain(sub);
            retained.push_back(child.object);
            node->children.push_back(std::move(child));
        }
        if (type == 0 && node->children.size() != 1) fail("NSInvalidArgumentException: a NOT predicate needs exactly one subpredicate");
        if (!self) return pr->make(cpd, node, std::move(retained));
        auto st = pr->s.objc.host_state<PredicateState>(self);
        st->node = node;
        st->retained = std::move(retained);
        st->initialized = true;
        return self;
    };
    support.method(cpd, "initWithType:subpredicates:", [compound](CpuState& s, GuestMemory&) { s.x[0] = compound(s.x[0], s.x[2], s.x[3]); }, "@@:Q@");
    support.method(cpd, "andPredicateWithSubpredicates:", [pr, compound](CpuState& s, GuestMemory&) { s.x[0] = pr->g.autorelease(compound(0, 1, s.x[2])); }, "@@:@", true);
    support.method(cpd, "orPredicateWithSubpredicates:", [pr, compound](CpuState& s, GuestMemory&) { s.x[0] = pr->g.autorelease(compound(0, 2, s.x[2])); }, "@@:@", true);
    support.method(cpd, "notPredicateWithSubpredicate:", [pr, compound](CpuState& s, GuestMemory&) {
        const auto list = pr->g.array({s.x[2]});
        Address made{};
        try { made = compound(0, 0, list); } catch (...) { pr->g.release(list); throw; }
        pr->g.release(list);
        s.x[0] = pr->g.autorelease(made);
    }, "@@:@", true);
    support.method(cpd, "compoundPredicateType", [pr](CpuState& s, GuestMemory&) {
        const auto k = pr->state(s.x[0])->node->kind;
        s.x[0] = k == Node::Kind::negation ? 0 : k == Node::Kind::conjunction ? 1 : 2;
    }, "Q@:");
    support.method(cpd, "subpredicates", [pr](CpuState& s, GuestMemory&) {
        const auto st = pr->state(s.x[0]);
        std::vector<Address> subs;
        std::vector<Address> made;
        for (const auto& c : st->node->children) {
            if (c.kind == Node::Kind::foreign) { subs.push_back(c.object); continue; }
            const auto p = pr->make(pr->class_for(c), std::make_shared<Node>(c), [&] { std::vector<Address> r; for (const auto o : st->retained) r.push_back(pr->g.retain(o)); return r; }());
            made.push_back(p);
            subs.push_back(p);
        }
        const auto list = pr->g.array(subs);
        for (const auto m : made) pr->g.release(m);
        s.x[0] = pr->g.autorelease(list);
    }, "@@:");

    // ---- collection filters ----
    const auto array = support.objc.find_class("NSArray"), mutable_array = support.objc.find_class("NSMutableArray");
    fill_method(support, array, "filteredArrayUsingPredicate:", [pr](CpuState& s, GuestMemory&) { s.x[0] = pr->g.auto_array(pr->filtered(s.x[0], s.x[2])); }, "@@:@");
    fill_method(support, mutable_array, "filterUsingPredicate:", [pr](CpuState& s, GuestMemory&) {
        const auto kept = pr->g.array(pr->filtered(s.x[0], s.x[2]));
        try { pr->s.send_id(s.x[0], "setArray:", {kept}); } catch (...) { pr->g.release(kept); throw; }
        pr->g.release(kept);
    }, "v@:@");
    if (const auto set = support.objc.find_class("NSSet")) {
        fill_method(support, set, "filteredSetUsingPredicate:", [pr, set](CpuState& s, GuestMemory&) { s.x[0] = pr->g.set_of(set, pr->filtered(s.x[0], s.x[2])); }, "@@:@");
        if (const auto mutable_set = support.objc.find_class("NSMutableSet"))
            fill_method(support, mutable_set, "filterUsingPredicate:", [pr](CpuState& s, GuestMemory&) {
                for (const auto v : pr->g.elements(s.x[0])) if (!pr->evaluate_object(s.x[2], v, 0)) pr->s.send_id(s.x[0], "removeObject:", {v});
            }, "v@:@");
    }
    if (const auto ordered = support.objc.find_class("NSOrderedSet")) {
        fill_method(support, ordered, "filteredOrderedSetUsingPredicate:", [pr, ordered](CpuState& s, GuestMemory&) {
            const auto kept = pr->g.array(pr->filtered(s.x[0], s.x[2]));
            Address made{};
            try { made = pr->s.send_id(ordered, "orderedSetWithArray:", {kept}); } catch (...) { pr->g.release(kept); throw; }
            pr->g.release(kept);
            s.x[0] = made;
        }, "@@:@");
        if (const auto mutable_ordered = support.objc.find_class("NSMutableOrderedSet"))
            fill_method(support, mutable_ordered, "filterUsingPredicate:", [pr](CpuState& s, GuestMemory&) {
                for (const auto v : pr->g.elements(s.x[0])) if (!pr->evaluate_object(s.x[2], v, 0)) pr->s.send_id(s.x[0], "removeObject:", {v});
            }, "v@:@");
    }
}

} // namespace alchemy::gui::completion_kit::data
