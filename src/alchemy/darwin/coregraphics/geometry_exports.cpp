#include "alchemy/darwin/coregraphics/q2d_internal.hpp"

#include <algorithm>
#include <cstring>

// CGGeometry and CGAffineTransform functions and constants. Apple ARM64:
// CGPoint / CGSize / CGRect are HFAs (d registers in and out);
// CGAffineTransform (six doubles) is passed by reference and returned
// through x8.
namespace alchemy::gui::quartz2d {

void Quartz::install_geometry() {
    const auto cg = core_graphics_library;
    const auto zero = std::array<std::uint8_t, 32>{};
    data_constant(cg, "_CGPointZero", std::span(zero).first(16));
    data_constant(cg, "_CGSizeZero", std::span(zero).first(16));
    data_constant(cg, "_CGRectZero", zero);
    const auto rect_bytes = [](const Rect& r) {
        std::array<std::uint8_t, 32> bytes{};
        const std::array<double, 4> values{r.origin.x, r.origin.y, r.size.width, r.size.height};
        std::memcpy(bytes.data(), values.data(), 32);
        return bytes;
    };
    data_constant(cg, "_CGRectNull", rect_bytes(null_rect()));
    data_constant(cg, "_CGRectInfinite", rect_bytes(infinite_rect()));
    {
        std::array<std::uint8_t, 48> bytes{};
        const std::array<double, 6> identity{1, 0, 0, 1, 0, 0};
        std::memcpy(bytes.data(), identity.data(), 48);
        data_constant(cg, "_CGAffineTransformIdentity", bytes);
    }

    // ---- rectangles ----
    const auto rect_value = [](const char* name, double (*value)(const Rect&)) {
        return std::pair{name, value};
    };
    for (const auto& [name, value] : {
             rect_value("_CGRectGetMinX", [](const Rect& r) { return rect_is_null(r) ? r.origin.x : min_x(r); }),
             rect_value("_CGRectGetMinY", [](const Rect& r) { return rect_is_null(r) ? r.origin.y : min_y(r); }),
             rect_value("_CGRectGetMaxX", [](const Rect& r) { return rect_is_null(r) ? r.origin.x : max_x(r); }),
             rect_value("_CGRectGetMaxY", [](const Rect& r) { return rect_is_null(r) ? r.origin.y : max_y(r); }),
             rect_value("_CGRectGetMidX", [](const Rect& r) { const auto s = standardize(r); return s.origin.x + s.size.width / 2; }),
             rect_value("_CGRectGetMidY", [](const Rect& r) { const auto s = standardize(r); return s.origin.y + s.size.height / 2; }),
             rect_value("_CGRectGetWidth", [](const Rect& r) { return std::abs(r.size.width); }),
             rect_value("_CGRectGetHeight", [](const Rect& r) { return std::abs(r.size.height); })}) {
        const auto compute = value;
        function(cg, name, [compute](CpuState& s, GuestMemory&) { set_d(s, 0, compute(rect_arg(s))); });
    }
    function(cg, "_CGRectIsEmpty", [](CpuState& s, GuestMemory&) { return_bool(s, rect_is_empty(rect_arg(s))); });
    function(cg, "_CGRectIsNull", [](CpuState& s, GuestMemory&) { return_bool(s, rect_is_null(rect_arg(s))); });
    function(cg, "_CGRectIsInfinite", [](CpuState& s, GuestMemory&) { return_bool(s, rect_is_infinite(rect_arg(s))); });
    function(cg, "_CGRectStandardize", [](CpuState& s, GuestMemory&) { return_rect(s, standardize(rect_arg(s))); });
    function(cg, "_CGRectIntegral", [](CpuState& s, GuestMemory&) { return_rect(s, rect_integral(rect_arg(s))); });
    function(cg, "_CGRectInset", [](CpuState& s, GuestMemory&) { return_rect(s, rect_inset(rect_arg(s), d(s, 4), d(s, 5))); });
    function(cg, "_CGRectOffset", [](CpuState& s, GuestMemory&) {
        auto r = rect_arg(s);
        if (!rect_is_null(r)) { r = standardize(r); r.origin.x += d(s, 4); r.origin.y += d(s, 5); }
        return_rect(s, r);
    });
    function(cg, "_CGRectUnion", [](CpuState& s, GuestMemory&) { return_rect(s, rect_union(rect_arg(s), rect_arg(s, 4))); });
    function(cg, "_CGRectIntersection", [](CpuState& s, GuestMemory&) { return_rect(s, rect_intersection(rect_arg(s), rect_arg(s, 4))); });
    function(cg, "_CGRectIntersectsRect", [](CpuState& s, GuestMemory&) {
        const auto r = rect_intersection(rect_arg(s), rect_arg(s, 4));
        return_bool(s, !rect_is_null(r) && !rect_is_empty(r));
    });
    function(cg, "_CGRectContainsPoint", [](CpuState& s, GuestMemory&) { return_bool(s, rect_contains_point(rect_arg(s), point_arg(s, 4))); });
    function(cg, "_CGRectContainsRect", [](CpuState& s, GuestMemory&) {
        const auto a = rect_arg(s), b = rect_arg(s, 4);
        if (rect_is_null(a) || rect_is_null(b)) { return_bool(s, false); return; }
        const auto u = rect_union(a, b);
        const auto sa = standardize(a);
        return_bool(s, u.origin.x == sa.origin.x && u.origin.y == sa.origin.y && u.size.width == sa.size.width && u.size.height == sa.size.height);
    });
    function(cg, "_CGRectEqualToRect", [](CpuState& s, GuestMemory&) {
        const auto a = standardize(rect_arg(s)), b = standardize(rect_arg(s, 4));
        return_bool(s, a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.size.width == b.size.width && a.size.height == b.size.height);
    });
    function(cg, "_CGPointEqualToPoint", [](CpuState& s, GuestMemory&) { return_bool(s, d(s, 0) == d(s, 2) && d(s, 1) == d(s, 3)); });
    function(cg, "_CGSizeEqualToSize", [](CpuState& s, GuestMemory&) { return_bool(s, d(s, 0) == d(s, 2) && d(s, 1) == d(s, 3)); });
    function(cg, "_CGRectDivide", [](CpuState& s, GuestMemory& m) {
        // (rect d0-d3, slice*, remainder*, amount d4, edge)
        auto r = standardize(rect_arg(s));
        const auto amount = std::max(0.0, d(s, 4));
        const auto edge = static_cast<std::uint32_t>(s.x[2]);   // CGRectMinXEdge, MinY, MaxX, MaxY
        Rect slice = r, remainder = r;
        const auto horizontal = edge == 0 || edge == 2;
        const auto extent = horizontal ? r.size.width : r.size.height;
        const auto cut = std::min(amount, extent);
        if (edge == 0) { slice.size.width = cut; remainder.origin.x += cut; remainder.size.width -= cut; }
        else if (edge == 1) { slice.size.height = cut; remainder.origin.y += cut; remainder.size.height -= cut; }
        else if (edge == 2) { slice.origin.x += r.size.width - cut; slice.size.width = cut; remainder.size.width -= cut; }
        else { slice.origin.y += r.size.height - cut; slice.size.height = cut; remainder.size.height -= cut; }
        const auto store = [&m](Address at, const Rect& value) {
            if (!at) return;
            const std::array<double, 4> values{value.origin.x, value.origin.y, value.size.width, value.size.height};
            for (std::size_t index = 0; index < 4; ++index) m.write64(at + index * 8, std::bit_cast<std::uint64_t>(values[index]));
        };
        store(s.x[0], slice);
        store(s.x[1], remainder);
    });
    // Dictionary representations: {X, Y, Width, Height} CFNumbers.
    const auto make_dictionary = [this](std::initializer_list<std::pair<const char*, double>> entries) {
        std::vector<std::pair<Address, Address>> pairs;
        for (const auto& [key, value] : entries) pairs.emplace_back(cf.string(key), cf.real(value));
        const auto dictionary = cf.dictionary(pairs);
        for (const auto& [key, value] : pairs) { release(key); release(value); }
        return dictionary;
    };
    const auto read_entries = [this](Address dictionary, std::initializer_list<const char*> keys, std::span<double> out) {
        if (!dictionary) return false;
        std::size_t index = 0;
        for (const auto* key : keys) {
            const auto value = number(dictionary_get(dictionary, key));
            if (!value) return false;
            out[index++] = *value;
        }
        return true;
    };
    function(cg, "_CGRectCreateDictionaryRepresentation", [make_dictionary](CpuState& s, GuestMemory&) {
        const auto r = rect_arg(s);
        s.x[0] = make_dictionary({{"X", r.origin.x}, {"Y", r.origin.y}, {"Width", r.size.width}, {"Height", r.size.height}});
    });
    function(cg, "_CGPointCreateDictionaryRepresentation", [make_dictionary](CpuState& s, GuestMemory&) {
        s.x[0] = make_dictionary({{"X", d(s, 0)}, {"Y", d(s, 1)}});
    });
    function(cg, "_CGSizeCreateDictionaryRepresentation", [make_dictionary](CpuState& s, GuestMemory&) {
        s.x[0] = make_dictionary({{"Width", d(s, 0)}, {"Height", d(s, 1)}});
    });
    function(cg, "_CGRectMakeWithDictionaryRepresentation", [this, read_entries](CpuState& s, GuestMemory&) {
        std::array<double, 4> values{};
        const bool ok = read_entries(s.x[0], {"X", "Y", "Width", "Height"}, values);
        if (ok && s.x[1]) for (std::size_t index = 0; index < 4; ++index) write_double(s.x[1] + index * 8, values[index]);
        return_bool(s, ok);
    });
    function(cg, "_CGPointMakeWithDictionaryRepresentation", [this, read_entries](CpuState& s, GuestMemory&) {
        std::array<double, 2> values{};
        const bool ok = read_entries(s.x[0], {"X", "Y"}, values);
        if (ok && s.x[1]) { write_double(s.x[1], values[0]); write_double(s.x[1] + 8, values[1]); }
        return_bool(s, ok);
    });
    function(cg, "_CGSizeMakeWithDictionaryRepresentation", [this, read_entries](CpuState& s, GuestMemory&) {
        std::array<double, 2> values{};
        const bool ok = read_entries(s.x[0], {"Width", "Height"}, values);
        if (ok && s.x[1]) { write_double(s.x[1], values[0]); write_double(s.x[1] + 8, values[1]); }
        return_bool(s, ok);
    });

    // ---- affine transforms (indirect result in x8) ----
    const auto result = [this](CpuState& s, const Affine& value) { write_affine(s.x[8], value); };
    function(cg, "_CGAffineTransformMake", [result](CpuState& s, GuestMemory&) {
        result(s, {d(s, 0), d(s, 1), d(s, 2), d(s, 3), d(s, 4), d(s, 5)});
    });
    function(cg, "_CGAffineTransformMakeTranslation", [result](CpuState& s, GuestMemory&) { result(s, Affine::translation(d(s, 0), d(s, 1))); });
    function(cg, "_CGAffineTransformMakeScale", [result](CpuState& s, GuestMemory&) { result(s, Affine::scaling(d(s, 0), d(s, 1))); });
    function(cg, "_CGAffineTransformMakeRotation", [result](CpuState& s, GuestMemory&) { result(s, Affine::rotation(d(s, 0))); });
    function(cg, "_CGAffineTransformTranslate", [this, result](CpuState& s, GuestMemory&) {
        result(s, Affine::translation(d(s, 0), d(s, 1)).then(read_affine(s.x[0])));
    });
    function(cg, "_CGAffineTransformScale", [this, result](CpuState& s, GuestMemory&) {
        result(s, Affine::scaling(d(s, 0), d(s, 1)).then(read_affine(s.x[0])));
    });
    function(cg, "_CGAffineTransformRotate", [this, result](CpuState& s, GuestMemory&) {
        result(s, Affine::rotation(d(s, 0)).then(read_affine(s.x[0])));
    });
    function(cg, "_CGAffineTransformConcat", [this, result](CpuState& s, GuestMemory&) {
        result(s, read_affine(s.x[0]).then(read_affine(s.x[1])));
    });
    function(cg, "_CGAffineTransformInvert", [this, result](CpuState& s, GuestMemory&) {
        const auto value = read_affine(s.x[0]);
        result(s, value.inverse().value_or(value));
    });
    function(cg, "_CGAffineTransformIsIdentity", [this](CpuState& s, GuestMemory&) { return_bool(s, read_affine(s.x[0]).is_identity()); });
    // CGAffineTransformComponents {CGSize scale; CGFloat horizontalShear;
    // CGFloat rotation; CGVector translation} (48 bytes, by reference both
    // ways): the transform is scale, then horizontal shear, then rotation,
    // then translation. A mirrored transform has a negative vertical scale.
    function(cg, "_CGAffineTransformMakeWithComponents", [result](CpuState& s, GuestMemory& m) {
        std::array<double, 6> c{};
        for (unsigned i = 0; i < 6; ++i) c[i] = std::bit_cast<double>(m.read64(s.x[0] + i * 8));
        const double sx = c[0], sy = c[1], shear = c[2], cosine = std::cos(c[3]), sine = std::sin(c[3]);
        // Rows: [sx 0; sy*shear sy] times [cos sin; -sin cos].
        result(s, {sx * cosine, sx * sine, sy * shear * cosine - sy * sine, sy * shear * sine + sy * cosine, c[4], c[5]});
    });
    function(cg, "_CGAffineTransformDecompose", [this](CpuState& s, GuestMemory& m) {
        const auto t = read_affine(s.x[0]);
        double sx = std::hypot(t.a, t.b), rotation = sx > 0 ? std::atan2(t.b, t.a) : 0.0;
        const double cosine = std::cos(rotation), sine = std::sin(rotation);
        const double sy = -t.c * sine + t.d * cosine;
        const double shear = sy != 0 ? (t.c * cosine + t.d * sine) / sy : 0.0;
        const std::array<double, 6> c{sx, sy, shear, rotation, t.tx, t.ty};
        for (unsigned i = 0; i < 6; ++i) m.write64(s.x[8] + i * 8, std::bit_cast<std::uint64_t>(c[i]));
    });
    function(cg, "_CGAffineTransformEqualToTransform", [this](CpuState& s, GuestMemory&) {
        return_bool(s, read_affine(s.x[0]) == read_affine(s.x[1]));
    });
    function(cg, "_CGPointApplyAffineTransform", [this](CpuState& s, GuestMemory&) {
        return_point(s, read_affine(s.x[0]).apply(point_arg(s)));
    });
    function(cg, "_CGSizeApplyAffineTransform", [this](CpuState& s, GuestMemory&) {
        const auto t = read_affine(s.x[0]);
        const auto v = t.apply_vector({d(s, 0), d(s, 1)});
        return_size(s, {v.x, v.y});
    });
    function(cg, "_CGRectApplyAffineTransform", [this](CpuState& s, GuestMemory&) {
        return_rect(s, transform_rect(read_affine(s.x[0]), rect_arg(s)));
    });
}

} // namespace alchemy::gui::quartz2d
