#include "alchemy/darwin/coregraphics/q2d_internal.hpp"

#include <algorithm>

// CGGradient, CGFunction, CGShading and CGPattern objects (drawing is in
// context.cpp). Shadings sample their guest CGFunction into 256 gradient
// stops when created.
namespace alchemy::gui::quartz2d {

Rgba GradientInfo::at(double t) const {
    if (stops.empty()) return {0, 0, 0, 0};
    if (t <= stops.front().location) return stops.front().color;
    if (t >= stops.back().location) return stops.back().color;
    for (std::size_t index = 1; index < stops.size(); ++index) {
        const auto& a = stops[index - 1];
        const auto& b = stops[index];
        if (t > b.location) continue;
        const auto span = b.location - a.location;
        const auto f = span > 0 ? (t - a.location) / span : 1.0;
        return {a.color.r + (b.color.r - a.color.r) * f, a.color.g + (b.color.g - a.color.g) * f,
                a.color.b + (b.color.b - a.color.b) * f, a.color.a + (b.color.a - a.color.a) * f};
    }
    return stops.back().color;
}

void Quartz::install_gradients() {
    const auto cg = core_graphics_library;
    gradient_class = declare_class("_AlchemyCGGradient");
    function_class = declare_class("_AlchemyCGFunction");
    shading_class = declare_class("_AlchemyCGShading");
    const auto finish = [this](std::shared_ptr<GradientInfo> info, Address space) {
        std::stable_sort(info->stops.begin(), info->stops.end(), [](const auto& a, const auto& b) { return a.location < b.location; });
        info->space = retain(space);
        return create(gradient_class, info, [this, info](Address) { release(std::exchange(info->space, 0)); });
    };
    function(cg, "_CGGradientCreateWithColorComponents", [this, finish](CpuState& s, GuestMemory&) {
        // (space, components, locations, count)
        const auto space = s.x[0], components = s.x[1], locations = s.x[2];
        const auto count = static_cast<std::size_t>(s.x[3]);
        if (!space || !components || count == 0) { s.x[0] = 0; return; }
        const auto n = space_info(space)->components();
        auto info = std::make_shared<GradientInfo>();
        for (std::size_t index = 0; index < count; ++index) {
            std::vector<double> values(n);
            for (unsigned component = 0; component < n; ++component) values[component] = read_double(components + (index * (n + 1) + component) * 8);
            const auto alpha = read_double(components + (index * (n + 1) + n) * 8);
            const auto location = locations ? read_double(locations + index * 8) : count == 1 ? 0.0 : static_cast<double>(index) / (count - 1);
            info->stops.push_back({location, to_rgba(space, values, alpha)});
        }
        s.x[0] = finish(info, space);
    });
    function(cg, "_CGGradientCreateWithColors", [this, finish](CpuState& s, GuestMemory&) {
        // (space, CFArray of CGColor, locations)
        if (!s.x[1]) { s.x[0] = 0; return; }
        const auto colors = cf.array_values(s.x[1]);
        if (colors.empty()) { s.x[0] = 0; return; }
        auto info = std::make_shared<GradientInfo>();
        for (std::size_t index = 0; index < colors.size(); ++index) {
            const auto location = s.x[2] ? read_double(s.x[2] + index * 8)
                                         : colors.size() == 1 ? 0.0 : static_cast<double>(index) / (colors.size() - 1);
            info->stops.push_back({location, color_rgba(colors[index])});
        }
        s.x[0] = finish(info, s.x[0] ? s.x[0] : srgb());
    });
    function(cg, "_CGGradientRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });
    function(cg, "_CGGradientRelease", [this](CpuState& s, GuestMemory&) { release(s.x[0]); });
    function(cg, "_CGGradientGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });

    // ---- CGFunction ----
    function(cg, "_CGFunctionCreate", [this](CpuState& s, GuestMemory& m) {
        // (info, domainDimension, domain, rangeDimension, range, callbacks {u32 version; evaluate(info, in, out); releaseInfo(info)})
        if (!s.x[5]) { s.x[0] = 0; return; }
        auto info = std::make_shared<FunctionInfo>();
        info->info = s.x[0];
        info->inputs = s.x[1];
        info->outputs = s.x[3];
        if (info->inputs > 32 || info->outputs > 32) { s.x[0] = 0; return; }
        for (Address index = 0; index < 2 * info->inputs && s.x[2]; ++index) info->domain.push_back(read_double(s.x[2] + index * 8));
        for (Address index = 0; index < 2 * info->outputs && s.x[4]; ++index) info->range.push_back(read_double(s.x[4] + index * 8));
        info->evaluate = m.read64(s.x[5] + 8);
        info->release = m.read64(s.x[5] + 16);
        s.x[0] = create(function_class, info, [this, info](Address) {
            if (info->release) static_cast<void>(call_guest(info->release, {info->info}));
        });
    });
    function(cg, "_CGFunctionRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });
    function(cg, "_CGFunctionRelease", [this](CpuState& s, GuestMemory&) { release(s.x[0]); });
    function(cg, "_CGFunctionGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });

    // ---- CGShading ----
    const auto make_shading = [this](bool radial, Address space, Point start, double start_radius, Point end, double end_radius,
                                     Address function_object, bool extend_start, bool extend_end) -> Address {
        if (!space || !function_object) return 0;
        const auto function_info = require<FunctionInfo>(function_object, "CGFunction");
        const auto components = space_info(space)->components();
        auto sampled = std::make_shared<GradientInfo>();
        if (function_info->evaluate) {
            const auto low = function_info->domain.size() >= 2 ? function_info->domain[0] : 0.0;
            const auto high = function_info->domain.size() >= 2 ? function_info->domain[1] : 1.0;
            const auto outputs = std::max<std::uint64_t>(function_info->outputs, components);
            Scratch in(*this, 8 * std::max<std::uint64_t>(function_info->inputs, 1)), out(*this, 8 * (outputs + 1));
            constexpr int samples = 256;
            for (int index = 0; index < samples; ++index) {
                const auto t = static_cast<double>(index) / (samples - 1);
                write_double(in.address(), low + (high - low) * t);
                for (std::uint64_t component = 0; component <= outputs; ++component) write_double(out.address() + component * 8, 0);
                if (components < outputs) write_double(out.address() + components * 8, 1);
                static_cast<void>(call_guest(function_info->evaluate, {function_info->info, in.address(), out.address()}));
                std::vector<double> values(components);
                for (unsigned component = 0; component < components; ++component) values[component] = read_double(out.address() + component * 8);
                const auto alpha = function_info->outputs > components ? read_double(out.address() + components * 8) : 1.0;
                sampled->stops.push_back({t, to_rgba(space, values, alpha)});
            }
        }
        auto info = std::make_shared<ShadingInfo>();
        info->radial = radial;
        info->start = start;
        info->end = end;
        info->start_radius = start_radius;
        info->end_radius = end_radius;
        info->extend_start = extend_start;
        info->extend_end = extend_end;
        info->space = retain(space);
        info->function = retain(function_object);
        info->sampled = sampled;
        return create(shading_class, info, [this, info](Address) {
            release(std::exchange(info->space, 0));
            release(std::exchange(info->function, 0));
        });
    };
    function(cg, "_CGShadingCreateAxial", [make_shading](CpuState& s, GuestMemory&) {
        // (space, start d0 d1, end d2 d3, function, extendStart, extendEnd)
        s.x[0] = make_shading(false, s.x[0], point_arg(s), 0, point_arg(s, 2), 0, s.x[1], bool_arg(s, 2), bool_arg(s, 3));
    });
    function(cg, "_CGShadingCreateRadial", [make_shading](CpuState& s, GuestMemory&) {
        // (space, start d0 d1, startRadius d2, end d3 d4, endRadius d5, function, extendStart, extendEnd)
        s.x[0] = make_shading(true, s.x[0], point_arg(s), d(s, 2), point_arg(s, 3), d(s, 5), s.x[1], bool_arg(s, 2), bool_arg(s, 3));
    });
    function(cg, "_CGShadingRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });
    function(cg, "_CGShadingRelease", [this](CpuState& s, GuestMemory&) { release(s.x[0]); });
    function(cg, "_CGShadingGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });

    // ---- CGPattern ----
    function(cg, "_CGPatternCreate", [this](CpuState& s, GuestMemory& m) {
        // (info, bounds d0-d3, const CGAffineTransform* matrix (by reference), xStep d4, yStep d5, tiling, isColored, callbacks)
        if (!s.x[4]) { s.x[0] = 0; return; }
        auto info = std::make_shared<PatternInfo>();
        info->info = s.x[0];
        info->bounds = rect_arg(s);
        info->matrix = read_affine(s.x[1]);
        info->x_step = d(s, 4);
        info->y_step = d(s, 5);
        info->tiling = int_arg(s, 2);
        info->colored = bool_arg(s, 3);
        info->draw = m.read64(s.x[4] + 8);
        info->release = m.read64(s.x[4] + 16);
        s.x[0] = create(pattern_class, info, [this, info](Address) {
            if (info->release) static_cast<void>(call_guest(info->release, {info->info}));
        });
    });
    function(cg, "_CGPatternRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });
    function(cg, "_CGPatternRelease", [this](CpuState& s, GuestMemory&) { release(s.x[0]); });
    function(cg, "_CGPatternGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });
}

} // namespace alchemy::gui::quartz2d
