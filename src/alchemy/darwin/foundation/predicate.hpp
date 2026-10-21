#pragma once

// NSPredicate's format language (host-only): parsing to a syntax tree and
// printing the canonical -predicateFormat.
//   Predicates: TRUEPREDICATE, FALSEPREDICATE, AND / &&, OR / ||, NOT / !,
//     parentheses, comparisons with the ANY / SOME / ALL / NONE modifiers.
//   Comparisons: == = != <> < <= =< > >= => BETWEEN IN CONTAINS BEGINSWITH
//     ENDSWITH LIKE MATCHES, with [c] [d] [cd] [n] options.
//   Expressions: key paths (SELF, a.b.@count, keyword-escaped #key), string,
//     number and boolean literals, NULL / NIL, $variables, aggregates
//     {a, b}, and format arguments %@ %K %d %i %u %x %o %c %f %e %g with
//     h / l / ll / q length modifiers (the caller supplies their values).
// Not provided (Status::unsupported): SUBQUERY, FUNCTION, arithmetic
// expressions, index subscripts ([FIRST], [SIZE], [n]) and the CAST family.
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::completion_kit::predicate {

struct Expression {
    enum class Kind { null, boolean, integer, real, string, object, key_path, self, variable, aggregate };
    Kind kind{Kind::null};
    bool boolean{};
    std::int64_t integer{};
    double real{};
    std::u16string text;              // string, key path, variable name
    std::uint64_t object{};           // object: an opaque handle (a guest object from %@)
    std::vector<Expression> items;    // aggregate
};

// NSPredicateOperatorType values.
enum class Operator {
    less = 0, less_or_equal = 1, greater = 2, greater_or_equal = 3, equal = 4, not_equal = 5,
    matches = 6, like = 7, begins_with = 8, ends_with = 9, in = 10, contains = 99, between = 100,
};
// NSComparisonPredicateModifier.
enum class Modifier { direct = 0, all = 1, any = 2 };
// NSComparisonPredicateOptions.
enum Options : unsigned { case_insensitive = 1, diacritic_insensitive = 2, normalized = 4 };

struct Node {
    enum class Kind { truth, falsity, comparison, conjunction, disjunction, negation, foreign };
    Kind kind{Kind::truth};
    Expression left, right;
    Operator op{Operator::equal};
    Modifier modifier{Modifier::direct};
    bool none{};                      // NONE: NOT ANY
    unsigned options{};
    std::vector<Node> children;       // conjunction / disjunction / negation
    std::uint64_t object{};           // foreign: an opaque predicate handle
};

enum class Status { ok, invalid, unsupported };

// Supplies the value of a format argument: `conversion` is the conversion
// character ('@', 'K', 'd', 'f', ...), `length` its modifier ("", "l", "ll", "h", "q").
using Arguments = std::function<Expression(char conversion, std::string_view length)>;

[[nodiscard]] std::optional<Node> parse(std::u16string_view format, const Arguments& arguments, Status& status, std::string& message);
// -predicateFormat. `describe` prints object constants and foreign predicates.
[[nodiscard]] std::u16string format(const Node& node, const std::function<std::u16string(std::uint64_t)>& describe);
[[nodiscard]] std::u16string format(const Expression& expression, const std::function<std::u16string(std::uint64_t)>& describe);

// LIKE: '*' any run, '?' one character, '\' escapes; options fold case / diacritics.
[[nodiscard]] bool like(std::u16string_view text, std::u16string_view pattern, unsigned options);

// Replaces $variables (predicateWithSubstitutionVariables:); `lookup` returns
// nullopt for a variable without a value (it stays a variable).
void substitute(Node& node, const std::function<std::optional<Expression>(std::u16string_view)>& lookup);

} // namespace alchemy::gui::completion_kit::predicate
