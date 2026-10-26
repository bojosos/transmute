#include "alchemy/darwin/appkit/windows/windows_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

// Auto Layout for AppKit (QLab gap group G4): NSLayoutConstraint,
// NSLayoutAnchor (X / Y axis anchors, dimensions), the NSView constraint
// API (constraints held by views, translatesAutoresizingMaskIntoConstraints,
// intrinsic content size with hugging and compression resistance,
// -updateConstraints / -layout / -layoutSubtreeIfNeeded / -fittingSize,
// baselines), NSLayoutGuide with the view's layout guides, safe area and
// layout margins and the window's content layout guide, and the layout
// engine over layout_solver.cpp.
//
// The engine. Every window has one, built over its content view's tree:
//   * the views that take part are the items of the active constraints held
//     by views of the tree, the views with
//     translatesAutoresizingMaskIntoConstraints == NO, the views a stack or
//     grid view arranges, and all their superviews up to the content view;
//   * layout guides the constraints refer to are items too, inside their
//     owning views: custom guides are placed by the solution (-[NSView
//     layout] keeps their frames), system guides sit inside their owner's
//     frame by their insets, refreshed in every pass;
//   * views that keep translatesAutoresizingMaskIntoConstraints follow their
//     superview by their autoresizing mask (layout_geometry add_autoresizing,
//     AppKit's NSAutoresizingMaskLayoutConstraint), from their frame at the
//     time of the pass; the others are positioned by the solution;
//   * the content view sits at the origin and keeps the window's content
//     size at NSLayoutPriorityWindowSizeStayPut (500): stronger constraints
//     resize the window, as in AppKit;
//   * each view's intrinsic content size adds hugging and compression
//     resistance constraints at its priorities.
// A pass runs in the display cycle before the window draws (AppKit's
// layout_window_hook) when something invalidated it: -updateConstraints
// goes to the views that need it (children first), the engine is rebuilt
// when the structure changed (constraints activated, views added or
// removed, priorities, translation flags) or updated incrementally
// (constants, intrinsic sizes, the window size as an edit variable), then
// -layout goes top-down to every view that needs it; -[NSView layout] sets
// the frames of its constraint-laid-out subviews from the solution. When a
// view's own resizing (a split view, a scroll view's tile) moves a
// translating view differently from its mask, the pass re-reads the frames
// and solves again (at most four rounds).
//
// A required constraint that contradicts the others is broken, as AppKit
// does: the engine leaves it out and logs "Unable to simultaneously satisfy
// constraints" once for it (stderr).
//
// Alignment rectangles are the views' frames (-alignmentRectInsets is
// zero). Layout space ignores bounds origins (a scrolled clip view's
// document view keeps its frame), so constraints between a document view
// and its clip view position it in the clip view's unscrolled coordinates.
namespace alchemy::gui::completion_kit::windows {
namespace {
namespace L = completion_kit::layout;
using appkit::d;
using appkit::f;
using appkit::put_rect;
using appkit::put_size;
using appkit::rect_arg;
using appkit::result_rect;
using appkit::result_size;
using appkit::return_bool;
using appkit::return_rect;
using appkit::return_size;
using appkit::set_d;
using appkit::set_f;
using appkit::size_arg;

struct Node {
    L::Box box;                                        // the alignment rectangle
    Address superview{};                               // a layout guide's: its owning view
    bool root{}, driven{}, flipped{}, guide{};
    // System layout guides: the insets within the owner's frame the guide's
    // constraints were made from (NaN: none yet).
    std::array<double, 4> guide_insets{std::nan(""), std::nan(""), std::nan(""), std::nan("")};
    L::Ids guide_ids;
    // -alignmentRectInsets (top, left, bottom, right) of views without
    // subviews in the layout; others use their frames.
    std::array<double, 4> insets{};
    // Translating views: the frame and superview size the pin was made from.
    L::Rect2 pin_frame;
    L::Size2 pin_super;
    std::uint64_t pin_mask{};
    L::Ids pin_ids;
    // Intrinsic content size constraints.
    L::Size2 intrinsic{-1, -1};
    std::array<float, 2> hug{}, comp{};
    L::Ids intrinsic_ids;
};

struct Engine final : ObjectPayload {
    enum class Mode { window, detached, fitting };
    Mode mode{Mode::window};
    Address root{}, window{};
    L::Solver solver;
    bool valid{}, engaged{}, pending{true}, running{};
    std::map<Address, Node> nodes;
    std::vector<Address> order;                       // pre-order
    std::set<Address> parents_of_driven;
    struct Applied { L::Ids ids; };
    std::map<Address, Applied> applied;               // NSLayoutConstraint -> solver constraints
    std::set<Address> broken;                          // reported once
    L::Ids root_ids;
    L::Size2 root_size{-1, -1};
    bool root_edits{};
    // Islands: subtrees whose root keeps its autoresizing frame and that no
    // constraint outside them refers to (a table's cell views, say). They
    // are laid out by engines of their own after this one, so a change
    // inside one rebuilds only that island.
    Engine* top{};                                     // the window's engine (or this one)
    std::map<Address, std::shared_ptr<Engine>> islands;
    std::map<Address, Engine*> island_index;           // top engine: every island in the window, by root
};

struct Module {
    std::shared_ptr<AppKit> kit;
    Address constraint_class{}, anchor_class{}, x_anchor_class{}, y_anchor_class{}, dimension_class{};
    unsigned applying{};                               // > 0 while the engine sets frames
    std::vector<Engine*> running;                      // engines inside a pass, innermost last
    std::vector<LayoutContributor> contributors;
    std::map<Address, bool> requires_constraints;      // +requiresConstraintBasedLayout by class
};
Module& module() {
    static Module instance;
    return instance;
}

struct AnchorState final : ObjectPayload {
    Address item{};                                    // not retained
    std::int64_t attribute{};
};

std::shared_ptr<ConstraintState> constraint_state(Address constraint) {
    if (!constraint) completion_kit::fail("nil NSLayoutConstraint");
    return kit().objc.host_state<ConstraintState>(constraint);
}

bool is_view(Address object) { return object && kit().is_kind(object, kit().view); }

std::shared_ptr<appkit::GuideState> guide_state(Address item) { return item ? kit().objc.find_host_state<appkit::GuideState>(item) : nullptr; }
bool is_guide(Address item) { return guide_state(item) != nullptr; }
// The view an item lays out in (a layout guide's owning view) and its parent in the layout tree.
Address view_of(Address item) {
    if (const auto guide = guide_state(item)) return guide->owner;
    return item;
}
Address parent_of(Address item) {
    if (const auto guide = guide_state(item)) return guide->owner;
    return kit().view_state(item)->superview;
}

std::string class_name(Address object) {
    if (!object) return "nil";
    return kit().text(kit().send_id(kit().send_id(object, "class"), "description"));
}

std::string attribute_name(std::int64_t value) {
    static const char* names[] = {"notAnAttribute", "left", "right", "top", "bottom", "leading", "trailing", "width", "height",
                                  "centerX", "centerY", "lastBaseline", "firstBaseline"};
    return value >= 0 && value <= 12 ? names[value] : std::to_string(value);
}

std::string describe(Address constraint) {
    const auto cs = constraint_state(constraint);
    char buffer[64];
    std::string text = "<NSLayoutConstraint " + class_name(cs->first);
    std::snprintf(buffer, sizeof buffer, ":0x%llx.", static_cast<unsigned long long>(cs->first));
    text += buffer + attribute_name(cs->first_attribute);
    text += cs->relation < 0 ? " <= " : cs->relation > 0 ? " >= " : " == ";
    if (cs->second) {
        text += class_name(cs->second);
        std::snprintf(buffer, sizeof buffer, ":0x%llx.", static_cast<unsigned long long>(cs->second));
        text += buffer + attribute_name(cs->second_attribute);
        if (cs->multiplier != 1) { std::snprintf(buffer, sizeof buffer, " * %g", cs->multiplier); text += buffer; }
        std::snprintf(buffer, sizeof buffer, " + %g", cs->constant);
    } else {
        std::snprintf(buffer, sizeof buffer, "%g", cs->constant);
    }
    text += buffer;
    if (cs->priority < priority_required) { std::snprintf(buffer, sizeof buffer, " @%g", static_cast<double>(cs->priority)); text += buffer; }
    return text + ">";
}

Engine* window_engine(Address window, bool create) {
    auto& k = kit();
    const auto state = k.window_state(window);
    if (auto existing = std::dynamic_pointer_cast<Engine>(state->layout_engine)) return existing.get();
    if (!create) return nullptr;
    auto engine = std::make_shared<Engine>();
    engine->window = window;
    engine->top = engine.get();
    state->layout_engine = engine;
    return engine.get();
}

// The superview chain of an item (itself first; a layout guide, then its owning view).
std::vector<Address> ancestors(Address item) {
    std::vector<Address> chain;
    for (auto current = item; current; current = parent_of(current)) chain.push_back(current);
    return chain;
}

// The closest view that contains both items.
Address common_ancestor(Address a, Address b) {
    if (!b) return view_of(a);
    const auto chain = ancestors(a);
    const std::set<Address> lookup(chain.begin(), chain.end());
    for (auto current = b; current; current = parent_of(current))
        if (lookup.contains(current)) return view_of(current);
    return 0;
}

double standard_spacing(const ConstraintState& cs) {
    // AppKit's standard space: 20 points to a superview's edge, 8 between siblings.
    auto& k = kit();
    if (cs.second && is_view(cs.first) && is_view(cs.second) &&
        (k.view_state(cs.first)->superview == cs.second || k.view_state(cs.second)->superview == cs.first))
        return 20;
    return 8;
}

// ---- engine build -----------------------------------------------------------------------
bool flipped(Address view) { return kit().send_bool(view, "isFlipped"); }

L::Rect2 rect2(const Rect& r) { return {r.origin.x, r.origin.y, r.size.width, r.size.height}; }

// A frame (superview coordinates) <-> its alignment rectangle; insets are
// visual (top is the top on screen whatever the superview's flippedness).
L::Rect2 alignment_of(L::Rect2 frame, const std::array<double, 4>& insets, bool super_flipped) {
    const auto [top, left, bottom, right] = insets;
    return {frame.x + left, frame.y + (super_flipped ? top : bottom), frame.width - left - right, frame.height - top - bottom};
}
L::Rect2 frame_of(L::Rect2 alignment, const std::array<double, 4>& insets, bool super_flipped) {
    const auto [top, left, bottom, right] = insets;
    return {alignment.x - left, alignment.y - (super_flipped ? top : bottom), alignment.width + left + right, alignment.height + top + bottom};
}

void add_pin(Engine& e, Address view, Node& node) {
    auto& k = kit();
    const auto state = k.view_state(view);
    const auto parent = k.view_state(node.superview);
    node.pin_frame = alignment_of(rect2(state->frame), node.insets, e.nodes.at(node.superview).flipped);
    const auto bounds = AppKit::view_bounds(*parent);
    node.pin_super = {bounds.size.width, bounds.size.height};
    node.pin_mask = state->autoresizing_mask;
    for (const auto id : std::exchange(node.pin_ids, {})) e.solver.remove_constraint(id);
    L::add_autoresizing(e.solver, node.pin_ids, node.box, e.nodes.at(node.superview).box, node.pin_frame, node.pin_super, node.pin_mask,
                        e.nodes.at(node.superview).flipped);
}

void refresh_intrinsic(Engine& e, Address view, Node& node) {
    auto& k = kit();
    if (node.guide) return;
    const auto state = k.view_state(view);
    const auto metric = intrinsic_size(view);
    if (metric.width == node.intrinsic.width && metric.height == node.intrinsic.height && state->hugging == node.hug &&
        state->compression == node.comp && !node.intrinsic_ids.empty())
        return;
    if (metric.width < 0 && metric.height < 0 && node.intrinsic_ids.empty()) {
        node.intrinsic = metric;
        node.hug = state->hugging;
        node.comp = state->compression;
        return;
    }
    for (const auto id : std::exchange(node.intrinsic_ids, {})) e.solver.remove_constraint(id);
    node.intrinsic = metric;
    node.hug = state->hugging;
    node.comp = state->compression;
    try {
        L::add_intrinsic(e.solver, node.intrinsic_ids, node.box, metric, node.hug, node.comp);
    } catch (const L::UnsatisfiableConstraint&) {
        std::fprintf(stderr, "[alchemy] Unable to simultaneously satisfy constraints: the required content size of %s (%g x %g) is broken\n",
                     class_name(view).c_str(), metric.width, metric.height);
    }
}

// The insets {top, left, bottom, right} of a system layout guide within its
// owner's frame: the safe area (-safeAreaInsets), the layout margins (the
// standard spacing to a superview's edge, 20 points, inside the safe area)
// and a window's content layout rectangle (-contentLayoutRect, for the
// guide on its content view).
std::array<double, 4> system_guide_insets(const appkit::GuideState& guide) {
    auto& k = kit();
    using Kind = appkit::GuideState::Kind;
    std::array<double, 4> insets{};
    if (!guide.owner || guide.kind == Kind::custom) return insets;
    if (guide.kind == Kind::content_layout) {
        const auto window = k.view_state(guide.owner)->window;
        if (!window) return insets;
        const auto rect = result_rect(k.send(window, "contentLayoutRect"));
        const auto size = k.view_state(guide.owner)->frame.size;
        return {std::max(0.0, size.height - rect.origin.y - rect.size.height), std::max(0.0, rect.origin.x), std::max(0.0, rect.origin.y),
                std::max(0.0, size.width - rect.origin.x - rect.size.width)};
    }
    const auto safe = k.send(guide.owner, "safeAreaInsets");
    for (unsigned i = 0; i < 4; ++i) insets[i] = safe.d(i) + (guide.kind == Kind::margins ? 20 : 0);
    return insets;
}

// System guides: required constraints that put each guide inside its
// owner's frame by its insets, made again when the insets changed (the
// window's title bar, the owner's place in the window). With `apply` false
// it only tells whether any changed.
bool refresh_guides(Engine& e, bool apply) {
    bool changed = false;
    for (const auto item : e.order) {
        auto& node = e.nodes.at(item);
        if (!node.guide) continue;
        const auto guide = guide_state(item);
        if (!guide || guide->kind == appkit::GuideState::Kind::custom) continue;
        const auto insets = system_guide_insets(*guide);
        if (insets == node.guide_insets) continue;
        changed = true;
        if (!apply) continue;
        node.guide_insets = insets;
        for (const auto id : std::exchange(node.guide_ids, {})) e.solver.remove_constraint(id);
        const auto& owner = e.nodes.at(node.superview);
        // The owner's box is its alignment rectangle, inside its frame by its insets.
        const auto [owner_top, owner_left, owner_bottom, owner_right] = owner.insets;
        const auto [top, left, bottom, right] = insets;
        try {
            L::add(e.solver, node.guide_ids, node.box.x, L::Relation::equal, L::Expression(owner.box.x).plus(left - owner_left), L::required);
            L::add(e.solver, node.guide_ids, node.box.y, L::Relation::equal, L::Expression(owner.box.y).plus(top - owner_top), L::required);
            L::add(e.solver, node.guide_ids, node.box.width, L::Relation::equal,
                   L::Expression(owner.box.width).plus(owner_left + owner_right - left - right), L::required);
            L::add(e.solver, node.guide_ids, node.box.height, L::Relation::equal,
                   L::Expression(owner.box.height).plus(owner_top + owner_bottom - top - bottom), L::required);
        } catch (const L::UnsatisfiableConstraint&) {
            std::fprintf(stderr, "[alchemy] Unable to simultaneously satisfy constraints: the system layout guide of %s is broken\n",
                         class_name(guide->owner).c_str());
        }
    }
    return changed;
}

void report_broken(Engine& e, Address constraint) {
    if (!e.broken.insert(constraint).second) return;
    std::fprintf(stderr, "[alchemy] Unable to simultaneously satisfy constraints. Will attempt to recover by breaking constraint %s\n",
                 describe(constraint).c_str());
}

void apply_constraint(Engine& e, Address constraint) {
    const auto cs = constraint_state(constraint);
    const auto first = e.nodes.find(cs->first);
    const auto second = cs->second ? e.nodes.find(cs->second) : e.nodes.end();
    if (first == e.nodes.end() || (cs->second && second == e.nodes.end())) return;
    const auto needs_baseline = [](std::int64_t a) { return a == attribute::first_baseline || a == attribute::last_baseline; };
    const auto offsets = [&](Address item, std::int64_t a) {
        return needs_baseline(a) ? baseline_offsets(item) : std::pair<double, double>{0, 0};
    };
    const auto [first_top, first_bottom] = offsets(cs->first, cs->first_attribute);
    L::Expression lhs, rhs;
    try {
        lhs = L::attribute_expression(first->second.box, cs->first_attribute, first_top, first_bottom);
        const double constant = cs->symbolic.empty() ? cs->constant : standard_spacing(*cs) * (cs->constant < 0 ? -1 : 1);
        if (cs->second) {
            const auto [second_top, second_bottom] = offsets(cs->second, cs->second_attribute);
            rhs = L::attribute_expression(second->second.box, cs->second_attribute, second_top, second_bottom).scaled(cs->multiplier).plus(constant);
        } else {
            if (cs->second_attribute != attribute::none)
                completion_kit::unsupported("NSLayoutConstraint " + describe(constraint) + ": a second attribute without a second item");
            rhs = L::Expression(constant);
        }
    } catch (const L::SolverError& error) {
        completion_kit::unsupported(std::string(error.what()) + " in " + describe(constraint));
    }
    const auto relation = cs->relation < 0 ? L::Relation::less_equal : cs->relation > 0 ? L::Relation::greater_equal : L::Relation::equal;
    auto& slot = e.applied[constraint];
    for (const auto id : std::exchange(slot.ids, {})) e.solver.remove_constraint(id);
    try {
        L::add(e.solver, slot.ids, lhs, relation, rhs, cs->priority);
    } catch (const L::UnsatisfiableConstraint&) {
        report_broken(e, constraint);
    }
}

Engine& top_of(Engine& e) { return e.top ? *e.top : e; }

void unregister_islands(Engine& top, Engine& e) {
    for (auto& [root, island] : e.islands) {
        top.island_index.erase(root);
        unregister_islands(top, *island);
    }
}

const LayoutContributor* contributor_of(Address view) {
    for (const auto& contributor : module().contributors)
        if (kit().is_kind(view, contributor.cls)) return &contributor;
    return nullptr;
}

// The islands of e's tree: translating views (not the root) with layout
// below them that no constraint outside their subtree refers to and no
// stack or grid view arranges, outermost first.
void update_islands(Engine& e) {
    auto& k = kit();
    auto& top = top_of(e);
    std::set<Address> roots;
    if (e.mode != Engine::Mode::fitting) {
        const auto all = k.all_views(e.root);
        std::set<Address> referenced, layout_below;
        for (const auto v : all) {
            const auto state = k.view_state(v);
            bool has_layout = (v != e.root && !state->translates_autoresizing);
            for (const auto c : state->constraints) {
                const auto cs = k.objc.find_host_state<ConstraintState>(c);
                if (!cs || !cs->active) continue;
                has_layout = true;
                for (const auto item : {cs->first, cs->second})
                    for (auto x = view_of(item); x && x != v && is_view(x); x = k.view_state(x)->superview) referenced.insert(x);
            }
            if (const auto contributor = contributor_of(v)) {
                has_layout = true;
                for (const auto managed : contributor->managed_views(v)) referenced.insert(managed);
            }
            if (has_layout)
                for (auto y = v; y; y = k.view_state(y)->superview) {
                    if (!layout_below.insert(y).second || y == e.root) break;
                }
        }
        for (const auto v : all) {
            if (v == e.root || !layout_below.contains(v) || referenced.contains(v)) continue;
            const auto state = k.view_state(v);
            if (!state->translates_autoresizing || contributor_of(v)) continue;
            bool nested = false;
            for (auto y = state->superview; y && y != e.root; y = k.view_state(y)->superview)
                if (roots.contains(y)) { nested = true; break; }
            if (!nested) roots.insert(v);
        }
    }
    std::map<Address, std::shared_ptr<Engine>> kept;
    for (const auto root : roots) {
        auto found = e.islands.find(root);
        std::shared_ptr<Engine> island;
        if (found != e.islands.end()) {
            island = found->second;
        } else {
            island = std::make_shared<Engine>();
            island->mode = Engine::Mode::detached;
            island->root = root;
            island->window = e.window;
            island->top = &top;
        }
        kept.emplace(root, island);
    }
    for (auto& [root, island] : e.islands)
        if (!kept.contains(root)) {
            top.island_index.erase(root);
            unregister_islands(top, *island);
        }
    e.islands = std::move(kept);
    for (auto& [root, island] : e.islands) top.island_index[root] = island.get();
}

void rebuild(Engine& e) {
    auto& k = kit();
    auto& m = module();
    e.solver = L::Solver{};
    e.nodes.clear();
    e.order.clear();
    e.parents_of_driven.clear();
    e.applied.clear();
    e.root_ids.clear();
    e.root_edits = false;
    e.root_size = {-1, -1};
    e.valid = true;
    e.engaged = false;
    if (!e.root) return;
    update_islands(e);
    // This engine's tree: the root's subtree without the islands.
    std::vector<Address> views;
    {
        std::vector<Address> pending{e.root};
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            views.push_back(current);
            const auto& children = k.view_state(current)->subviews;
            for (auto it = children.rbegin(); it != children.rend(); ++it)
                if (!e.islands.contains(*it)) pending.push_back(*it);
        }
    }
    const std::set<Address> tree(views.begin(), views.end());
    // The layout guides of the tree's views are items too (positioned by the solution).
    std::set<Address> items = tree;
    for (const auto v : views)
        for (const auto guide : k.view_state(v)->layout_guides) items.insert(guide);
    std::set<Address> part, driven;
    std::vector<Address> constraints;
    std::vector<std::pair<Address, const LayoutContributor*>> containers;
    for (const auto v : views) {
        const auto state = k.view_state(v);
        if (v != e.root && !state->translates_autoresizing) { part.insert(v); driven.insert(v); }
        for (const auto c : state->constraints) {
            const auto cs = k.objc.find_host_state<ConstraintState>(c);
            if (!cs || !cs->active) continue;
            if (!items.contains(cs->first) || (cs->second && !items.contains(cs->second))) continue;
            constraints.push_back(c);
            for (const auto item : {cs->first, cs->second}) {
                if (!item) continue;
                part.insert(item);
                if (!tree.contains(item)) driven.insert(item);
            }
        }
        for (const auto& contributor : m.contributors)
            if (k.is_kind(v, contributor.cls)) {
                containers.emplace_back(v, &contributor);
                part.insert(v);
                for (const auto managed : contributor.managed_views(v))
                    if (tree.contains(managed) && k.view_state(managed)->superview == v) { part.insert(managed); driven.insert(managed); }
                break;
            }
    }
    if (part.empty() && e.mode != Engine::Mode::fitting) return;
    // Every superview up to the root takes part too (a guide's owning view first).
    for (const auto v : std::vector<Address>(part.begin(), part.end()))
        for (auto current = parent_of(v); current && tree.contains(current); current = k.view_state(current)->superview)
            if (!part.insert(current).second) break;
    part.insert(e.root);
    driven.erase(e.root);
    for (const auto v : views) {
        if (part.contains(v)) e.order.push_back(v);
        for (const auto guide : k.view_state(v)->layout_guides)
            if (part.contains(guide)) e.order.push_back(guide);
    }
    for (const auto v : e.order) {
        Node node;
        node.guide = !tree.contains(v);
        node.box = L::make_box(e.solver, node.guide ? "guide" : "view");
        node.root = v == e.root;
        node.superview = node.root ? 0 : parent_of(v);
        node.driven = driven.contains(v);
        node.flipped = !node.guide && flipped(v);
        e.nodes.emplace(v, std::move(node));
    }
    // Alignment insets of the leaves.
    std::set<Address> parents;
    for (const auto v : e.order)
        if (v != e.root && !e.nodes.at(v).guide) parents.insert(k.view_state(v)->superview);
    for (const auto v : e.order) {
        if (v == e.root || parents.contains(v) || e.nodes.at(v).guide) continue;
        const auto insets = k.send(v, "alignmentRectInsets");
        e.nodes.at(v).insets = {insets.d(0), insets.d(1), insets.d(2), insets.d(3)};
    }
    for (const auto v : e.order) {
        if (driven.contains(v)) e.parents_of_driven.insert(e.nodes.at(v).superview);
    }
    const auto& root_box = e.nodes.at(e.root).box;
    L::add(e.solver, e.root_ids, root_box.x, L::Relation::equal, 0, L::required);
    L::add(e.solver, e.root_ids, root_box.y, L::Relation::equal, 0, L::required);
    for (const auto v : e.order) {
        auto& node = e.nodes.at(v);
        if (node.root || node.driven) continue;
        add_pin(e, v, node);
    }
    for (const auto v : e.order) refresh_intrinsic(e, v, e.nodes.at(v));
    for (const auto c : constraints) apply_constraint(e, c);
    for (const auto& [container, contributor] : containers) {
        L::Ids ids;
        const auto box_of = [&e](Address view) -> std::optional<L::Box> {
            const auto found = e.nodes.find(view);
            if (found == e.nodes.end()) return std::nullopt;
            return found->second.box;
        };
        try {
            contributor->add_constraints(container, e.solver, ids, e.nodes.at(container).box, box_of);
        } catch (const L::UnsatisfiableConstraint&) {
            std::fprintf(stderr, "[alchemy] Unable to simultaneously satisfy constraints: the arrangement of %s is broken\n",
                         class_name(container).c_str());
        }
    }
    e.engaged = true;
}

// Root size: the window's content size (an edit variable at 500), a fixed
// size (a detached view tree), or as small as possible (fitting size).
void place_root(Engine& e) {
    auto& k = kit();
    const auto& box = e.nodes.at(e.root).box;
    const auto frame = k.view_state(e.root)->frame.size;
    switch (e.mode) {
    case Engine::Mode::window:
        if (!e.root_edits) {
            e.solver.add_edit_variable(box.width, 500);
            e.solver.add_edit_variable(box.height, 500);
            e.root_edits = true;
        }
        e.solver.suggest_value(box.width, frame.width);
        e.solver.suggest_value(box.height, frame.height);
        break;
    case Engine::Mode::detached:
        if (e.root_size.width == frame.width && e.root_size.height == frame.height) break;
        if (e.root_ids.size() > 2) {
            for (std::size_t i = 2; i < e.root_ids.size(); ++i) e.solver.remove_constraint(e.root_ids[i]);
            e.root_ids.resize(2);
        }
        L::add(e.solver, e.root_ids, box.width, L::Relation::equal, frame.width, L::required);
        L::add(e.solver, e.root_ids, box.height, L::Relation::equal, frame.height, L::required);
        e.root_size = {frame.width, frame.height};
        break;
    case Engine::Mode::fitting:
        if (e.root_ids.size() == 2) {
            L::add(e.solver, e.root_ids, box.width, L::Relation::equal, 0, 50);   // NSLayoutPriorityFittingSizeCompression
            L::add(e.solver, e.root_ids, box.height, L::Relation::equal, 0, 50);
        }
        break;
    }
}

// -updateConstraints to the views that need it, children before parents.
void update_constraints(Address root) {
    auto& k = kit();
    if (!root) return;
    const auto views = k.all_views(root);
    for (auto it = views.rbegin(); it != views.rend(); ++it) {
        const auto state = k.view_state(*it);
        if (!state->needs_update_constraints) continue;
        state->needs_update_constraints = false;
        static_cast<void>(k.send(*it, "updateConstraints"));
    }
}

bool requires_constraint_layout(Address view) {
    auto& k = kit();
    auto& m = module();
    const auto cls = k.objc.object_class(view);
    if (const auto found = m.requires_constraints.find(cls); found != m.requires_constraints.end()) return found->second;
    const bool value = k.send_bool(cls, "requiresConstraintBasedLayout");
    m.requires_constraints[cls] = value;
    return value;
}

// Whether the tree uses constraint-based layout at all.
bool engaged_tree(Address root) {
    auto& k = kit();
    for (const auto v : k.all_views(root)) {
        const auto state = k.view_state(v);
        if (!state->constraints.empty() || (!state->translates_autoresizing && v != root) || requires_constraint_layout(v)) return true;
        for (const auto& contributor : module().contributors)
            if (k.is_kind(v, contributor.cls)) return true;
    }
    return false;
}

// -layout top-down to the views that need it (and to the superviews of
// constraint-positioned views).
void layout_sweep(Engine* e, Address root) {
    auto& k = kit();
    for (const auto v : k.all_views(root)) {
        const auto state = k.view_state(v);
        const bool parent = e && e->engaged && e->parents_of_driven.contains(v);
        if (!parent && !state->needs_layout) continue;
        state->needs_layout = false;
        static_cast<void>(k.send(v, "layout"));
    }
}

// Translating views whose frames moved differently from their pins are
// pinned again; true when any was.
bool repin(Engine& e) {
    auto& k = kit();
    bool changed = false;
    for (const auto v : e.order) {
        auto& node = e.nodes.at(v);
        if (node.root || node.driven) continue;
        const auto state = k.view_state(v);
        const auto bounds = AppKit::view_bounds(*k.view_state(node.superview));
        const auto expected = L::autoresized_frame(node.pin_frame, node.pin_super, {bounds.size.width, bounds.size.height}, node.pin_mask);
        const auto actual = alignment_of(rect2(state->frame), node.insets, e.nodes.at(node.superview).flipped);
        const auto off = [](double a, double b) { return std::abs(a - b) > 0.5; };
        if (state->autoresizing_mask != node.pin_mask || off(expected.x, actual.x) || off(expected.y, actual.y) ||
            off(expected.width, actual.width) || off(expected.height, actual.height)) {
            add_pin(e, v, node);
            changed = true;
        }
    }
    return changed;
}

void run_own_pass(Engine& e);

void run_pass(Engine& e) {
    if (e.running || !e.root) return;
    run_own_pass(e);
    // Islands whose contents changed or whose root was resized.
    for (auto& [root, island] : std::map<Address, std::shared_ptr<Engine>>(e.islands)) {
        const auto state = kit().objc.find_host_state<appkit::ViewState>(root);
        if (!state || state->superview == 0) continue;
        const auto size = state->frame.size;
        if (island->pending || !island->valid || island->root_size.width != size.width || island->root_size.height != size.height)
            run_pass(*island);
    }
}

void run_own_pass(Engine& e) {
    auto& k = kit();
    auto& m = module();
    e.running = true;
    m.running.push_back(&e);
    struct Leave {
        Engine& e;
        ~Leave() {
            e.running = false;
            auto& list = module().running;
            list.erase(std::remove(list.begin(), list.end(), &e), list.end());
        }
    } leave{e};
    e.pending = false;
    const bool engaged = e.mode == Engine::Mode::fitting || engaged_tree(e.root);
    if (!engaged) {
        e.engaged = false;
        if (!e.islands.empty()) {
            unregister_islands(top_of(e), e);
            e.islands.clear();
        }
        layout_sweep(nullptr, e.root);
        return;
    }
    update_constraints(e.root);
    if (!e.valid) rebuild(e);
    if (!e.engaged) {
        layout_sweep(nullptr, e.root);
        return;
    }
    for (int round = 0; round < 4; ++round) {
        place_root(e);
        for (const auto v : e.order) refresh_intrinsic(e, v, e.nodes.at(v));
        static_cast<void>(refresh_guides(e, true));
        e.solver.update_variables();
        if (e.mode == Engine::Mode::fitting) return;
        if (e.mode == Engine::Mode::window && e.window) {
            // Stronger constraints resize the window.
            const auto solved = L::box_value(e.solver, e.nodes.at(e.root).box);
            const auto current = k.view_state(e.root)->frame.size;
            const auto scale = k.backing_scale(e.window);
            const Size wanted{std::round(solved.width * scale) / scale, std::round(solved.height * scale) / scale};
            if ((std::abs(wanted.width - current.width) > 0.5 || std::abs(wanted.height - current.height) > 0.5) && round < 3) {
                ++m.applying;
                try {
                    CallRegs registers;
                    put_size(registers, 0, wanted);
                    static_cast<void>(k.send(e.window, "setContentSize:", registers));
                } catch (...) { --m.applying; throw; }
                --m.applying;
                const auto now = k.view_state(e.root)->frame.size;
                if (std::abs(now.width - current.width) > 0.5 || std::abs(now.height - current.height) > 0.5) continue;
            }
        }
        layout_sweep(&e, e.root);
        // Moved views can change the system guides' insets (the safe area under the title bar).
        const bool repinned = repin(e);
        if (!repinned && !refresh_guides(e, false)) break;
    }
}

// The innermost running engine that lays out `view`.
Engine* running_engine_for(Address view) {
    auto& list = module().running;
    for (auto it = list.rbegin(); it != list.rend(); ++it)
        if ((*it)->nodes.contains(view)) return *it;
    return nullptr;
}

// -[NSView layout]: the frames of the receiver's constraint-positioned subviews.
void apply_subview_frames(Address view) {
    auto& k = kit();
    auto* e = running_engine_for(view);
    if (!e || !e->engaged) return;
    const auto& parent = e->nodes.at(view);
    const auto parent_box = L::box_value(e->solver, parent.box);
    const auto scale = e->window ? k.backing_scale(e->window) : 1.0;
    const auto children = k.view_state(view)->subviews;
    for (const auto child : children) {
        const auto found = e->nodes.find(child);
        if (found == e->nodes.end() || !found->second.driven) continue;
        const auto aligned = L::frame_from_box(L::box_value(e->solver, found->second.box), parent_box, parent.flipped, scale);
        const auto frame = frame_of(aligned, found->second.insets, parent.flipped);
        const auto current = k.view_state(child)->frame;
        if (current.origin.x == frame.x && current.origin.y == frame.y && current.size.width == frame.width &&
            current.size.height == frame.height)
            continue;
        ++module().applying;
        try {
            CallRegs registers;
            put_rect(registers, 0, {{frame.x, frame.y}, {frame.width, frame.height}});
            static_cast<void>(k.send(child, "setFrame:", registers));
        } catch (...) { --module().applying; throw; }
        --module().applying;
    }
    // The receiver's layout guides keep their solved frames (in its coordinates;
    // its alignment rectangle starts inside its frame by its insets).
    for (const auto guide : k.view_state(view)->layout_guides) {
        const auto found = e->nodes.find(guide);
        if (found == e->nodes.end()) continue;
        auto frame = L::frame_from_box(L::box_value(e->solver, found->second.box), parent_box, parent.flipped, scale);
        const auto [top, left, bottom, right] = parent.insets;
        frame.x += left;
        frame.y += parent.flipped ? top : bottom;
        if (const auto state = guide_state(guide)) state->frame = {{frame.x, frame.y}, {frame.width, frame.height}};
    }
}

void layout_window(Address window) {
    auto& k = kit();
    const auto state = k.window_state(window);
    const auto content = state->content_view;
    auto* e = window_engine(window, true);
    if (e->root != content) { e->root = content; e->valid = false; e->pending = true; }
    if (!content) return;
    // Content changes show as redisplay: a redrawn view whose intrinsic
    // size changed (a label's new text) needs a pass.
    const std::function<bool(Engine&)> changed = [&](Engine& engine) {
        bool any = false;
        if (engine.valid && engine.engaged && refresh_guides(engine, false)) { engine.pending = true; any = true; }
        if (engine.valid && engine.engaged && !engine.pending)
            for (const auto& [v, node] : engine.nodes) {
                if (node.guide || !k.view_state(v)->needs_display) continue;
                const auto metric = intrinsic_size(v);
                if (metric.width != node.intrinsic.width || metric.height != node.intrinsic.height) { engine.pending = true; any = true; break; }
            }
        for (auto& [root, island] : engine.islands) any = changed(*island) || any;
        return any;
    };
    if (!e->pending && changed(*e)) e->pending = true;
    if (!e->pending) return;
    run_pass(*e);
}

// A detached view tree (no window): rooted at its topmost superview.
void layout_detached(Address view) {
    auto& k = kit();
    auto root = view;
    while (const auto parent = k.view_state(root)->superview) root = parent;
    Engine e;
    e.mode = Engine::Mode::detached;
    e.root = root;
    run_pass(e);
}

Size fitting_size(Address view) {
    auto& k = kit();
    Engine e;
    e.mode = Engine::Mode::fitting;
    e.root = view;
    run_pass(e);
    if (!e.engaged || !e.nodes.contains(view)) return {0, 0};
    const auto box = L::box_value(e.solver, e.nodes.at(view).box);
    return {std::max(0.0, box.width), std::max(0.0, box.height)};
}

// ---- constraint bookkeeping ------------------------------------------------------------
void hold(Address view, Address constraint) {
    auto& k = kit();
    const auto cs = constraint_state(constraint);
    cs->holder = view;
    cs->active = true;
    k.view_state(view)->constraints.push_back(k.retain(constraint));
    invalidate_layout(view, true);
}

void unhold(Address constraint) {
    auto& k = kit();
    const auto cs = constraint_state(constraint);
    const auto holder = std::exchange(cs->holder, 0);
    cs->active = false;
    if (!holder) return;
    auto& list = k.view_state(holder)->constraints;
    const auto found = std::find(list.begin(), list.end(), constraint);
    if (found == list.end()) return;
    list.erase(found);
    invalidate_layout(holder, true);
    k.release(constraint);
}

// A constant or priority changed: update the engine in place when it is current.
void constraint_changed(Address constraint) {
    auto& k = kit();
    const auto cs = constraint_state(constraint);
    if (!cs->active || !cs->holder) return;
    const auto window = k.view_state(cs->holder)->window;
    if (!window) return;
    auto* top = window_engine(window, false);
    if (!top) return;
    top->pending = true;
    Engine* e = top;
    for (auto current = cs->holder; current; current = k.view_state(current)->superview)
        if (const auto found = top->island_index.find(current); found != top->island_index.end()) { e = found->second; break; }
    e->pending = true;
    if (!e->valid || e->running || !e->applied.contains(constraint)) { e->valid = false; return; }
    e->broken.erase(constraint);
    apply_constraint(*e, constraint);
}

// Removing `view` from `parent`: constraints held by `parent` and its
// superviews that refer to the removed tree go (AppKit's
// -removeFromSuperview).
void drop_crossing_constraints(Address view, Address parent) {
    auto& k = kit();
    const auto removed_list = k.all_views(view);
    std::set<Address> removed(removed_list.begin(), removed_list.end());
    for (const auto v : removed_list)
        for (const auto guide : k.view_state(v)->layout_guides) removed.insert(guide);
    for (auto current = parent; current; current = k.view_state(current)->superview) {
        const auto list = k.view_state(current)->constraints;
        for (const auto c : list) {
            const auto cs = k.objc.find_host_state<ConstraintState>(c);
            if (cs && (removed.contains(cs->first) || (cs->second && removed.contains(cs->second)))) unhold(c);
        }
    }
}

Address make_anchor(Address item, std::int64_t attribute_value) {
    auto& k = kit();
    auto& m = module();
    Address cls = m.y_anchor_class;
    if (attribute_value == attribute::width || attribute_value == attribute::height) cls = m.dimension_class;
    else if (L::attribute_is_horizontal(attribute_value)) cls = m.x_anchor_class;
    const auto anchor = k.alloc_init(cls);
    const auto state = k.objc.host_state<AnchorState>(anchor);
    state->item = item;
    state->attribute = attribute_value;
    return k.autorelease(anchor);
}

std::shared_ptr<AnchorState> anchor_state(Address anchor) {
    const auto state = kit().objc.find_host_state<AnchorState>(anchor);
    if (!state) completion_kit::fail("an NSLayoutAnchor argument is not an anchor");
    return state;
}

// ---- layout guides ------------------------------------------------------------------------
// -[NSView addLayoutGuide:] (from another owner too) and -removeLayoutGuide:
// (the constraints that refer to the guide go, as for a removed view).
void add_guide(Address view, Address guide) {
    auto& k = kit();
    const auto state = guide_state(guide);
    if (!state) completion_kit::fail("-[NSView addLayoutGuide:] with a " + class_name(guide));
    if (state->owner == view) return;
    const auto keep = k.retain(guide);
    if (state->owner) static_cast<void>(k.send_id(state->owner, "removeLayoutGuide:", {guide}));
    state->owner = view;
    k.view_state(view)->layout_guides.push_back(keep);
    invalidate_layout(view, true);
}

void remove_guide(Address view, Address guide) {
    auto& k = kit();
    const auto state = guide_state(guide);
    if (!state || state->owner != view) return;
    for (auto current = view; current; current = k.view_state(current)->superview) {
        const auto list = k.view_state(current)->constraints;
        for (const auto c : list) {
            const auto cs = k.objc.find_host_state<ConstraintState>(c);
            if (cs && (cs->first == guide || cs->second == guide)) unhold(c);
        }
    }
    auto& guides = k.view_state(view)->layout_guides;
    guides.erase(std::remove(guides.begin(), guides.end(), guide), guides.end());
    state->owner = 0;
    invalidate_layout(view, true);
    k.release(guide);
}

// A view's system guide of `kind` (made on first use; the window's content
// layout guide lives on its content view).
Address system_guide(Address view, appkit::GuideState::Kind kind) {
    auto& k = kit();
    for (const auto guide : k.view_state(view)->layout_guides)
        if (const auto state = guide_state(guide); state && state->kind == kind) return guide;
    const auto guide = k.alloc_init(k.objc.find_class("NSLayoutGuide"));
    guide_state(guide)->kind = kind;
    add_guide(view, guide);
    k.release(guide);
    return guide;
}

// -[NSLayoutGuide frame]: a system guide's from its owner's bounds now, a custom guide's as last solved.
Rect guide_frame(Address guide) {
    auto& k = kit();
    const auto state = guide_state(guide);
    if (!state->owner || state->kind == appkit::GuideState::Kind::custom) return state->frame;
    const auto [top, left, bottom, right] = system_guide_insets(*state);
    const auto owner = k.view_state(state->owner);
    const auto bounds = AppKit::view_bounds(*owner);
    const bool upside_down = flipped(state->owner);
    return {{bounds.origin.x + left, bounds.origin.y + (upside_down ? top : bottom)},
            {std::max(0.0, bounds.size.width - left - right), std::max(0.0, bounds.size.height - top - bottom)}};
}

// -constraintsAffectingLayoutForOrientation:: the active NSLayoutConstraints
// that refer to the item on that axis (not the engine's own content-size or
// autoresizing constraints, which are not NSLayoutConstraint objects here).
std::vector<Address> constraints_affecting(Address item, std::uint64_t orientation) {
    auto& k = kit();
    if (orientation > 1) completion_kit::fail("NSLayoutConstraintOrientation " + std::to_string(orientation));
    std::vector<Address> found;
    for (auto current = view_of(item); current; current = k.view_state(current)->superview)
        for (const auto c : k.view_state(current)->constraints) {
            const auto cs = k.objc.find_host_state<ConstraintState>(c);
            if (!cs || !cs->active) continue;
            const auto on_axis = [orientation](std::int64_t a) { return L::attribute_is_horizontal(a) == (orientation == 0); };
            if ((cs->first == item && on_axis(cs->first_attribute)) || (cs->second == item && on_axis(cs->second_attribute))) found.push_back(c);
        }
    return found;
}

Address constraint_with_anchors(Address first, std::int64_t relation, Address second, double multiplier, double constant) {
    const auto a = anchor_state(first);
    if (!second) return kit().autorelease(make_constraint(a->item, a->attribute, relation, 0, attribute::none, 1, constant));
    const auto b = anchor_state(second);
    const bool dimension = a->attribute == attribute::width || a->attribute == attribute::height;
    const bool other_dimension = b->attribute == attribute::width || b->attribute == attribute::height;
    if (dimension != other_dimension || (!dimension && L::attribute_is_horizontal(a->attribute) != L::attribute_is_horizontal(b->attribute)))
        completion_kit::fail("NSLayoutAnchor: a constraint between anchors of different axes");
    return kit().autorelease(make_constraint(a->item, a->attribute, relation, b->item, b->attribute, multiplier, constant));
}
} // namespace

AppKit& kit() {
    auto& m = module();
    if (!m.kit) {
        m.kit = appkit::instance();
        if (!m.kit) completion_kit::fail("AppKit is not installed");
    }
    return *m.kit;
}

void register_layout_contributor(LayoutContributor contributor) { module().contributors.push_back(std::move(contributor)); }

layout::Size2 intrinsic_size(Address view) {
    const auto size = result_size(kit().send(view, "intrinsicContentSize"));
    return {size.width, size.height};
}

std::pair<double, double> baseline_offsets(Address view) {
    auto& k = kit();
    return {k.send(view, "firstBaselineOffsetFromTop").d(0), k.send(view, "lastBaselineOffsetFromBottom").d(0)};
}

Address make_constraint(Address first, std::int64_t first_attribute, std::int64_t relation, Address second,
                        std::int64_t second_attribute, double multiplier, double constant, float priority) {
    auto& k = kit();
    if (!first) completion_kit::fail("NSLayoutConstraint without a first item");
    if (relation < -1 || relation > 1) completion_kit::fail("NSLayoutConstraint relation " + std::to_string(relation));
    const auto constraint = k.alloc_init(module().constraint_class);
    const auto cs = constraint_state(constraint);
    cs->first = first;
    cs->second = second;
    cs->first_attribute = first_attribute;
    cs->second_attribute = second ? second_attribute : attribute::none;
    cs->relation = relation;
    cs->multiplier = multiplier;
    cs->constant = constant;
    cs->priority = priority;
    return constraint;
}

void set_active(Address constraint, bool active) {
    const auto cs = constraint_state(constraint);
    if (cs->active == active && (!active || cs->holder)) return;
    if (!active) { unhold(constraint); return; }
    for (const auto item : {cs->first, cs->second}) {
        if (!item || is_view(item)) continue;
        const auto guide = guide_state(item);
        if (!guide) completion_kit::unsupported("activating an NSLayoutConstraint whose item is a " + class_name(item));
        if (!guide->owner)
            completion_kit::fail("Unable to activate constraint " + describe(constraint) +
                                 " because its layout guide has no owning view (-[NSView addLayoutGuide:])");
    }
    const auto holder = common_ancestor(cs->first, cs->second);
    if (!holder)
        completion_kit::fail("Unable to activate constraint " + describe(constraint) +
                   " because it has no common ancestor (the items are in different view hierarchies)");
    hold(holder, constraint);
}

void invalidate_layout(Address view, bool structure) {
    auto& k = kit();
    if (!view || !is_view(view)) return;
    const auto window = k.view_state(view)->window;
    if (!window) return;
    auto* e = window_engine(window, true);
    e->pending = true;
    Engine* target = e;
    for (auto current = view; current; current = k.view_state(current)->superview)
        if (const auto found = e->island_index.find(current); found != e->island_index.end()) { target = found->second; break; }
    target->pending = true;
    if (structure) target->valid = false;
}

void install_auto_layout(Support& support) {
    static_cast<void>(support);
    auto& m = module();
    // A fresh provider per AppKit install (tests install several runtimes).
    m.kit = appkit::instance();
    if (!m.kit) completion_kit::fail("AppKit is not installed");
    m.contributors.clear();
    m.requires_constraints.clear();
    m.running.clear();
    m.applying = 0;
    auto& k = kit();
    const auto v = k.view, w = k.window;

    // ---- the AppKit hooks ----
    k.layout_window_hook = [](Address window) { layout_window(window); };
    k.frame_changed_hook = [](Address view) {
        auto& kk = kit();
        const auto state = kk.view_state(view);
        state->needs_layout = true;
        if (module().applying) return;
        invalidate_layout(view, false);
        if (state->superview) invalidate_layout(state->superview, false);
    };
    k.hierarchy_changed_hook = [](Address view, Address old_parent) {
        if (old_parent) {
            drop_crossing_constraints(view, old_parent);
            invalidate_layout(old_parent, true);
        } else {
            kit().view_state(view)->needs_update_constraints = true;
            invalidate_layout(view, true);
        }
    };

    // ---- classes ----
    m.constraint_class = k.declare_class("NSLayoutConstraint", "NSObject");
    m.anchor_class = k.declare_class("NSLayoutAnchor", "NSObject");
    m.x_anchor_class = k.declare_class("NSLayoutXAxisAnchor", "NSLayoutAnchor");
    m.y_anchor_class = k.declare_class("NSLayoutYAxisAnchor", "NSLayoutAnchor");
    m.dimension_class = k.declare_class("NSLayoutDimension", "NSLayoutAnchor");
    // const CGFloat NSViewNoIntrinsicMetric = -1.
    for (const auto library : {appkit::appkit_library, appkit::cocoa_library}) {
        if (k.process.resolve(library, "_NSViewNoIntrinsicMetric")) continue;
        const auto slot = k.process.allocate_guest(8);
        k.process.main_engine().write64(slot, std::bit_cast<std::uint64_t>(-1.0));
        k.process.install_export(std::string(library), "_NSViewNoIntrinsicMetric", {slot, ExportKind::data});
    }

    // ---- NSLayoutConstraint ----
    const auto c = m.constraint_class;
    k.class_method(c, "constraintWithItem:attribute:relatedBy:toItem:attribute:multiplier:constant:", [](CpuState& s, GuestMemory&) {
        s.x[0] = kit().autorelease(make_constraint(s.x[2], static_cast<std::int64_t>(s.x[3]), static_cast<std::int64_t>(s.x[4]), s.x[5],
                                                   static_cast<std::int64_t>(s.x[6]), d(s, 0), d(s, 1)));
    }, "@@:@qq@qdd");
    k.class_method(c, "activateConstraints:", [](CpuState& s, GuestMemory&) {
        for (const auto item : kit().items(s.x[2])) set_active(item, true);
    }, "v@:@");
    k.class_method(c, "deactivateConstraints:", [](CpuState& s, GuestMemory&) {
        for (const auto item : kit().items(s.x[2])) set_active(item, false);
    }, "v@:@");
    k.method(c, "firstItem", [](CpuState& s, GuestMemory&) { s.x[0] = constraint_state(s.x[0])->first; });
    k.method(c, "secondItem", [](CpuState& s, GuestMemory&) { s.x[0] = constraint_state(s.x[0])->second; });
    k.method(c, "firstAttribute", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(constraint_state(s.x[0])->first_attribute); }, "q@:");
    k.method(c, "secondAttribute", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(constraint_state(s.x[0])->second_attribute); }, "q@:");
    k.method(c, "relation", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(constraint_state(s.x[0])->relation); }, "q@:");
    k.method(c, "multiplier", [](CpuState& s, GuestMemory&) { set_d(s, 0, constraint_state(s.x[0])->multiplier); }, "d@:");
    k.method(c, "constant", [](CpuState& s, GuestMemory&) { set_d(s, 0, constraint_state(s.x[0])->constant); }, "d@:");
    k.method(c, "setConstant:", [](CpuState& s, GuestMemory&) {
        const auto cs = constraint_state(s.x[0]);
        const auto value = d(s, 0);
        if (cs->constant == value && cs->symbolic.empty()) return;
        cs->constant = value;
        cs->symbolic.clear();
        constraint_changed(s.x[0]);
    }, "v@:d");
    k.method(c, "priority", [](CpuState& s, GuestMemory&) { set_f(s, 0, constraint_state(s.x[0])->priority); }, "f@:");
    k.method(c, "setPriority:", [](CpuState& s, GuestMemory&) {
        const auto cs = constraint_state(s.x[0]);
        const auto value = f(s, 0);
        if (!(value > 0) || value > priority_required) completion_kit::fail("NSLayoutConstraint priority " + std::to_string(value) + " is outside (0, 1000]");
        if (cs->priority == value) return;
        if (cs->active && cs->holder && (cs->priority == priority_required) != (value == priority_required))
            completion_kit::fail("Mutating a priority from required to not on an installed constraint (or vice-versa) is not supported: " +
                       describe(s.x[0]));
        cs->priority = value;
        constraint_changed(s.x[0]);
    }, "v@:f");
    k.method(c, "isActive", [](CpuState& s, GuestMemory&) { return_bool(s, constraint_state(s.x[0])->active); }, "B@:");
    k.method(c, "setActive:", [](CpuState& s, GuestMemory&) { set_active(s.x[0], appkit::bool_arg(s, 2)); }, "v@:B");
    k.method(c, "shouldBeArchived", [](CpuState& s, GuestMemory&) { return_bool(s, constraint_state(s.x[0])->archived); }, "B@:");
    k.method(c, "setShouldBeArchived:", [](CpuState& s, GuestMemory&) { constraint_state(s.x[0])->archived = appkit::bool_arg(s, 2); }, "v@:B");
    k.method(c, "identifier", [](CpuState& s, GuestMemory&) { s.x[0] = constraint_state(s.x[0])->identifier; });
    k.method(c, "setIdentifier:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        kk.release(std::exchange(constraint_state(s.x[0])->identifier, s.x[2] ? kk.cf.copy(s.x[2]) : 0));
    }, "v@:@");
    k.method(c, "firstAnchor", [](CpuState& s, GuestMemory&) {
        const auto cs = constraint_state(s.x[0]);
        s.x[0] = make_anchor(cs->first, cs->first_attribute);
    });
    k.method(c, "secondAnchor", [](CpuState& s, GuestMemory&) {
        const auto cs = constraint_state(s.x[0]);
        s.x[0] = cs->second ? make_anchor(cs->second, cs->second_attribute) : 0;
    });
    k.method(c, "description", [](CpuState& s, GuestMemory&) { s.x[0] = kit().auto_string(describe(s.x[0])); });
    // NSAnimatablePropertyContainer: changes apply at once (no animation engine).
    k.method(c, "animator", [](CpuState&, GuestMemory&) {});
    k.method(c, "dealloc", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        if (const auto cs = kk.objc.find_host_state<ConstraintState>(s.x[0])) kk.release(std::exchange(cs->identifier, 0));
        static_cast<void>(kk.objc.send_super(s.x[0], module().constraint_class, kk.sel("dealloc")));
    }, "v@:");
    // Nib decoding (IB archives NSLayoutConstraint with these keys).
    k.method(c, "initWithCoder:", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto self = s.x[0], coder = s.x[2];
        const auto key = [&](const char* name) { return kk.auto_string(name); };
        const auto has = [&](const char* name) { return kk.send_bool(coder, "containsValueForKey:", {key(name)}); };
        const auto integer = [&](const char* name) { return static_cast<std::int64_t>(kk.send_id(coder, "decodeIntegerForKey:", {key(name)})); };
        const auto real = [&](const char* name) { return kk.send(coder, "decodeDoubleForKey:", [&] { CallRegs r; r.x[2] = key(name); return r; }()).d(0); };
        const auto cs = constraint_state(self);
        cs->first = kk.send_id(coder, "decodeObjectForKey:", {key("NSFirstItem")});
        cs->second = kk.send_id(coder, "decodeObjectForKey:", {key("NSSecondItem")});
        cs->first_attribute = has("NSFirstAttributeV2") ? integer("NSFirstAttributeV2") : integer("NSFirstAttribute");
        cs->second_attribute = has("NSSecondAttributeV2") ? integer("NSSecondAttributeV2") : integer("NSSecondAttribute");
        if (!cs->second) cs->second_attribute = attribute::none;
        cs->relation = integer("NSRelation");
        cs->multiplier = has("NSMultiplier") ? real("NSMultiplier") : 1.0;
        cs->constant = has("NSConstantV2") ? real("NSConstantV2") : real("NSConstant");
        cs->priority = has("NSPriority") ? static_cast<float>(integer("NSPriority")) : priority_required;
        cs->archived = kk.send_bool(coder, "decodeBoolForKey:", {key("NSShouldBeArchived")});
        if (const auto symbolic = kk.send_id(coder, "decodeObjectForKey:", {key("NSSymbolicConstant")})) {
            cs->symbolic = kk.text(symbolic);
            if (cs->symbolic != "NSSpace") completion_kit::unsupported("nib NSLayoutConstraint symbolic constant " + cs->symbolic);
        }
        if (!cs->first) completion_kit::fail("nib NSLayoutConstraint without NSFirstItem");
        if (!(cs->priority > 0) || cs->priority > priority_required) completion_kit::fail("nib NSLayoutConstraint priority out of range");
        s.x[0] = self;
    });

    // ---- NSLayoutAnchor ----
    const auto a = m.anchor_class;
    for (const auto& [name, relation] : std::initializer_list<std::pair<const char*, std::int64_t>>{
             {"constraintEqualToAnchor:", 0}, {"constraintGreaterThanOrEqualToAnchor:", 1}, {"constraintLessThanOrEqualToAnchor:", -1}}) {
        const std::string base = name;
        const auto rel = relation;
        k.method(a, base, [rel](CpuState& s, GuestMemory&) { s.x[0] = constraint_with_anchors(s.x[0], rel, s.x[2], 1, 0); }, "@@:@");
        k.method(a, base + "constant:", [rel](CpuState& s, GuestMemory&) {
            s.x[0] = constraint_with_anchors(s.x[0], rel, s.x[2], 1, d(s, 0));
        }, "@@:@d");
    }
    const auto dim = m.dimension_class;
    for (const auto& [name, relation] : std::initializer_list<std::pair<const char*, std::int64_t>>{
             {"constraintEqualTo", 0}, {"constraintGreaterThanOrEqualTo", 1}, {"constraintLessThanOrEqualTo", -1}}) {
        const std::string base = name;
        const auto rel = relation;
        k.method(dim, base + "Constant:", [rel](CpuState& s, GuestMemory&) { s.x[0] = constraint_with_anchors(s.x[0], rel, 0, 1, d(s, 0)); }, "@@:d");
        k.method(dim, base + "Anchor:multiplier:", [rel](CpuState& s, GuestMemory&) {
            s.x[0] = constraint_with_anchors(s.x[0], rel, s.x[2], d(s, 0), 0);
        }, "@@:@d");
        k.method(dim, base + "Anchor:multiplier:constant:", [rel](CpuState& s, GuestMemory&) {
            s.x[0] = constraint_with_anchors(s.x[0], rel, s.x[2], d(s, 0), d(s, 1));
        }, "@@:@dd");
        // X / Y axis anchors: the standard spacing (8 points) times a multiplier after another anchor.
        for (const auto axis : {m.x_anchor_class, m.y_anchor_class})
            k.method(axis, base + "SystemSpacingAfterAnchor:multiplier:", [rel](CpuState& s, GuestMemory&) {
                s.x[0] = constraint_with_anchors(s.x[0], rel, s.x[2], 1, 8 * d(s, 0));
            }, "@@:@d");
    }
    k.method(a, "item", [](CpuState& s, GuestMemory&) { s.x[0] = anchor_state(s.x[0])->item; });

    // ---- NSView ----
    static_cast<void>(w);
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, std::int64_t>>{
             {"leftAnchor", attribute::left}, {"rightAnchor", attribute::right}, {"topAnchor", attribute::top},
             {"bottomAnchor", attribute::bottom}, {"leadingAnchor", attribute::leading}, {"trailingAnchor", attribute::trailing},
             {"widthAnchor", attribute::width}, {"heightAnchor", attribute::height}, {"centerXAnchor", attribute::center_x},
             {"centerYAnchor", attribute::center_y}, {"firstBaselineAnchor", attribute::first_baseline},
             {"lastBaselineAnchor", attribute::last_baseline}}) {
        const auto attribute_value = value;
        k.method(v, name, [attribute_value](CpuState& s, GuestMemory&) { s.x[0] = make_anchor(s.x[0], attribute_value); });
    }
    k.method(v, "constraints", [](CpuState& s, GuestMemory&) { s.x[0] = kit().auto_array(kit().view_state(s.x[0])->constraints); });
    const auto add_one = [](Address self, Address constraint) {
        if (!constraint) completion_kit::fail("-[NSView addConstraint:] with nil");
        const auto cs = constraint_state(constraint);
        if (cs->holder == self && cs->active) return;
        const auto keep = kit().retain(constraint);
        if (cs->holder) unhold(constraint);
        hold(self, constraint);
        kit().release(keep);
    };
    k.method(v, "addConstraint:", [add_one](CpuState& s, GuestMemory&) { add_one(s.x[0], s.x[2]); }, "v@:@");
    k.method(v, "addConstraints:", [add_one](CpuState& s, GuestMemory&) {
        for (const auto item : kit().items(s.x[2])) add_one(s.x[0], item);
    }, "v@:@");
    const auto remove_one = [](Address self, Address constraint) {
        if (!constraint) return;
        const auto cs = constraint_state(constraint);
        if (cs->holder == self) unhold(constraint);
    };
    k.method(v, "removeConstraint:", [remove_one](CpuState& s, GuestMemory&) { remove_one(s.x[0], s.x[2]); }, "v@:@");
    k.method(v, "removeConstraints:", [remove_one](CpuState& s, GuestMemory&) {
        for (const auto item : kit().items(s.x[2])) remove_one(s.x[0], item);
    }, "v@:@");
    k.method(v, "translatesAutoresizingMaskIntoConstraints", [](CpuState& s, GuestMemory&) {
        return_bool(s, kit().view_state(s.x[0])->translates_autoresizing);
    }, "B@:");
    k.method(v, "setTranslatesAutoresizingMaskIntoConstraints:", [](CpuState& s, GuestMemory&) {
        const auto state = kit().view_state(s.x[0]);
        const bool value = appkit::bool_arg(s, 2);
        if (state->translates_autoresizing == value) return;
        state->translates_autoresizing = value;
        invalidate_layout(s.x[0], true);
        if (state->superview) invalidate_layout(state->superview, true);
    }, "v@:B");
    k.class_method(v, "requiresConstraintBasedLayout", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    k.method(v, "intrinsicContentSize", [](CpuState& s, GuestMemory&) { return_size(s, {-1, -1}); }, "{CGSize=dd}@:");
    k.method(v, "invalidateIntrinsicContentSize", [](CpuState& s, GuestMemory&) {
        kit().view_state(s.x[0])->needs_layout = true;
        invalidate_layout(s.x[0], false);
    }, "v@:");
    const auto priority_axis = [](Address orientation) -> std::size_t {
        if (orientation > 1) completion_kit::fail("NSLayoutConstraintOrientation " + std::to_string(orientation));
        return static_cast<std::size_t>(orientation);
    };
    k.method(v, "contentHuggingPriorityForOrientation:", [priority_axis](CpuState& s, GuestMemory&) {
        set_f(s, 0, kit().view_state(s.x[0])->hugging[priority_axis(s.x[2])]);
    }, "f@:q");
    k.method(v, "setContentHuggingPriority:forOrientation:", [priority_axis](CpuState& s, GuestMemory&) {
        kit().view_state(s.x[0])->hugging[priority_axis(s.x[2])] = f(s, 0);
        invalidate_layout(s.x[0], false);
    }, "v@:fq");
    k.method(v, "contentCompressionResistancePriorityForOrientation:", [priority_axis](CpuState& s, GuestMemory&) {
        set_f(s, 0, kit().view_state(s.x[0])->compression[priority_axis(s.x[2])]);
    }, "f@:q");
    k.method(v, "setContentCompressionResistancePriority:forOrientation:", [priority_axis](CpuState& s, GuestMemory&) {
        kit().view_state(s.x[0])->compression[priority_axis(s.x[2])] = f(s, 0);
        invalidate_layout(s.x[0], false);
    }, "v@:fq");
    k.method(v, "needsUpdateConstraints", [](CpuState& s, GuestMemory&) { return_bool(s, kit().view_state(s.x[0])->needs_update_constraints); }, "B@:");
    k.method(v, "setNeedsUpdateConstraints:", [](CpuState& s, GuestMemory&) {
        const bool value = appkit::bool_arg(s, 2);
        kit().view_state(s.x[0])->needs_update_constraints = value;
        if (value) invalidate_layout(s.x[0], false);
    }, "v@:B");
    k.method(v, "updateConstraints", [](CpuState& s, GuestMemory&) { kit().view_state(s.x[0])->needs_update_constraints = false; }, "v@:");
    k.method(v, "updateConstraintsForSubtreeIfNeeded", [](CpuState& s, GuestMemory&) { update_constraints(s.x[0]); }, "v@:");
    k.method(v, "needsLayout", [](CpuState& s, GuestMemory&) { return_bool(s, kit().view_state(s.x[0])->needs_layout); }, "B@:");
    k.method(v, "setNeedsLayout:", [](CpuState& s, GuestMemory&) {
        const bool value = appkit::bool_arg(s, 2);
        kit().view_state(s.x[0])->needs_layout = value;
        if (value) invalidate_layout(s.x[0], false);
    }, "v@:B");
    k.method(v, "layout", [](CpuState& s, GuestMemory&) { apply_subview_frames(s.x[0]); }, "v@:");
    k.method(v, "layoutSubtreeIfNeeded", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto window = kk.view_state(s.x[0])->window;
        if (window && kk.window_state(window)->content_view) {
            auto* e = window_engine(window, true);
            e->pending = true;
            layout_window(window);
        } else {
            layout_detached(s.x[0]);
        }
    }, "v@:");
    k.method(v, "fittingSize", [](CpuState& s, GuestMemory&) { return_size(s, fitting_size(s.x[0])); }, "{CGSize=dd}@:");
    // Alignment rectangles are frames here.
    k.method(v, "alignmentRectInsets", [](CpuState& s, GuestMemory&) {
        for (unsigned i = 0; i < 4; ++i) set_d(s, i, 0);
    }, "{NSEdgeInsets=dddd}@:");
    k.method(v, "alignmentRectForFrame:", [](CpuState& s, GuestMemory&) { return_rect(s, rect_arg(s)); },
             "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    k.method(v, "frameForAlignmentRect:", [](CpuState& s, GuestMemory&) { return_rect(s, rect_arg(s)); },
             "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    for (const auto* name : {"firstBaselineOffsetFromTop", "lastBaselineOffsetFromBottom", "baselineOffsetFromBottom"})
        k.method(v, name, [](CpuState& s, GuestMemory&) { set_d(s, 0, 0); }, "d@:");
    // NSControl: the baseline of its cell's single line of text at the
    // control's intrinsic height (the text is centered vertically).
    const auto control_baseline = [](Address control, bool from_top) {
        auto& kk = kit();
        const auto font = kk.send(control, "font").x0;
        const auto metrics = appkit::drawing::font_metrics(kk.font_spec(font));
        auto height = result_size(kk.send(control, "intrinsicContentSize")).height;
        if (!(height > 0)) height = kk.view_state(control)->frame.size.height;
        const auto content = metrics.ascent + metrics.descent;
        const auto top = (height - content) / 2 + metrics.ascent;
        return from_top ? top : height - top;
    };
    k.method(k.control, "firstBaselineOffsetFromTop", [control_baseline](CpuState& s, GuestMemory&) { set_d(s, 0, control_baseline(s.x[0], true)); }, "d@:");
    for (const auto* name : {"lastBaselineOffsetFromBottom", "baselineOffsetFromBottom"})
        k.method(k.control, name, [control_baseline](CpuState& s, GuestMemory&) { set_d(s, 0, control_baseline(s.x[0], false)); }, "d@:");

    // ---- NSLayoutGuide ----
    const auto g = k.declare_class("NSLayoutGuide", "NSObject");
    k.method(g, "init", [g](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto self = kk.objc.send_super(s.x[0], g, kk.sel("init")).x0;
        if (self) static_cast<void>(kk.objc.host_state<appkit::GuideState>(self));
        s.x[0] = self;
    });
    // Interface Builder archives a view's guides with the view (NSView's
    // decoder adopts them), so the guide itself needs no keys.
    k.method(g, "initWithCoder:", [](CpuState& s, GuestMemory&) { s.x[0] = kit().send(s.x[0], "init").x0; });
    k.method(g, "dealloc", [g](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        if (const auto state = guide_state(s.x[0])) kk.release(std::exchange(state->identifier, 0));
        static_cast<void>(kk.objc.send_super(s.x[0], g, kk.sel("dealloc")));
    }, "v@:");
    const auto guide_of = [](Address guide) {
        const auto state = guide_state(guide);
        if (!state) completion_kit::fail("an NSLayoutGuide that was not initialized");
        return state;
    };
    k.method(g, "owningView", [guide_of](CpuState& s, GuestMemory&) { s.x[0] = guide_of(s.x[0])->owner; });
    k.method(g, "setOwningView:", [guide_of](CpuState& s, GuestMemory&) {
        const auto owner = guide_of(s.x[0])->owner;
        if (s.x[2]) add_guide(s.x[2], s.x[0]);
        else if (owner) remove_guide(owner, s.x[0]);
    }, "v@:@");
    k.method(g, "frame", [guide_of](CpuState& s, GuestMemory&) {
        static_cast<void>(guide_of(s.x[0]));
        return_rect(s, guide_frame(s.x[0]));
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    k.method(g, "identifier", [guide_of](CpuState& s, GuestMemory&) {
        const auto value = guide_of(s.x[0])->identifier;
        s.x[0] = value ? value : kit().auto_string("");
    });
    k.method(g, "setIdentifier:", [guide_of](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        kk.release(std::exchange(guide_of(s.x[0])->identifier, s.x[2] ? kk.cf.copy(s.x[2]) : 0));
    }, "v@:@");
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, std::int64_t>>{
             {"leftAnchor", attribute::left}, {"rightAnchor", attribute::right}, {"topAnchor", attribute::top},
             {"bottomAnchor", attribute::bottom}, {"leadingAnchor", attribute::leading}, {"trailingAnchor", attribute::trailing},
             {"widthAnchor", attribute::width}, {"heightAnchor", attribute::height}, {"centerXAnchor", attribute::center_x},
             {"centerYAnchor", attribute::center_y}}) {
        const auto attribute_value = value;
        k.method(g, name, [attribute_value, guide_of](CpuState& s, GuestMemory&) {
            static_cast<void>(guide_of(s.x[0]));
            s.x[0] = make_anchor(s.x[0], attribute_value);
        });
    }
    // Ambiguity is not detected: the solver always settles on one solution.
    for (const auto cls : {g, v})
        for (const auto* name : {"hasAmbiguousLayout", "exerciseAmbiguityInLayout"}) {
            const std::string selector = name;
            k.method(cls, name, [selector](CpuState& s, GuestMemory&) {
                completion_kit::unsupported("-[" + class_name(s.x[0]) + " " + selector + "] (layout ambiguity is not detected)");
            });
        }
    for (const auto cls : {g, v})
        k.method(cls, "constraintsAffectingLayoutForOrientation:", [](CpuState& s, GuestMemory&) {
            s.x[0] = kit().auto_array(constraints_affecting(s.x[0], s.x[2]));
        }, "@@:q");

    // ---- NSView layout guides and the safe area ----
    k.method(v, "layoutGuides", [](CpuState& s, GuestMemory&) {
        std::vector<Address> custom;
        for (const auto guide : kit().view_state(s.x[0])->layout_guides)
            if (const auto state = guide_state(guide); state && state->kind == appkit::GuideState::Kind::custom) custom.push_back(guide);
        s.x[0] = kit().auto_array(custom);
    });
    k.method(v, "addLayoutGuide:", [](CpuState& s, GuestMemory&) {
        if (!s.x[2]) completion_kit::fail("-[NSView addLayoutGuide:] with nil");
        add_guide(s.x[0], s.x[2]);
    }, "v@:@");
    k.method(v, "removeLayoutGuide:", [](CpuState& s, GuestMemory&) { remove_guide(s.x[0], s.x[2]); }, "v@:@");
    k.method(v, "safeAreaLayoutGuide", [](CpuState& s, GuestMemory&) { s.x[0] = system_guide(s.x[0], appkit::GuideState::Kind::safe_area); });
    k.method(v, "layoutMarginsGuide", [](CpuState& s, GuestMemory&) { s.x[0] = system_guide(s.x[0], appkit::GuideState::Kind::margins); });
    k.method(v, "additionalSafeAreaInsets", [](CpuState& s, GuestMemory&) {
        const auto& insets = kit().view_state(s.x[0])->additional_safe_area;
        for (unsigned i = 0; i < 4; ++i) set_d(s, i, insets[i]);
    }, "{NSEdgeInsets=dddd}@:");
    k.method(v, "setAdditionalSafeAreaInsets:", [](CpuState& s, GuestMemory&) {
        const auto state = kit().view_state(s.x[0]);
        const std::array<double, 4> insets{d(s, 0), d(s, 1), d(s, 2), d(s, 3)};
        if (insets == state->additional_safe_area) return;
        state->additional_safe_area = insets;
        state->needs_layout = true;
        invalidate_layout(s.x[0], false);
    }, "v@:{NSEdgeInsets=dddd}");

    // ---- NSWindow ----
    k.method(k.window, "contentLayoutGuide", [](CpuState& s, GuestMemory&) {
        const auto content = kit().window_state(s.x[0])->content_view;
        s.x[0] = content ? system_guide(content, appkit::GuideState::Kind::content_layout) : 0;
    });
    k.method(k.window, "layoutIfNeeded", [](CpuState& s, GuestMemory&) {
        if (auto* e = window_engine(s.x[0], true)) e->pending = true;
        layout_window(s.x[0]);
    }, "v@:");
    k.method(k.window, "updateConstraintsIfNeeded", [](CpuState& s, GuestMemory&) {
        update_constraints(kit().window_state(s.x[0])->content_view);
    }, "v@:");
    // AppKit's -[NSWindow display] / -displayIfNeeded run the layout pass
    // first, as the display cycle does, and -[NSView setAutoresizingMask:]
    // invalidates the layout of a view whose mask changed.
    k.set_layout_engine("QLab Auto Layout", {
        [](Address window) { layout_window(window); },
        [](Address view) { invalidate_layout(view, false); }});
}

} // namespace alchemy::gui::completion_kit::windows
