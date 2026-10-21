#include "alchemy/darwin/foundation/undo_core.hpp"

#include <algorithm>

namespace alchemy::gui::completion_kit::undo {

bool Stack::enable() {
    if (disabled_ == 0) return false;
    --disabled_;
    return true;
}

void Stack::begin_group() { open_.emplace_back(); }

bool Stack::end_group(std::vector<Action>* dropped) {
    if (open_.empty()) return false;
    auto group = std::move(open_.back());
    open_.pop_back();
    if (!open_.empty()) {
        auto& parent = open_.back();
        parent.actions.insert(parent.actions.end(), group.actions.begin(), group.actions.end());
        if (parent.name.empty()) parent.name = std::move(group.name);
        parent.discardable = parent.discardable || group.discardable;
        return true;
    }
    if (group.actions.empty()) return true;
    auto& stack = target_stack();
    stack.push_back(std::move(group));
    trim(stack, dropped);
    return true;
}

std::vector<Action> Stack::add(Action action) {
    std::vector<Action> cleared;
    if (open_.empty()) return cleared;
    open_.back().actions.push_back(action);
    if (mode_ == Mode::normal) {
        for (auto& group : redo_) cleared.insert(cleared.end(), group.actions.begin(), group.actions.end());
        redo_.clear();
    }
    return cleared;
}

bool Stack::can_undo() const {
    if (!undo_.empty()) return true;
    return mode_ == Mode::normal && !open_.empty() && !open_.front().actions.empty();
}

std::optional<Group> Stack::take_undo() {
    if (undo_.empty()) return std::nullopt;
    auto group = std::move(undo_.back());
    undo_.pop_back();
    return group;
}

std::optional<Group> Stack::take_redo() {
    if (redo_.empty()) return std::nullopt;
    auto group = std::move(redo_.back());
    redo_.pop_back();
    return group;
}

void Stack::begin_perform(Mode mode, std::string name) {
    mode_ = mode;
    Group group;
    group.name = std::move(name);
    open_.push_back(std::move(group));
}

void Stack::end_perform(std::vector<Action>* dropped) {
    // Groups the actions opened and left open are closed into the perform group.
    while (open_.size() > 1) end_group(dropped);
    if (!open_.empty()) {
        auto group = std::move(open_.back());
        open_.pop_back();
        if (!group.actions.empty()) {
            auto& stack = target_stack();
            stack.push_back(std::move(group));
            trim(stack, dropped);
        }
    }
    mode_ = Mode::normal;
}

std::string Stack::undo_name() const {
    if (mode_ == Mode::normal && !open_.empty() && !open_.front().actions.empty()) return open_.front().name;
    return undo_.empty() ? std::string{} : undo_.back().name;
}

std::string Stack::redo_name() const { return redo_.empty() ? std::string{} : redo_.back().name; }

void Stack::set_name(std::string name) {
    if (!open_.empty()) open_.front().name = std::move(name);
    else if (mode_ == Mode::normal && !undo_.empty()) undo_.back().name = std::move(name);
}

void Stack::set_discardable(bool value) {
    if (!open_.empty()) open_.front().discardable = value;
    else if (!undo_.empty()) undo_.back().discardable = value;
}

bool Stack::undo_discardable() const {
    if (mode_ == Mode::normal && !open_.empty() && !open_.front().actions.empty()) return open_.front().discardable;
    return !undo_.empty() && undo_.back().discardable;
}

bool Stack::redo_discardable() const { return !redo_.empty() && redo_.back().discardable; }

void Stack::trim(std::deque<Group>& stack, std::vector<Action>* dropped) {
    if (!levels_) return;
    while (stack.size() > levels_) {
        if (dropped) dropped->insert(dropped->end(), stack.front().actions.begin(), stack.front().actions.end());
        stack.pop_front();
    }
}

void Stack::set_levels(std::size_t levels, std::vector<Action>* dropped) {
    levels_ = levels;
    trim(undo_, dropped);
    trim(redo_, dropped);
}

std::vector<Action> Stack::clear() {
    std::vector<Action> all;
    for (auto* stack : {&undo_, &redo_})
        for (auto& group : *stack) all.insert(all.end(), group.actions.begin(), group.actions.end());
    for (auto& group : open_) all.insert(all.end(), group.actions.begin(), group.actions.end());
    undo_.clear();
    redo_.clear();
    open_.clear();
    disabled_ = 0;
    return all;
}

std::vector<Action> Stack::remove_target(std::uint64_t target) {
    std::vector<Action> removed;
    const auto filter = [&](Group& group) {
        auto& list = group.actions;
        for (const auto& action : list) if (action.target == target) removed.push_back(action);
        list.erase(std::remove_if(list.begin(), list.end(), [target](const Action& action) { return action.target == target; }), list.end());
    };
    for (auto* stack : {&undo_, &redo_}) {
        for (auto& group : *stack) filter(group);
        stack->erase(std::remove_if(stack->begin(), stack->end(), [](const Group& group) { return group.actions.empty(); }), stack->end());
    }
    for (auto& group : open_) filter(group);
    return removed;
}

} // namespace alchemy::gui::completion_kit::undo
