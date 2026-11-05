#include "alchemy/darwin/appkit/controls/g3.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// NSCollectionViewCompositionalLayout and its descriptions: dimensions,
// sizes, spacing, items, groups, sections, anchors, supplementary,
// boundary and decoration items, and the configuration.
// * Sections come from the layout's section or its section provider block
//   (called with an environment whose container is the collection view's
//   size). They scroll vertically, one after another with the
//   configuration's inter-section spacing.
// * Sizes resolve against their container: fractional width / height of
//   the enclosing group (a section's top-level group: the section's width
//   inside its content insets, and the visible height), absolute points;
//   estimated sizes are used as given (no self-sizing).
// * Groups repeat down a section until its items are placed (the last one
//   may be partial). Horizontal and vertical groups place their subitems in
//   a row / column with the inter-item spacing (flexible spacing spreads
//   the room left) and leading / top edge spacing; `count` groups split
//   their room equally; groups nest; custom groups place the frames their
//   item provider gives. Items are inset by their content insets.
// * Supplementary items sit on their item by their container anchor (edges
//   plus an absolute or fractional offset); boundary supplementary items
//   (headers on top, footers below) span the section or the whole layout
//   (the configuration's); background decoration items fill the section
//   behind its items (the view class registered for the kind).
// * Orthogonal scrolling sections lay their groups out in one row; the row
//   does not scroll (what is wider than the view is clipped). Horizontal
//   scroll direction fails explicitly. The NSRectAlignment and
//   NSDirectionalRectEdge values follow their documented order (unverified).
namespace alchemy::gui::completion_kit::controls {

Address collection_layout_attributes(AppKit& kit, Rect frame, Address path, std::int64_t category, std::string_view kind);
Address collection_view_of_layout(AppKit& kit, Address layout_object);
std::vector<std::int64_t> collection_section_counts(AppKit& kit, Address collection);

namespace {
using namespace appkit;

enum class DimensionKind { fractional_width, fractional_height, absolute, estimated };
struct DimensionState final : ObjectPayload { DimensionKind kind{DimensionKind::absolute}; double value{}; };
struct SizeState final : ObjectPayload { Address width{}, height{}; };                  // +1
struct SpacingState final : ObjectPayload { bool flexible{}; double value{}; };
struct EdgeSpacingState final : ObjectPayload { Address leading{}, top{}, trailing{}, bottom{}; };   // +1
struct AnchorState final : ObjectPayload { std::uint64_t edges{}; Point offset{}; bool fractional{}; };
struct Insets { double top{}, leading{}, bottom{}, trailing{}; };
// Items, groups, supplementary / boundary / decoration items share this state.
struct ItemState final : ObjectPayload {
    Address size{};                                         // +1 NSCollectionLayoutSize
    Insets insets;
    Address edge_spacing{};                                 // +1
    std::vector<Address> supplementaries;                   // +1 NSCollectionLayoutSupplementaryItem
    // groups
    bool group{}, vertical{}, custom{};
    std::vector<Address> subitems;                          // +1
    std::int64_t count{};                                   // subitem:count: (0: subitems as given)
    Address inter_item_spacing{};                           // +1
    Address item_provider{};                                // +1 block (custom groups)
    // supplementary items
    Address kind{};                                         // +1 element kind
    Address container_anchor{}, item_anchor{};              // +1
    std::int64_t z{};
    // boundary items
    std::int64_t alignment{};
    Point offset{};
    bool pin{}, extends{true};
    // custom items
    Rect frame{};
};
struct SectionState final : ObjectPayload {
    Address group{};                                        // +1
    Insets insets;
    double inter_group_spacing{};
    std::int64_t orthogonal{};
    std::vector<Address> boundaries, decorations;           // +1
    bool follow_insets{true};
    Address invalidation_handler{};                         // +1 block (kept)
};
struct ConfigurationState final : ObjectPayload {
    std::int64_t direction{};
    double inter_section_spacing{};
    std::vector<Address> boundaries;                        // +1
};
struct ContainerState final : ObjectPayload { Size size; Insets insets; };
struct EnvironmentState final : ObjectPayload { Address container{}; };   // +1

struct Placed {
    Rect frame;
    std::int64_t section{}, item{};
    std::int64_t category{};                                // 0 item, 1 supplementary, 2 decoration
    std::string kind;
    std::int64_t z{};
};
struct Computed {
    std::vector<Placed> elements;
    Size content;
};
struct LayoutInfo {
    Address section{}, provider{}, configuration{};         // +1 (provider: a block)
    Computed computed;
};
std::mutex info_mutex;
std::map<Address, LayoutInfo>& infos() {
    static std::map<Address, LayoutInfo> table;
    return table;
}

// NSRectAlignment (top / top leading / top trailing headers; bottom ones footers).
constexpr std::int64_t align_top = 1, align_top_leading = 2, align_bottom_leading = 4, align_bottom = 5, align_bottom_trailing = 6,
                       align_top_trailing = 8;
// NSDirectionalRectEdge.
constexpr std::uint64_t edge_top = 1, edge_leading = 2, edge_bottom = 4, edge_trailing = 8;

struct Builder {
    AppKit& kit;
    Address collection{};
    Size container;
    std::vector<Placed> out;

    template <class T>
    std::shared_ptr<T> state(Address object) { return kit.objc.host_state<T>(object); }

    double resolve(Address dimension, Size relative) {
        if (!dimension) return 0;
        const auto d = state<DimensionState>(dimension);
        switch (d->kind) {
        case DimensionKind::fractional_width: return d->value * relative.width;
        case DimensionKind::fractional_height: return d->value * relative.height;
        default: return d->value;
        }
    }
    Size resolve_size(Address size, Size relative) {
        if (!size) return {};
        const auto s = state<SizeState>(size);
        return {resolve(s->width, relative), resolve(s->height, relative)};
    }
    double spacing_value(Address spacing) { return spacing ? state<SpacingState>(spacing)->value : 0; }
    bool spacing_flexible(Address spacing) { return spacing && state<SpacingState>(spacing)->flexible; }

    Address path(std::int64_t item, std::int64_t section) {
        return kit.send_id(kit.objc.find_class("NSIndexPath"), "indexPathForItem:inSection:", {static_cast<Address>(item), static_cast<Address>(section)});
    }

    // An anchored supplementary item on `frame`.
    void place_supplementary(Address item, Rect frame, std::int64_t section, std::int64_t index) {
        const auto info = state<ItemState>(item);
        const auto size = resolve_size(info->size, frame.size);
        Rect placed{{frame.origin.x + (frame.size.width - size.width) / 2, frame.origin.y + (frame.size.height - size.height) / 2}, size};
        if (info->container_anchor) {
            const auto anchor = state<AnchorState>(info->container_anchor);
            if (anchor->edges & edge_leading) placed.origin.x = frame.origin.x;
            if (anchor->edges & edge_trailing) placed.origin.x = frame.origin.x + frame.size.width - size.width;
            if (anchor->edges & edge_top) placed.origin.y = frame.origin.y;
            if (anchor->edges & edge_bottom) placed.origin.y = frame.origin.y + frame.size.height - size.height;
            placed.origin.x += anchor->fractional ? anchor->offset.x * size.width : anchor->offset.x;
            placed.origin.y += anchor->fractional ? anchor->offset.y * size.height : anchor->offset.y;
        }
        out.push_back({placed, section, index, 1, info->kind ? kit.text(info->kind) : "", info->z});
    }

    // Places `entry` (an item or a group) in `cell`; returns the items placed.
    std::int64_t place(Address entry, Rect cell, std::int64_t section, std::int64_t& next, std::int64_t count, int depth) {
        if (depth > 16) return 0;
        const auto info = state<ItemState>(entry);
        const Rect inner{{cell.origin.x + info->insets.leading, cell.origin.y + info->insets.top},
                         {std::max(0.0, cell.size.width - info->insets.leading - info->insets.trailing),
                          std::max(0.0, cell.size.height - info->insets.top - info->insets.bottom)}};
        if (!info->group) {
            if (next >= count) return 0;
            const auto index = next++;
            out.push_back({inner, section, index, 0, "", 0});
            for (const auto supplementary : info->supplementaries) place_supplementary(supplementary, inner, section, index);
            return 1;
        }
        std::int64_t placed = 0;
        if (info->custom) {
            // The item provider's frames (relative to the group).
            const auto environment = make_environment(cell.size);
            const auto frames = info->item_provider ? kit.invoke_block(info->item_provider, {environment}) : 0;
            for (const auto custom : frames ? kit.items(frames) : std::vector<Address>{}) {
                if (next >= count) break;
                const auto frame = state<ItemState>(custom)->frame;
                const auto index = next++;
                out.push_back({{{inner.origin.x + frame.origin.x, inner.origin.y + frame.origin.y}, frame.size}, section, index, 0, "", state<ItemState>(custom)->z});
                ++placed;
            }
            return placed;
        }
        // The subitems and their sizes along the group's axis.
        std::vector<Address> subitems;
        if (info->count > 0 && !info->subitems.empty()) subitems.assign(static_cast<std::size_t>(info->count), info->subitems.front());
        else subitems = info->subitems;
        if (subitems.empty()) return 0;
        const double spacing = spacing_value(info->inter_item_spacing);
        const bool flexible = spacing_flexible(info->inter_item_spacing);
        const double room = info->vertical ? inner.size.height : inner.size.width;
        std::vector<Size> sizes;
        for (const auto subitem : subitems) sizes.push_back(resolve_size(state<ItemState>(subitem)->size, inner.size));
        if (info->count > 0) {
            const double each = std::max(0.0, (room - spacing * static_cast<double>(info->count - 1)) / static_cast<double>(info->count));
            for (auto& size : sizes) (info->vertical ? size.height : size.width) = each;
        }
        double used = 0;
        for (const auto& size : sizes) used += info->vertical ? size.height : size.width;
        const double gap = flexible && subitems.size() > 1 ? std::max(spacing, (room - used) / static_cast<double>(subitems.size() - 1)) : spacing;
        double at = 0;
        for (std::size_t i = 0; i < subitems.size(); ++i) {
            if (next >= count) break;
            const auto edge = state<ItemState>(subitems[i])->edge_spacing;
            if (edge) {
                const auto e = state<EdgeSpacingState>(edge);
                at += spacing_value(info->vertical ? e->top : e->leading);
            }
            const auto& size = sizes[i];
            const Rect sub = info->vertical ? Rect{{inner.origin.x, inner.origin.y + at}, {size.width, size.height}}
                                            : Rect{{inner.origin.x + at, inner.origin.y}, {size.width, size.height}};
            placed += place(subitems[i], sub, section, next, count, depth + 1);
            at += (info->vertical ? size.height : size.width) + gap;
        }
        return placed;
    }

    Address make_environment(Size size) {
        const auto container_object = kit.alloc_init(kit.objc.find_class("_AlchemyCollectionLayoutContainer"));
        kit.objc.host_state<ContainerState>(container_object)->size = size;
        const auto environment = kit.alloc_init(kit.objc.find_class("_AlchemyCollectionLayoutEnvironment"));
        kit.objc.host_state<EnvironmentState>(environment)->container = container_object;
        return kit.autorelease(environment);
    }

    // Boundary items of `alignment` kind (top: headers, else footers) across `width` at `y`; returns the height used.
    double boundaries(const std::vector<Address>& items, bool top, double x, double width, double y, std::int64_t section) {
        double used = 0;
        for (const auto item : items) {
            const auto info = state<ItemState>(item);
            const bool header = info->alignment == align_top || info->alignment == align_top_leading || info->alignment == align_top_trailing;
            const bool footer = info->alignment == align_bottom || info->alignment == align_bottom_leading || info->alignment == align_bottom_trailing;
            if (top ? !header : !footer) continue;
            const auto size = resolve_size(info->size, {width, container.height});
            double left = x;
            if (info->alignment == align_top_trailing || info->alignment == align_bottom_trailing) left = x + width - size.width;
            else if (info->alignment == align_top || info->alignment == align_bottom) left = x + (width - size.width) / 2;
            out.push_back({{{left + info->offset.x, y + used + info->offset.y}, size}, section, 0, 1, info->kind ? kit.text(info->kind) : "", info->z});
            used += size.height;
        }
        return used;
    }

    double lay_section(Address section_object, std::int64_t section, std::int64_t count, double top) {
        const auto info = state<SectionState>(section_object);
        const double width = std::max(0.0, container.width - info->insets.leading - info->insets.trailing);
        double y = top;
        const double header_x = info->follow_insets ? info->insets.leading : 0;
        const double header_width = info->follow_insets ? width : container.width;
        y += boundaries(info->boundaries, true, header_x, header_width, y, section);
        const double content_top = y;
        y += info->insets.top;
        std::int64_t next = 0;
        if (info->group) {
            const auto group_size = resolve_size(state<ItemState>(info->group)->size, {width, container.height});
            double x = info->insets.leading;
            double row_bottom = y;
            while (next < count) {
                const auto before = next;
                if (info->orthogonal != 0) {
                    place(info->group, {{x, y}, group_size}, section, next, count, 0);
                    x += group_size.width + info->inter_group_spacing;
                    row_bottom = y + group_size.height;
                } else {
                    place(info->group, {{info->insets.leading, y}, group_size}, section, next, count, 0);
                    row_bottom = y + group_size.height;
                    y = row_bottom + info->inter_group_spacing;
                }
                if (next == before || group_size.height <= 0) break;   // a group that places nothing
            }
            y = row_bottom;
        }
        y += info->insets.bottom;
        const double content_bottom = y;
        for (const auto decoration : info->decorations) {
            const auto item = state<ItemState>(decoration);
            const Rect area{{item->insets.leading, content_top + item->insets.top},
                            {std::max(0.0, container.width - item->insets.leading - item->insets.trailing),
                             std::max(0.0, content_bottom - content_top - item->insets.top - item->insets.bottom)}};
            out.push_back({area, section, 0, 2, item->kind ? kit.text(item->kind) : "", item->z ? item->z : -1});
        }
        y += boundaries(info->boundaries, false, header_x, header_width, y, section);
        return y;
    }
};

} // namespace

void install_compositional_layout(AppKit& kit) {
    auto* const k = &kit;
    const auto copy_block = [k](Address block) { return block ? k->guest_call(libsystem_library, "__Block_copy", {block}) : 0; };
    const auto object_list = [k](Address array) {
        std::vector<Address> out;
        for (const auto item : array ? k->items(array) : std::vector<Address>{}) out.push_back(k->retain(item));
        return out;
    };
    const auto make = [k](const char* class_name) { return k->objc.allocate(k->objc.find_class(class_name)); };

    // ---- NSCollectionLayoutDimension ----
    const auto dim = kit.declare_class("NSCollectionLayoutDimension", "NSObject");
    for (const auto& [name, kind] : std::initializer_list<std::pair<const char*, DimensionKind>>{
             {"fractionalWidthDimension:", DimensionKind::fractional_width}, {"fractionalHeightDimension:", DimensionKind::fractional_height},
             {"absoluteDimension:", DimensionKind::absolute}, {"estimatedDimension:", DimensionKind::estimated}}) {
        const auto which = kind;
        kit.class_method(dim, name, [k, make, which](CpuState& s, GuestMemory&) {
            const auto made = make("NSCollectionLayoutDimension");
            const auto state = k->objc.host_state<DimensionState>(made);
            state->kind = which;
            state->value = d(s, 0);
            s.x[0] = k->autorelease(made);
        }, "@@:d");
    }
    kit.method(dim, "dimension", [k](CpuState& s, GuestMemory&) { set_d(s, 0, k->objc.host_state<DimensionState>(s.x[0])->value); }, "d@:");
    for (const auto& [name, kind] : std::initializer_list<std::pair<const char*, DimensionKind>>{
             {"isFractionalWidth", DimensionKind::fractional_width}, {"isFractionalHeight", DimensionKind::fractional_height},
             {"isAbsolute", DimensionKind::absolute}, {"isEstimated", DimensionKind::estimated}}) {
        const auto which = kind;
        kit.method(dim, name, [k, which](CpuState& s, GuestMemory&) { return_bool(s, k->objc.host_state<DimensionState>(s.x[0])->kind == which); }, "B@:");
    }
    kit.method(dim, "copyWithZone:", [k](CpuState& s, GuestMemory&) { s.x[0] = k->retain(s.x[0]); }, "@@:^v");

    // ---- NSCollectionLayoutSize ----
    const auto size_class = kit.declare_class("NSCollectionLayoutSize", "NSObject");
    kit.class_method(size_class, "sizeWithWidthDimension:heightDimension:", [k, make](CpuState& s, GuestMemory&) {
        const auto made = make("NSCollectionLayoutSize");
        const auto state = k->objc.host_state<SizeState>(made);
        state->width = k->retain(s.x[2]);
        state->height = k->retain(s.x[3]);
        s.x[0] = k->autorelease(made);
    });
    kit.method(size_class, "widthDimension", [k](CpuState& s, GuestMemory&) { s.x[0] = k->objc.host_state<SizeState>(s.x[0])->width; });
    kit.method(size_class, "heightDimension", [k](CpuState& s, GuestMemory&) { s.x[0] = k->objc.host_state<SizeState>(s.x[0])->height; });
    kit.method(size_class, "copyWithZone:", [k](CpuState& s, GuestMemory&) { s.x[0] = k->retain(s.x[0]); }, "@@:^v");

    // ---- NSCollectionLayoutSpacing / NSCollectionLayoutEdgeSpacing ----
    const auto spacing_class = kit.declare_class("NSCollectionLayoutSpacing", "NSObject");
    for (const bool flexible : {false, true})
        kit.class_method(spacing_class, flexible ? "flexibleSpacing:" : "fixedSpacing:", [k, make, flexible](CpuState& s, GuestMemory&) {
            const auto made = make("NSCollectionLayoutSpacing");
            const auto state = k->objc.host_state<SpacingState>(made);
            state->flexible = flexible;
            state->value = d(s, 0);
            s.x[0] = k->autorelease(made);
        }, "@@:d");
    kit.method(spacing_class, "spacing", [k](CpuState& s, GuestMemory&) { set_d(s, 0, k->objc.host_state<SpacingState>(s.x[0])->value); }, "d@:");
    kit.method(spacing_class, "isFlexibleSpacing", [k](CpuState& s, GuestMemory&) { return_bool(s, k->objc.host_state<SpacingState>(s.x[0])->flexible); }, "B@:");
    kit.method(spacing_class, "isFixedSpacing", [k](CpuState& s, GuestMemory&) { return_bool(s, !k->objc.host_state<SpacingState>(s.x[0])->flexible); }, "B@:");
    kit.method(spacing_class, "copyWithZone:", [k](CpuState& s, GuestMemory&) { s.x[0] = k->retain(s.x[0]); }, "@@:^v");
    const auto edge_class = kit.declare_class("NSCollectionLayoutEdgeSpacing", "NSObject");
    kit.class_method(edge_class, "spacingForLeading:top:trailing:bottom:", [k, make](CpuState& s, GuestMemory&) {
        const auto made = make("NSCollectionLayoutEdgeSpacing");
        const auto state = k->objc.host_state<EdgeSpacingState>(made);
        state->leading = k->retain(s.x[2]);
        state->top = k->retain(s.x[3]);
        state->trailing = k->retain(s.x[4]);
        state->bottom = k->retain(s.x[5]);
        s.x[0] = k->autorelease(made);
    });
    for (const auto& [name, member] : std::initializer_list<std::pair<const char*, Address EdgeSpacingState::*>>{
             {"leading", &EdgeSpacingState::leading}, {"top", &EdgeSpacingState::top}, {"trailing", &EdgeSpacingState::trailing}, {"bottom", &EdgeSpacingState::bottom}}) {
        const auto field = member;
        kit.method(edge_class, name, [k, field](CpuState& s, GuestMemory&) { s.x[0] = (*k->objc.host_state<EdgeSpacingState>(s.x[0])).*field; });
    }

    // ---- NSCollectionLayoutAnchor ----
    const auto anchor_class = kit.declare_class("NSCollectionLayoutAnchor", "NSObject");
    const auto make_anchor = [k, make](std::uint64_t edges, Point offset, bool fractional) {
        const auto made = make("NSCollectionLayoutAnchor");
        const auto state = k->objc.host_state<AnchorState>(made);
        state->edges = edges;
        state->offset = offset;
        state->fractional = fractional;
        return k->autorelease(made);
    };
    kit.class_method(anchor_class, "layoutAnchorWithEdges:", [make_anchor](CpuState& s, GuestMemory&) { s.x[0] = make_anchor(s.x[2], {}, false); }, "@@:Q");
    kit.class_method(anchor_class, "layoutAnchorWithEdges:absoluteOffset:", [make_anchor](CpuState& s, GuestMemory&) {
        s.x[0] = make_anchor(s.x[2], point_arg(s), false);
    }, "@@:Q{CGPoint=dd}");
    kit.class_method(anchor_class, "layoutAnchorWithEdges:fractionalOffset:", [make_anchor](CpuState& s, GuestMemory&) {
        s.x[0] = make_anchor(s.x[2], point_arg(s), true);
    }, "@@:Q{CGPoint=dd}");
    kit.method(anchor_class, "edges", [k](CpuState& s, GuestMemory&) { s.x[0] = k->objc.host_state<AnchorState>(s.x[0])->edges; }, "Q@:");
    kit.method(anchor_class, "offset", [k](CpuState& s, GuestMemory&) { return_point(s, k->objc.host_state<AnchorState>(s.x[0])->offset); }, "{CGPoint=dd}@:");
    kit.method(anchor_class, "isAbsoluteOffset", [k](CpuState& s, GuestMemory&) { return_bool(s, !k->objc.host_state<AnchorState>(s.x[0])->fractional); }, "B@:");
    kit.method(anchor_class, "isFractionalOffset", [k](CpuState& s, GuestMemory&) { return_bool(s, k->objc.host_state<AnchorState>(s.x[0])->fractional); }, "B@:");

    // ---- NSCollectionLayoutItem and its subclasses ----
    const auto item_class = kit.declare_class("NSCollectionLayoutItem", "NSObject");
    const auto group_class = kit.declare_class("NSCollectionLayoutGroup", "NSCollectionLayoutItem");
    const auto supplementary_class = kit.declare_class("NSCollectionLayoutSupplementaryItem", "NSCollectionLayoutItem");
    const auto boundary_class = kit.declare_class("NSCollectionLayoutBoundarySupplementaryItem", "NSCollectionLayoutSupplementaryItem");
    const auto decoration_class = kit.declare_class("NSCollectionLayoutDecorationItem", "NSCollectionLayoutItem");
    const auto custom_item_class = kit.declare_class("NSCollectionLayoutGroupCustomItem", "NSObject");
    const auto is = [k](Address object) { return k->objc.host_state<ItemState>(object); };
    const auto make_item = [k, is](Address cls, Address size) {
        const auto made = k->objc.allocate(cls);
        is(made)->size = k->retain(size);
        return made;
    };
    kit.class_method(item_class, "itemWithLayoutSize:", [k, make_item](CpuState& s, GuestMemory&) { s.x[0] = k->autorelease(make_item(s.x[0], s.x[2])); });
    kit.class_method(item_class, "itemWithLayoutSize:supplementaryItems:", [k, make_item, is, object_list](CpuState& s, GuestMemory&) {
        const auto made = make_item(s.x[0], s.x[2]);
        is(made)->supplementaries = object_list(s.x[3]);
        s.x[0] = k->autorelease(made);
    });
    kit.method(item_class, "dealloc", [k, item_class](CpuState& s, GuestMemory&) {
        if (const auto state = k->objc.find_host_state<ItemState>(s.x[0])) {
            for (const auto object : {state->size, state->edge_spacing, state->inter_item_spacing, state->kind, state->container_anchor, state->item_anchor})
                k->release(object);
            if (state->item_provider) static_cast<void>(k->guest_call(libsystem_library, "__Block_release", {state->item_provider}));
            for (const auto object : state->supplementaries) k->release(object);
            for (const auto object : state->subitems) k->release(object);
            *state = {};
        }
        static_cast<void>(k->objc.send_super(s.x[0], item_class, k->sel("dealloc")));
    }, "v@:");
    kit.method(item_class, "copyWithZone:", [k](CpuState& s, GuestMemory&) { s.x[0] = k->retain(s.x[0]); }, "@@:^v");
    kit.method(item_class, "layoutSize", [is](CpuState& s, GuestMemory&) { s.x[0] = is(s.x[0])->size; });
    kit.method(item_class, "supplementaryItems", [k, is](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array(is(s.x[0])->supplementaries); });
    kit.method(item_class, "contentInsets", [is](CpuState& s, GuestMemory&) {
        const auto& i = is(s.x[0])->insets;
        set_d(s, 0, i.top); set_d(s, 1, i.leading); set_d(s, 2, i.bottom); set_d(s, 3, i.trailing);
    }, "{NSDirectionalEdgeInsets=dddd}@:");
    kit.method(item_class, "setContentInsets:", [is](CpuState& s, GuestMemory&) { is(s.x[0])->insets = {d(s, 0), d(s, 1), d(s, 2), d(s, 3)}; },
               "v@:{NSDirectionalEdgeInsets=dddd}");
    kit.method(item_class, "edgeSpacing", [is](CpuState& s, GuestMemory&) { s.x[0] = is(s.x[0])->edge_spacing; });
    kit.method(item_class, "setEdgeSpacing:", [k, is](CpuState& s, GuestMemory&) { k->assign(is(s.x[0])->edge_spacing, s.x[2]); }, "v@:@");
    // Groups.
    for (const bool vertical : {false, true}) {
        kit.class_method(group_class, vertical ? "verticalGroupWithLayoutSize:subitems:" : "horizontalGroupWithLayoutSize:subitems:",
                         [k, make_item, is, object_list, vertical](CpuState& s, GuestMemory&) {
            const auto made = make_item(s.x[0], s.x[2]);
            const auto state = is(made);
            state->group = true;
            state->vertical = vertical;
            state->subitems = object_list(s.x[3]);
            s.x[0] = k->autorelease(made);
        });
        kit.class_method(group_class, vertical ? "verticalGroupWithLayoutSize:subitem:count:" : "horizontalGroupWithLayoutSize:subitem:count:",
                         [k, make_item, is, vertical](CpuState& s, GuestMemory&) {
            const auto made = make_item(s.x[0], s.x[2]);
            const auto state = is(made);
            state->group = true;
            state->vertical = vertical;
            state->subitems = {k->retain(s.x[3])};
            state->count = std::max<std::int64_t>(1, static_cast<std::int64_t>(s.x[4]));
            s.x[0] = k->autorelease(made);
        }, "@@:@@q");
    }
    kit.class_method(group_class, "customGroupWithLayoutSize:itemProvider:", [k, make_item, is, copy_block](CpuState& s, GuestMemory&) {
        const auto made = make_item(s.x[0], s.x[2]);
        const auto state = is(made);
        state->group = true;
        state->custom = true;
        state->item_provider = copy_block(s.x[3]);
        s.x[0] = k->autorelease(made);
    }, "@@:@@?");
    kit.method(group_class, "subitems", [k, is](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array(is(s.x[0])->subitems); });
    kit.method(group_class, "setSupplementaryItems:", [k, is, object_list](CpuState& s, GuestMemory&) {
        const auto state = is(s.x[0]);
        for (const auto object : std::exchange(state->supplementaries, object_list(s.x[2]))) k->release(object);
    }, "v@:@");
    kit.method(group_class, "interItemSpacing", [is](CpuState& s, GuestMemory&) { s.x[0] = is(s.x[0])->inter_item_spacing; });
    kit.method(group_class, "setInterItemSpacing:", [k, is](CpuState& s, GuestMemory&) { k->assign(is(s.x[0])->inter_item_spacing, s.x[2]); }, "v@:@");
    kit.method(group_class, "visualDescription", [k, is](CpuState& s, GuestMemory&) {
        const auto state = is(s.x[0]);
        const auto text = std::string(state->custom ? "custom" : state->vertical ? "vertical" : "horizontal") + " group of " +
                          std::to_string(state->count > 0 ? state->count : static_cast<std::int64_t>(state->subitems.size())) + " items";
        s.x[0] = k->auto_string(text);
    });
    // Custom items.
    for (const bool z : {false, true})
        kit.class_method(custom_item_class, z ? "customItemWithFrame:zIndex:" : "customItemWithFrame:", [k, make, z](CpuState& s, GuestMemory&) {
            const auto made = make("NSCollectionLayoutGroupCustomItem");
            const auto state = k->objc.host_state<ItemState>(made);
            state->frame = rect_arg(s);
            if (z) state->z = static_cast<std::int64_t>(s.x[2]);
            s.x[0] = k->autorelease(made);
        }, z ? "@@:{CGRect={CGPoint=dd}{CGSize=dd}}q" : "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(custom_item_class, "frame", [is](CpuState& s, GuestMemory&) { return_rect(s, is(s.x[0])->frame); }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    kit.method(custom_item_class, "zIndex", [is](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(is(s.x[0])->z); }, "q@:");
    // Supplementary items.
    for (const bool item_anchor : {false, true})
        kit.class_method(supplementary_class, item_anchor ? "supplementaryItemWithLayoutSize:elementKind:containerAnchor:itemAnchor:"
                                                          : "supplementaryItemWithLayoutSize:elementKind:containerAnchor:",
                         [k, make_item, is, item_anchor](CpuState& s, GuestMemory&) {
            const auto made = make_item(s.x[0], s.x[2]);
            const auto state = is(made);
            state->kind = s.x[3] ? k->cf.copy(s.x[3]) : 0;
            state->container_anchor = k->retain(s.x[4]);
            if (item_anchor) state->item_anchor = k->retain(s.x[5]);
            s.x[0] = k->autorelease(made);
        });
    kit.method(supplementary_class, "elementKind", [is](CpuState& s, GuestMemory&) { s.x[0] = is(s.x[0])->kind; });
    kit.method(supplementary_class, "containerAnchor", [is](CpuState& s, GuestMemory&) { s.x[0] = is(s.x[0])->container_anchor; });
    kit.method(supplementary_class, "itemAnchor", [is](CpuState& s, GuestMemory&) { s.x[0] = is(s.x[0])->item_anchor; });
    kit.method(supplementary_class, "zIndex", [is](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(is(s.x[0])->z); }, "q@:");
    kit.method(supplementary_class, "setZIndex:", [is](CpuState& s, GuestMemory&) { is(s.x[0])->z = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    // Boundary supplementary items.
    for (const bool offset : {false, true})
        kit.class_method(boundary_class, offset ? "boundarySupplementaryItemWithLayoutSize:elementKind:alignment:absoluteOffset:"
                                                : "boundarySupplementaryItemWithLayoutSize:elementKind:alignment:",
                         [k, make_item, is, offset](CpuState& s, GuestMemory&) {
            const auto made = make_item(s.x[0], s.x[2]);
            const auto state = is(made);
            state->kind = s.x[3] ? k->cf.copy(s.x[3]) : 0;
            state->alignment = static_cast<std::int64_t>(s.x[4]);
            if (offset) state->offset = point_arg(s);
            s.x[0] = k->autorelease(made);
        }, offset ? "@@:@@q{CGPoint=dd}" : "@@:@@q");
    kit.method(boundary_class, "alignment", [is](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(is(s.x[0])->alignment); }, "q@:");
    kit.method(boundary_class, "offset", [is](CpuState& s, GuestMemory&) { return_point(s, is(s.x[0])->offset); }, "{CGPoint=dd}@:");
    kit.method(boundary_class, "pinToVisibleBounds", [is](CpuState& s, GuestMemory&) { return_bool(s, is(s.x[0])->pin); }, "B@:");
    kit.method(boundary_class, "setPinToVisibleBounds:", [is](CpuState& s, GuestMemory&) {
        if (bool_arg(s, 2)) unsupported("NSCollectionLayoutBoundarySupplementaryItem pinned to the visible bounds");
        is(s.x[0])->pin = false;
    }, "v@:B");
    kit.method(boundary_class, "extendsBoundary", [is](CpuState& s, GuestMemory&) { return_bool(s, is(s.x[0])->extends); }, "B@:");
    kit.method(boundary_class, "setExtendsBoundary:", [is](CpuState& s, GuestMemory&) { is(s.x[0])->extends = bool_arg(s, 2); }, "v@:B");
    // Decoration items.
    kit.class_method(decoration_class, "backgroundDecorationItemWithElementKind:", [k, is](CpuState& s, GuestMemory&) {
        const auto made = k->objc.allocate(s.x[0]);
        is(made)->kind = s.x[2] ? k->cf.copy(s.x[2]) : 0;
        s.x[0] = k->autorelease(made);
    });
    kit.method(decoration_class, "elementKind", [is](CpuState& s, GuestMemory&) { s.x[0] = is(s.x[0])->kind; });
    kit.method(decoration_class, "zIndex", [is](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(is(s.x[0])->z); }, "q@:");
    kit.method(decoration_class, "setZIndex:", [is](CpuState& s, GuestMemory&) { is(s.x[0])->z = static_cast<std::int64_t>(s.x[2]); }, "v@:q");

    // ---- NSCollectionLayoutSection ----
    const auto section_class = kit.declare_class("NSCollectionLayoutSection", "NSObject");
    const auto ss = [k](Address object) { return k->objc.host_state<SectionState>(object); };
    kit.class_method(section_class, "sectionWithGroup:", [k, ss](CpuState& s, GuestMemory&) {
        const auto made = k->objc.allocate(s.x[0]);
        ss(made)->group = k->retain(s.x[2]);
        s.x[0] = k->autorelease(made);
    });
    kit.method(section_class, "dealloc", [k, section_class](CpuState& s, GuestMemory&) {
        if (const auto state = k->objc.find_host_state<SectionState>(s.x[0])) {
            k->release(std::exchange(state->group, 0));
            for (const auto object : std::exchange(state->boundaries, {})) k->release(object);
            for (const auto object : std::exchange(state->decorations, {})) k->release(object);
            if (const auto handler = std::exchange(state->invalidation_handler, 0)) static_cast<void>(k->guest_call(libsystem_library, "__Block_release", {handler}));
        }
        static_cast<void>(k->objc.send_super(s.x[0], section_class, k->sel("dealloc")));
    }, "v@:");
    kit.method(section_class, "copyWithZone:", [k](CpuState& s, GuestMemory&) { s.x[0] = k->retain(s.x[0]); }, "@@:^v");
    kit.method(section_class, "contentInsets", [ss](CpuState& s, GuestMemory&) {
        const auto& i = ss(s.x[0])->insets;
        set_d(s, 0, i.top); set_d(s, 1, i.leading); set_d(s, 2, i.bottom); set_d(s, 3, i.trailing);
    }, "{NSDirectionalEdgeInsets=dddd}@:");
    kit.method(section_class, "setContentInsets:", [ss](CpuState& s, GuestMemory&) { ss(s.x[0])->insets = {d(s, 0), d(s, 1), d(s, 2), d(s, 3)}; },
               "v@:{NSDirectionalEdgeInsets=dddd}");
    kit.method(section_class, "interGroupSpacing", [ss](CpuState& s, GuestMemory&) { set_d(s, 0, ss(s.x[0])->inter_group_spacing); }, "d@:");
    kit.method(section_class, "setInterGroupSpacing:", [ss](CpuState& s, GuestMemory&) { ss(s.x[0])->inter_group_spacing = d(s, 0); }, "v@:d");
    kit.method(section_class, "orthogonalScrollingBehavior", [ss](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(ss(s.x[0])->orthogonal); }, "q@:");
    kit.method(section_class, "setOrthogonalScrollingBehavior:", [ss](CpuState& s, GuestMemory&) { ss(s.x[0])->orthogonal = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    for (const bool decorations : {false, true}) {
        kit.method(section_class, decorations ? "decorationItems" : "boundarySupplementaryItems", [k, ss, decorations](CpuState& s, GuestMemory&) {
            const auto state = ss(s.x[0]);
            s.x[0] = k->auto_array(decorations ? state->decorations : state->boundaries);
        });
        kit.method(section_class, decorations ? "setDecorationItems:" : "setBoundarySupplementaryItems:", [k, ss, object_list, decorations](CpuState& s, GuestMemory&) {
            const auto state = ss(s.x[0]);
            auto& slot = decorations ? state->decorations : state->boundaries;
            for (const auto object : std::exchange(slot, object_list(s.x[2]))) k->release(object);
        }, "v@:@");
    }
    kit.method(section_class, "supplementariesFollowContentInsets", [ss](CpuState& s, GuestMemory&) { return_bool(s, ss(s.x[0])->follow_insets); }, "B@:");
    kit.method(section_class, "setSupplementariesFollowContentInsets:", [ss](CpuState& s, GuestMemory&) { ss(s.x[0])->follow_insets = bool_arg(s, 2); }, "v@:B");
    kit.method(section_class, "visibleItemsInvalidationHandler", [ss](CpuState& s, GuestMemory&) { s.x[0] = ss(s.x[0])->invalidation_handler; }, "@?@:");
    kit.method(section_class, "setVisibleItemsInvalidationHandler:", [k, ss, copy_block](CpuState& s, GuestMemory&) {
        const auto state = ss(s.x[0]);
        if (const auto old = std::exchange(state->invalidation_handler, copy_block(s.x[2]))) static_cast<void>(k->guest_call(libsystem_library, "__Block_release", {old}));
    }, "v@:@?");

    // ---- the environment a section provider sees ----
    const auto container_class = kit.declare_class("_AlchemyCollectionLayoutContainer", "NSObject");
    for (const auto* name : {"contentSize", "effectiveContentSize"})
        kit.method(container_class, name, [k](CpuState& s, GuestMemory&) { return_size(s, k->objc.host_state<ContainerState>(s.x[0])->size); }, "{CGSize=dd}@:");
    for (const auto* name : {"contentInsets", "effectiveContentInsets"})
        kit.method(container_class, name, [](CpuState& s, GuestMemory&) { for (unsigned i = 0; i < 4; ++i) set_d(s, i, 0); }, "{NSDirectionalEdgeInsets=dddd}@:");
    const auto environment_class = kit.declare_class("_AlchemyCollectionLayoutEnvironment", "NSObject");
    kit.method(environment_class, "container", [k](CpuState& s, GuestMemory&) { s.x[0] = k->objc.host_state<EnvironmentState>(s.x[0])->container; });
    kit.method(environment_class, "dealloc", [k, environment_class](CpuState& s, GuestMemory&) {
        if (const auto state = k->objc.find_host_state<EnvironmentState>(s.x[0])) k->release(std::exchange(state->container, 0));
        static_cast<void>(k->objc.send_super(s.x[0], environment_class, k->sel("dealloc")));
    }, "v@:");

    // ---- NSCollectionViewCompositionalLayoutConfiguration ----
    const auto configuration_class = kit.declare_class("NSCollectionViewCompositionalLayoutConfiguration", "NSObject");
    const auto cs = [k](Address object) { return k->objc.host_state<ConfigurationState>(object); };
    kit.method(configuration_class, "scrollDirection", [cs](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cs(s.x[0])->direction); }, "q@:");
    kit.method(configuration_class, "setScrollDirection:", [cs](CpuState& s, GuestMemory&) {
        if (s.x[2] != 0) unsupported("NSCollectionViewCompositionalLayout horizontal scrolling");
        cs(s.x[0])->direction = 0;
    }, "v@:q");
    kit.method(configuration_class, "interSectionSpacing", [cs](CpuState& s, GuestMemory&) { set_d(s, 0, cs(s.x[0])->inter_section_spacing); }, "d@:");
    kit.method(configuration_class, "setInterSectionSpacing:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->inter_section_spacing = d(s, 0); }, "v@:d");
    kit.method(configuration_class, "boundarySupplementaryItems", [k, cs](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array(cs(s.x[0])->boundaries); });
    kit.method(configuration_class, "setBoundarySupplementaryItems:", [k, cs, object_list](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        for (const auto object : std::exchange(state->boundaries, object_list(s.x[2]))) k->release(object);
    }, "v@:@");
    kit.method(configuration_class, "copyWithZone:", [k, cs](CpuState& s, GuestMemory&) {
        const auto copy = k->alloc_init(k->objc.object_class(s.x[0]));
        const auto from = cs(s.x[0]);
        auto to = cs(copy);
        to->direction = from->direction;
        to->inter_section_spacing = from->inter_section_spacing;
        for (const auto object : from->boundaries) to->boundaries.push_back(k->retain(object));
        s.x[0] = copy;
    }, "@@:^v");

    // ---- NSCollectionViewCompositionalLayout ----
    const auto layout_class = kit.declare_class("NSCollectionViewCompositionalLayout", "NSCollectionViewLayout");
    const auto init_layout = [k, layout_class, copy_block](Address self, Address section, Address provider, Address configuration) {
        self = k->objc.send_super(self, layout_class, k->sel("init")).x0;
        if (!self) return self;
        std::lock_guard lock(info_mutex);
        auto& info = infos()[self];
        info.section = k->retain(section);
        info.provider = copy_block(provider);
        info.configuration = configuration ? k->send_id(configuration, "copy")
                                           : k->alloc_init(k->objc.find_class("NSCollectionViewCompositionalLayoutConfiguration"));
        return self;
    };
    kit.method(layout_class, "initWithSection:", [init_layout](CpuState& s, GuestMemory&) { s.x[0] = init_layout(s.x[0], s.x[2], 0, 0); });
    kit.method(layout_class, "initWithSection:configuration:", [init_layout](CpuState& s, GuestMemory&) { s.x[0] = init_layout(s.x[0], s.x[2], 0, s.x[3]); });
    kit.method(layout_class, "initWithSectionProvider:", [init_layout](CpuState& s, GuestMemory&) { s.x[0] = init_layout(s.x[0], 0, s.x[2], 0); }, "@@:@?");
    kit.method(layout_class, "initWithSectionProvider:configuration:", [init_layout](CpuState& s, GuestMemory&) { s.x[0] = init_layout(s.x[0], 0, s.x[2], s.x[3]); }, "@@:@?@");
    kit.method(layout_class, "dealloc", [k, layout_class](CpuState& s, GuestMemory&) {
        LayoutInfo info;
        {
            std::lock_guard lock(info_mutex);
            if (const auto found = infos().find(s.x[0]); found != infos().end()) { info = found->second; infos().erase(found); }
        }
        k->release(info.section);
        k->release(info.configuration);
        if (info.provider) static_cast<void>(k->guest_call(libsystem_library, "__Block_release", {info.provider}));
        static_cast<void>(k->objc.send_super(s.x[0], layout_class, k->sel("dealloc")));
    }, "v@:");
    const auto info_of = [](Address self) -> LayoutInfo {
        std::lock_guard lock(info_mutex);
        const auto found = infos().find(self);
        if (found == infos().end()) fail("not an NSCollectionViewCompositionalLayout");
        return found->second;
    };
    kit.method(layout_class, "configuration", [info_of](CpuState& s, GuestMemory&) { s.x[0] = info_of(s.x[0]).configuration; });
    kit.method(layout_class, "setConfiguration:", [k](CpuState& s, GuestMemory&) {
        Address old{};
        {
            std::lock_guard lock(info_mutex);
            auto& info = infos()[s.x[0]];
            old = std::exchange(info.configuration, s.x[2] ? k->send_id(s.x[2], "copy") : 0);
        }
        k->release(old);
        static_cast<void>(k->send(s.x[0], "invalidateLayout"));
    }, "v@:@");
    kit.method(layout_class, "prepareLayout", [k, info_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto info = info_of(self);
        const auto collection = collection_view_of_layout(*k, self);
        Builder builder{*k, collection};
        Computed computed;
        if (collection) {
            const auto bounds = AppKit::view_bounds(*k->view_state(collection));
            Size visible = bounds.size;
            if (const auto parent = k->view_state(collection)->superview; parent && k->is_kind(parent, k->clip_view))
                visible = AppKit::view_bounds(*k->view_state(parent)).size;
            builder.container = {bounds.size.width, visible.height};
            const auto configuration = info.configuration ? k->objc.host_state<ConfigurationState>(info.configuration) : nullptr;
            double y = 0;
            if (configuration) y += builder.boundaries(configuration->boundaries, true, 0, builder.container.width, y, 0);
            const auto counts = collection_section_counts(*k, collection);
            for (std::size_t section = 0; section < counts.size(); ++section) {
                Address section_object = info.section;
                if (info.provider) section_object = k->invoke_block(info.provider, {static_cast<Address>(section), builder.make_environment(builder.container)});
                if (!section_object) continue;
                if (section > 0 && configuration) y += configuration->inter_section_spacing;
                y = builder.lay_section(section_object, static_cast<std::int64_t>(section), counts[section], y);
            }
            if (configuration) y += builder.boundaries(configuration->boundaries, false, 0, builder.container.width, y, 0);
            computed.content = {builder.container.width, y};
        }
        computed.elements = std::move(builder.out);
        std::lock_guard lock(info_mutex);
        infos()[self].computed = std::move(computed);
    }, "v@:");
    kit.method(layout_class, "collectionViewContentSize", [info_of](CpuState& s, GuestMemory&) { return_size(s, info_of(s.x[0]).computed.content); }, "{CGSize=dd}@:");
    kit.method(layout_class, "shouldInvalidateLayoutForBoundsChange:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    const auto attributes = [k](const Placed& element) {
        const auto path = k->send_id(k->objc.find_class("NSIndexPath"), "indexPathForItem:inSection:", {static_cast<Address>(element.item), static_cast<Address>(element.section)});
        const auto made = collection_layout_attributes(*k, element.frame, path, element.category, element.kind);
        if (element.z) static_cast<void>(k->send_id(made, "setZIndex:", {static_cast<Address>(element.z)}));
        return made;
    };
    kit.method(layout_class, "layoutAttributesForElementsInRect:", [k, info_of, attributes](CpuState& s, GuestMemory&) {
        const auto info = info_of(s.x[0]);
        const auto rect = rect_arg(s);
        std::vector<Address> list;
        for (const auto& element : info.computed.elements) {
            const auto& r = element.frame;
            if (r.origin.x < rect.origin.x + rect.size.width && r.origin.x + r.size.width > rect.origin.x &&
                r.origin.y < rect.origin.y + rect.size.height && r.origin.y + r.size.height > rect.origin.y)
                list.push_back(attributes(element));
        }
        s.x[0] = k->auto_array(list);
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(layout_class, "layoutAttributesForItemAtIndexPath:", [k, info_of, attributes](CpuState& s, GuestMemory&) {
        const auto info = info_of(s.x[0]);
        const auto section = static_cast<std::int64_t>(k->send_id(s.x[2], "section")), item = static_cast<std::int64_t>(k->send_id(s.x[2], "item"));
        s.x[0] = 0;
        for (const auto& element : info.computed.elements)
            if (element.category == 0 && element.section == section && element.item == item) { s.x[0] = attributes(element); return; }
    });
    kit.method(layout_class, "layoutAttributesForSupplementaryViewOfKind:atIndexPath:", [k, info_of, attributes](CpuState& s, GuestMemory&) {
        const auto info = info_of(s.x[0]);
        const auto kind = k->text(s.x[2]);
        const auto section = static_cast<std::int64_t>(k->send_id(s.x[3], "section")), item = static_cast<std::int64_t>(k->send_id(s.x[3], "item"));
        s.x[0] = 0;
        for (const auto& element : info.computed.elements)
            if (element.category == 1 && element.kind == kind && element.section == section && element.item == item) { s.x[0] = attributes(element); return; }
    });
}

} // namespace alchemy::gui::completion_kit::controls
