// Host-only tests of the Auto Layout constraint solver (QLab gap group G4,
// src/alchemy/darwin/kit/completion/windows/layout_solver.cpp): layouts with known
// solutions, inequalities, lexicographic priorities, required conflicts,
// removal and edit variables.
#include "alchemy/darwin/appkit/windows/layout_solver.hpp"
#include "alchemy/darwin/appkit/tests/windows_test_support.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

namespace layout = alchemy::gui::completion_kit::layout;
using layout::Expression;
using layout::Relation;
using layout::Solver;
using windows_tests::near;
using windows_tests::require;

namespace windows_tests {

namespace {
void expect_value(Solver& solver, layout::Variable variable, double wanted, const std::string& what) {
    solver.update_variables();
    const auto got = solver.value(variable);
    require(near(got, wanted), what + ": got " + std::to_string(got) + ", wanted " + std::to_string(wanted));
}
template <class F>
void expect_unsatisfiable(F&& action, const std::string& what) {
    try {
        action();
    } catch (const layout::UnsatisfiableConstraint&) {
        return;
    }
    throw std::runtime_error(what + ": no UnsatisfiableConstraint");
}
template <class F>
void expect_error(F&& action, const std::string& what) {
    try {
        action();
    } catch (const layout::SolverError&) {
        return;
    }
    throw std::runtime_error(what + ": no SolverError");
}

void equalities() {
    Solver s;
    const auto x = s.add_variable("x"), y = s.add_variable("y");
    s.add_constraint(x, Relation::equal, 10);
    expect_value(s, x, 10, "x == 10");
    s.add_constraint(Expression(x).plus(y), Relation::equal, 30);
    expect_value(s, y, 20, "x + y == 30");
    // A second, consistent required equality is redundant, not a conflict.
    s.add_constraint(y, Relation::equal, Expression(x) * 2);
    expect_value(s, y, 20, "redundant y == 2x");
    // Chains with multipliers (aspect ratios).
    Solver chain;
    const auto w = chain.add_variable("w"), h = chain.add_variable("h");
    chain.add_constraint(w, Relation::equal, Expression(h) * 1.5);
    chain.add_constraint(Expression(w).plus(h), Relation::equal, 250);
    expect_value(chain, w, 150, "aspect width");
    expect_value(chain, h, 100, "aspect height");
}

void inequalities() {
    Solver s;
    const auto x = s.add_variable("x");
    s.add_constraint(x, Relation::greater_equal, 40);
    expect_value(s, x, 40, "x >= 40 alone is satisfied at its bound");
    s.add_constraint(x, Relation::equal, 100, 250);
    expect_value(s, x, 100, "a preference inside the feasible region");
    s.add_constraint(x, Relation::less_equal, 70);
    expect_value(s, x, 70, "a required upper bound clips the preference");
    s.add_constraint(x, Relation::equal, 10, 500);
    expect_value(s, x, 40, "the higher preference is clipped by the required lower bound");
}

void priorities() {
    // Lexicographic: one error unit at 751 outweighs any error at 750.
    Solver s;
    const auto x = s.add_variable("x");
    s.add_constraint(x, Relation::equal, 10, 751);
    for (int i = 0; i < 6; ++i) s.add_constraint(x, Relation::equal, 20, 750);
    expect_value(s, x, 10, "751 beats six constraints at 750");
    // At equal priority the summed error is minimized (the median).
    Solver median;
    const auto m = median.add_variable("m");
    median.add_constraint(m, Relation::equal, 0, 500);
    median.add_constraint(m, Relation::equal, 10, 500);
    median.add_constraint(m, Relation::equal, 12, 500);
    expect_value(median, m, 10, "same-priority errors add up");
    // Hugging (<= at 250) against compression resistance (>= at 750) and a
    // required container: the label keeps its intrinsic width when there is
    // room, is stretched when the container forces it, never compressed by
    // a 500 preference.
    Solver label;
    const auto left = label.add_variable("left"), width = label.add_variable("width"), container = label.add_variable("container");
    label.add_constraint(left, Relation::equal, 20);
    label.add_constraint(width, Relation::less_equal, 80, 250);      // hugging
    label.add_constraint(width, Relation::greater_equal, 80, 750);   // compression resistance
    label.add_constraint(Expression(left).plus(width).plus(20), Relation::less_equal, container);
    label.add_constraint(container, Relation::equal, 300, 500);
    expect_value(label, width, 80, "intrinsic width with room to spare");
    label.add_constraint(Expression(left).plus(width).plus(20), Relation::equal, container, 260);
    expect_value(label, width, 260, "a 260 pin overrides 250 hugging");
    label.add_constraint(container, Relation::equal, 60, 600);
    expect_value(label, width, 80, "compression resistance at 750 beats a 600 container size");
    expect_value(label, container, 120, "the container grows to fit");
}

void conflicts() {
    Solver s;
    const auto x = s.add_variable("x"), y = s.add_variable("y");
    s.add_constraint(x, Relation::equal, 1);
    expect_unsatisfiable([&] { s.add_constraint(x, Relation::equal, 2); }, "x == 1 and x == 2");
    expect_value(s, x, 1, "the solver is unchanged by a rejected constraint");
    s.add_constraint(y, Relation::greater_equal, x);
    expect_unsatisfiable([&] { s.add_constraint(y, Relation::less_equal, 0); }, "y >= x == 1 and y <= 0");
    s.add_constraint(y, Relation::less_equal, 5);
    s.update_variables();
    require(s.value(y) >= 1 - 1e-9 && s.value(y) <= 5 + 1e-9, "1 <= y <= 5 after the rejected constraint");
    s.add_constraint(y, Relation::equal, 0, 100);
    expect_value(s, y, 1, "a weak pull towards 0 stops at the required bound");
    // A conflicting preference is not an error.
    s.add_constraint(x, Relation::equal, 3, 999);
    expect_value(s, x, 1, "a 999 preference loses to the required x == 1");
    // Arguments.
    expect_error([&] { s.add_constraint(x, Relation::equal, 0, 0); }, "priority 0");
    expect_error([&] { s.add_constraint(x, Relation::equal, 0, 1001); }, "priority 1001");
    expect_error([&] { s.add_constraint(x, Relation::equal, std::nan("")); }, "NaN constant");
    expect_error([&] { s.add_edit_variable(x, layout::required); }, "required edit variable");
    expect_error([&] { s.suggest_value(x, 3); }, "suggestion without an edit variable");
    expect_error([&] { s.remove_constraint(12345); }, "removing an unknown constraint");
}

void removal() {
    Solver s;
    const auto x = s.add_variable("x");
    const auto weak = s.add_constraint(x, Relation::equal, 10, 250);
    const auto strong = s.add_constraint(x, Relation::equal, 30, 500);
    const auto bound = s.add_constraint(x, Relation::less_equal, 20);
    expect_value(s, x, 20, "bounded strong preference");
    s.remove_constraint(bound);
    expect_value(s, x, 30, "bound removed");
    s.remove_constraint(strong);
    expect_value(s, x, 10, "strong preference removed");
    s.remove_constraint(weak);
    expect_value(s, x, 0, "nothing left: 0");
    require(s.constraint_count() == 0 && !s.has_constraint(weak), "constraint bookkeeping");
    // Removing a required equality whose marker is not basic.
    Solver t;
    const auto a = t.add_variable("a"), b = t.add_variable("b");
    const auto ab = t.add_constraint(a, Relation::equal, Expression(b).plus(5));
    t.add_constraint(b, Relation::equal, 7);
    expect_value(t, a, 12, "a == b + 5");
    t.remove_constraint(ab);
    t.add_constraint(a, Relation::equal, Expression(b) * 3);
    expect_value(t, a, 21, "replaced constraint");
}

void edits() {
    // A row of two views between margins in a container whose width is edited.
    Solver s;
    const auto width = s.add_variable("container.width");
    const auto a_left = s.add_variable("a.left"), a_width = s.add_variable("a.width");
    const auto b_left = s.add_variable("b.left"), b_width = s.add_variable("b.width");
    s.add_constraint(a_left, Relation::equal, 20);
    s.add_constraint(b_left, Relation::equal, Expression(a_left).plus(a_width).plus(8));
    s.add_constraint(Expression(b_left).plus(b_width).plus(20), Relation::equal, width);
    s.add_constraint(a_width, Relation::equal, b_width);
    s.add_constraint(a_width, Relation::greater_equal, 50);
    s.add_edit_variable(width, 500);
    s.suggest_value(width, 400);
    expect_value(s, a_width, 176, "equal widths at 400");
    expect_value(s, b_left, 204, "second view position at 400");
    s.suggest_value(width, 800);
    expect_value(s, b_width, 376, "equal widths at 800");
    s.suggest_value(width, 100);
    expect_value(s, width, 148, "minimum widths push the edit back (148)");
    expect_value(s, a_width, 50, "minimum width");
    s.suggest_value(width, 300);
    expect_value(s, a_width, 126, "back above the minimum");
    expect_error([&] { s.add_edit_variable(width, 400); }, "duplicate edit variable");
    s.remove_edit_variable(width);
    require(!s.has_edit_variable(width), "edit variable removed");
    // Two edit variables at different priorities.
    Solver t;
    const auto x = t.add_variable("x"), y = t.add_variable("y");
    t.add_constraint(Expression(x).plus(y), Relation::equal, 100);
    t.add_edit_variable(x, 300);
    t.add_edit_variable(y, 600);
    t.suggest_value(x, 70);
    t.suggest_value(y, 60);
    expect_value(t, y, 60, "the stronger edit wins");
    expect_value(t, x, 40, "the weaker edit gives way");
}

// Random systems of required equalities x_i = x_{i-1} + c_i with x_0 fixed,
// solved incrementally in random order with preference noise, compared with
// the closed form; then removal and re-addition.
void randomized_chains() {
    std::mt19937 random(12345);
    std::uniform_real_distribution<double> step(-50, 50);
    for (int round = 0; round < 20; ++round) {
        Solver s;
        const int n = 30;
        std::vector<layout::Variable> v;
        std::vector<double> c(n), expected(n);
        for (int i = 0; i < n; ++i) v.push_back(s.add_variable("v" + std::to_string(i)));
        expected[0] = step(random);
        for (int i = 1; i < n; ++i) { c[i] = step(random); expected[i] = expected[i - 1] + c[i]; }
        std::vector<int> order(n);
        for (int i = 0; i < n; ++i) order[i] = i;
        std::shuffle(order.begin(), order.end(), random);
        for (int i = 0; i < n; ++i) s.add_constraint(v[i], Relation::equal, step(random), 1 + (i % 7) * 100);
        std::vector<layout::ConstraintId> ids(n);
        for (const auto i : order)
            ids[i] = i == 0 ? s.add_constraint(v[0], Relation::equal, expected[0])
                            : s.add_constraint(v[i], Relation::equal, Expression(v[i - 1]).plus(c[i]));
        s.update_variables();
        for (int i = 0; i < n; ++i) require(near(s.value(v[i]), expected[i]), "chain value " + std::to_string(i));
        // Cut the chain in the middle: the upper half follows its preferences
        // but keeps its internal differences.
        s.remove_constraint(ids[n / 2]);
        s.update_variables();
        for (int i = n / 2 + 1; i < n; ++i)
            require(near(s.value(v[i]) - s.value(v[i - 1]), c[i]), "upper half differences " + std::to_string(i));
        for (int i = 1; i < n / 2; ++i) require(near(s.value(v[i]), expected[i]), "lower half " + std::to_string(i));
        ids[n / 2] = s.add_constraint(v[n / 2], Relation::equal, Expression(v[n / 2 - 1]).plus(c[n / 2]));
        s.update_variables();
        for (int i = 0; i < n; ++i) require(near(s.value(v[i]), expected[i]), "rejoined chain " + std::to_string(i));
    }
}
} // namespace

void solver_tests() {
    equalities();
    inequalities();
    priorities();
    conflicts();
    removal();
    edits();
    randomized_chains();
}

} // namespace windows_tests
