#pragma once

// Linear constraint solver for Auto Layout (QLab gap group G4, owner:
// src/alchemy/darwin/kit/completion/windows/). Project-authored, after the
// incremental Cassowary algorithm (Badros, Borning and Stuckey, "The
// Cassowary Linear Arithmetic Constraint Solving Algorithm", 2001): a dual
// simplex tableau in which constraints are added, removed and edited one at
// a time and the solution is re-optimized incrementally.
//
// Constraints are linear (in)equalities over real variables, each with an
// Auto Layout priority in (0, 1000]:
//   * priority 1000 (required) must hold. Adding a required constraint that
//     contradicts the required constraints already present throws
//     UnsatisfiableConstraint and leaves the solver unchanged.
//   * a lower priority is a preference. The solver minimizes the error of
//     the preferences lexicographically by priority, as NSLayoutPriority
//     documents ("higher priority constraints are satisfied before lower
//     priority constraints"): no amount of error at priority p is traded for
//     less error at any priority below p. Errors of preferences at the same
//     priority add up (each counts with weight 1).
// The objective is therefore a vector of rows, one per priority level, and
// "negative" and "smaller" in the simplex pivot rules compare those vectors
// lexicographically from the highest level down. Entering symbols are chosen
// by Bland's rule (lowest symbol first), so the pivoting terminates.
//
// Edit variables (suggest_value) are preferences "variable == value" whose
// value changes without rebuilding the tableau (a window being resized, a
// constraint constant being animated).
//
// Values of variables that no constraint determines are 0.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit::layout {

inline constexpr double required = 1000;

enum class Relation : int { less_equal = -1, equal = 0, greater_equal = 1 };

struct Variable {
    std::uint32_t id{UINT32_MAX};
    [[nodiscard]] bool valid() const { return id != UINT32_MAX; }
    friend bool operator==(Variable a, Variable b) { return a.id == b.id; }
    friend bool operator<(Variable a, Variable b) { return a.id < b.id; }
};

struct Term {
    Variable variable;
    double coefficient{1};
};

// sum(coefficient * variable) + constant.
struct Expression {
    std::vector<Term> terms;
    double constant{};
    Expression() = default;
    Expression(double value) : constant(value) {}   // NOLINT: an expression from a constant
    Expression(Variable variable, double coefficient = 1) { terms.push_back({variable, coefficient}); }   // NOLINT
    Expression& plus(Variable variable, double coefficient = 1) { terms.push_back({variable, coefficient}); return *this; }
    Expression& plus(double value) { constant += value; return *this; }
    Expression& plus(const Expression& other, double scale = 1);
    [[nodiscard]] Expression scaled(double factor) const;
};
[[nodiscard]] Expression operator+(Expression a, const Expression& b);
[[nodiscard]] Expression operator-(Expression a, const Expression& b);
[[nodiscard]] Expression operator*(const Expression& a, double factor);
[[nodiscard]] Expression operator*(double factor, const Expression& a);

using ConstraintId = std::uint64_t;

// Every failure is a std::runtime_error whose message names the reason.
struct SolverError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// A required constraint that contradicts the required constraints present.
struct UnsatisfiableConstraint : SolverError {
    using SolverError::SolverError;
};

class Solver {
public:
    Solver();
    ~Solver();
    Solver(Solver&&) noexcept;
    Solver& operator=(Solver&&) noexcept;
    Solver(const Solver&) = delete;
    Solver& operator=(const Solver&) = delete;

    Variable add_variable(std::string name = {});
    [[nodiscard]] const std::string& name(Variable variable) const;

    // lhs (relation) rhs at `priority` (0 < priority <= 1000).
    ConstraintId add_constraint(const Expression& lhs, Relation relation, const Expression& rhs, double priority = required);
    void remove_constraint(ConstraintId id);
    [[nodiscard]] bool has_constraint(ConstraintId id) const;
    [[nodiscard]] std::size_t constraint_count() const;

    // Edit variables: a preference "variable == suggested value" at
    // `priority` (< 1000), initially the variable's current value.
    void add_edit_variable(Variable variable, double priority);
    void remove_edit_variable(Variable variable);
    [[nodiscard]] bool has_edit_variable(Variable variable) const;
    void suggest_value(Variable variable, double value);

    // Copies the solution into the variables (value()).
    void update_variables();
    [[nodiscard]] double value(Variable variable) const;

    // Removes every constraint and edit variable; variables stay.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace alchemy::gui::completion_kit::layout
