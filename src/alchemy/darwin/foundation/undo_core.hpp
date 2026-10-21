#pragma once

// NSUndoManager's stacks and grouping (host-only core, unit tested in
// tests/runtime_tests.cpp). Actions are opaque {target, id} pairs; the
// provider (undo.cpp) maps ids to registered selectors, blocks and
// invocations and performs them.
//
// Semantics (Foundation's documented behaviour):
// * Registration outside undo/redo goes to the undo stack and clears the
//   redo stack; while undoing it goes to the redo stack; while redoing to
//   the undo stack (without clearing redo).
// * Actions are recorded in the innermost open group; closing the outermost
//   group pushes it on its stack (an empty group is dropped). Nested groups
//   are flattened into their parent, in order.
// * undo performs the newest group's actions newest first, inside a new
//   group on the redo stack, which takes the undone group's action name.
// * levelsOfUndo (0 = unlimited) bounds each stack; the oldest groups go.
// * Registration is ignored while disabled (disableUndoRegistration nests).

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit::undo {

struct Action { std::uint64_t target{}, id{}; };

struct Group {
    std::vector<Action> actions;   // in registration order
    std::string name;
    bool discardable{};
};

class Stack {
public:
    enum class Mode { normal, undoing, redoing };

    [[nodiscard]] int level() const noexcept { return static_cast<int>(open_.size()); }
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] bool registration_enabled() const noexcept { return disabled_ == 0; }
    void disable() { ++disabled_; }
    // False when registration was not disabled (Foundation raises).
    bool enable();

    void begin_group();
    // Closes the innermost group; returns the closed outermost group's ids
    // that were dropped by levelsOfUndo. False when no group is open.
    bool end_group(std::vector<Action>* dropped = nullptr);

    // Records an action in the innermost open group. Returns the ids the
    // registration cleared (the redo stack), which the caller releases.
    // Requires an open group (the provider opens one when groupsByEvent).
    std::vector<Action> add(Action action);

    [[nodiscard]] bool can_undo() const;
    [[nodiscard]] bool can_redo() const { return !redo_.empty(); }

    // Pops the newest undo (redo) group for performing; the caller performs
    // its actions newest first between begin_perform and end_perform, which
    // collect the registrations into the opposite stack.
    std::optional<Group> take_undo();
    std::optional<Group> take_redo();
    void begin_perform(Mode mode, std::string name);
    // Closes the group of the registrations made while performing.
    void end_perform(std::vector<Action>* dropped = nullptr);

    // The action name of the group the next undo (redo) performs: the open
    // group's when one is open (registration in progress), else the top's.
    [[nodiscard]] std::string undo_name() const;
    [[nodiscard]] std::string redo_name() const;
    // setActionName: names the group being built (or the top undo group).
    void set_name(std::string name);
    void set_discardable(bool value);
    [[nodiscard]] bool undo_discardable() const;
    [[nodiscard]] bool redo_discardable() const;

    void set_levels(std::size_t levels, std::vector<Action>* dropped = nullptr);
    [[nodiscard]] std::size_t levels() const noexcept { return levels_; }

    // Removes everything (the ids to release); re-enables registration.
    std::vector<Action> clear();
    // Removes the actions of one target; groups left empty disappear.
    std::vector<Action> remove_target(std::uint64_t target);

    [[nodiscard]] std::size_t undo_count() const noexcept { return undo_.size(); }
    [[nodiscard]] std::size_t redo_count() const noexcept { return redo_.size(); }
    [[nodiscard]] const std::deque<Group>& undo_groups() const noexcept { return undo_; }
    [[nodiscard]] const std::deque<Group>& redo_groups() const noexcept { return redo_; }

private:
    std::deque<Group>& target_stack() { return mode_ == Mode::undoing ? redo_ : undo_; }
    void trim(std::deque<Group>& stack, std::vector<Action>* dropped);

    std::deque<Group> undo_, redo_;   // back() is the newest
    std::vector<Group> open_;         // open groups, innermost last
    Mode mode_{Mode::normal};
    int disabled_{};
    std::size_t levels_{};
};

} // namespace alchemy::gui::completion_kit::undo
