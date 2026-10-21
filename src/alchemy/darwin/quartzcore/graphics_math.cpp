#include "alchemy/darwin/quartzcore/graphics_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>

namespace alchemy::gui::completion_kit::graphics {

// ---- CATransform3D --------------------------------------------------------------

Transform3D transform_identity() {
    Transform3D t{};
    t[0] = t[5] = t[10] = t[15] = 1;
    return t;
}

Transform3D transform_translation(double tx, double ty, double tz) {
    auto t = transform_identity();
    t[12] = tx; t[13] = ty; t[14] = tz;
    return t;
}

Transform3D transform_scale(double sx, double sy, double sz) {
    auto t = transform_identity();
    t[0] = sx; t[5] = sy; t[10] = sz;
    return t;
}

Transform3D transform_rotation(double angle, double x, double y, double z) {
    const auto length = std::sqrt(x * x + y * y + z * z);
    if (length == 0 || !std::isfinite(length)) return transform_identity();
    x /= length; y /= length; z /= length;
    const auto c = std::cos(angle), s = std::sin(angle), t = 1 - c;
    // Row-vector form (the transpose of the column-vector axis-angle matrix):
    // a rotation about +z maps (1, 0, 0) to (cos, sin, 0).
    auto m = transform_identity();
    m[0] = c + x * x * t;     m[1] = x * y * t + z * s; m[2] = x * z * t - y * s;
    m[4] = x * y * t - z * s; m[5] = c + y * y * t;     m[6] = y * z * t + x * s;
    m[8] = x * z * t + y * s; m[9] = y * z * t - x * s; m[10] = c + z * z * t;
    return m;
}

Transform3D transform_concat(const Transform3D& a, const Transform3D& b) {
    Transform3D r{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            double sum = 0;
            for (int k = 0; k < 4; ++k) sum += a[row * 4 + k] * b[k * 4 + column];
            r[row * 4 + column] = sum;
        }
    return r;
}

Transform3D transform_translate(const Transform3D& t, double tx, double ty, double tz) {
    return transform_concat(transform_translation(tx, ty, tz), t);
}
Transform3D transform_scale_by(const Transform3D& t, double sx, double sy, double sz) {
    return transform_concat(transform_scale(sx, sy, sz), t);
}
Transform3D transform_rotate(const Transform3D& t, double angle, double x, double y, double z) {
    return transform_concat(transform_rotation(angle, x, y, z), t);
}

Transform3D transform_invert(const Transform3D& t) {
    // Gauss-Jordan elimination with partial pivoting.
    std::array<std::array<double, 8>, 4> m{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            m[row][column] = t[row * 4 + column];
            m[row][column + 4] = row == column ? 1 : 0;
        }
    for (int column = 0; column < 4; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row)
            if (std::abs(m[row][column]) > std::abs(m[pivot][column])) pivot = row;
        if (std::abs(m[pivot][column]) < 1e-12) return t;   // singular: unchanged, as Core Animation
        std::swap(m[pivot], m[column]);
        const auto scale = m[column][column];
        for (auto& value : m[column]) value /= scale;
        for (int row = 0; row < 4; ++row) {
            if (row == column) continue;
            const auto factor = m[row][column];
            if (factor == 0) continue;
            for (int k = 0; k < 8; ++k) m[row][k] -= factor * m[column][k];
        }
    }
    Transform3D r{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) r[row * 4 + column] = m[row][column + 4];
    return r;
}

bool transform_is_identity(const Transform3D& t) { return t == transform_identity(); }

bool transform_is_affine(const Transform3D& t) {
    // Only m11 m12 m21 m22 m41 m42 may differ from the identity.
    return t[2] == 0 && t[3] == 0 && t[6] == 0 && t[7] == 0 && t[8] == 0 && t[9] == 0 && t[10] == 1 && t[11] == 0 &&
           t[14] == 0 && t[15] == 1;
}

Transform3D transform_from_affine(const std::array<double, 6>& affine) {
    auto t = transform_identity();
    t[0] = affine[0]; t[1] = affine[1];
    t[4] = affine[2]; t[5] = affine[3];
    t[12] = affine[4]; t[13] = affine[5];
    return t;
}

std::array<double, 6> transform_to_affine(const Transform3D& t) { return {t[0], t[1], t[4], t[5], t[12], t[13]}; }

// ---- CAMediaTimingFunction -----------------------------------------------------------

double timing_curve_value(const std::array<float, 4>& points, double x) {
    x = std::clamp(x, 0.0, 1.0);
    const double x1 = std::clamp<double>(points[0], 0, 1), y1 = points[1];
    const double x2 = std::clamp<double>(points[2], 0, 1), y2 = points[3];
    // B(s) = 3(1-s)^2 s P1 + 3(1-s) s^2 P2 + s^3 (P0 = 0, P3 = 1).
    const auto bezier = [](double s, double p1, double p2) {
        const auto r = 1 - s;
        return 3 * r * r * s * p1 + 3 * r * s * s * p2 + s * s * s;
    };
    const auto slope = [](double s, double p1, double p2) {
        const auto r = 1 - s;
        return 3 * r * r * p1 + 6 * r * s * (p2 - p1) + 3 * s * s * (1 - p2);
    };
    // Newton's method from s = x, then bisection when it does not converge.
    double s = x;
    for (int i = 0; i < 8; ++i) {
        const auto error = bezier(s, x1, x2) - x;
        if (std::abs(error) < 1e-9) return bezier(s, y1, y2);
        const auto d = slope(s, x1, x2);
        if (std::abs(d) < 1e-7) break;
        s -= error / d;
        if (s < 0 || s > 1) break;
    }
    double low = 0, high = 1;
    s = x;
    for (int i = 0; i < 64; ++i) {
        const auto value = bezier(s, x1, x2);
        if (std::abs(value - x) < 1e-10) break;
        if (value < x) low = s; else high = s;
        s = (low + high) / 2;
    }
    return bezier(s, y1, y2);
}

// ---- NSGradient -------------------------------------------------------------------------

Rgba gradient_color_at(std::vector<GradientStop> stops, double location) {
    if (stops.empty()) return {0, 0, 0, 0};
    std::stable_sort(stops.begin(), stops.end(), [](const GradientStop& a, const GradientStop& b) { return a.location < b.location; });
    if (location <= stops.front().location) return stops.front().color;
    if (location >= stops.back().location) return stops.back().color;
    for (std::size_t i = 1; i < stops.size(); ++i) {
        const auto& a = stops[i - 1];
        const auto& b = stops[i];
        if (location > b.location) continue;
        const auto span = b.location - a.location;
        const auto f = span > 0 ? (location - a.location) / span : 1.0;
        return {a.color.r + (b.color.r - a.color.r) * f, a.color.g + (b.color.g - a.color.g) * f,
                a.color.b + (b.color.b - a.color.b) * f, a.color.a + (b.color.a - a.color.a) * f};
    }
    return stops.back().color;
}

GradientLine gradient_line(double x, double y, double width, double height, double degrees) {
    const auto radians = degrees * 3.14159265358979323846 / 180.0;
    auto dx = std::cos(radians), dy = std::sin(radians);
    // Exact axis directions for multiples of 90 degrees.
    if (std::abs(dx) < 1e-12) dx = 0;
    if (std::abs(dy) < 1e-12) dy = 0;
    const auto cx = x + width / 2, cy = y + height / 2;
    // Half the rectangle's extent along the direction.
    const auto half = (std::abs(dx) * width + std::abs(dy) * height) / 2;
    return {cx - dx * half, cy - dy * half, cx + dx * half, cy + dy * half};
}

// ---- NSAffineTransform ---------------------------------------------------------------------

Affine affine_multiply(const Affine& a, const Affine& b) {
    return {a.m11 * b.m11 + a.m12 * b.m21, a.m11 * b.m12 + a.m12 * b.m22,
            a.m21 * b.m11 + a.m22 * b.m21, a.m21 * b.m12 + a.m22 * b.m22,
            a.tx * b.m11 + a.ty * b.m21 + b.tx, a.tx * b.m12 + a.ty * b.m22 + b.ty};
}

Affine affine_translate(const Affine& t, double x, double y) { return affine_multiply({1, 0, 0, 1, x, y}, t); }

Affine affine_rotate(const Affine& t, double radians) {
    const auto c = std::cos(radians), s = std::sin(radians);
    return affine_multiply({c, s, -s, c, 0, 0}, t);
}

Affine affine_scale(const Affine& t, double sx, double sy) { return affine_multiply({sx, 0, 0, sy, 0, 0}, t); }

std::optional<Affine> affine_invert(const Affine& t) {
    const auto det = t.m11 * t.m22 - t.m12 * t.m21;
    if (det == 0 || !std::isfinite(det)) return std::nullopt;
    Affine r;
    r.m11 = t.m22 / det;
    r.m12 = -t.m12 / det;
    r.m21 = -t.m21 / det;
    r.m22 = t.m11 / det;
    r.tx = -(t.tx * r.m11 + t.ty * r.m21);
    r.ty = -(t.tx * r.m12 + t.ty * r.m22);
    return r;
}

std::array<double, 2> affine_point(const Affine& t, double x, double y) {
    return {t.m11 * x + t.m21 * y + t.tx, t.m12 * x + t.m22 * y + t.ty};
}

std::array<double, 2> affine_size(const Affine& t, double width, double height) {
    return {t.m11 * width + t.m21 * height, t.m12 * width + t.m22 * height};
}

// ---- HSB ---------------------------------------------------------------------------------------

std::array<double, 3> hsb_to_rgb(double hue, double saturation, double brightness) {
    saturation = std::clamp(saturation, 0.0, 1.0);
    brightness = std::clamp(brightness, 0.0, 1.0);
    if (saturation == 0) return {brightness, brightness, brightness};
    auto h = std::fmod(hue, 1.0);
    if (h < 0) h += 1;
    h *= 6;
    const auto sector = static_cast<int>(std::floor(h)) % 6;
    const auto f = h - std::floor(h);
    const auto p = brightness * (1 - saturation);
    const auto q = brightness * (1 - saturation * f);
    const auto t = brightness * (1 - saturation * (1 - f));
    switch (sector) {
    case 0: return {brightness, t, p};
    case 1: return {q, brightness, p};
    case 2: return {p, brightness, t};
    case 3: return {p, q, brightness};
    case 4: return {t, p, brightness};
    default: return {brightness, p, q};
    }
}

std::array<double, 3> rgb_to_hsb(double red, double green, double blue) {
    const auto high = std::max({red, green, blue}), low = std::min({red, green, blue});
    const auto delta = high - low;
    const auto brightness = high;
    const auto saturation = high > 0 ? delta / high : 0.0;
    double hue = 0;
    if (delta > 0) {
        if (high == red) hue = (green - blue) / delta;
        else if (high == green) hue = 2 + (blue - red) / delta;
        else hue = 4 + (red - green) / delta;
        hue /= 6;
        if (hue < 0) hue += 1;
    }
    return {hue, saturation, brightness};
}

// ---- CIVector strings ----------------------------------------------------------------------------

std::string vector_string(const std::vector<double>& values) {
    std::string text = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        char number[64];
        std::snprintf(number, sizeof number, "%g", values[i]);
        if (i) text += ' ';
        text += number;
    }
    return text + "]";
}

std::optional<std::vector<double>> parse_vector_string(std::string_view text) {
    std::string body(text);
    const auto first = body.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::vector<double>{};
    body = body.substr(first);
    while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back()))) body.pop_back();
    if (!body.empty() && body.front() == '[') {
        if (body.back() != ']') return std::nullopt;
        body = body.substr(1, body.size() - 2);
    }
    std::vector<double> values;
    const char* cursor = body.c_str();
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') ++cursor;
        if (!*cursor) break;
        char* end{};
        const auto value = std::strtod(cursor, &end);
        if (end == cursor) return std::nullopt;
        values.push_back(value);
        cursor = end;
    }
    return values;
}

// ---- GLKQuaternion ------------------------------------------------------------------------------

Quaternion quaternion_slerp(const Quaternion& from, const Quaternion& to, float t) {
    float cosine = from[0] * to[0] + from[1] * to[1] + from[2] * to[2] + from[3] * to[3];
    auto end = to;
    if (cosine < 0) {   // the shorter arc
        cosine = -cosine;
        for (auto& v : end) v = -v;
    }
    float a = 1 - t, b = t;
    if (cosine < 0.9995f) {
        const auto angle = std::acos(cosine);
        const auto sine = std::sin(angle);
        a = std::sin((1 - t) * angle) / sine;
        b = std::sin(t * angle) / sine;
    }
    Quaternion r{};
    for (int i = 0; i < 4; ++i) r[i] = a * from[i] + b * end[i];
    if (cosine >= 0.9995f) {   // linear blend: renormalize
        const auto length = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
        if (length > 0) for (auto& v : r) v /= length;
    }
    return r;
}

} // namespace alchemy::gui::completion_kit::graphics
