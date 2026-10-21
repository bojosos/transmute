#pragma once

// QLab gap group G4 (src/alchemy/darwin/kit/completion/windows/): shared declarations
// of the providers in this directory. They extend the AppKit provider
// (src/alchemy/darwin/appkit/, also owned by G4 for the files the gap list names)
// through its process instance (appkit::instance()), so they work on the
// same host state as AppKit's own NSView / NSWindow implementation.
//   auto_layout.cpp   NSLayoutConstraint, NSLayoutAnchor, the NSView /
//                     NSWindow Auto Layout API and the layout engine over
//                     layout_solver.cpp
//   control_metrics.cpp alignment insets and intrinsic sizes of push buttons,
//                     pop-up buttons and labels
//   stack_grid.cpp    NSStackView (gravity areas, NSStackViewContainer) and
//                     NSGridView / NSGridRow / NSGridColumn / NSGridCell
//   install.cpp       install_group_windows

#include "alchemy/darwin/kit/completion/completion_kit.hpp"
#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/darwin/appkit/windows/layout_geometry.hpp"
#include "alchemy/darwin/appkit/windows/layout_solver.hpp"

namespace alchemy::gui::completion_kit::windows {

using appkit::AppKit;

// The AppKit provider; fails when AppKit is not installed.
AppKit& kit();

// ---- Auto Layout (auto_layout.cpp) ----------------------------------------------------
// NSLayoutAttribute / NSLayoutRelation values.
namespace attribute {
inline constexpr std::int64_t none = 0, left = 1, right = 2, top = 3, bottom = 4, leading = 5, trailing = 6, width = 7,
    height = 8, center_x = 9, center_y = 10, last_baseline = 11, first_baseline = 12;
}
inline constexpr float priority_required = 1000, priority_default_high = 750, priority_default_low = 250;

struct ConstraintState final : ObjectPayload {
    Address first{}, second{};               // the items (not retained, as AppKit's weak items)
    std::int64_t first_attribute{}, second_attribute{};
    std::int64_t relation{};                 // -1 <=, 0 ==, 1 >=
    double multiplier{1}, constant{};
    float priority{priority_required};
    bool active{}, archived{};
    std::string symbolic;                    // NSSymbolicConstant ("NSSpace": the standard spacing)
    Address identifier{};                    // +1
    Address holder{};                        // the view whose -constraints list holds it while active
    Address owner{};                         // a stack / grid view that made it (internal constraints)
};

// A new NSLayoutConstraint (+1).
Address make_constraint(Address first, std::int64_t first_attribute, std::int64_t relation, Address second,
                        std::int64_t second_attribute, double multiplier, double constant, float priority = priority_required);
// -setActive: (the holder is the items' closest common ancestor).
void set_active(Address constraint, bool active);
// The view's window needs a layout pass (and a rebuild of the engine when
// `structure` changed: constraints, the view tree, priorities).
void invalidate_layout(Address view, bool structure);

// A view class that arranges some of its subviews with constraints of its
// own (NSStackView, NSGridView): the engine treats the views it manages as
// laid out by constraints and asks it for those constraints in each build.
struct LayoutContributor {
    Address cls{};
    std::function<std::vector<Address>(Address container)> managed_views;
    std::function<void(Address container, layout::Solver& solver, layout::Ids& ids, const layout::Box& container_box,
                       const std::function<std::optional<layout::Box>(Address view)>& box_of)>
        add_constraints;
};
void register_layout_contributor(LayoutContributor contributor);
// The view's intrinsic content size / baseline offsets, by message.
layout::Size2 intrinsic_size(Address view);
std::pair<double, double> baseline_offsets(Address view);

void install_auto_layout(Support& support);
void install_control_metrics(Support& support);   // control_metrics.cpp
void install_window_extras(Support& support);     // window_extras.cpp
void install_workspace_and_dragging(Support& support);   // workspace_dragging.cpp
void install_popover(Support& support);           // popover.cpp
void install_drag_session(Support& support);      // drag_session.cpp
void install_stack_and_grid(Support& support);

} // namespace alchemy::gui::completion_kit::windows
