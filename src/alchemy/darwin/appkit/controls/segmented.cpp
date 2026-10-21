#include "alchemy/darwin/appkit/controls/g3.hpp"
#include "alchemy/darwin/appkit/controls/layout.hpp"

#include <algorithm>
#include <cmath>

// NSSegmentedControl / NSSegmentedCell / NSSegmentItem (Workspace.nib's view
// switcher, the fade and video inspectors): segments with labels, images,
// tags, fixed or distributed widths, per-segment enabling, menus and tool
// tips; the select-one, select-any and momentary tracking modes; drawing on
// AppKit's software layer (a rounded bezel, the selected segments in the
// accent color, separators); nib decoding of the cell and its NSSegmentItem
// archive objects. The control forwards its API to the cell, as NSControl
// does. Segments are laid out by layout.cpp (segment_spans).
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;
using drawing::Color;

// NSSegmentSwitchTracking.
namespace tracking {
inline constexpr std::int64_t select_one = 0, select_any = 1, momentary = 2, momentary_accelerator = 3;
}
// NSSegmentStyle.
namespace segment_style {
inline constexpr std::int64_t automatic = 0, rounded = 1, textured_rounded = 2, round_rect = 3, textured_square = 4,
    capsule = 5, small_square = 6, separated = 8;
}

struct Segment {
    Address label{}, image{}, menu{}, tool_tip{};   // +1
    double width{};                                 // 0: sized by the distribution
    std::int64_t tag{};
    std::int64_t image_scaling{};                   // NSImageScaleProportionallyDown
    std::int64_t alignment{1};                      // NSTextAlignmentCenter (arm64 values: left 0, center 1, right 2)
    bool selected{}, enabled{true}, menu_indicator{};
};
struct SegmentedState final : ObjectPayload {
    std::vector<Segment> segments;
    std::int64_t tracking{tracking::select_one}, style{segment_style::automatic};
    std::int64_t distribution{layout::distribution::fill};
    std::int64_t selected{-1};                      // the last segment selected (-1: none)
    std::int64_t pressed{-1};                       // the segment drawn pressed while tracking
    Address bezel_color{};                          // +1 -selectedSegmentBezelColor
};
// NSSegmentItem: one archived segment (nib-only private class).
struct SegmentItemState final : ObjectPayload { Segment segment; };

constexpr const char* rect_type = "{CGRect={CGPoint=dd}{CGSize=dd}}";

void retain_segment(AppKit& kit, Segment& segment) {
    for (auto* slot : {&segment.label, &segment.image, &segment.menu, &segment.tool_tip}) *slot = kit.retain(*slot);
}
void release_segment(AppKit& kit, Segment& segment) {
    for (auto* slot : {&segment.label, &segment.image, &segment.menu, &segment.tool_tip}) kit.release(std::exchange(*slot, 0));
}

std::shared_ptr<SegmentedState> segmented(AppKit& kit, Address cell) { return cell_extension<SegmentedState>(kit, cell); }

// The content width of a segment (label or image plus AppKit's padding).
double content_width(AppKit& kit, Address cell, const Segment& segment) {
    const auto state = cell_state(kit, cell);
    const auto style = cell_text_style(kit, cell, theme(false));
    const double padding = state->control_size == 0 ? 10 : state->control_size == 1 ? 8 : 6;
    double width = 0;
    if (segment.image) width += result_size(kit.send(segment.image, "size")).width;
    if (segment.label) {
        const auto text = kit.string16(segment.label);
        if (!text.empty()) width += (width > 0 ? 4 : 0) + std::ceil(drawing::text_width(style.font, text));
    }
    if (segment.menu && segment.menu_indicator) width += 10;
    return std::max(width + 2 * padding, 20.0);
}

std::vector<layout::Span> spans_for(AppKit& kit, Address cell, Rect frame) {
    const auto state = segmented(kit, cell);
    std::vector<layout::SegmentSize> sizes;
    for (const auto& segment : state->segments) sizes.push_back({segment.width, content_width(kit, cell, segment)});
    return layout::segment_spans(sizes, frame.origin.x, frame.size.width, state->distribution);
}

// The bezel of the control within its frame (AppKit's heights per control size).
Rect bezel_rect(AppKit& kit, Address cell, Rect frame) {
    const auto size = cell_state(kit, cell)->control_size;
    const double height = std::min(frame.size.height, size == 0 ? 24.0 : size == 1 ? 20.0 : 16.0);
    return {{frame.origin.x, frame.origin.y + std::floor((frame.size.height - height) / 2)}, {frame.size.width, height}};
}
double corner_radius(std::int64_t style, double height) {
    switch (style) {
    case segment_style::capsule: return height / 2;
    case segment_style::small_square: return 0;
    case segment_style::textured_square: return 3;
    default: return std::min(5.0, height / 2);
    }
}

bool momentary(std::int64_t mode) { return mode == tracking::momentary || mode == tracking::momentary_accelerator; }

void set_selected_segment(AppKit& kit, Address cell, std::int64_t index) {
    const auto state = segmented(kit, cell);
    const auto count = static_cast<std::int64_t>(state->segments.size());
    if (index >= count) unsupported("-[NSSegmentedCell setSelectedSegment:] index " + std::to_string(index) + " beyond the " +
                                    std::to_string(count) + " segments (NSRangeException)");
    if (index < 0) {
        for (auto& segment : state->segments) segment.selected = false;
        state->selected = -1;
    } else if (state->tracking == tracking::select_one) {
        for (std::int64_t i = 0; i < count; ++i) state->segments[static_cast<std::size_t>(i)].selected = i == index;
        state->selected = index;
    } else if (state->tracking == tracking::select_any) {
        state->segments[static_cast<std::size_t>(index)].selected = true;
        state->selected = index;
    } else {
        state->selected = index;   // momentary: selected while its action runs
    }
    if (const auto view = cell_state(kit, cell)->control_view) kit.invalidate_view(view);
}

Segment& segment_at_index(AppKit& kit, Address cell, std::uint64_t index, std::string_view selector) {
    const auto state = segmented(kit, cell);
    if (index >= state->segments.size())
        unsupported(std::string("-[NSSegmentedCell ") + std::string(selector) + "] segment " + std::to_string(index) + " beyond the " +
                    std::to_string(state->segments.size()) + " segments (NSRangeException)");
    return state->segments[index];
}

} // namespace

void install_segmented(AppKit& kit) {
    auto* const k = &kit;
    const auto cell_class = kit.declare_class("NSSegmentedCell", "NSActionCell");
    const auto control_class = kit.declare_class("NSSegmentedControl", "NSControl");
    const auto item_class = kit.declare_class("NSSegmentItem", "NSObject");
    static_cast<void>(kit.send_id(control_class, "setCellClass:", {cell_class}));
    kit.string_constant("_NSSegmentedControlSegmentLabelKey", "NSSegmentedControlSegmentLabelKey");

    const auto redisplay = [k](Address cell) {
        if (const auto view = cell_state(*k, cell)->control_view) k->invalidate_view(view);
    };

    // ---- NSSegmentedCell ----
    const auto c = cell_class;
    kit.method(c, "initTextCell:", [k, c](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto self = k->objc.send_super(s.x[0], c, k->sel("initTextCell:"), registers).x0;
        s.x[0] = self;
        if (!self) return;
        const auto state = cell_state(*k, self);
        state->bordered = state->bezeled = true;
        state->alignment = 1;
        static_cast<void>(segmented(*k, self));
    });
    kit.method(c, "initWithCoder:", [k, c](CpuState& s, GuestMemory&) {
        const auto self = super_init_with_coder(*k, s.x[0], c, s.x[2]);
        s.x[0] = self;
        if (!self) return;
        const Decoder in{*k, s.x[2]};
        const auto state = segmented(*k, self);
        for (auto& segment : state->segments) release_segment(*k, segment);
        state->segments.clear();
        if (const auto items = in.object("NSSegmentImages"))
            for (const auto item : k->items(items)) {
                const auto archived = k->objc.find_host_state<SegmentItemState>(item);
                if (!archived) fail("NSSegmentImages holds a " + k->text(k->send_id(k->send_id(item, "class"), "description")) +
                                    ", not an NSSegmentItem");
                auto segment = archived->segment;
                retain_segment(*k, segment);
                state->segments.push_back(segment);
            }
        state->style = in.integer("NSSegmentStyle", segment_style::automatic);
        // Archives without a distribution predate it: their segments fit.
        state->distribution = in.integer("NSSegmentDistribution", layout::distribution::fit);
        state->tracking = in.integer("NSTrackingMode", tracking::select_one);
        state->selected = -1;
        for (std::size_t i = 0; i < state->segments.size(); ++i)
            if (state->segments[i].selected) state->selected = static_cast<std::int64_t>(i);
        if (in.has("NSSelectedSegment")) {
            const auto index = in.integer("NSSelectedSegment", -1);
            if (index >= 0 && index < static_cast<std::int64_t>(state->segments.size())) set_selected_segment(*k, self, index);
        }
        if (const auto color = in.object("NSSelectedSegmentBezelColor")) k->assign(state->bezel_color, color);
    });
    kit.method(c, "copyWithZone:", [k, c](CpuState& s, GuestMemory&) {
        const auto from = segmented(*k, s.x[0]);
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto copy = k->objc.send_super(s.x[0], c, k->sel("copyWithZone:"), registers).x0;
        auto to = std::make_shared<SegmentedState>(*from);
        for (auto& segment : to->segments) retain_segment(*k, segment);
        to->bezel_color = k->retain(to->bezel_color);
        to->pressed = -1;
        cell_state(*k, copy)->extension = to;
        s.x[0] = copy;
    }, "@@:^v");
    kit.method(c, "dealloc", [k, c](CpuState& s, GuestMemory&) {
        if (const auto cell = k->objc.find_host_state<CellState>(s.x[0]))
            if (const auto state = std::dynamic_pointer_cast<SegmentedState>(cell->extension)) {
                for (auto& segment : state->segments) release_segment(*k, segment);
                state->segments.clear();
                k->release(std::exchange(state->bezel_color, 0));
            }
        static_cast<void>(k->objc.send_super(s.x[0], c, k->sel("dealloc")));
    }, "v@:");

    kit.method(c, "segmentCount", [k](CpuState& s, GuestMemory&) { s.x[0] = segmented(*k, s.x[0])->segments.size(); }, "q@:");
    kit.method(c, "setSegmentCount:", [k, redisplay](CpuState& s, GuestMemory&) {
        const auto state = segmented(*k, s.x[0]);
        const auto count = static_cast<std::int64_t>(s.x[2]);
        if (count < 0) unsupported("-[NSSegmentedCell setSegmentCount:] with a negative count");
        while (static_cast<std::int64_t>(state->segments.size()) > count) {
            release_segment(*k, state->segments.back());
            state->segments.pop_back();
        }
        state->segments.resize(static_cast<std::size_t>(count));
        if (state->selected >= count) state->selected = -1;
        redisplay(s.x[0]);
    }, "v@:q");
    kit.method(c, "selectedSegment", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(segmented(*k, s.x[0])->selected); }, "q@:");
    kit.method(c, "setSelectedSegment:", [k](CpuState& s, GuestMemory&) {
        set_selected_segment(*k, s.x[0], static_cast<std::int64_t>(s.x[2]));
    }, "v@:q");
    kit.method(c, "selectSegmentWithTag:", [k](CpuState& s, GuestMemory&) {
        const auto state = segmented(*k, s.x[0]);
        for (std::size_t i = 0; i < state->segments.size(); ++i)
            if (state->segments[i].tag == static_cast<std::int64_t>(s.x[2])) {
                set_selected_segment(*k, s.x[0], static_cast<std::int64_t>(i));
                s.x[0] = 1;
                return;
            }
        s.x[0] = 0;
    }, "B@:q");
    kit.method(c, "makeNextSegmentKey", [k](CpuState& s, GuestMemory&) {
        const auto state = segmented(*k, s.x[0]);
        if (!state->segments.empty()) set_selected_segment(*k, s.x[0], (state->selected + 1) % static_cast<std::int64_t>(state->segments.size()));
    }, "v@:");
    kit.method(c, "makePreviousSegmentKey", [k](CpuState& s, GuestMemory&) {
        const auto state = segmented(*k, s.x[0]);
        const auto count = static_cast<std::int64_t>(state->segments.size());
        if (count) set_selected_segment(*k, s.x[0], (state->selected - 1 + count) % count);
    }, "v@:");
    kit.method(c, "setSelected:forSegment:", [k, redisplay](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const bool on = bool_arg(s, 2);
        const auto index = static_cast<std::int64_t>(s.x[3]);
        const auto state = segmented(*k, self);
        auto& segment = segment_at_index(*k, self, s.x[3], "setSelected:forSegment:");
        if (on) {
            set_selected_segment(*k, self, index);
            if (state->tracking == tracking::select_any) segment.selected = true;
        } else {
            segment.selected = false;
            if (state->selected == index) state->selected = -1;
            if (state->tracking == tracking::select_any)   // the last selected one that remains
                for (std::size_t i = 0; i < state->segments.size(); ++i) if (state->segments[i].selected) state->selected = static_cast<std::int64_t>(i);
        }
        redisplay(self);
    }, "v@:Bq");
    kit.method(c, "isSelectedForSegment:", [k](CpuState& s, GuestMemory&) {
        const auto state = segmented(*k, s.x[0]);
        const auto& segment = segment_at_index(*k, s.x[0], s.x[2], "isSelectedForSegment:");
        return_bool(s, momentary(state->tracking) ? state->selected == static_cast<std::int64_t>(s.x[2]) : segment.selected);
    }, "B@:q");
    // Per-segment objects.
    const auto object_property = [&kit, c, k, redisplay](const char* getter, const char* setter, Address Segment::*member, bool copy) {
        kit.method(c, getter, [k, member, getter](CpuState& s, GuestMemory&) {
            s.x[0] = segment_at_index(*k, s.x[0], s.x[2], getter).*member;
        }, "@@:q");
        kit.method(c, setter, [k, member, copy, setter, redisplay](CpuState& s, GuestMemory&) {
            auto& slot = segment_at_index(*k, s.x[0], s.x[3], setter).*member;
            if (copy) k->release(std::exchange(slot, s.x[2] ? k->cf.copy(s.x[2]) : 0));
            else k->assign(slot, s.x[2]);
            redisplay(s.x[0]);
        }, "v@:@q");
    };
    object_property("labelForSegment:", "setLabel:forSegment:", &Segment::label, true);
    object_property("imageForSegment:", "setImage:forSegment:", &Segment::image, false);
    object_property("menuForSegment:", "setMenu:forSegment:", &Segment::menu, false);
    object_property("toolTipForSegment:", "setToolTip:forSegment:", &Segment::tool_tip, true);
    kit.method(c, "widthForSegment:", [k](CpuState& s, GuestMemory&) {
        set_d(s, 0, segment_at_index(*k, s.x[0], s.x[2], "widthForSegment:").width);
    }, "d@:q");
    kit.method(c, "setWidth:forSegment:", [k, redisplay](CpuState& s, GuestMemory&) {
        segment_at_index(*k, s.x[0], s.x[2], "setWidth:forSegment:").width = std::max(0.0, d(s, 0));
        redisplay(s.x[0]);
    }, "v@:dq");
    const auto integer_property = [&kit, c, k, redisplay](const char* getter, const char* setter, std::int64_t Segment::*member) {
        kit.method(c, getter, [k, member, getter](CpuState& s, GuestMemory&) {
            s.x[0] = static_cast<Address>(segment_at_index(*k, s.x[0], s.x[2], getter).*member);
        }, "q@:q");
        kit.method(c, setter, [k, member, setter, redisplay](CpuState& s, GuestMemory&) {
            segment_at_index(*k, s.x[0], s.x[3], setter).*member = static_cast<std::int64_t>(s.x[2]);
            redisplay(s.x[0]);
        }, "v@:qq");
    };
    integer_property("tagForSegment:", "setTag:forSegment:", &Segment::tag);
    integer_property("imageScalingForSegment:", "setImageScaling:forSegment:", &Segment::image_scaling);
    integer_property("alignmentForSegment:", "setAlignment:forSegment:", &Segment::alignment);
    const auto bool_property = [&kit, c, k, redisplay](const char* getter, const char* setter, bool Segment::*member) {
        kit.method(c, getter, [k, member, getter](CpuState& s, GuestMemory&) {
            return_bool(s, segment_at_index(*k, s.x[0], s.x[2], getter).*member);
        }, "B@:q");
        kit.method(c, setter, [k, member, setter, redisplay](CpuState& s, GuestMemory&) {
            segment_at_index(*k, s.x[0], s.x[3], setter).*member = bool_arg(s, 2);
            redisplay(s.x[0]);
        }, "v@:Bq");
    };
    bool_property("isEnabledForSegment:", "setEnabled:forSegment:", &Segment::enabled);
    bool_property("showsMenuIndicatorForSegment:", "setShowsMenuIndicator:forSegment:", &Segment::menu_indicator);
    // Control-wide properties.
    const auto mode_property = [&kit, c, k, redisplay](const char* getter, const char* setter, std::int64_t SegmentedState::*member) {
        kit.method(c, getter, [k, member](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>((*segmented(*k, s.x[0])).*member); }, "Q@:");
        kit.method(c, setter, [k, member, redisplay](CpuState& s, GuestMemory&) {
            (*segmented(*k, s.x[0])).*member = static_cast<std::int64_t>(s.x[2]);
            redisplay(s.x[0]);
        }, "v@:Q");
    };
    mode_property("trackingMode", "setTrackingMode:", &SegmentedState::tracking);
    mode_property("segmentStyle", "setSegmentStyle:", &SegmentedState::style);
    mode_property("segmentDistribution", "setSegmentDistribution:", &SegmentedState::distribution);
    kit.method(c, "selectedSegmentBezelColor", [k](CpuState& s, GuestMemory&) { s.x[0] = segmented(*k, s.x[0])->bezel_color; });
    kit.method(c, "setSelectedSegmentBezelColor:", [k, redisplay](CpuState& s, GuestMemory&) {
        k->assign(segmented(*k, s.x[0])->bezel_color, s.x[2]);
        redisplay(s.x[0]);
    }, "v@:@");
    // Geometry.
    kit.method(c, "cellSizeForBounds:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = segmented(*k, self);
        std::vector<layout::SegmentSize> sizes;
        for (const auto& segment : state->segments) sizes.push_back({segment.width, content_width(*k, self, segment)});
        const auto control_size = cell_state(*k, self)->control_size;
        const Size size{layout::segments_fitting_width(sizes, state->distribution) + 2,
                        control_size == 0 ? 24.0 : control_size == 1 ? 20.0 : 16.0};
        const auto bounds = rect_arg(s);
        return_size(s, {std::min(size.width, bounds.size.width), std::min(size.height, bounds.size.height)});
    }, "{CGSize=dd}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(c, "_alchemySegmentAtPoint:inFrame:", [k](CpuState& s, GuestMemory&) {
        const auto point = point_arg(s, 0);
        const Rect frame{{d(s, 2), d(s, 3)}, {d(s, 4), d(s, 5)}};
        const auto spans = spans_for(*k, s.x[0], frame);
        s.x[0] = static_cast<Address>(rect_contains(frame, point) ? layout::segment_at(spans, point.x) : -1);
    }, "q@:{CGPoint=dd}{CGRect={CGPoint=dd}{CGSize=dd}}");
    // Drawing.
    kit.method(c, "drawWithFrame:inView:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[2];
        const auto cell = cell_state(*k, self);
        const auto state = segmented(*k, self);
        const auto& th = k->theme_for(owner);
        auto& canvas = k->require_canvas("NSSegmentedCell drawing");
        const auto frame = rect_arg(s);
        const auto bezel = bezel_rect(*k, self, frame);
        const auto radius = corner_radius(state->style, bezel.size.height);
        const auto spans = spans_for(*k, self, bezel);
        const bool separated = state->style == segment_style::separated;
        const auto face = cell->enabled ? th.control_face : th.control_face.with_alpha(0.5);
        if (!separated) draw_bezel(*k, bezel, radius, face, th.control_border);
        const auto accent = state->bezel_color ? k->color_value(state->bezel_color, th.accent) : th.accent;
        for (std::size_t i = 0; i < state->segments.size(); ++i) {
            const auto& segment = state->segments[i];
            const Rect area{{spans[i].x, bezel.origin.y}, {spans[i].width, bezel.size.height}};
            if (area.size.width <= 0) continue;
            if (separated) {
                const Rect own{{area.origin.x + 1, area.origin.y}, {std::max(0.0, area.size.width - 2), area.size.height}};
                draw_bezel(*k, own, radius, face, th.control_border);
            }
            const bool lit = !momentary(state->tracking) && segment.selected;
            const bool pressed = state->pressed == static_cast<std::int64_t>(i);
            if (lit || pressed) {
                auto fill = lit ? accent : th.control_face_pressed;
                if (lit && pressed) fill = {fill.r * 0.85, fill.g * 0.85, fill.b * 0.85, fill.a};
                if (!cell->enabled || !segment.enabled) fill = fill.with_alpha(0.5);
                canvas.save();
                canvas.clip_to_rect(area);
                drawing::Path path;
                const Rect inner{{bezel.origin.x + 1, bezel.origin.y + 1}, {std::max(0.0, bezel.size.width - 2), std::max(0.0, bezel.size.height - 2)}};
                path.add_rounded_rect(separated ? Rect{{area.origin.x + 2, inner.origin.y}, {std::max(0.0, area.size.width - 4), inner.size.height}} : inner,
                                      std::max(0.0, radius - 1), std::max(0.0, radius - 1));
                canvas.fill_path(path, fill);
                canvas.restore();
            }
            // Separators between unselected neighbours.
            if (!separated && i > 0) {
                const bool beside_lit = lit || (!momentary(state->tracking) && state->segments[i - 1].selected);
                if (!beside_lit) canvas.fill_rect({{area.origin.x, bezel.origin.y + 4}, {1, std::max(0.0, bezel.size.height - 8)}}, th.separator);
            }
            CallRegs registers;
            registers.x[2] = i;
            put_rect(registers, 0, area);
            registers.x[3] = owner;
            static_cast<void>(k->send(self, "drawSegment:inFrame:withView:", registers));
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    kit.method(c, "drawSegment:inFrame:withView:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[3];
        const auto cell = cell_state(*k, self);
        const auto state = segmented(*k, self);
        if (s.x[2] >= state->segments.size()) return;
        const auto& segment = state->segments[s.x[2]];
        const auto& th = k->theme_for(owner);
        const auto area = rect_arg(s);
        auto style = cell_text_style(*k, self, th);
        const bool lit = !momentary(state->tracking) && segment.selected;
        style.color = !cell->enabled || !segment.enabled ? th.disabled_text : lit ? th.accent_text : th.text;
        const auto text = segment.label ? k->string16(segment.label) : std::u16string{};
        Size picture{};
        if (segment.image) picture = result_size(k->send(segment.image, "size"));
        const double gap = picture.width > 0 && !text.empty() ? 4 : 0;
        const double room = std::max(0.0, area.size.width - 8);
        const auto shown = truncate_text(style.font, text, std::max(0.0, room - picture.width - gap));
        const double content = picture.width + gap + drawing::text_width(style.font, shown);
        double x = area.origin.x + 4;
        if (segment.alignment == 1 /* center */ || segment.alignment == 4 /* natural */) x += std::max(0.0, (room - content) / 2);
        else if (segment.alignment == 2 /* right */) x += std::max(0.0, room - content);
        if (segment.image) {
            const double height = std::min(picture.height, area.size.height - 4);
            const double scale = picture.height > 0 ? height / picture.height : 1;
            const Rect target{{x, area.origin.y + (area.size.height - height) / 2}, {picture.width * scale, height}};
            k->draw_image_in(segment.image, target, cell->enabled && segment.enabled ? 1 : 0.5, true);
            x += picture.width * scale + gap;
        }
        if (!shown.empty()) draw_line_in(*k, shown, style, {{x, area.origin.y}, {area.origin.x + area.size.width - x, area.size.height}}, 0, false);
        if (segment.menu && segment.menu_indicator) {
            // A small downward triangle at the trailing edge.
            const bool flipped = k->current_flipped();
            const double cx = area.origin.x + area.size.width - 9, cy = area.origin.y + area.size.height / 2;
            drawing::Path arrow;
            arrow.move_to({cx - 3, flipped ? cy - 1.5 : cy + 1.5});
            arrow.line_to({cx + 3, flipped ? cy - 1.5 : cy + 1.5});
            arrow.line_to({cx, flipped ? cy + 2 : cy - 2});
            arrow.close();
            k->require_canvas("NSSegmentedCell menu indicator").fill_path(arrow, style.color);
        }
    }, "v@:q{CGRect={CGPoint=dd}{CGSize=dd}}@");

    // ---- NSSegmentItem (archived segments) ----
    kit.method(item_class, "initWithCoder:", [k](CpuState& s, GuestMemory&) {
        const auto self = k->send(s.x[0], "init").x0;
        s.x[0] = self;
        const Decoder in{*k, s.x[2]};
        auto& segment = k->objc.host_state<SegmentItemState>(self)->segment;
        if (const auto label = in.object("NSSegmentItemLabel")) segment.label = k->cf.copy(label);
        if (const auto image = in.object("NSSegmentItemImage")) segment.image = k->retain(image);
        if (const auto menu = in.object("NSSegmentItemMenu")) segment.menu = k->retain(menu);
        if (const auto tip = in.object("NSSegmentItemTooltip")) segment.tool_tip = k->cf.copy(tip);
        segment.width = in.real("NSSegmentItemWidth", 0);
        segment.tag = in.integer("NSSegmentItemTag", 0);
        segment.selected = in.flag("NSSegmentItemSelected");
        segment.enabled = !in.flag("NSSegmentItemDisabled");
        segment.image_scaling = in.integer("NSSegmentItemImageScaling", 0);
        segment.menu_indicator = in.flag("NSSegmentItemShowsMenuIndicator");
    });
    kit.method(item_class, "dealloc", [k, item_class](CpuState& s, GuestMemory&) {
        if (const auto state = k->objc.find_host_state<SegmentItemState>(s.x[0])) release_segment(*k, state->segment);
        static_cast<void>(k->objc.send_super(s.x[0], item_class, k->sel("dealloc")));
    }, "v@:");

    // ---- NSSegmentedControl ----
    const auto n = control_class;
    for (const auto& [name, types, redraw] : std::initializer_list<std::tuple<const char*, const char*, bool>>{
             {"segmentCount", "q@:", false}, {"setSegmentCount:", "v@:q", true}, {"selectedSegment", "q@:", false},
             {"setSelectedSegment:", "v@:q", true}, {"selectSegmentWithTag:", "B@:q", true}, {"setSelected:forSegment:", "v@:Bq", true},
             {"isSelectedForSegment:", "B@:q", false}, {"labelForSegment:", "@@:q", false}, {"setLabel:forSegment:", "v@:@q", true},
             {"imageForSegment:", "@@:q", false}, {"setImage:forSegment:", "v@:@q", true}, {"menuForSegment:", "@@:q", false},
             {"setMenu:forSegment:", "v@:@q", true}, {"toolTipForSegment:", "@@:q", false}, {"setToolTip:forSegment:", "v@:@q", false},
             {"widthForSegment:", "d@:q", false}, {"setWidth:forSegment:", "v@:dq", true}, {"tagForSegment:", "q@:q", false},
             {"setTag:forSegment:", "v@:qq", false}, {"imageScalingForSegment:", "Q@:q", false},
             {"setImageScaling:forSegment:", "v@:Qq", true}, {"alignmentForSegment:", "q@:q", false},
             {"setAlignment:forSegment:", "v@:qq", true}, {"isEnabledForSegment:", "B@:q", false},
             {"setEnabled:forSegment:", "v@:Bq", true}, {"showsMenuIndicatorForSegment:", "B@:q", false},
             {"setShowsMenuIndicator:forSegment:", "v@:Bq", true}, {"trackingMode", "Q@:", false}, {"setTrackingMode:", "v@:Q", true},
             {"segmentStyle", "q@:", false}, {"setSegmentStyle:", "v@:q", true}, {"segmentDistribution", "q@:", false},
             {"setSegmentDistribution:", "v@:q", true}, {"selectedSegmentBezelColor", "@@:", false},
             {"setSelectedSegmentBezelColor:", "v@:@", true}})
        forward_to_cell(kit, n, name, types, redraw);
    // Accept the first click in an inactive window, as AppKit's segmented control does.
    kit.method(n, "acceptsFirstMouse:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:@");
    // Segment-level clicks: highlight the pressed segment while the pointer
    // stays on it; a release on it selects it (by tracking mode) and sends
    // the action. A segment with a menu (and no action, or a menu
    // indicator) pops its menu up below itself instead.
    kit.method(n, "mouseDown:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], event = s.x[2];
        const auto cell = control_cell(*k, self);
        if (!cell || !cell_state(*k, cell)->enabled) return;
        const auto state = segmented(*k, cell);
        const auto bounds = AppKit::view_bounds(*k->view_state(self));
        const auto bezel = bezel_rect(*k, cell, bounds);
        const auto spans = spans_for(*k, cell, bezel);
        const auto start = event_location_in(*k, self, event);
        const auto index = layout::segment_at(spans, start.x);
        if (index < 0 || !state->segments[static_cast<std::size_t>(index)].enabled) return;
        const auto& segment = state->segments[static_cast<std::size_t>(index)];
        const auto info = cell_state(*k, cell);
        if (segment.menu && (segment.menu_indicator || !info->action)) {
            const Rect anchor{{spans[static_cast<std::size_t>(index)].x, bezel.origin.y}, {spans[static_cast<std::size_t>(index)].width, bezel.size.height}};
            state->pressed = index;
            k->invalidate_view(self);
            std::int64_t chosen{};
            try { chosen = k->popup_menu_tracking(segment.menu, self, anchor, -1); }
            catch (...) { state->pressed = -1; k->invalidate_view(self); throw; }
            state->pressed = -1;
            k->invalidate_view(self);
            if (chosen >= 0) static_cast<void>(k->send_id(segment.menu, "performActionForItemAtIndex:", {static_cast<Address>(chosen)}));
            return;
        }
        Point last = start;
        const auto on_segment = [&](Point p) {
            return rect_contains(bezel, p) && layout::segment_at(spans, p.x) == index;
        };
        state->pressed = index;
        static_cast<void>(track_mouse(*k, self, event, [&](bool) {
            state->pressed = on_segment(last) ? index : -1;
            k->invalidate_view(self);
        }, [&](Point p) {
            last = p;
            const auto now = on_segment(p) ? index : -1;
            if (now != state->pressed) { state->pressed = now; k->invalidate_view(self); }
        }));
        state->pressed = -1;
        k->invalidate_view(self);
        if (!on_segment(last)) return;
        static_cast<void>(k->send_id(self, "_alchemyClickSegment:", {static_cast<Address>(index)}));
    }, "v@:@");
    // A completed click on a segment (also -performClick:'s path).
    kit.method(n, "_alchemyClickSegment:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto cell = control_cell(*k, self);
        if (!cell) return;
        const auto state = segmented(*k, cell);
        const auto index = static_cast<std::int64_t>(s.x[2]);
        if (index < 0 || index >= static_cast<std::int64_t>(state->segments.size())) return;
        if (state->tracking == tracking::select_any) {
            auto& segment = state->segments[static_cast<std::size_t>(index)];
            segment.selected = !segment.selected;
            state->selected = index;
        } else {
            set_selected_segment(*k, cell, index);
        }
        k->invalidate_view(self);
        const auto info = cell_state(*k, cell);
        static_cast<void>(k->send_id(self, "sendAction:to:", {info->action, info->target}));
        if (momentary(state->tracking)) state->selected = -1;   // selected only while the action runs
    }, "v@:q");
    kit.method(n, "performClick:", [k](CpuState& s, GuestMemory&) {
        const auto cell = control_cell(*k, s.x[0]);
        if (!cell || !cell_state(*k, cell)->enabled) return;
        const auto selected = segmented(*k, cell)->selected;
        if (selected >= 0) static_cast<void>(k->send_id(s.x[0], "_alchemyClickSegment:", {static_cast<Address>(selected)}));
    }, "v@:@");
    // Factories.
    const auto factory = [k](Address cls, Address values, std::uint64_t mode, Address target, Address action, bool images) {
        CallRegs registers;
        put_rect(registers, 0, {{0, 0}, {100, 24}});
        const auto made = k->send(k->send(cls, "alloc").x0, "initWithFrame:", registers).x0;
        const auto list = k->items(values);
        static_cast<void>(k->send_id(made, "setSegmentCount:", {list.size()}));
        for (std::size_t i = 0; i < list.size(); ++i)
            static_cast<void>(k->send_id(made, images ? "setImage:forSegment:" : "setLabel:forSegment:", {list[i], i}));
        static_cast<void>(k->send_id(made, "setTrackingMode:", {mode}));
        static_cast<void>(k->send_id(made, "setTarget:", {target}));
        static_cast<void>(k->send_id(made, "setAction:", {action}));
        static_cast<void>(k->send(made, "sizeToFit"));
        return k->autorelease(made);
    };
    kit.class_method(n, "segmentedControlWithLabels:trackingMode:target:action:", [factory](CpuState& s, GuestMemory&) {
        s.x[0] = factory(s.x[0], s.x[2], s.x[3], s.x[4], s.x[5], false);
    });
    kit.class_method(n, "segmentedControlWithImages:trackingMode:target:action:", [factory](CpuState& s, GuestMemory&) {
        s.x[0] = factory(s.x[0], s.x[2], s.x[3], s.x[4], s.x[5], true);
    });
    static_cast<void>(rect_type);
}

} // namespace alchemy::gui::completion_kit::controls
