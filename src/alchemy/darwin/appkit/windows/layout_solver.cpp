#include "alchemy/darwin/appkit/windows/layout_solver.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

// The incremental simplex tableau of layout_solver.hpp. The tableau keeps
// every basic symbol's row as "basic = constant + sum(coefficient * parametric
// symbol)"; external (variable) symbols may be basic with any sign, every
// other basic symbol is kept non-negative (the restricted rows). Each
// constraint contributes a marker symbol (and possibly a second error
// symbol) so it can be removed again; preferences contribute error symbols
// weighted in the objective level of their priority.
//
// Rows keep their cells as vectors sorted by symbol (layouts of a few
// thousand constraints pivot thousands of times, and every pivot substitutes
// into every row, so lookups and row additions must be cheap: a binary
// search, and a linear merge).
namespace alchemy::gui::completion_kit::layout {

Expression& Expression::plus(const Expression& other, double scale) {
    for (const auto& term : other.terms) terms.push_back({term.variable, term.coefficient * scale});
    constant += other.constant * scale;
    return *this;
}
Expression Expression::scaled(double factor) const {
    Expression result;
    result.constant = constant * factor;
    for (const auto& term : terms) result.terms.push_back({term.variable, term.coefficient * factor});
    return result;
}
Expression operator+(Expression a, const Expression& b) { return a.plus(b); }
Expression operator-(Expression a, const Expression& b) { return a.plus(b, -1); }
Expression operator*(const Expression& a, double factor) { return a.scaled(factor); }
Expression operator*(double factor, const Expression& a) { return a.scaled(factor); }

namespace {
constexpr double epsilon = 1.0e-8;
bool near_zero(double value) { return std::fabs(value) < epsilon; }

enum class SymbolType : std::uint8_t { invalid, external, slack, error, dummy };
struct Symbol {
    std::uint64_t id{};
    SymbolType type{SymbolType::invalid};
    [[nodiscard]] bool valid() const { return type != SymbolType::invalid; }
    friend bool operator<(const Symbol& a, const Symbol& b) { return a.id < b.id; }
    friend bool operator==(const Symbol& a, const Symbol& b) { return a.id == b.id; }
};

// A row's cells: (symbol, coefficient) sorted by symbol, no zero coefficients.
class Cells {
public:
    using Entry = std::pair<Symbol, double>;
    using const_iterator = std::vector<Entry>::const_iterator;
    using iterator = std::vector<Entry>::iterator;
    [[nodiscard]] const_iterator begin() const { return entries_.begin(); }
    [[nodiscard]] const_iterator end() const { return entries_.end(); }
    [[nodiscard]] iterator begin() { return entries_.begin(); }
    [[nodiscard]] iterator end() { return entries_.end(); }
    [[nodiscard]] bool empty() const { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }
    [[nodiscard]] iterator find(const Symbol& symbol) {
        const auto at = lower(symbol);
        return at != entries_.end() && at->first == symbol ? at : entries_.end();
    }
    [[nodiscard]] const_iterator find(const Symbol& symbol) const {
        const auto at = std::lower_bound(entries_.begin(), entries_.end(), symbol, [](const Entry& e, const Symbol& s) { return e.first < s; });
        return at != entries_.end() && at->first == symbol ? at : entries_.end();
    }
    void erase(iterator at) { entries_.erase(at); }
    void erase(const Symbol& symbol) {
        if (const auto at = find(symbol); at != entries_.end()) entries_.erase(at);
    }
    // Adds `coefficient` to the symbol's cell (dropping it when it cancels).
    void add(const Symbol& symbol, double coefficient) {
        const auto at = lower(symbol);
        if (at != entries_.end() && at->first == symbol) {
            at->second += coefficient;
            if (near_zero(at->second)) entries_.erase(at);
        } else if (!near_zero(coefficient)) {
            entries_.insert(at, {symbol, coefficient});
        }
    }
    // this += other * coefficient, as one merge.
    void add(const Cells& other, double coefficient) {
        if (other.entries_.empty()) return;
        std::vector<Entry> merged;
        merged.reserve(entries_.size() + other.entries_.size());
        auto a = entries_.begin();
        auto b = other.entries_.begin();
        while (a != entries_.end() || b != other.entries_.end()) {
            if (b == other.entries_.end() || (a != entries_.end() && a->first < b->first)) {
                merged.push_back(*a++);
            } else if (a == entries_.end() || b->first < a->first) {
                const double value = b->second * coefficient;
                if (!near_zero(value)) merged.push_back({b->first, value});
                ++b;
            } else {
                const double value = a->second + b->second * coefficient;
                if (!near_zero(value)) merged.push_back({a->first, value});
                ++a;
                ++b;
            }
        }
        entries_ = std::move(merged);
    }

private:
    iterator lower(const Symbol& symbol) {
        return std::lower_bound(entries_.begin(), entries_.end(), symbol, [](const Entry& e, const Symbol& s) { return e.first < s; });
    }
    std::vector<Entry> entries_;
};

struct Row {
    double constant{};
    Cells cells;

    Row() = default;
    explicit Row(double value) : constant(value) {}
    double add(double value) { return constant += value; }
    void insert(const Symbol& symbol, double coefficient = 1) { cells.add(symbol, coefficient); }
    void insert(const Row& other, double coefficient = 1) {
        constant += other.constant * coefficient;
        cells.add(other.cells, coefficient);
    }
    void remove(const Symbol& symbol) { cells.erase(symbol); }
    void reverse_sign() {
        constant = -constant;
        for (auto& [symbol, value] : cells) value = -value;
    }
    // Solves "0 = this" for `symbol` (which must be present): the row becomes
    // the expression `symbol` equals.
    void solve_for(const Symbol& symbol) {
        const auto found = cells.find(symbol);
        const double coefficient = -1.0 / found->second;
        cells.erase(found);
        constant *= coefficient;
        for (auto& [other, value] : cells) value *= coefficient;
    }
    // This row is "lhs = ..."; re-solves it for `rhs`.
    void solve_for(const Symbol& lhs, const Symbol& rhs) {
        insert(lhs, -1.0);
        solve_for(rhs);
    }
    [[nodiscard]] double coefficient(const Symbol& symbol) const {
        const auto found = cells.find(symbol);
        return found == cells.end() ? 0.0 : found->second;
    }
    void substitute(const Symbol& symbol, const Row& row) {
        const auto found = cells.find(symbol);
        if (found == cells.end()) return;
        const double coefficient = found->second;
        cells.erase(found);
        insert(row, coefficient);
    }
};

struct Tag {
    Symbol marker, other;
};

struct ConstraintRecord {
    Tag tag;
    double priority{required};
    Relation relation{Relation::equal};
};

struct EditInfo {
    Tag tag;
    ConstraintId constraint{};
    double constant{};
};

std::string priority_text(double priority) {
    auto text = std::to_string(priority);
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text;
}
} // namespace

struct Solver::Impl {
    struct VariableInfo {
        std::string name;
        Symbol symbol;
        double value{};
    };
    std::vector<VariableInfo> variables;
    std::map<ConstraintId, ConstraintRecord> constraints;
    std::map<Variable, EditInfo> edits;
    std::map<Symbol, Row> rows;
    // The objective: one row per preference priority, highest first.
    std::map<double, Row, std::greater<double>> objective;
    std::optional<Row> artificial;
    std::vector<Symbol> infeasible;
    std::uint64_t next_symbol{1};
    ConstraintId next_constraint{1};

    Symbol make_symbol(SymbolType type) { return {next_symbol++, type}; }

    Symbol variable_symbol(Variable variable) {
        if (!variable.valid() || variable.id >= variables.size()) throw SolverError("layout solver: unknown variable");
        return variables[variable.id].symbol;
    }

    // ---- the objective vector ----
    void objective_insert(double priority, const Symbol& symbol, double coefficient) {
        objective[priority].insert(symbol, coefficient);
    }
    void objective_insert(double priority, const Row& row, double coefficient) {
        auto& level = objective[priority];
        // Only the cells matter to the optimization; the constant is the
        // objective's value.
        level.insert(row, coefficient);
    }
    void objective_substitute(const Symbol& symbol, const Row& row) {
        for (auto& [priority, level] : objective) level.substitute(symbol, row);
    }
    void objective_remove(const Symbol& symbol) {
        for (auto& [priority, level] : objective) level.remove(symbol);
    }
    void prune_objective() {
        for (auto it = objective.begin(); it != objective.end();) {
            if (it->second.cells.empty()) it = objective.erase(it);
            else ++it;
        }
    }
    // Compares objective(a) / ca with objective(b) / cb lexicographically.
    [[nodiscard]] int compare_ratio(const Symbol& a, double ca, const Symbol& b, double cb) const {
        for (const auto& [priority, level] : objective) {
            const auto ra = level.coefficient(a) / ca, rb = level.coefficient(b) / cb;
            if (std::fabs(ra - rb) >= epsilon) return ra < rb ? -1 : 1;
        }
        return 0;
    }

    // ---- tableau operations ----
    Row create_row(const Expression& expression, Relation relation, double priority, Tag& tag) {
        Row row(expression.constant);
        for (const auto& term : expression.terms) {
            if (near_zero(term.coefficient)) continue;
            const auto symbol = variable_symbol(term.variable);
            if (const auto basic = rows.find(symbol); basic != rows.end()) row.insert(basic->second, term.coefficient);
            else row.insert(symbol, term.coefficient);
        }
        const bool preference = priority < required;
        if (relation != Relation::equal) {
            // expression <= 0: expression + slack = 0; >= 0: expression - slack = 0.
            const double coefficient = relation == Relation::less_equal ? 1.0 : -1.0;
            const auto slack = make_symbol(SymbolType::slack);
            tag.marker = slack;
            row.insert(slack, coefficient);
            if (preference) {
                const auto error = make_symbol(SymbolType::error);
                tag.other = error;
                row.insert(error, -coefficient);
                objective_insert(priority, error, 1.0);
            }
        } else if (preference) {
            // expression = error_plus - error_minus, both minimized.
            const auto plus = make_symbol(SymbolType::error), minus = make_symbol(SymbolType::error);
            tag.marker = plus;
            tag.other = minus;
            row.insert(plus, -1.0);
            row.insert(minus, 1.0);
            objective_insert(priority, plus, 1.0);
            objective_insert(priority, minus, 1.0);
        } else {
            const auto dummy = make_symbol(SymbolType::dummy);
            tag.marker = dummy;
            row.insert(dummy);
        }
        if (row.constant < 0) row.reverse_sign();
        return row;
    }

    static Symbol choose_subject(const Row& row, const Tag& tag) {
        for (const auto& [symbol, value] : row.cells)
            if (symbol.type == SymbolType::external) return symbol;
        for (const auto& candidate : {tag.marker, tag.other})
            if ((candidate.type == SymbolType::slack || candidate.type == SymbolType::error) && row.coefficient(candidate) < 0)
                return candidate;
        return {};
    }

    static bool all_dummies(const Row& row) {
        for (const auto& [symbol, value] : row.cells)
            if (symbol.type != SymbolType::dummy) return false;
        return true;
    }

    void substitute(const Symbol& symbol, const Row& row) {
        for (auto& [basic, other] : rows) {
            other.substitute(symbol, row);
            if (basic.type != SymbolType::external && other.constant < 0) infeasible.push_back(basic);
        }
        objective_substitute(symbol, row);
        if (artificial) artificial->substitute(symbol, row);
    }

    void pivot_in(std::map<Symbol, Row>::iterator leaving_row, const Symbol& entering) {
        const auto leaving = leaving_row->first;
        Row row = std::move(leaving_row->second);
        rows.erase(leaving_row);
        row.solve_for(leaving, entering);
        substitute(entering, row);
        rows.emplace(entering, std::move(row));
    }

    // Lowest ratio among restricted rows with a negative entering coefficient.
    std::map<Symbol, Row>::iterator leaving_row(const Symbol& entering) {
        auto ratio = std::numeric_limits<double>::max();
        auto found = rows.end();
        for (auto it = rows.begin(); it != rows.end(); ++it) {
            if (it->first.type == SymbolType::external) continue;
            const auto value = it->second.coefficient(entering);
            if (value < 0) {
                const auto candidate = -it->second.constant / value;
                if (candidate < ratio) { ratio = candidate; found = it; }
            }
        }
        return found;
    }

    // Minimizes a plain row (the artificial objective).
    void optimize_row(Row& target) {
        for (;;) {
            Symbol entering;
            for (const auto& [symbol, value] : target.cells)
                if (symbol.type != SymbolType::dummy && value < 0) { entering = symbol; break; }
            if (!entering.valid()) return;
            const auto leaving = leaving_row(entering);
            if (leaving == rows.end()) throw SolverError("layout solver: the artificial objective is unbounded");
            pivot_in(leaving, entering);
        }
    }

    // Bland's rule over the objective vector: the lowest symbol whose
    // coefficient vector is lexicographically negative. The levels' cells are
    // sorted by symbol, so one merge across the levels (highest priority
    // first) visits each symbol once with its coefficients in level order.
    Symbol entering_symbol() const {
        struct Head { Cells::const_iterator at, end; };
        std::vector<Head> heads;
        heads.reserve(objective.size());
        for (const auto& [priority, level] : objective) heads.push_back({level.cells.begin(), level.cells.end()});
        for (;;) {
            const Symbol* lowest = nullptr;
            for (const auto& head : heads)
                if (head.at != head.end && (!lowest || head.at->first < *lowest)) lowest = &head.at->first;
            if (!lowest) return {};
            const auto symbol = *lowest;
            int sign = 0;
            for (auto& head : heads) {   // in priority order
                if (head.at == head.end || !(head.at->first == symbol)) continue;
                if (sign == 0 && !near_zero(head.at->second)) sign = head.at->second < 0 ? -1 : 1;
                ++head.at;
            }
            if (sign < 0 && symbol.type != SymbolType::dummy) return symbol;
        }
    }

    // Minimizes the lexicographic objective.
    void optimize() {
        for (;;) {
            const auto entering = entering_symbol();
            if (!entering.valid()) break;
            const auto leaving = leaving_row(entering);
            if (leaving == rows.end()) throw SolverError("layout solver: the objective is unbounded");
            pivot_in(leaving, entering);
        }
        prune_objective();
    }

    // Restores feasibility after edit suggestions (dual simplex).
    void dual_optimize() {
        while (!infeasible.empty()) {
            const auto leaving = infeasible.back();
            infeasible.pop_back();
            const auto it = rows.find(leaving);
            if (it == rows.end() || near_zero(it->second.constant) || it->second.constant >= 0) continue;
            Symbol entering;
            double entering_value{};
            for (const auto& [symbol, value] : it->second.cells) {
                if (value <= 0 || symbol.type == SymbolType::dummy) continue;
                if (!entering.valid() || compare_ratio(symbol, value, entering, entering_value) < 0) {
                    entering = symbol;
                    entering_value = value;
                }
            }
            if (!entering.valid()) throw SolverError("layout solver: dual optimization failed");
            pivot_in(it, entering);
        }
        prune_objective();
    }

    static Symbol any_pivotable(const Row& row) {
        for (const auto& [symbol, value] : row.cells)
            if (symbol.type == SymbolType::slack || symbol.type == SymbolType::error) return symbol;
        return {};
    }

    bool add_with_artificial_variable(const Row& row) {
        const auto art = make_symbol(SymbolType::slack);
        rows.emplace(art, row);
        artificial = row;
        optimize_row(*artificial);
        const bool success = near_zero(artificial->constant);
        artificial.reset();
        if (!success) {
            // The artificial symbol is still basic (it could not reach 0):
            // dropping its row drops the new constraint; the other rows are
            // an equivalent form of the constraints present.
            rows.erase(art);
            return false;
        }
        if (const auto it = rows.find(art); it != rows.end()) {
            Row basic = std::move(it->second);
            rows.erase(it);
            if (basic.cells.empty()) return success;
            const auto entering = any_pivotable(basic);
            if (!entering.valid()) return false;
            basic.solve_for(art, entering);
            substitute(entering, basic);
            rows.emplace(entering, std::move(basic));
        }
        for (auto& [symbol, other] : rows) other.remove(art);
        objective_remove(art);
        return success;
    }

    // The row the marker of a removed constraint leaves the basis from.
    std::map<Symbol, Row>::iterator marker_leaving_row(const Symbol& marker) {
        const auto big = std::numeric_limits<double>::max();
        double r1 = big, r2 = big;
        auto first = rows.end(), second = rows.end(), third = rows.end();
        for (auto it = rows.begin(); it != rows.end(); ++it) {
            const auto value = it->second.coefficient(marker);
            if (value == 0) continue;
            if (it->first.type == SymbolType::external) {
                third = it;
            } else if (value < 0) {
                const auto r = -it->second.constant / value;
                if (r < r1) { r1 = r; first = it; }
            } else {
                const auto r = it->second.constant / value;
                if (r < r2) { r2 = r; second = it; }
            }
        }
        if (first != rows.end()) return first;
        if (second != rows.end()) return second;
        return third;
    }

    void remove_marker_effects(const Symbol& marker, double priority) {
        if (const auto it = rows.find(marker); it != rows.end()) objective_insert(priority, it->second, -1.0);
        else objective_insert(priority, marker, -1.0);
    }
};

Solver::Solver() : impl_(std::make_unique<Impl>()) {}
Solver::~Solver() = default;
Solver::Solver(Solver&&) noexcept = default;
Solver& Solver::operator=(Solver&&) noexcept = default;

Variable Solver::add_variable(std::string name) {
    auto& v = impl_->variables;
    if (v.size() >= UINT32_MAX - 1) throw SolverError("layout solver: too many variables");
    v.push_back({std::move(name), impl_->make_symbol(SymbolType::external), 0});
    return Variable{static_cast<std::uint32_t>(v.size() - 1)};
}

const std::string& Solver::name(Variable variable) const {
    if (!variable.valid() || variable.id >= impl_->variables.size()) throw SolverError("layout solver: unknown variable");
    return impl_->variables[variable.id].name;
}

ConstraintId Solver::add_constraint(const Expression& lhs, Relation relation, const Expression& rhs, double priority) {
    if (!(priority > 0) || priority > required || !std::isfinite(priority))
        throw SolverError("layout solver: priority " + priority_text(priority) + " is outside (0, 1000]");
    Expression expression = lhs - rhs;
    for (const auto& term : expression.terms)
        if (!std::isfinite(term.coefficient)) throw SolverError("layout solver: a constraint coefficient is not finite");
    if (!std::isfinite(expression.constant)) throw SolverError("layout solver: a constraint constant is not finite");
    auto& d = *impl_;
    Tag tag;
    auto row = d.create_row(expression, relation, priority, tag);
    auto subject = Impl::choose_subject(row, tag);
    if (!subject.valid() && Impl::all_dummies(row)) {
        if (!near_zero(row.constant)) {
            d.prune_objective();
            throw UnsatisfiableConstraint("layout solver: the required constraint contradicts the required constraints present");
        }
        subject = tag.marker;
    }
    if (!subject.valid()) {
        // The artificial-variable phase only changes the basis: when it fails,
        // the artificial row (the new constraint) goes and the tableau is an
        // equivalent form of the constraints present, re-optimized, so a
        // rejected constraint leaves the solution unchanged (no copy of the
        // tableau is needed).
        bool added{};
        try {
            added = d.add_with_artificial_variable(row);
        } catch (...) {
            d.artificial.reset();
            d.prune_objective();
            throw;
        }
        if (!added) {
            d.optimize();
            throw UnsatisfiableConstraint("layout solver: the required constraint contradicts the required constraints present");
        }
    } else {
        row.solve_for(subject);
        d.substitute(subject, row);
        d.rows.emplace(subject, std::move(row));
    }
    const auto id = d.next_constraint++;
    d.constraints.emplace(id, ConstraintRecord{tag, priority, relation});
    d.optimize();
    return id;
}

void Solver::remove_constraint(ConstraintId id) {
    auto& d = *impl_;
    const auto found = d.constraints.find(id);
    if (found == d.constraints.end()) throw SolverError("layout solver: removing an unknown constraint");
    for (const auto& [variable, info] : d.edits)
        if (info.constraint == id) throw SolverError("layout solver: an edit variable's constraint is removed with remove_edit_variable");
    const auto record = found->second;
    d.constraints.erase(found);
    // The error effects leave the objective before pivoting.
    if (record.tag.marker.type == SymbolType::error) d.remove_marker_effects(record.tag.marker, record.priority);
    if (record.tag.other.type == SymbolType::error) d.remove_marker_effects(record.tag.other, record.priority);
    if (const auto it = d.rows.find(record.tag.marker); it != d.rows.end()) {
        d.rows.erase(it);
    } else {
        const auto leaving = d.marker_leaving_row(record.tag.marker);
        if (leaving == d.rows.end()) throw SolverError("layout solver: no row to remove the constraint from");
        const auto leaving_symbol = leaving->first;
        Row row = std::move(leaving->second);
        d.rows.erase(leaving);
        row.solve_for(leaving_symbol, record.tag.marker);
        d.substitute(record.tag.marker, row);
    }
    d.optimize();
}

bool Solver::has_constraint(ConstraintId id) const { return impl_->constraints.contains(id); }
std::size_t Solver::constraint_count() const { return impl_->constraints.size() - impl_->edits.size(); }

void Solver::add_edit_variable(Variable variable, double priority) {
    auto& d = *impl_;
    static_cast<void>(d.variable_symbol(variable));
    if (d.edits.contains(variable)) throw SolverError("layout solver: duplicate edit variable " + d.variables[variable.id].name);
    if (!(priority < required)) throw SolverError("layout solver: an edit variable cannot be required");
    const auto current = d.variables[variable.id].value;
    const auto id = add_constraint(Expression(variable), Relation::equal, Expression(current), priority);
    d.edits.emplace(variable, EditInfo{d.constraints.at(id).tag, id, current});
}

void Solver::remove_edit_variable(Variable variable) {
    auto& d = *impl_;
    const auto found = d.edits.find(variable);
    if (found == d.edits.end()) throw SolverError("layout solver: unknown edit variable");
    const auto id = found->second.constraint;
    d.edits.erase(found);
    remove_constraint(id);
}

bool Solver::has_edit_variable(Variable variable) const { return impl_->edits.contains(variable); }

void Solver::suggest_value(Variable variable, double value) {
    auto& d = *impl_;
    if (!std::isfinite(value)) throw SolverError("layout solver: a suggested value is not finite");
    const auto found = d.edits.find(variable);
    if (found == d.edits.end()) throw SolverError("layout solver: suggest_value for a variable that is not an edit variable");
    auto& info = found->second;
    const double delta = value - info.constant;
    info.constant = value;
    // The constraint is "variable - value = error_plus - error_minus".
    if (const auto it = d.rows.find(info.tag.marker); it != d.rows.end()) {
        if (it->second.add(-delta) < 0) d.infeasible.push_back(it->first);
    } else if (const auto other = d.rows.find(info.tag.other); other != d.rows.end()) {
        if (other->second.add(delta) < 0) d.infeasible.push_back(other->first);
    } else {
        for (auto& [symbol, row] : d.rows) {
            const auto coefficient = row.coefficient(info.tag.marker);
            if (coefficient != 0 && row.add(delta * coefficient) < 0 && symbol.type != SymbolType::external)
                d.infeasible.push_back(symbol);
        }
    }
    d.dual_optimize();
}

void Solver::update_variables() {
    auto& d = *impl_;
    for (auto& info : d.variables) {
        const auto it = d.rows.find(info.symbol);
        info.value = it == d.rows.end() ? 0.0 : it->second.constant;
        if (near_zero(info.value)) info.value = 0;
    }
}

double Solver::value(Variable variable) const {
    if (!variable.valid() || variable.id >= impl_->variables.size()) throw SolverError("layout solver: unknown variable");
    return impl_->variables[variable.id].value;
}

void Solver::reset() {
    auto& d = *impl_;
    d.constraints.clear();
    d.edits.clear();
    d.rows.clear();
    d.objective.clear();
    d.artificial.reset();
    d.infeasible.clear();
    for (auto& info : d.variables) info.value = 0;
}

} // namespace alchemy::gui::completion_kit::layout
