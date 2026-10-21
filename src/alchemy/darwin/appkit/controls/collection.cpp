#include "alchemy/darwin/appkit/controls/g3.hpp"
#include "alchemy/darwin/appkit/controls/layout.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

// NSCollectionView (QLab's template chooser) with one section:
// * items from the data source (collectionView:numberOfItemsInSection:,
//   collectionView:itemForRepresentedObjectAtIndexPath:), made with
//   -makeItemWithIdentifier:forIndexPath: from a registered class or nib
//   (else the nib named like the identifier, as AppKit falls back to);
//   supplementary views likewise (the flow layout's headers and footers);
// * layout through the collection view's layout object (-prepareLayout,
//   -collectionViewContentSize, -layoutAttributesForElementsInRect:), so a
//   guest layout subclass's overrides place the items;
//   NSCollectionViewFlowLayout computes its attributes with layout.cpp's
//   flow_layout (vertical scrolling, one item size; the delegate's
//   size / inset / spacing answers are honoured);
// * selection by click (Command toggles, Shift extends when multiple
//   selection is allowed) and arrow keys, the delegate's should/did select
//   messages, -[NSCollectionViewItem setSelected:];
// * nib decoding of the collection view and its _NSCollectionViewCore
//   (which archives the layout), NSCollectionViewLayout subclasses and
//   NSCollectionViewItem (an NSViewController).
// Several sections, horizontal scrolling and drag and drop fail explicitly.
// NSIndexPath (Foundation; the AppKit item/section additions) is
// supplemented when Foundation lacks +indexPathForItem:inSection:.
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;
using drawing::Color;

struct CollectionState final : ObjectPayload {
    Address data_source{}, delegate{};                       // not retained
    Address layout{};                                        // +1
    Address background_colors{};                             // +1 array
    std::map<std::string, Address> item_classes, item_nibs;  // nibs +1
    std::map<std::string, Address> supplementary_classes, supplementary_nibs;   // "kind/identifier"; nibs +1
    std::vector<Address> items;                              // +1 NSCollectionViewItems, by item index
    std::map<std::string, Address> supplementaries;          // +1 views by "kind/index"
    std::set<std::int64_t> selection;
    std::int64_t count{};
    bool selectable{}, multiple{}, empty_selection{true}, laying_out{}, loaded{};
};
// NSCollectionViewItem state (the object's payload is NSViewController's,
// so this lives beside it, removed by the item's -dealloc).
struct ItemState {
    Address collection{};                                    // not retained
    Address image_view{}, text_field{};                      // outlets (not retained: the view owns them)
    std::int64_t highlight{};
    bool selected{};
};
std::map<Address, ItemState>& item_states() {
    static std::map<Address, ItemState> table;
    return table;
}
ItemState& item_state(Address item) { return item_states()[item]; }
struct LayoutState final : ObjectPayload {
    Address collection{};                                    // not retained
    layout::FlowLayout flow;
    layout::FlowResult result;                               // the last -prepareLayout
    Size estimated{};
    std::int64_t direction{};                                // NSCollectionViewScrollDirectionVertical
    std::map<std::string, Address> decoration_classes;
    bool pin_headers{}, pin_footers{}, flow_layout{};
};
struct AttributesState final : ObjectPayload {
    Rect frame;
    Address index_path{};                                    // +1
    Address kind{};                                          // +1 (supplementary kind)
    std::int64_t category{};                                 // NSCollectionElementCategoryItem 0, supplementary 1
    double alpha{1};
    std::int64_t z{};
    bool hidden{};
};
struct CoreState final : ObjectPayload { Address layout{}; };   // +1 (_NSCollectionViewCore)
struct PathState final : ObjectPayload { std::vector<std::uint64_t> indexes; };

Address attributes_class{}, index_path_class{};
const char* const header_kind = "UICollectionElementKindSectionHeader";
const char* const footer_kind = "UICollectionElementKindSectionFooter";

std::shared_ptr<CollectionState> collection_of(AppKit& kit, Address view) { return kit.extension<CollectionState>(view); }
std::shared_ptr<LayoutState> layout_of(AppKit& kit, Address layout) { return kit.objc.host_state<LayoutState>(layout); }

Address index_path(AppKit& kit, std::int64_t item) {
    return kit.send_id(index_path_class, "indexPathForItem:inSection:", {static_cast<Address>(item), 0});
}
std::int64_t item_of(AppKit& kit, Address path) {
    if (!path) return -1;
    if (static_cast<std::int64_t>(kit.send_id(path, "section")) != 0) unsupported("NSCollectionView sections other than 0");
    return static_cast<std::int64_t>(kit.send_id(path, "item"));
}
Address path_set(AppKit& kit, const std::set<std::int64_t>& items) {
    std::vector<Address> paths;
    for (const auto item : items) paths.push_back(index_path(kit, item));
    const auto array = kit.auto_array(paths);
    return kit.send_id(kit.objc.find_class("NSSet"), "setWithArray:", {array});
}
std::set<std::int64_t> items_of_set(AppKit& kit, Address set) {
    std::set<std::int64_t> out;
    if (!set) return out;
    const auto all = kit.responds(set, "allObjects") ? kit.send_id(set, "allObjects") : set;
    for (const auto path : kit.items(all)) out.insert(item_of(kit, path));
    return out;
}

Address make_attributes(AppKit& kit, Rect frame, std::int64_t item, std::int64_t category, std::string_view kind) {
    const auto made = kit.objc.allocate(attributes_class);
    const auto state = kit.objc.host_state<AttributesState>(made);
    state->frame = frame;
    state->index_path = kit.retain(index_path(kit, item));
    state->category = category;
    if (!kind.empty()) state->kind = kit.string(kind);
    return kit.autorelease(made);
}

// The flow layout's metrics, with the delegate's answers (one section).
void flow_metrics(AppKit& kit, Address layout_object, LayoutState& state) {
    const auto collection = state.collection;
    if (!collection) return;
    const auto delegate = collection_of(kit, collection)->delegate;
    if (!delegate) return;
    const auto ask = [&](const char* selector, CallRegs registers) {
        registers.x[2] = collection;
        registers.x[3] = layout_object;
        return kit.send_optional(delegate, selector, registers);
    };
    CallRegs section;
    section.x[4] = 0;
    if (const auto inset = ask("collectionView:layout:insetForSectionAtIndex:", section))
        state.flow.section_inset = {inset->d(0), inset->d(1), inset->d(2), inset->d(3)};
    if (const auto value = ask("collectionView:layout:minimumLineSpacingForSectionAtIndex:", section)) state.flow.line_spacing = value->d(0);
    if (const auto value = ask("collectionView:layout:minimumInteritemSpacingForSectionAtIndex:", section)) state.flow.interitem_spacing = value->d(0);
    if (const auto value = ask("collectionView:layout:referenceSizeForHeaderInSection:", section)) state.flow.header = value->d(1);
    if (const auto value = ask("collectionView:layout:referenceSizeForFooterInSection:", section)) state.flow.footer = value->d(1);
    if (kit.responds(delegate, "collectionView:layout:sizeForItemAtIndexPath:")) {
        const auto count = collection_of(kit, collection)->count;
        std::optional<Size> uniform;
        for (std::int64_t i = 0; i < count; ++i) {
            CallRegs registers;
            registers.x[4] = index_path(kit, i);
            const auto answer = ask("collectionView:layout:sizeForItemAtIndexPath:", registers);
            const Size size{answer->d(0), answer->d(1)};
            if (uniform && (uniform->width != size.width || uniform->height != size.height))
                unsupported("NSCollectionViewFlowLayout items of different sizes");
            uniform = size;
        }
        if (uniform) state.flow.item = *uniform;
    }
}

// Lays the items and supplementary views out from the layout's attributes.
void layout_items(AppKit& kit, Address self) {
    const auto state = collection_of(kit, self);
    if (state->laying_out || !state->layout) return;
    state->laying_out = true;
    try {
        static_cast<void>(kit.send(state->layout, "prepareLayout"));
        const auto content = result_size(kit.send(state->layout, "collectionViewContentSize"));
        // The view covers its content and at least its clip view.
        auto size = kit.view_state(self)->frame.size;
        size.height = content.height;
        if (const auto parent = kit.view_state(self)->superview; parent && kit.is_kind(parent, kit.clip_view)) {
            const auto visible = AppKit::view_bounds(*kit.view_state(parent)).size;
            size.width = visible.width;
            size.height = std::max(size.height, visible.height);
        }
        CallRegs frame_size;
        put_size(frame_size, 0, size);
        static_cast<void>(kit.send(self, "setFrameSize:", frame_size));
        CallRegs everything;
        put_rect(everything, 0, {{0, 0}, {std::max(size.width, content.width), std::max(size.height, content.height)}});
        const auto attributes = kit.send(state->layout, "layoutAttributesForElementsInRect:", everything).x0;
        for (const auto entry : attributes ? kit.items(attributes) : std::vector<Address>{}) {
            CallRegs none;
            const auto frame = result_rect(kit.send(entry, "frame", none));
            const auto item = item_of(kit, kit.send_id(entry, "indexPath"));
            const auto category = static_cast<std::int64_t>(kit.send_id(entry, "representedElementCategory"));
            Address view{};
            if (category == 0) {
                if (item >= 0 && item < static_cast<std::int64_t>(state->items.size()) && state->items[static_cast<std::size_t>(item)])
                    view = kit.send_id(state->items[static_cast<std::size_t>(item)], "view");
            } else if (category == 1) {
                const auto kind = kit.text(kit.send_id(entry, "representedElementKind"));
                const auto key = kind + "/" + std::to_string(item);
                if (!state->supplementaries.contains(key) && state->delegate &&
                    kit.responds(state->delegate, "collectionView:viewForSupplementaryElementOfKind:atIndexPath:")) {
                    const auto made = kit.send_id(state->delegate, "collectionView:viewForSupplementaryElementOfKind:atIndexPath:",
                                                  {self, kit.auto_string(kind), index_path(kit, item)});
                    if (made) {
                        state->supplementaries[key] = kit.retain(made);
                        static_cast<void>(kit.send_id(self, "addSubview:", {made}));
                    }
                }
                if (const auto found = state->supplementaries.find(key); found != state->supplementaries.end()) view = found->second;
            }
            if (!view) continue;
            CallRegs registers;
            put_rect(registers, 0, frame);
            static_cast<void>(kit.send(view, "setFrame:", registers));
            static_cast<void>(kit.send_id(view, "setHidden:", {kit.send_bool(entry, "isHidden") ? 1u : 0u}));
        }
    } catch (...) {
        state->laying_out = false;
        throw;
    }
    state->laying_out = false;
    kit.invalidate_view(self);
}

void discard_items(AppKit& kit, CollectionState& state) {
    for (const auto item : std::exchange(state.items, {})) {
        if (!item) continue;
        const auto view = kit.send_id(item, "view");
        if (view) static_cast<void>(kit.send(view, "removeFromSuperview"));
        if (const auto found = item_states().find(item); found != item_states().end()) found->second.collection = 0;
        kit.release(item);
    }
    for (auto& [key, view] : std::exchange(state.supplementaries, {})) {
        static_cast<void>(kit.send(view, "removeFromSuperview"));
        kit.release(view);
    }
}

void reload(AppKit& kit, Address self) {
    const auto state = collection_of(kit, self);
    discard_items(kit, *state);
    state->loaded = true;
    state->count = 0;
    if (const auto source = state->data_source) {
        if (kit.responds(source, "numberOfSectionsInCollectionView:")) {
            const auto sections = static_cast<std::int64_t>(kit.send_id(source, "numberOfSectionsInCollectionView:", {self}));
            if (sections > 1) unsupported("NSCollectionView with " + std::to_string(sections) + " sections");
            if (sections == 0) { layout_items(kit, self); return; }
        }
        state->count = static_cast<std::int64_t>(kit.send_id(source, "collectionView:numberOfItemsInSection:", {self, 0}));
    }
    for (auto it = state->selection.begin(); it != state->selection.end();) it = *it >= state->count ? state->selection.erase(it) : std::next(it);
    for (std::int64_t i = 0; i < state->count; ++i) {
        const auto item = kit.send_id(state->data_source, "collectionView:itemForRepresentedObjectAtIndexPath:", {self, index_path(kit, i)});
        if (!item) unsupported("collectionView:itemForRepresentedObjectAtIndexPath: answered nil");
        state->items.push_back(kit.retain(item));
        item_state(item).collection = self;
        const auto view = kit.send_id(item, "view");
        if (view) static_cast<void>(kit.send_id(self, "addSubview:", {view}));
        if (state->selection.contains(i)) static_cast<void>(kit.send_id(item, "setSelected:", {1}));
    }
    layout_items(kit, self);
}

// Selection changes through the delegate (should / did select and deselect).
void change_selection(AppKit& kit, Address self, std::set<std::int64_t> wanted, bool from_user) {
    const auto state = collection_of(kit, self);
    std::set<std::int64_t> added, removed;
    for (const auto item : wanted) if (!state->selection.contains(item)) added.insert(item);
    for (const auto item : state->selection) if (!wanted.contains(item)) removed.insert(item);
    const auto delegate = state->delegate;
    if (from_user && delegate) {
        if (!added.empty() && kit.responds(delegate, "collectionView:shouldSelectItemsAtIndexPaths:"))
            added = items_of_set(kit, kit.send_id(delegate, "collectionView:shouldSelectItemsAtIndexPaths:", {self, path_set(kit, added)}));
        if (!removed.empty() && kit.responds(delegate, "collectionView:shouldDeselectItemsAtIndexPaths:"))
            removed = items_of_set(kit, kit.send_id(delegate, "collectionView:shouldDeselectItemsAtIndexPaths:", {self, path_set(kit, removed)}));
    }
    for (const auto item : removed) state->selection.erase(item);
    for (const auto item : added) state->selection.insert(item);
    for (const auto item : removed)
        if (item < static_cast<std::int64_t>(state->items.size())) static_cast<void>(kit.send_id(state->items[static_cast<std::size_t>(item)], "setSelected:", {0}));
    for (const auto item : added)
        if (item < static_cast<std::int64_t>(state->items.size())) static_cast<void>(kit.send_id(state->items[static_cast<std::size_t>(item)], "setSelected:", {1}));
    if (from_user && delegate) {
        if (!removed.empty()) static_cast<void>(kit.send_optional(delegate, "collectionView:didDeselectItemsAtIndexPaths:",
                                                                [&] { CallRegs r; r.x[2] = self; r.x[3] = path_set(kit, removed); return r; }()));
        if (!added.empty()) static_cast<void>(kit.send_optional(delegate, "collectionView:didSelectItemsAtIndexPaths:",
                                                              [&] { CallRegs r; r.x[2] = self; r.x[3] = path_set(kit, added); return r; }()));
    }
    kit.invalidate_view(self);
}

void install_index_paths(AppKit& kit) {
    index_path_class = kit.objc.find_class("NSIndexPath");
    if (index_path_class && kit.responds(index_path_class, "indexPathForItem:inSection:")) return;   // Foundation's
    auto* const k = &kit;
    if (!index_path_class) {
        index_path_class = kit.objc.register_native_class("NSIndexPath", kit.objc.find_class("NSObject"), 8);
        for (const auto* library : {"/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation"}) {
            kit.process.install_export(library, "_OBJC_CLASS_$_NSIndexPath", {index_path_class, ExportKind::data});
            kit.process.install_export(library, "_OBJC_METACLASS_$_NSIndexPath", {kit.process.main_engine().read64(index_path_class), ExportKind::data});
        }
    }
    const auto c = index_path_class;
    const auto ps = [k](Address value) { return k->objc.host_state<PathState>(value); };
    const auto make = [k, ps](Address cls, std::vector<std::uint64_t> indexes) {
        const auto made = k->objc.allocate(cls);
        ps(made)->indexes = std::move(indexes);
        return k->autorelease(made);
    };
    kit.class_method(c, "indexPathForItem:inSection:", [make](CpuState& s, GuestMemory&) { s.x[0] = make(s.x[0], {s.x[3], s.x[2]}); });
    kit.class_method(c, "indexPathWithIndex:", [make](CpuState& s, GuestMemory&) { s.x[0] = make(s.x[0], {s.x[2]}); });
    kit.class_method(c, "indexPathWithIndexes:length:", [make](CpuState& s, GuestMemory& m) {
        std::vector<std::uint64_t> indexes;
        for (Address i = 0; i < s.x[3]; ++i) indexes.push_back(m.read64(s.x[2] + i * 8));
        s.x[0] = make(s.x[0], std::move(indexes));
    });
    kit.method(c, "length", [ps](CpuState& s, GuestMemory&) { s.x[0] = ps(s.x[0])->indexes.size(); }, "Q@:");
    kit.method(c, "indexAtPosition:", [ps](CpuState& s, GuestMemory&) {
        const auto& list = ps(s.x[0])->indexes;
        s.x[0] = s.x[2] < list.size() ? list[s.x[2]] : static_cast<Address>(not_found);
    }, "Q@:Q");
    kit.method(c, "section", [ps](CpuState& s, GuestMemory&) { const auto& list = ps(s.x[0])->indexes; s.x[0] = list.empty() ? 0 : list[0]; }, "q@:");
    kit.method(c, "item", [ps](CpuState& s, GuestMemory&) { const auto& list = ps(s.x[0])->indexes; s.x[0] = list.size() > 1 ? list[1] : 0; }, "q@:");
    kit.method(c, "indexPathByAddingIndex:", [ps, make](CpuState& s, GuestMemory&) {
        auto list = ps(s.x[0])->indexes;
        list.push_back(s.x[2]);
        s.x[0] = make(index_path_class, std::move(list));
    });
    kit.method(c, "isEqual:", [k, ps](CpuState& s, GuestMemory&) {
        const auto other = s.x[2] ? k->objc.find_host_state<PathState>(s.x[2]) : nullptr;
        return_bool(s, other && other->indexes == ps(s.x[0])->indexes);
    }, "B@:@");
    kit.method(c, "hash", [ps](CpuState& s, GuestMemory&) {
        std::uint64_t value = 1469598103934665603ULL;
        for (const auto index : ps(s.x[0])->indexes) value = (value ^ index) * 1099511628211ULL;
        s.x[0] = value;
    }, "Q@:");
    kit.method(c, "compare:", [k, ps](CpuState& s, GuestMemory&) {
        const auto& a = ps(s.x[0])->indexes;
        const auto other = s.x[2] ? k->objc.find_host_state<PathState>(s.x[2]) : nullptr;
        const auto& b = other ? other->indexes : std::vector<std::uint64_t>{};
        s.x[0] = a < b ? static_cast<Address>(-1) : a == b ? 0 : 1;
    }, "q@:@");
    kit.method(c, "copyWithZone:", [k](CpuState& s, GuestMemory&) { s.x[0] = k->retain(s.x[0]); }, "@@:^v");
}

} // namespace

void install_collection_view(AppKit& kit) {
    auto* const k = &kit;
    install_index_paths(kit);
    const auto view_class = kit.declare_class("NSCollectionView", "NSView");
    const auto item_class = kit.declare_class("NSCollectionViewItem", "NSViewController");
    const auto layout_class = kit.declare_class("NSCollectionViewLayout", "NSObject");
    const auto flow_class = kit.declare_class("NSCollectionViewFlowLayout", "NSCollectionViewLayout");
    attributes_class = kit.declare_class("NSCollectionViewLayoutAttributes", "NSObject");
    const auto core_class = kit.declare_class("_NSCollectionViewCore", "NSObject");
    kit.string_constant("_NSCollectionElementKindSectionHeader", header_kind);
    kit.string_constant("_NSCollectionElementKindSectionFooter", footer_kind);
    kit.string_constant("_NSCollectionElementKindInterItemGapIndicator", "NSCollectionElementKindInterItemGapIndicator");

    // ---- NSCollectionViewLayoutAttributes ----
    const auto at = attributes_class;
    const auto as = [k](Address value) { return k->objc.host_state<AttributesState>(value); };
    kit.class_method(at, "layoutAttributesForItemWithIndexPath:", [k](CpuState& s, GuestMemory&) {
        s.x[0] = make_attributes(*k, {}, item_of(*k, s.x[2]), 0, {});
    });
    kit.class_method(at, "layoutAttributesForSupplementaryViewOfKind:withIndexPath:", [k](CpuState& s, GuestMemory&) {
        s.x[0] = make_attributes(*k, {}, item_of(*k, s.x[3]), 1, k->text(s.x[2]));
    });
    kit.method(at, "frame", [as](CpuState& s, GuestMemory&) { return_rect(s, as(s.x[0])->frame); }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    kit.method(at, "setFrame:", [as](CpuState& s, GuestMemory&) { as(s.x[0])->frame = rect_arg(s); }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(at, "size", [as](CpuState& s, GuestMemory&) { return_size(s, as(s.x[0])->frame.size); }, "{CGSize=dd}@:");
    kit.method(at, "setSize:", [as](CpuState& s, GuestMemory&) { as(s.x[0])->frame.size = size_arg(s); }, "v@:{CGSize=dd}");
    kit.method(at, "indexPath", [as](CpuState& s, GuestMemory&) { s.x[0] = as(s.x[0])->index_path; });
    kit.method(at, "setIndexPath:", [k, as](CpuState& s, GuestMemory&) { k->assign(as(s.x[0])->index_path, s.x[2]); }, "v@:@");
    kit.method(at, "representedElementCategory", [as](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(as(s.x[0])->category); }, "q@:");
    kit.method(at, "representedElementKind", [as](CpuState& s, GuestMemory&) { s.x[0] = as(s.x[0])->kind; });
    kit.method(at, "alpha", [as](CpuState& s, GuestMemory&) { set_d(s, 0, as(s.x[0])->alpha); }, "d@:");
    kit.method(at, "setAlpha:", [as](CpuState& s, GuestMemory&) { as(s.x[0])->alpha = d(s, 0); }, "v@:d");
    kit.method(at, "zIndex", [as](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(as(s.x[0])->z); }, "q@:");
    kit.method(at, "setZIndex:", [as](CpuState& s, GuestMemory&) { as(s.x[0])->z = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    kit.method(at, "isHidden", [as](CpuState& s, GuestMemory&) { return_bool(s, as(s.x[0])->hidden); }, "B@:");
    kit.method(at, "setHidden:", [as](CpuState& s, GuestMemory&) { as(s.x[0])->hidden = bool_arg(s, 2); }, "v@:B");
    kit.method(at, "copyWithZone:", [k, as](CpuState& s, GuestMemory&) {
        const auto from = as(s.x[0]);
        const auto copy = k->objc.allocate(k->objc.object_class(s.x[0]));
        auto to = std::make_shared<AttributesState>(*from);
        to->index_path = k->retain(to->index_path);
        to->kind = k->retain(to->kind);
        static_cast<void>(k->objc.host_state<AttributesState>(copy, [&] { return to; }));
        s.x[0] = copy;
    }, "@@:^v");
    kit.method(at, "dealloc", [k, at](CpuState& s, GuestMemory&) {
        if (const auto state = k->objc.find_host_state<AttributesState>(s.x[0])) {
            k->release(std::exchange(state->index_path, 0));
            k->release(std::exchange(state->kind, 0));
        }
        static_cast<void>(k->objc.send_super(s.x[0], at, k->sel("dealloc")));
    }, "v@:");

    // ---- NSCollectionViewLayout ----
    const auto lc = layout_class;
    kit.class_method(lc, "layoutAttributesClass", [](CpuState& s, GuestMemory&) { s.x[0] = attributes_class; });
    kit.method(lc, "initWithCoder:", [k](CpuState& s, GuestMemory&) { s.x[0] = k->send(s.x[0], "init").x0; });
    kit.method(lc, "collectionView", [k](CpuState& s, GuestMemory&) { s.x[0] = layout_of(*k, s.x[0])->collection; });
    kit.method(lc, "invalidateLayout", [k](CpuState& s, GuestMemory&) {
        if (const auto collection = layout_of(*k, s.x[0])->collection) layout_items(*k, collection);
    }, "v@:");
    kit.method(lc, "invalidateLayoutWithContext:", [k](CpuState& s, GuestMemory&) { static_cast<void>(k->send(s.x[0], "invalidateLayout")); }, "v@:@");
    kit.method(lc, "prepareLayout", [](CpuState&, GuestMemory&) {}, "v@:");   // a custom layout's own preparation
    kit.method(lc, "collectionViewContentSize", [](CpuState& s, GuestMemory&) { return_size(s, {}); }, "{CGSize=dd}@:");
    kit.method(lc, "layoutAttributesForElementsInRect:", [k](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array({}); },
               "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(lc, "layoutAttributesForItemAtIndexPath:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    kit.method(lc, "layoutAttributesForSupplementaryViewOfKind:atIndexPath:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    kit.method(lc, "shouldInvalidateLayoutForBoundsChange:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(lc, "registerClass:forDecorationViewOfKind:", [k](CpuState& s, GuestMemory&) {
        layout_of(*k, s.x[0])->decoration_classes[k->text(s.x[3])] = s.x[2];
    }, "v@:#@");

    // ---- NSCollectionViewFlowLayout ----
    const auto fl = flow_class;
    kit.method(fl, "init", [k, fl](CpuState& s, GuestMemory&) {
        s.x[0] = k->objc.send_super(s.x[0], fl, k->sel("init")).x0;
        if (!s.x[0]) return;
        const auto state = layout_of(*k, s.x[0]);
        state->flow_layout = true;
        state->flow.item = {50, 50};   // AppKit's default item size
        state->flow.line_spacing = 10;
        state->flow.interitem_spacing = 10;
    });
    kit.method(fl, "itemSize", [k](CpuState& s, GuestMemory&) { return_size(s, layout_of(*k, s.x[0])->flow.item); }, "{CGSize=dd}@:");
    kit.method(fl, "setItemSize:", [k](CpuState& s, GuestMemory&) {
        layout_of(*k, s.x[0])->flow.item = size_arg(s);
        static_cast<void>(k->send(s.x[0], "invalidateLayout"));
    }, "v@:{CGSize=dd}");
    kit.method(fl, "estimatedItemSize", [k](CpuState& s, GuestMemory&) { return_size(s, layout_of(*k, s.x[0])->estimated); }, "{CGSize=dd}@:");
    kit.method(fl, "setEstimatedItemSize:", [k](CpuState& s, GuestMemory&) {
        const auto size = size_arg(s);
        if (size.width != 0 || size.height != 0) unsupported("NSCollectionViewFlowLayout self-sizing items (estimatedItemSize)");
        layout_of(*k, s.x[0])->estimated = size;
    }, "v@:{CGSize=dd}");
    const auto spacing = [&kit, fl, k](const char* getter, const char* setter, double layout::FlowLayout::*member) {
        kit.method(fl, getter, [k, member](CpuState& s, GuestMemory&) { set_d(s, 0, layout_of(*k, s.x[0])->flow.*member); }, "d@:");
        kit.method(fl, setter, [k, member](CpuState& s, GuestMemory&) {
            layout_of(*k, s.x[0])->flow.*member = d(s, 0);
            static_cast<void>(k->send(s.x[0], "invalidateLayout"));
        }, "v@:d");
    };
    spacing("minimumLineSpacing", "setMinimumLineSpacing:", &layout::FlowLayout::line_spacing);
    spacing("minimumInteritemSpacing", "setMinimumInteritemSpacing:", &layout::FlowLayout::interitem_spacing);
    kit.method(fl, "sectionInset", [k](CpuState& s, GuestMemory&) {
        const auto& i = layout_of(*k, s.x[0])->flow.section_inset;
        set_d(s, 0, i.top); set_d(s, 1, i.left); set_d(s, 2, i.bottom); set_d(s, 3, i.right);
    }, "{NSEdgeInsets=dddd}@:");
    kit.method(fl, "setSectionInset:", [k](CpuState& s, GuestMemory&) {
        layout_of(*k, s.x[0])->flow.section_inset = {d(s, 0), d(s, 1), d(s, 2), d(s, 3)};
        static_cast<void>(k->send(s.x[0], "invalidateLayout"));
    }, "v@:{NSEdgeInsets=dddd}");
    for (const bool header : {true, false}) {
        kit.method(fl, header ? "headerReferenceSize" : "footerReferenceSize", [k, header](CpuState& s, GuestMemory&) {
            const auto& flow = layout_of(*k, s.x[0])->flow;
            return_size(s, {0, header ? flow.header : flow.footer});
        }, "{CGSize=dd}@:");
        kit.method(fl, header ? "setHeaderReferenceSize:" : "setFooterReferenceSize:", [k, header](CpuState& s, GuestMemory&) {
            auto& flow = layout_of(*k, s.x[0])->flow;
            (header ? flow.header : flow.footer) = size_arg(s).height;
            static_cast<void>(k->send(s.x[0], "invalidateLayout"));
        }, "v@:{CGSize=dd}");
    }
    kit.method(fl, "scrollDirection", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(layout_of(*k, s.x[0])->direction); }, "q@:");
    kit.method(fl, "setScrollDirection:", [k](CpuState& s, GuestMemory&) {
        if (s.x[2] != 0) unsupported("NSCollectionViewFlowLayout horizontal scrolling");
        layout_of(*k, s.x[0])->direction = 0;
    }, "v@:q");
    for (const auto& [getter, setter, member] : std::initializer_list<std::tuple<const char*, const char*, bool LayoutState::*>>{
             {"sectionHeadersPinToVisibleBounds", "setSectionHeadersPinToVisibleBounds:", &LayoutState::pin_headers},
             {"sectionFootersPinToVisibleBounds", "setSectionFootersPinToVisibleBounds:", &LayoutState::pin_footers}}) {
        const auto field = member;
        kit.method(fl, getter, [k, field](CpuState& s, GuestMemory&) { return_bool(s, (*layout_of(*k, s.x[0])).*field); }, "B@:");
        kit.method(fl, setter, [k, field](CpuState& s, GuestMemory&) {
            if (bool_arg(s, 2)) unsupported("NSCollectionViewFlowLayout pinned section headers / footers");
            (*layout_of(*k, s.x[0])).*field = false;
        }, "v@:B");
    }
    kit.method(fl, "prepareLayout", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = layout_of(*k, self);
        if (state->direction != 0) unsupported("NSCollectionViewFlowLayout horizontal scrolling");
        const auto collection = state->collection;
        if (!collection) { state->result = {}; return; }
        flow_metrics(*k, self, *state);
        const auto width = AppKit::view_bounds(*k->view_state(collection)).size.width;
        state->result = layout::flow_layout(state->flow, static_cast<std::size_t>(collection_of(*k, collection)->count), width);
    }, "v@:");
    kit.method(fl, "collectionViewContentSize", [k](CpuState& s, GuestMemory&) { return_size(s, layout_of(*k, s.x[0])->result.content); }, "{CGSize=dd}@:");
    kit.method(fl, "layoutAttributesForElementsInRect:", [k](CpuState& s, GuestMemory&) {
        const auto state = layout_of(*k, s.x[0]);
        const auto rect = rect_arg(s);
        const auto meets = [&](Rect r) {
            return r.origin.x < rect.origin.x + rect.size.width && r.origin.x + r.size.width > rect.origin.x &&
                   r.origin.y < rect.origin.y + rect.size.height && r.origin.y + r.size.height > rect.origin.y;
        };
        std::vector<Address> list;
        const auto width = state->result.content.width;
        if (state->flow.header > 0) {
            const Rect header{{0, 0}, {width, state->flow.header}};
            if (meets(header)) list.push_back(make_attributes(*k, header, 0, 1, header_kind));
        }
        for (std::size_t i = 0; i < state->result.frames.size(); ++i)
            if (meets(state->result.frames[i])) list.push_back(make_attributes(*k, state->result.frames[i], static_cast<std::int64_t>(i), 0, {}));
        if (state->flow.footer > 0) {
            const Rect footer{{0, state->result.content.height - state->flow.footer}, {width, state->flow.footer}};
            if (meets(footer)) list.push_back(make_attributes(*k, footer, 0, 1, footer_kind));
        }
        s.x[0] = k->auto_array(list);
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(fl, "layoutAttributesForItemAtIndexPath:", [k](CpuState& s, GuestMemory&) {
        const auto state = layout_of(*k, s.x[0]);
        const auto item = item_of(*k, s.x[2]);
        s.x[0] = item >= 0 && item < static_cast<std::int64_t>(state->result.frames.size())
                     ? make_attributes(*k, state->result.frames[static_cast<std::size_t>(item)], item, 0, {}) : 0;
    });
    kit.method(fl, "layoutAttributesForSupplementaryViewOfKind:atIndexPath:", [k](CpuState& s, GuestMemory&) {
        const auto state = layout_of(*k, s.x[0]);
        const auto kind = k->text(s.x[2]);
        const auto width = state->result.content.width;
        if (kind == header_kind && state->flow.header > 0) s.x[0] = make_attributes(*k, {{0, 0}, {width, state->flow.header}}, 0, 1, kind);
        else if (kind == footer_kind && state->flow.footer > 0)
            s.x[0] = make_attributes(*k, {{0, state->result.content.height - state->flow.footer}, {width, state->flow.footer}}, 0, 1, kind);
        else s.x[0] = 0;
    });

    // ---- _NSCollectionViewCore (archived) ----
    kit.method(core_class, "initWithCoder:", [k](CpuState& s, GuestMemory&) {
        const auto self = k->send(s.x[0], "init").x0;
        s.x[0] = self;
        const Decoder in{*k, s.x[2]};
        k->objc.host_state<CoreState>(self)->layout = k->retain(in.object("UICollectionLayout"));
    });
    kit.method(core_class, "dealloc", [k, core_class](CpuState& s, GuestMemory&) {
        if (const auto state = k->objc.find_host_state<CoreState>(s.x[0])) k->release(std::exchange(state->layout, 0));
        static_cast<void>(k->objc.send_super(s.x[0], core_class, k->sel("dealloc")));
    }, "v@:");

    // ---- NSCollectionViewItem ----
    const auto it = item_class;
    const auto is = [](Address value) { return &item_state(value); };
    kit.method(it, "dealloc", [k, it](CpuState& s, GuestMemory&) {
        item_states().erase(s.x[0]);
        static_cast<void>(k->objc.send_super(s.x[0], it, k->sel("dealloc")));
    }, "v@:");
    kit.method(it, "collectionView", [is](CpuState& s, GuestMemory&) { s.x[0] = is(s.x[0])->collection; });
    kit.method(it, "isSelected", [is](CpuState& s, GuestMemory&) { return_bool(s, is(s.x[0])->selected); }, "B@:");
    kit.method(it, "setSelected:", [k, is](CpuState& s, GuestMemory&) {
        is(s.x[0])->selected = bool_arg(s, 2);
        if (const auto view = k->send(s.x[0], "viewIfLoaded").x0) k->invalidate_view(view);
    }, "v@:B");
    kit.method(it, "highlightState", [is](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(is(s.x[0])->highlight); }, "q@:");
    kit.method(it, "setHighlightState:", [is](CpuState& s, GuestMemory&) { is(s.x[0])->highlight = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    kit.method(it, "imageView", [is](CpuState& s, GuestMemory&) { s.x[0] = is(s.x[0])->image_view; });
    kit.method(it, "setImageView:", [is](CpuState& s, GuestMemory&) { is(s.x[0])->image_view = s.x[2]; }, "v@:@");
    kit.method(it, "textField", [is](CpuState& s, GuestMemory&) { s.x[0] = is(s.x[0])->text_field; });
    kit.method(it, "setTextField:", [is](CpuState& s, GuestMemory&) { is(s.x[0])->text_field = s.x[2]; }, "v@:@");
    kit.method(it, "draggingImageComponents", [](CpuState&, GuestMemory&) { unsupported("NSCollectionViewItem dragging images (drag and drop)"); });

    // ---- NSCollectionView ----
    const auto cv = view_class;
    const auto attach_layout = [k](Address self, Address layout_object) {
        const auto state = collection_of(*k, self);
        if (state->layout) layout_of(*k, state->layout)->collection = 0;
        k->assign(state->layout, layout_object);
        if (layout_object) layout_of(*k, layout_object)->collection = self;
    };
    kit.method(cv, "initWithFrame:", [k, cv](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        s.x[0] = k->objc.send_super(s.x[0], cv, k->sel("initWithFrame:"), registers).x0;
        if (s.x[0]) static_cast<void>(collection_of(*k, s.x[0]));
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(cv, "initWithCoder:", [k, cv, attach_layout](CpuState& s, GuestMemory&) {
        static_cast<void>(collection_of(*k, s.x[0]));
        const auto self = super_init_with_coder(*k, s.x[0], cv, s.x[2]);
        s.x[0] = self;
        if (!self) return;
        const Decoder in{*k, s.x[2]};
        const auto state = collection_of(*k, self);
        if (const auto core = in.object("NSCollectionViewCore"))
            if (const auto info = k->objc.find_host_state<CoreState>(core)) attach_layout(self, info->layout);
        if (const auto colors = in.object("NSBackgroundColors")) k->assign(state->background_colors, colors);
        state->selectable = in.flag("NSSelectable", false);
        state->multiple = in.flag("NSAllowsMultipleSelection", false);
        state->empty_selection = in.flag("NSAllowsEmptySelection", true);
    });
    kit.method(cv, "dealloc", [k, cv](CpuState& s, GuestMemory&) {
        if (const auto state = k->find_extension<CollectionState>(s.x[0])) {
            discard_items(*k, *state);
            if (state->layout) layout_of(*k, state->layout)->collection = 0;
            k->release(std::exchange(state->layout, 0));
            k->release(std::exchange(state->background_colors, 0));
            for (auto& [key, nib] : std::exchange(state->item_nibs, {})) k->release(nib);
            for (auto& [key, nib] : std::exchange(state->supplementary_nibs, {})) k->release(nib);
        }
        static_cast<void>(k->objc.send_super(s.x[0], cv, k->sel("dealloc")));
    }, "v@:");
    kit.method(cv, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    kit.method(cv, "dataSource", [k](CpuState& s, GuestMemory&) { s.x[0] = collection_of(*k, s.x[0])->data_source; });
    kit.method(cv, "setDataSource:", [k](CpuState& s, GuestMemory&) { collection_of(*k, s.x[0])->data_source = s.x[2]; }, "v@:@");
    kit.method(cv, "delegate", [k](CpuState& s, GuestMemory&) { s.x[0] = collection_of(*k, s.x[0])->delegate; });
    kit.method(cv, "setDelegate:", [k](CpuState& s, GuestMemory&) { collection_of(*k, s.x[0])->delegate = s.x[2]; }, "v@:@");
    kit.method(cv, "collectionViewLayout", [k](CpuState& s, GuestMemory&) { s.x[0] = collection_of(*k, s.x[0])->layout; });
    kit.method(cv, "setCollectionViewLayout:", [k, attach_layout](CpuState& s, GuestMemory&) {
        attach_layout(s.x[0], s.x[2]);
        if (collection_of(*k, s.x[0])->loaded) layout_items(*k, s.x[0]);
    }, "v@:@");
    kit.method(cv, "backgroundColors", [k](CpuState& s, GuestMemory&) {
        const auto colors = collection_of(*k, s.x[0])->background_colors;
        s.x[0] = colors ? colors : k->auto_array({k->send(k->color, "controlBackgroundColor").x0});
    });
    kit.method(cv, "setBackgroundColors:", [k](CpuState& s, GuestMemory&) {
        const auto state = collection_of(*k, s.x[0]);
        k->release(std::exchange(state->background_colors, s.x[2] ? k->cf.copy(s.x[2]) : 0));
        k->invalidate_view(s.x[0]);
    }, "v@:@");
    const auto flag = [&kit, cv, k](const char* getter, const char* setter, bool CollectionState::*member) {
        kit.method(cv, getter, [k, member](CpuState& s, GuestMemory&) { return_bool(s, (*collection_of(*k, s.x[0])).*member); }, "B@:");
        kit.method(cv, setter, [k, member](CpuState& s, GuestMemory&) { (*collection_of(*k, s.x[0])).*member = bool_arg(s, 2); }, "v@:B");
    };
    flag("isSelectable", "setSelectable:", &CollectionState::selectable);
    flag("allowsMultipleSelection", "setAllowsMultipleSelection:", &CollectionState::multiple);
    flag("allowsEmptySelection", "setAllowsEmptySelection:", &CollectionState::empty_selection);
    // Registration.
    kit.method(cv, "registerClass:forItemWithIdentifier:", [k](CpuState& s, GuestMemory&) {
        const auto state = collection_of(*k, s.x[0]);
        const auto key = k->text(s.x[3]);
        if (const auto found = state->item_nibs.find(key); found != state->item_nibs.end()) { k->release(found->second); state->item_nibs.erase(found); }
        if (s.x[2]) state->item_classes[key] = s.x[2]; else state->item_classes.erase(key);
    }, "v@:#@");
    kit.method(cv, "registerNib:forItemWithIdentifier:", [k](CpuState& s, GuestMemory&) {
        const auto state = collection_of(*k, s.x[0]);
        const auto key = k->text(s.x[3]);
        state->item_classes.erase(key);
        if (const auto found = state->item_nibs.find(key); found != state->item_nibs.end()) { k->release(found->second); state->item_nibs.erase(found); }
        if (s.x[2]) state->item_nibs[key] = k->retain(s.x[2]);
    }, "v@:@@");
    kit.method(cv, "registerClass:forSupplementaryViewOfKind:withIdentifier:", [k](CpuState& s, GuestMemory&) {
        const auto state = collection_of(*k, s.x[0]);
        const auto key = k->text(s.x[3]) + "/" + k->text(s.x[4]);
        if (s.x[2]) state->supplementary_classes[key] = s.x[2]; else state->supplementary_classes.erase(key);
    }, "v@:#@@");
    kit.method(cv, "registerNib:forSupplementaryViewOfKind:withIdentifier:", [k](CpuState& s, GuestMemory&) {
        const auto state = collection_of(*k, s.x[0]);
        const auto key = k->text(s.x[3]) + "/" + k->text(s.x[4]);
        if (const auto found = state->supplementary_nibs.find(key); found != state->supplementary_nibs.end()) { k->release(found->second); state->supplementary_nibs.erase(found); }
        if (s.x[2]) state->supplementary_nibs[key] = k->retain(s.x[2]);
    }, "v@:@@@");
    // The first top-level object of a nib that is a `kind` (the load's reference is kept: +1).
    const auto from_nib = [k](Address nib_object, Address owner, Address kind, const std::string& what) {
        const auto slot = k->process.allocate_guest(8);
        Address objects{};
        try {
            k->memory().write64(slot, 0);
            static_cast<void>(k->send_id(nib_object, "instantiateWithOwner:topLevelObjects:", {owner, slot}));
            objects = k->memory().read64(slot);
        } catch (...) { k->process.release_guest(slot); throw; }
        k->process.release_guest(slot);
        Address made{};
        for (const auto object : objects ? k->cf.array_values(objects) : std::vector<Address>{}) {
            if (!made && object && k->is_kind(object, kind)) made = object;
            else k->release(object);
        }
        if (!made) fail("nib for " + what + " has no top-level " + k->text(k->send_id(kind, "description")));
        return made;
    };
    kit.method(cv, "makeItemWithIdentifier:forIndexPath:", [k, item_class, from_nib](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = collection_of(*k, self);
        const auto key = k->text(s.x[2]);
        Address made{};
        if (const auto found = state->item_classes.find(key); found != state->item_classes.end()) {
            made = k->alloc_init(found->second);
        } else if (const auto nib_found = state->item_nibs.find(key); nib_found != state->item_nibs.end()) {
            made = from_nib(nib_found->second, 0, item_class, "item '" + key + "'");
        } else {
            // AppKit's fallback: the nib named like the identifier in the main bundle.
            const auto nib_object = k->send_id(k->send(k->nib, "alloc").x0, "initWithNibNamed:bundle:", {s.x[2], 0});
            if (!nib_object) unsupported("-[NSCollectionView makeItemWithIdentifier:forIndexPath:] with the unregistered identifier '" + key + "'");
            try { made = from_nib(nib_object, 0, item_class, "item '" + key + "'"); } catch (...) { k->release(nib_object); throw; }
            k->release(nib_object);
        }
        item_state(made).collection = self;
        s.x[0] = k->autorelease(made);
    });
    kit.method(cv, "makeSupplementaryViewOfKind:withIdentifier:forIndexPath:", [k, from_nib](CpuState& s, GuestMemory&) {
        const auto state = collection_of(*k, s.x[0]);
        const auto key = k->text(s.x[2]) + "/" + k->text(s.x[3]);
        Address made{};
        if (const auto found = state->supplementary_classes.find(key); found != state->supplementary_classes.end()) {
            CallRegs registers;
            put_rect(registers, 0, {});
            made = k->send(k->send(found->second, "alloc").x0, "initWithFrame:", registers).x0;
        } else if (const auto nib_found = state->supplementary_nibs.find(key); nib_found != state->supplementary_nibs.end()) {
            made = from_nib(nib_found->second, 0, k->view, "supplementary view '" + key + "'");
        } else {
            unsupported("-[NSCollectionView makeSupplementaryViewOfKind:withIdentifier:forIndexPath:] with the unregistered '" + key + "'");
        }
        s.x[0] = k->autorelease(made);
    });
    // Data.
    kit.method(cv, "reloadData", [k](CpuState& s, GuestMemory&) { reload(*k, s.x[0]); }, "v@:");
    for (const auto* name : {"reloadItemsAtIndexPaths:", "insertItemsAtIndexPaths:", "deleteItemsAtIndexPaths:", "reloadSections:",
             "insertSections:", "deleteSections:"})
        kit.method(kit.objc.find_class("NSCollectionView"), name, [k](CpuState& s, GuestMemory&) { reload(*k, s.x[0]); }, "v@:@");
    kit.method(cv, "performBatchUpdates:completionHandler:", [k](CpuState& s, GuestMemory&) {
        if (s.x[2]) static_cast<void>(k->invoke_block(s.x[2], {}));
        reload(*k, s.x[0]);
        if (s.x[3]) static_cast<void>(k->invoke_block(s.x[3], {1}));
    }, "v@:@?@?");
    kit.method(cv, "numberOfSections", [k](CpuState& s, GuestMemory&) {
        const auto state = collection_of(*k, s.x[0]);
        if (!state->loaded) reload(*k, s.x[0]);
        s.x[0] = 1;
    }, "q@:");
    kit.method(cv, "numberOfItemsInSection:", [k](CpuState& s, GuestMemory&) {
        const auto state = collection_of(*k, s.x[0]);
        if (s.x[2] != 0) unsupported("NSCollectionView sections other than 0");
        if (!state->loaded) reload(*k, s.x[0]);
        s.x[0] = static_cast<Address>(state->count);
    }, "q@:q");
    kit.method(cv, "itemAtIndexPath:", [k](CpuState& s, GuestMemory&) {
        const auto state = collection_of(*k, s.x[0]);
        const auto item = item_of(*k, s.x[2]);
        s.x[0] = item >= 0 && item < static_cast<std::int64_t>(state->items.size()) ? state->items[static_cast<std::size_t>(item)] : 0;
    });
    kit.method(cv, "itemAtIndex:", [k](CpuState& s, GuestMemory&) {
        const auto state = collection_of(*k, s.x[0]);
        s.x[0] = s.x[2] < state->items.size() ? state->items[s.x[2]] : 0;
    }, "@@:Q");
    kit.method(cv, "indexPathForItem:", [k](CpuState& s, GuestMemory&) {
        const auto& list = collection_of(*k, s.x[0])->items;
        const auto found = std::find(list.begin(), list.end(), s.x[2]);
        s.x[0] = found == list.end() ? 0 : index_path(*k, found - list.begin());
    });
    kit.method(cv, "visibleItems", [k](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array(collection_of(*k, s.x[0])->items); });
    kit.method(cv, "indexPathsForVisibleItems", [k](CpuState& s, GuestMemory&) {
        std::set<std::int64_t> all;
        for (std::int64_t i = 0; i < collection_of(*k, s.x[0])->count; ++i) all.insert(i);
        s.x[0] = path_set(*k, all);
    });
    kit.method(cv, "frameForItemAtIndex:", [k](CpuState& s, GuestMemory&) {
        const auto state = collection_of(*k, s.x[0]);
        if (s.x[2] >= state->items.size()) { return_rect(s, {}); return; }
        const auto view = k->send_id(state->items[s.x[2]], "view");
        return_rect(s, view ? k->view_state(view)->frame : Rect{});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:Q");
    kit.method(cv, "indexPathForItemAtPoint:", [k](CpuState& s, GuestMemory&) {
        const auto state = collection_of(*k, s.x[0]);
        const auto point = point_arg(s);
        for (std::size_t i = 0; i < state->items.size(); ++i) {
            const auto view = k->send_id(state->items[i], "view");
            if (view && !k->view_state(view)->hidden && rect_contains(k->view_state(view)->frame, point)) { s.x[0] = index_path(*k, static_cast<std::int64_t>(i)); return; }
        }
        s.x[0] = 0;
    }, "@@:{CGPoint=dd}");
    // Selection.
    kit.method(cv, "selectionIndexPaths", [k](CpuState& s, GuestMemory&) { s.x[0] = path_set(*k, collection_of(*k, s.x[0])->selection); });
    kit.method(cv, "setSelectionIndexPaths:", [k](CpuState& s, GuestMemory&) { change_selection(*k, s.x[0], items_of_set(*k, s.x[2]), false); }, "v@:@");
    kit.method(cv, "selectionIndexes", [k](CpuState& s, GuestMemory&) {
        const auto made = k->send(k->send(k->objc.find_class("NSMutableIndexSet"), "alloc").x0, "init").x0;
        for (const auto item : collection_of(*k, s.x[0])->selection) static_cast<void>(k->send_id(made, "addIndex:", {static_cast<Address>(item)}));
        s.x[0] = k->autorelease(made);
    });
    kit.method(cv, "setSelectionIndexes:", [k](CpuState& s, GuestMemory&) {
        std::set<std::int64_t> wanted;
        if (s.x[2])
            for (auto index = k->send(s.x[2], "firstIndex").x0; index != static_cast<Address>(not_found); index = k->send_id(s.x[2], "indexGreaterThanIndex:", {index}))
                wanted.insert(static_cast<std::int64_t>(index));
        change_selection(*k, s.x[0], wanted, false);
    }, "v@:@");
    kit.method(cv, "selectItemsAtIndexPaths:scrollPosition:", [k](CpuState& s, GuestMemory&) {
        auto wanted = collection_of(*k, s.x[0])->selection;
        for (const auto item : items_of_set(*k, s.x[2])) wanted.insert(item);
        change_selection(*k, s.x[0], wanted, false);
        if (s.x[3] != 0 && !wanted.empty()) static_cast<void>(k->send_id(s.x[0], "scrollToItemsAtIndexPaths:scrollPosition:", {s.x[2], s.x[3]}));
    }, "v@:@Q");
    kit.method(cv, "deselectItemsAtIndexPaths:", [k](CpuState& s, GuestMemory&) {
        auto wanted = collection_of(*k, s.x[0])->selection;
        for (const auto item : items_of_set(*k, s.x[2])) wanted.erase(item);
        change_selection(*k, s.x[0], wanted, false);
    }, "v@:@");
    kit.method(cv, "selectAll:", [k](CpuState& s, GuestMemory&) {
        const auto state = collection_of(*k, s.x[0]);
        if (!state->selectable || !state->multiple) return;
        std::set<std::int64_t> all;
        for (std::int64_t i = 0; i < state->count; ++i) all.insert(i);
        change_selection(*k, s.x[0], all, true);
    }, "v@:@");
    kit.method(cv, "deselectAll:", [k](CpuState& s, GuestMemory&) { change_selection(*k, s.x[0], {}, true); }, "v@:@");
    kit.method(cv, "scrollToItemsAtIndexPaths:scrollPosition:", [k](CpuState& s, GuestMemory&) {
        const auto state = collection_of(*k, s.x[0]);
        Rect target{};
        bool any = false;
        for (const auto item : items_of_set(*k, s.x[2])) {
            if (item < 0 || item >= static_cast<std::int64_t>(state->items.size())) continue;
            const auto view = k->send_id(state->items[static_cast<std::size_t>(item)], "view");
            if (!view) continue;
            target = any ? drawing::unite(target, k->view_state(view)->frame) : k->view_state(view)->frame;
            any = true;
        }
        if (!any) return;
        CallRegs registers;
        put_rect(registers, 0, target);
        static_cast<void>(k->send(s.x[0], "scrollRectToVisible:", registers));
    }, "v@:@Q");
    // Layout follows the view's width.
    kit.method(cv, "setFrameSize:", [k, cv](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto before = k->view_state(self)->frame.size.width;
        CallRegs registers;
        put_size(registers, 0, size_arg(s));
        static_cast<void>(k->objc.send_super(self, cv, k->sel("setFrameSize:"), registers));
        const auto state = collection_of(*k, self);
        if (state->loaded && !state->laying_out && k->view_state(self)->frame.size.width != before) layout_items(*k, self);
    }, "v@:{CGSize=dd}");
    kit.method(cv, "viewDidMoveToSuperview", [k](CpuState& s, GuestMemory&) {
        const auto state = collection_of(*k, s.x[0]);
        if (state->loaded) layout_items(*k, s.x[0]);
    }, "v@:");
    kit.method(cv, "acceptsFirstResponder", [k](CpuState& s, GuestMemory&) { return_bool(s, collection_of(*k, s.x[0])->selectable); }, "B@:");
    kit.method(cv, "drawRect:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto colors = k->send_id(self, "backgroundColors");
        const auto list = colors ? k->items(colors) : std::vector<Address>{};
        if (list.empty() || !list.front()) return;
        k->require_canvas("NSCollectionView drawing").fill_rect(rect_arg(s), k->color_value(list.front(), k->theme_for(self).control_background));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    // Clicks select items (Command toggles, Shift extends with multiple selection).
    kit.method(cv, "mouseDown:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = collection_of(*k, self);
        if (const auto owner = k->view_state(self)->window; owner && state->selectable)
            static_cast<void>(k->send_id(owner, "makeFirstResponder:", {self}));
        if (!state->selectable) return;
        const auto point = event_location_in(*k, self, s.x[2]);
        CallRegs at;
        put_point(at, 0, point);
        const auto path = k->send(self, "indexPathForItemAtPoint:", at).x0;
        const auto item = path ? item_of(*k, path) : -1;
        const auto flags = k->event_state(s.x[2])->event.modifier_flags;
        auto wanted = state->selection;
        if (item < 0) {
            if (state->empty_selection) wanted.clear();
        } else if (state->multiple && (flags & modifier::command)) {
            if (!wanted.erase(item)) wanted.insert(item);
        } else if (state->multiple && (flags & modifier::shift) && !wanted.empty()) {
            const auto anchor = *wanted.begin();
            for (auto i = std::min(anchor, item); i <= std::max(anchor, item); ++i) wanted.insert(i);
        } else {
            wanted = {item};
        }
        if (wanted.empty() && !state->empty_selection) return;
        change_selection(*k, self, wanted, true);
    }, "v@:@");
    kit.method(cv, "keyDown:", [k](CpuState& s, GuestMemory&) { static_cast<void>(k->send_id(s.x[0], "interpretKeyEvents:", {k->auto_array({s.x[2]})})); }, "v@:@");
    for (const auto& [name, step] : std::initializer_list<std::pair<const char*, int>>{{"moveLeft:", -1}, {"moveRight:", 1}, {"moveUp:", -2}, {"moveDown:", 2}}) {
        const auto delta = step;
        kit.method(cv, name, [k, delta](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            const auto state = collection_of(*k, self);
            if (!state->selectable || !state->count) return;
            std::int64_t columns = 1;
            if (state->layout) if (const auto info = k->objc.find_host_state<LayoutState>(state->layout); info && info->flow_layout) columns = static_cast<std::int64_t>(std::max<std::size_t>(1, info->result.columns));
            const auto move = delta == -1 ? -1 : delta == 1 ? 1 : delta == -2 ? -columns : columns;
            const auto current = state->selection.empty() ? (move > 0 ? -1 : state->count) : (move > 0 ? *state->selection.rbegin() : *state->selection.begin());
            const auto next = std::clamp<std::int64_t>(current + move, 0, state->count - 1);
            change_selection(*k, self, {next}, true);
            static_cast<void>(k->send_id(self, "scrollToItemsAtIndexPaths:scrollPosition:", {path_set(*k, {next}), 0}));
        }, "v@:@");
    }
}

} // namespace alchemy::gui::completion_kit::controls
