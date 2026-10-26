// AppKit software drawing, standard controls and nib loading (owner: AppKit)
// over the fake window server: the host rasterizer, window backing stores
// and their presentation, -drawRect: with flipped coordinates and dirty
// rectangles, layer-hosting windows left alone, NSButton / NSTextField (field
// editor) / NSPopUpButton / NSScrollView / NSTableView behaviour driven by
// window-server events, NIBArchive parsing (a synthetic archive and, when the
// user's copy is present, every nib of QLab.app), nib instantiation and
// compiled asset catalogs (a synthetic Assets.car and QLab's named colors).
// Native classes stand in for guest subclasses, so every call goes through
// ObjcRuntime::send. No window-server windows are created (headless).
#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/darwin/appkit/drawing.hpp"
#include "alchemy/darwin/appkit/nib_archive.hpp"
#include "alchemy/darwin/appkit/asset_catalog.hpp"
#include "alchemy/darwin/appkit/rtf.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_numeric.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/darwin/coregraphics/quartz2d.hpp"
#include "alchemy/darwin/quartzcore/quartzcore.hpp"
#include "alchemy/darwin/appkit/tests/fake_window_server.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace gui = alchemy::gui;
namespace drawing = alchemy::gui::appkit::drawing;
namespace nib = alchemy::gui::nib;
using Address = std::uint64_t;
using alchemy::CallRegs;
using alchemy::CallResult;
using transmute::CpuState;
using transmute::GuestMemory;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::vector<std::string> journal;
bool log_has(const std::string& entry) { return std::find(journal.begin(), journal.end(), entry) != journal.end(); }

// ---- the rasterizer ------------------------------------------------------------------
std::uint32_t alpha_of(std::uint32_t pixel) { return pixel >> 24; }

void rasterizer() {
    const drawing::Color red{1, 0, 0, 1}, blue{0, 0, 1, 1};
    {
        drawing::HostBuffer buffer(20, 20);
        drawing::Canvas canvas(buffer);
        canvas.fill_rect({{2, 2}, {4, 4}}, red);
        require(buffer.at(3, 3) == 0xffff0000u && buffer.at(2, 2) == 0xffff0000u && buffer.at(6, 6) == 0 && buffer.at(1, 3) == 0,
                "fill_rect pixel coverage");
        canvas.fill_rect({{10.5, 0}, {1, 1}}, red);
        require(alpha_of(buffer.at(10, 0)) >= 126 && alpha_of(buffer.at(10, 0)) <= 129 && alpha_of(buffer.at(11, 0)) >= 126,
                "half-pixel fill_rect coverage");
        // Source-over: 50% blue over opaque red.
        canvas.fill_rect({{2, 2}, {1, 1}}, blue.with_alpha(0.5));
        const auto mixed = buffer.at(2, 2);
        require(alpha_of(mixed) == 255 && ((mixed >> 16) & 0xff) > 120 && ((mixed >> 16) & 0xff) < 135 && (mixed & 0xff) > 120,
                "source-over blending");
    }
    {
        drawing::HostBuffer buffer(20, 20);
        drawing::Canvas canvas(buffer);
        drawing::Path circle;
        circle.add_oval({{0, 0}, {20, 20}});
        canvas.fill_path(circle, blue);
        double covered{};
        for (const auto pixel : buffer.pixels()) covered += alpha_of(pixel) / 255.0;
        require(std::abs(covered - 314.16) < 6, "circle area " + std::to_string(covered));
        // Clipping.
        drawing::HostBuffer clipped(20, 20);
        drawing::Canvas clip_canvas(clipped);
        clip_canvas.clip_to_rect({{0, 0}, {5, 5}});
        clip_canvas.fill_rect({{0, 0}, {20, 20}}, red);
        std::size_t lit{};
        for (const auto pixel : clipped.pixels()) lit += pixel != 0;
        require(lit == 25, "clip_to_rect");
    }
    {
        drawing::HostBuffer buffer(30, 10);
        drawing::Canvas canvas(buffer);
        canvas.stroke_line({2, 5}, {28, 5}, red, 2);
        require(alpha_of(buffer.at(15, 4)) > 200 && alpha_of(buffer.at(15, 5)) > 200 && buffer.at(15, 8) == 0, "stroke_line width");
        // A y-up transform (unflipped view space): user y=0 is the bottom row.
        drawing::HostBuffer flipped(10, 10);
        drawing::Canvas up(flipped);
        up.state.ctm = {1, 0, 0, -1, 0, 10};
        up.fill_rect({{0, 0}, {10, 2}}, red);
        require(flipped.at(5, 9) == 0xffff0000u && flipped.at(5, 0) == 0, "y-up transform");
    }
    // Text: glyph coverage, metrics, line layout.
    {
        drawing::HostBuffer buffer(80, 30);
        drawing::Canvas canvas(buffer);
        drawing::FontSpec font;
        font.size = 20;
        const auto advance = canvas.draw_text(u"Hi!", font, {2, 22}, {0, 0, 0, 1});
        std::size_t inked{};
        for (const auto pixel : buffer.pixels()) inked += alpha_of(pixel) > 0;
        require(advance > 10 && advance < 60 && inked > 20, "text drawing (advance " + std::to_string(advance) + ", inked " + std::to_string(inked) + ")");
        const auto metrics = drawing::font_metrics(font);
        require(metrics.ascent > 10 && metrics.descent > 2 && metrics.ascent < 30, "font metrics");
        const auto widths = drawing::prefix_widths(font, u"abc");
        require(widths.size() == 3 && widths[0] < widths[1] && widths[1] < widths[2] && std::abs(widths[2] - drawing::text_width(font, u"abc")) < 1,
                "prefix widths");
        font.size = 13;
        const auto lines = drawing::layout_lines(font, u"alpha beta gamma\ndelta", drawing::text_width(font, u"alpha beta") + 1);
        require(lines.size() == 3 && lines[0].start == 0 && lines[1].start == 11 && lines[2].start == 17, "line layout");
        require(drawing::host_family({".AppleSystemUIFont"}) == L"Segoe UI" && drawing::host_family({"Menlo-Regular"}) == L"Consolas",
                "font family mapping");
    }
    const std::u16string sample{u'a', static_cast<char16_t>(0xd83d), static_cast<char16_t>(0xde00), u'b'};
    require(drawing::utf16(drawing::utf8(sample)) == sample && drawing::utf8(sample).size() == 6, "UTF-8 / UTF-16 conversion");
}

// ---- NIBArchive ------------------------------------------------------------------------------
// Builds archives value by value.
struct NibBuilder {
    nib::Archive archive;
    std::uint32_t class_index(const std::string& name) {
        const auto found = std::find(archive.class_names.begin(), archive.class_names.end(), name);
        if (found != archive.class_names.end()) return static_cast<std::uint32_t>(found - archive.class_names.begin());
        archive.class_names.push_back(name);
        return static_cast<std::uint32_t>(archive.class_names.size() - 1);
    }
    std::uint32_t key(const std::string& name) {
        const auto found = std::find(archive.keys.begin(), archive.keys.end(), name);
        if (found != archive.keys.end()) return static_cast<std::uint32_t>(found - archive.keys.begin());
        archive.keys.push_back(name);
        return static_cast<std::uint32_t>(archive.keys.size() - 1);
    }
    // Objects are declared first (so references can point forward), then filled.
    std::uint32_t declare(const std::string& cls) {
        archive.objects.push_back({class_index(cls), 0, 0});
        pending.emplace_back();
        return static_cast<std::uint32_t>(archive.objects.size() - 1);
    }
    std::vector<std::vector<nib::Value>> pending;
    void put(std::uint32_t object, const std::string& name, nib::Value value) {
        value.key = key(name);
        pending.at(object).push_back(std::move(value));
    }
    void ref(std::uint32_t object, const std::string& name, std::uint32_t target) {
        nib::Value v; v.type = nib::ValueType::object; v.integer = target; put(object, name, v);
    }
    void integer(std::uint32_t object, const std::string& name, std::int64_t value) {
        nib::Value v; v.type = nib::ValueType::int32; v.integer = value; put(object, name, v);
    }
    void boolean(std::uint32_t object, const std::string& name, bool value) {
        nib::Value v; v.type = value ? nib::ValueType::true_ : nib::ValueType::false_; v.integer = value; put(object, name, v);
    }
    std::uint32_t string(const std::string& text) {
        const auto object = declare("NSString");
        nib::Value v; v.type = nib::ValueType::data; v.data.assign(text.begin(), text.end()); put(object, "NS.bytes", v);
        return object;
    }
    std::uint32_t array(const std::string& cls, const std::vector<std::uint32_t>& elements) {
        const auto object = declare(cls);
        boolean(object, "NSInlinedValue", false);
        for (const auto element : elements) ref(object, "UINibEncoderEmptyKey", element);
        return object;
    }
    nib::Archive finish() {
        archive.values.clear();
        for (std::size_t i = 0; i < archive.objects.size(); ++i) {
            archive.objects[i].first_value = static_cast<std::uint32_t>(archive.values.size());
            archive.objects[i].value_count = static_cast<std::uint32_t>(pending[i].size());
            archive.values.insert(archive.values.end(), pending[i].begin(), pending[i].end());
        }
        archive.coder_version = 10;
        return archive;
    }
};

// A window with a button whose outlet and action connect to the owner.
nib::Archive sample_nib() {
    NibBuilder b;
    const auto root = b.declare("NSObject");
    const auto data = b.declare("NSIBObjectData");
    const auto owner = b.declare("NSCustomObject");
    const auto window_template = b.declare("NSWindowTemplate");
    const auto content = b.declare("NSView");
    const auto button = b.declare("NSButton");
    const auto button_cell = b.declare("NSButtonCell");
    const auto outlet = b.declare("NSNibOutletConnector");
    const auto action = b.declare("NSNibControlConnector");
    b.ref(root, "IB.objectdata", data);
    b.integer(root, "IB.systemFontUpdateVersion", 1);
    b.ref(owner, "NSClassName", b.string("TestNibOwner"));
    b.integer(window_template, "NSWindowStyleMask", 7);
    b.integer(window_template, "NSWindowBacking", 2);
    b.ref(window_template, "NSWindowRect", b.string("{{100, 100}, {300, 200}}"));
    b.ref(window_template, "NSWindowTitle", b.string("Nib Window"));
    b.ref(window_template, "NSWindowClass", b.string("NSWindow"));
    b.ref(window_template, "NSWindowView", content);
    b.integer(content, "NSvFlags", 256);
    b.ref(content, "NSFrameSize", b.string("{300, 200}"));
    b.ref(content, "NSSubviews", b.array("NSMutableArray", {button}));
    b.integer(button, "NSvFlags", 268);
    b.ref(button, "NSFrame", b.string("{{20, 20}, {100, 32}}"));
    b.ref(button, "NSSuperview", content);
    b.ref(button, "NSCell", button_cell);
    b.boolean(button, "NSEnabled", true);
    b.integer(button_cell, "NSCellFlags", 0x04000000);
    b.integer(button_cell, "NSCellFlags2", 0x08000000);
    b.ref(button_cell, "NSContents", b.string("Press"));
    b.integer(button_cell, "NSBezelStyle", 1);
    b.integer(button_cell, "NSButtonFlags", static_cast<std::int32_t>(0x84000000u | 0x800000));
    b.ref(button_cell, "NSControlView", button);
    b.ref(outlet, "NSSource", owner);
    b.ref(outlet, "NSDestination", button);
    b.ref(outlet, "NSLabel", b.string("button"));
    b.ref(action, "NSSource", button);
    b.ref(action, "NSDestination", owner);
    b.ref(action, "NSLabel", b.string("pressed:"));
    b.ref(data, "NSRoot", owner);
    b.ref(data, "NSVisibleWindows", b.array("NSMutableSet", {window_template}));
    b.ref(data, "NSConnections", b.array("NSMutableArray", {outlet, action}));
    b.ref(data, "NSObjectsKeys", b.array("NSArray", {window_template, content, button}));
    b.ref(data, "NSObjectsValues", b.array("NSArray", {owner, window_template, content}));
    return b.finish();
}

// ---- keyed-archive nibs ----------------------------------------------------------------------
// A minimal binary property list writer (2-byte object references, 4-byte offsets).
struct PlistWriter {
    struct Node {
        enum class Kind { string, integer, boolean, uid, data, array, dict } kind;
        std::string text;
        std::int64_t integer{};
        std::vector<std::size_t> items;
        std::vector<std::pair<std::size_t, std::size_t>> entries;
    };
    std::vector<Node> nodes;
    std::size_t add(Node node) { nodes.push_back(std::move(node)); return nodes.size() - 1; }
    std::size_t string(const std::string& text) { return add({Node::Kind::string, text}); }
    std::size_t integer(std::int64_t value) { Node n{Node::Kind::integer}; n.integer = value; return add(n); }
    std::size_t boolean(bool value) { Node n{Node::Kind::boolean}; n.integer = value; return add(n); }
    std::size_t uid(std::uint64_t value) { Node n{Node::Kind::uid}; n.integer = static_cast<std::int64_t>(value); return add(n); }
    std::size_t data(const std::vector<std::uint8_t>& bytes) { Node n{Node::Kind::data}; n.text.assign(bytes.begin(), bytes.end()); return add(n); }
    std::size_t array(std::vector<std::size_t> items) { Node n{Node::Kind::array}; n.items = std::move(items); return add(n); }
    std::size_t dict(std::vector<std::pair<std::size_t, std::size_t>> entries) { Node n{Node::Kind::dict}; n.entries = std::move(entries); return add(n); }
    std::vector<std::uint8_t> encode(std::size_t top) const {
        std::vector<std::uint8_t> out{'b', 'p', 'l', 'i', 's', 't', '0', '0'};
        const auto be = [&out](std::uint64_t value, int bytes) { for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(value >> (i * 8))); };
        const auto marker = [&](std::uint8_t type, std::size_t count) {
            if (count < 15) { out.push_back(static_cast<std::uint8_t>((type << 4) | count)); return; }
            out.push_back(static_cast<std::uint8_t>((type << 4) | 0xf));
            out.push_back(0x13);
            be(count, 8);
        };
        std::vector<std::uint64_t> offsets;
        for (const auto& n : nodes) {
            offsets.push_back(out.size());
            switch (n.kind) {
            case Node::Kind::string: marker(0x5, n.text.size()); out.insert(out.end(), n.text.begin(), n.text.end()); break;
            case Node::Kind::data: marker(0x4, n.text.size()); out.insert(out.end(), n.text.begin(), n.text.end()); break;
            case Node::Kind::integer: out.push_back(0x13); be(static_cast<std::uint64_t>(n.integer), 8); break;
            case Node::Kind::boolean: out.push_back(n.integer ? 0x09 : 0x08); break;
            case Node::Kind::uid: out.push_back(0x83); be(static_cast<std::uint64_t>(n.integer), 4); break;
            case Node::Kind::array: marker(0xa, n.items.size()); for (const auto i : n.items) be(i, 2); break;
            case Node::Kind::dict:
                marker(0xd, n.entries.size());
                for (const auto& e : n.entries) be(e.first, 2);
                for (const auto& e : n.entries) be(e.second, 2);
                break;
            }
        }
        const auto table = out.size();
        for (const auto offset : offsets) be(offset, 4);
        for (int i = 0; i < 6; ++i) out.push_back(0);
        out.push_back(4);
        out.push_back(2);
        be(nodes.size(), 8);
        be(top, 8);
        be(table, 8);
        return out;
    }
};

// The same object graph as an NSKeyedArchiver archive: NSString objects
// become plain $objects strings, collections NS.objects arrays, and the
// geometry keys inline strings (as -encodeRect: writes them).
std::vector<std::uint8_t> keyed_archive(const nib::Archive& archive) {
    PlistWriter w;
    std::vector<std::size_t> objects{w.string("$null")};
    std::map<std::string, std::uint64_t> class_uids;
    std::vector<std::uint64_t> uid_of(archive.objects.size());
    // $objects slots first (UIDs), contents after.
    for (std::uint32_t i = 1; i < archive.objects.size(); ++i) { uid_of[i] = objects.size(); objects.push_back(0); }
    const auto class_uid = [&](const std::string& name) {
        if (const auto found = class_uids.find(name); found != class_uids.end()) return found->second;
        const auto uid = objects.size();
        objects.push_back(w.dict({{w.string("$classname"), w.string(name)}, {w.string("$classes"), w.array({w.string(name), w.string("NSObject")})}}));
        return class_uids[name] = uid;
    };
    const auto value_node = [&](const nib::Value& v) -> std::size_t {
        switch (v.type) {
        case nib::ValueType::object: return w.uid(uid_of.at(static_cast<std::size_t>(v.integer)));
        case nib::ValueType::true_: case nib::ValueType::false_: return w.boolean(v.type == nib::ValueType::true_);
        case nib::ValueType::data: return w.data(v.data);
        case nib::ValueType::nil: return w.uid(0);
        default: return w.integer(v.integer);
        }
    };
    const std::set<std::string> geometry{"NSFrame", "NSFrameSize", "NSWindowRect"};
    for (std::uint32_t i = 1; i < archive.objects.size(); ++i) {
        const auto& cls = archive.class_name(i);
        if (const auto text = archive.string_value(i)) { objects[uid_of[i]] = w.string(*text); continue; }
        std::vector<std::pair<std::size_t, std::size_t>> entries{{w.string("$class"), w.uid(class_uid(cls))}};
        if (cls.find("Array") != std::string::npos || cls.find("Set") != std::string::npos) {
            std::vector<std::size_t> items;
            for (const auto element : archive.elements(i)) items.push_back(w.uid(uid_of[element]));
            entries.emplace_back(w.string("NS.objects"), w.array(items));
        } else {
            for (const auto& v : archive.values_of(i)) {
                const auto& key = archive.key_name(v);
                if (geometry.contains(key) && v.is_object()) entries.emplace_back(w.string(key), w.string(*archive.string_value(static_cast<std::uint32_t>(v.integer))));
                else entries.emplace_back(w.string(key), value_node(v));
            }
        }
        objects[uid_of[i]] = w.dict(std::move(entries));
    }
    std::vector<std::pair<std::size_t, std::size_t>> top;
    for (const auto& v : archive.values_of(0)) top.emplace_back(w.string(archive.key_name(v)), value_node(v));
    const auto root = w.dict({{w.string("$archiver"), w.string("NSKeyedArchiver")}, {w.string("$version"), w.integer(100000)},
                              {w.string("$top"), w.dict(std::move(top))}, {w.string("$objects"), w.array(objects)}});
    return w.encode(root);
}

void keyed_nibs() {
    const auto bytes = keyed_archive(sample_nib());
    require(nib::Archive::is_keyed_archive(bytes) && !nib::Archive::is_nib_archive(bytes), "keyed archive magic");
    const auto parsed = nib::Archive::from_keyed_archive(bytes);
    const auto graph = nib::read_graph(parsed);
    require(parsed.class_name(graph.root) == "NSCustomObject" && parsed.string_at(graph.root, "NSClassName") == "TestNibOwner" &&
                graph.visible_windows.size() == 1 && graph.top_level.size() == 1 &&
                parsed.class_name(graph.top_level[0]) == "NSWindowTemplate" &&
                parsed.string_at(graph.top_level[0], "NSWindowRect") == "{{100, 100}, {300, 200}}",
            "keyed nib graph");
    require(graph.connections.size() == 2 && graph.connections[0].label == "button" && graph.connections[1].label == "pressed:" &&
                graph.connections[1].kind == nib::Connection::Kind::action, "keyed nib connections");
    const auto rejects = [](std::vector<std::uint8_t> data, const char* what) {
        bool failed{};
        try { static_cast<void>(nib::Archive::from_keyed_archive(data)); } catch (const std::runtime_error&) { failed = true; }
        require(failed, std::string("corrupt keyed archive accepted: ") + what);
    };
    rejects(std::vector<std::uint8_t>(bytes.begin(), bytes.end() - 5), "truncated");
    auto bad_top = bytes;
    bad_top[bad_top.size() - 9] = 0xff;   // the top object index
    rejects(bad_top, "top object");
    auto bad_offsets = bytes;
    bad_offsets[bad_offsets.size() - 1] = 0xff;   // the offset table offset
    rejects(bad_offsets, "offset table");
    PlistWriter not_keyed;
    rejects(not_keyed.encode(not_keyed.dict({})), "not an NSKeyedArchiver archive");
}

void nib_archives() {
    const auto archive = sample_nib();
    const auto bytes = archive.encode();
    require(nib::Archive::is_nib_archive(bytes), "encoded archive magic");
    const auto parsed = nib::Archive::parse(bytes);
    require(parsed.objects.size() == archive.objects.size() && parsed.values.size() == archive.values.size() &&
                parsed.class_names == archive.class_names && parsed.keys == archive.keys, "NIBArchive round trip");
    const auto graph = nib::read_graph(parsed);
    require(parsed.class_name(graph.root) == "NSCustomObject" && graph.visible_windows.size() == 1 && graph.connections.size() == 2 &&
                graph.top_level.size() == 1 && parsed.class_name(graph.top_level[0]) == "NSWindowTemplate", "nib graph");
    require(graph.connections[0].kind == nib::Connection::Kind::outlet && graph.connections[0].label == "button" &&
                graph.connections[1].kind == nib::Connection::Kind::action && graph.connections[1].label == "pressed:", "nib connections");
    const auto numbers = nib::parse_numbers("{{1.5, -2}, {3, 4e1}}");
    require(numbers && numbers->size() == 4 && (*numbers)[0] == 1.5 && (*numbers)[1] == -2 && (*numbers)[3] == 40, "geometry strings");
    // Corrupt inputs fail explicitly.
    const auto rejects = [](std::vector<std::uint8_t> data, const char* what) {
        bool failed{};
        try { static_cast<void>(nib::Archive::parse(data)); } catch (const std::runtime_error&) { failed = true; }
        require(failed, std::string("corrupt archive accepted: ") + what);
    };
    rejects(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() - 3)), "truncated");
    auto bad_reference = bytes;
    // The object table's first entry: point its class index past the table.
    bad_reference[50] = 0xff;
    rejects(bad_reference, "class index");
    rejects({'b', 'p', 'l', 'i', 's', 't'}, "not an archive");
}

// Every compiled nib of the user's local QLab copy (read only; skipped when
// absent): the app's own, localized ones and the frameworks' (Sparkle), each
// variant of a nib bundle (keyedobjects.nib, keyedobjects-101300.nib, ...).
void qlab_nibs() {
    std::filesystem::path app = "C:/dev/transmute/artifacts/dmg/qlab/QLab 5.6.3/QLab.app";
    if (const char* override_path = std::getenv("ALCHEMY_QLAB_APP")) app = override_path;
    std::error_code error;
    if (!std::filesystem::is_directory(app, error)) {
        std::cout << "QLab nib sweep skipped: " << app.string() << " is absent\n";
        return;
    }
    std::size_t parsed{}, objects{}, connections{}, outlets{}, actions{}, windows{};
    std::map<std::string, std::size_t> classes;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(app, std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.path().extension() != ".nib" || !entry.is_regular_file()) continue;
        const auto& file = entry.path();
        std::ifstream stream(file, std::ios::binary);
        require(stream.good(), "cannot read " + file.string());
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        nib::Archive archive;
        try { archive = nib::Archive::parse_any(bytes); }
        catch (const std::exception& e) { throw std::runtime_error(file.string() + ": " + e.what()); }
        const auto graph = nib::read_graph(archive);
        ++parsed;
        objects += archive.objects.size();
        connections += graph.connections.size();
        for (const auto& c : graph.connections) {
            outlets += c.kind == nib::Connection::Kind::outlet;
            actions += c.kind == nib::Connection::Kind::action;
            require(c.kind == nib::Connection::Kind::other || (c.source && !c.label.empty()), "connection without source or label");
        }
        for (std::uint32_t i = 0; i < archive.objects.size(); ++i) {
            ++classes[archive.class_name(i)];
            if (archive.class_name(i) == "NSWindowTemplate") ++windows;
        }
    }
    require(parsed > 0, "no QLab nibs parsed");
    std::cout << "QLab nibs: " << parsed << " parsed, " << objects << " objects, " << windows << " window templates, " << connections
              << " connections (" << outlets << " outlets, " << actions << " actions), " << classes.size() << " classes\n";
}

// ---- compiled asset catalogs (Assets.car) ------------------------------------------------------
// A BOMStore writer for the tests: block 0 is the null block.
struct BomWriter {
    std::vector<std::vector<std::uint8_t>> blocks{{}};
    std::vector<std::pair<std::string, std::uint32_t>> variables;
    static void be32(std::vector<std::uint8_t>& out, std::uint32_t v) { for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(v >> s)); }
    static void be16(std::vector<std::uint8_t>& out, std::uint16_t v) { out.push_back(static_cast<std::uint8_t>(v >> 8)); out.push_back(static_cast<std::uint8_t>(v)); }
    static void le32(std::vector<std::uint8_t>& out, std::uint32_t v) { for (int s = 0; s < 32; s += 8) out.push_back(static_cast<std::uint8_t>(v >> s)); }
    static void le16(std::vector<std::uint8_t>& out, std::uint16_t v) { out.push_back(static_cast<std::uint8_t>(v)); out.push_back(static_cast<std::uint8_t>(v >> 8)); }
    std::uint32_t add(std::vector<std::uint8_t> bytes) { blocks.push_back(std::move(bytes)); return static_cast<std::uint32_t>(blocks.size() - 1); }
    std::uint32_t add_text(std::string_view text) { return add(std::vector<std::uint8_t>(text.begin(), text.end())); }
    std::uint32_t node(bool leaf, std::uint32_t forward, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& entries) {
        std::vector<std::uint8_t> out;
        be16(out, leaf ? 1 : 0); be16(out, static_cast<std::uint16_t>(entries.size())); be32(out, forward); be32(out, 0);
        for (const auto& [value, key] : entries) { be32(out, value); be32(out, key); }
        return add(std::move(out));
    }
    std::uint32_t tree(std::uint32_t root, std::uint32_t paths) {
        std::vector<std::uint8_t> out{'t', 'r', 'e', 'e'};
        be32(out, 1); be32(out, root); be32(out, 4096); be32(out, paths); out.push_back(0);
        return add(std::move(out));
    }
    std::vector<std::uint8_t> finish() const {
        std::vector<std::uint8_t> data;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> index;
        for (const auto& block : blocks) {
            index.emplace_back(block.empty() ? 0 : static_cast<std::uint32_t>(32 + data.size()), static_cast<std::uint32_t>(block.size()));
            data.insert(data.end(), block.begin(), block.end());
        }
        std::vector<std::uint8_t> index_bytes, variable_bytes;
        be32(index_bytes, static_cast<std::uint32_t>(index.size()));
        for (const auto& [address, length] : index) { be32(index_bytes, address); be32(index_bytes, length); }
        be32(variable_bytes, static_cast<std::uint32_t>(variables.size()));
        for (const auto& [name, block] : variables) {
            be32(variable_bytes, block); variable_bytes.push_back(static_cast<std::uint8_t>(name.size()));
            variable_bytes.insert(variable_bytes.end(), name.begin(), name.end());
        }
        std::vector<std::uint8_t> out{'B', 'O', 'M', 'S', 't', 'o', 'r', 'e'};
        const auto index_offset = static_cast<std::uint32_t>(32 + data.size());
        be32(out, 1); be32(out, static_cast<std::uint32_t>(blocks.size()));
        be32(out, index_offset); be32(out, static_cast<std::uint32_t>(index_bytes.size()));
        be32(out, index_offset + static_cast<std::uint32_t>(index_bytes.size())); be32(out, static_cast<std::uint32_t>(variable_bytes.size()));
        out.insert(out.end(), data.begin(), data.end());
        out.insert(out.end(), index_bytes.begin(), index_bytes.end());
        out.insert(out.end(), variable_bytes.begin(), variable_bytes.end());
        return out;
    }
};

// A CSI rendition: layout 1009 named color ("RLOC") or another layout with opaque data.
std::vector<std::uint8_t> csi_rendition(std::uint16_t layout, std::uint32_t space, const std::vector<double>& components) {
    std::vector<std::uint8_t> out{'I', 'S', 'T', 'C'};
    for (int i = 0; i < 7; ++i) BomWriter::le32(out, i == 0 ? 1 : 0);   // version, flags, size, scale, format, color space
    BomWriter::le32(out, 0);                                          // modification time
    BomWriter::le16(out, layout); BomWriter::le16(out, 0);
    out.resize(out.size() + 128);                                     // name
    std::vector<std::uint8_t> data;
    if (layout == 1009) {
        data = {'R', 'L', 'O', 'C'};
        BomWriter::le32(data, 1); BomWriter::le32(data, space); BomWriter::le32(data, static_cast<std::uint32_t>(components.size()));
        for (const auto value : components) {
            const auto bits = std::bit_cast<std::uint64_t>(value);
            for (int s = 0; s < 64; s += 8) data.push_back(static_cast<std::uint8_t>(bits >> s));
        }
    } else {
        data = {'M', 'L', 'E', 'C', 0, 0, 0, 0};
    }
    BomWriter::le32(out, 4); BomWriter::le32(out, 0); BomWriter::le32(out, 0); BomWriter::le32(out, static_cast<std::uint32_t>(data.size()));
    out.insert(out.end(), {0xee, 0xee, 0xee, 0xee});                  // TLV bytes (skipped)
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::vector<std::uint8_t> sample_asset_catalog() {
    BomWriter w;
    // Rendition keys: (appearance, name id, scale).
    std::vector<std::uint8_t> format{'t', 'm', 'f', 'k'};
    BomWriter::le32(format, 0); BomWriter::le32(format, 3);
    for (const std::uint32_t attribute : {7u, 17u, 12u}) BomWriter::le32(format, attribute);
    const auto facet = [](std::uint16_t name_id) {
        std::vector<std::uint8_t> out;
        BomWriter::le16(out, 0); BomWriter::le16(out, 0); BomWriter::le16(out, 1); BomWriter::le16(out, 17); BomWriter::le16(out, name_id);
        return out;
    };
    const auto key = [](std::uint16_t appearance, std::uint16_t name_id) {
        std::vector<std::uint8_t> out;
        BomWriter::le16(out, appearance); BomWriter::le16(out, name_id); BomWriter::le16(out, 1);
        return out;
    };
    const auto id_value = [](std::uint16_t id) { std::vector<std::uint8_t> out; BomWriter::le16(out, id); return out; };
    const auto facets_leaf = w.node(true, 0, {{w.add(facet(5)), w.add_text("accent")}, {w.add(facet(6)), w.add_text("shade")},
                                              {w.add(facet(7)), w.add_text("icon")}});
    const auto appearance_leaf = w.node(true, 0, {{w.add(id_value(0)), w.add_text("NSAppearanceNameSystem")},
                                                  {w.add(id_value(1)), w.add_text("NSAppearanceNameDarkAqua")}});
    // RENDITIONS: a branch over two chained leaves.
    const auto leaf2 = w.node(true, 0, {{w.add(csi_rendition(1009, 6, {0.25, 0.5})), w.add(key(0, 6))},
                                        {w.add(csi_rendition(1003, 0, {})), w.add(key(0, 7))}});
    const auto leaf1 = w.node(true, leaf2, {{w.add(csi_rendition(1009, 1, {0.1, 0.2, 0.3, 1})), w.add(key(0, 5))},
                                            {w.add(csi_rendition(1009, 1, {0.9, 0.8, 0.7, 0.5})), w.add(key(1, 5))}});
    const auto branch = w.node(false, 0, {{leaf1, w.add(key(0, 5))}, {leaf2, w.add(key(0, 6))}});
    const auto format_block = w.add(std::move(format));
    const auto facets_tree = w.tree(facets_leaf, 3), appearance_tree = w.tree(appearance_leaf, 2), renditions_tree = w.tree(branch, 4);
    w.variables = {{"KEYFORMAT", format_block}, {"FACETKEYS", facets_tree}, {"APPEARANCEKEYS", appearance_tree}, {"RENDITIONS", renditions_tree}};
    return w.finish();
}

void asset_catalogs() {
    using alchemy::gui::car::AssetCatalog;
    const auto bytes = sample_asset_catalog();
    require(AssetCatalog::is_asset_catalog(bytes), "BOMStore magic");
    const auto catalog = AssetCatalog::parse(bytes);
    require(catalog.color_count() == 2, "two named colors (the image rendition is skipped)");
    const auto light = catalog.color("accent", "NSAppearanceNameAqua");
    require(light && light->appearance == "NSAppearanceNameSystem" && light->color_space == 1
                && light->components == std::vector<double>{0.1, 0.2, 0.3, 1},
            "accent falls back to the System appearance");
    const auto dark = catalog.color("accent", "NSAppearanceNameDarkAqua");
    require(dark && dark->components == std::vector<double>{0.9, 0.8, 0.7, 0.5}, "accent's DarkAqua variant");
    const auto shade = catalog.color("shade", "NSAppearanceNameDarkAqua");
    require(shade && shade->color_space == 6 && shade->components == std::vector<double>{0.25, 0.5}, "gray named color");
    require(!catalog.color("icon", "") && !catalog.color("missing", ""), "names without a color give nothing");
    // Truncations fail as a whole.
    for (const std::size_t size : {std::size_t{16}, bytes.size() / 2, bytes.size() - 3}) {
        bool threw = false;
        try { static_cast<void>(AssetCatalog::parse(std::span<const std::uint8_t>(bytes).first(size))); }
        catch (const std::runtime_error&) { threw = true; }
        require(threw, "truncated catalog (" + std::to_string(size) + " bytes) is rejected");
    }
    auto corrupt = bytes;
    corrupt[0] = 'X';
    require(!AssetCatalog::is_asset_catalog(corrupt), "bad magic is not a catalog");
}

// RTF text extraction: escapes, destinations, paragraphs, Unicode fallbacks.
void rtf_documents() {
    namespace rtf = alchemy::gui::rtf;
    const std::string sample =
        "{\\rtf1\\ansi\\ansicpg1252{\\fonttbl\\f0\\fswiss Helvetica;}{\\colortbl;\\red255\\green0\\blue0;}\n"
        "{\\*\\expandedcolortbl;;}\\pard\\qc\\f0\\b\\fs36 Title\\\n"
        "\\b0 caf\\'e9 \\{x\\} a\\tab b\\par \\u8364? \\uc2\\u8211 ab end\\line \\ldblquote q\\rdblquote}";
    const std::vector<std::uint8_t> bytes(sample.begin(), sample.end());
    require(rtf::looks_like_rtf(bytes), "RTF magic");
    const auto text = rtf::plain_text(bytes);
    const std::u16string expected = u"Title\ncaf\u00e9 {x} a\tb\n\u20ac \u2013 end\n\u201cq\u201d";
    require(text == expected, "RTF plain text");
    for (const auto* broken : {"{\\rtf1 {unterminated", "{\\rtf1 \\'4", "{\\rtf1\\ansicpg932 x}"}) {
        const std::string input(broken);
        bool threw = false;
        try { static_cast<void>(rtf::plain_text(std::vector<std::uint8_t>(input.begin(), input.end()))); } catch (const std::runtime_error&) { threw = true; }
        require(threw, std::string("malformed RTF rejected: ") + broken);
    }
    // The formatting subset: fonts, sizes, bold, colours, alignment; and writing it back.
    const auto runs = rtf::runs(bytes);
    require(runs.size() >= 2 && runs[0].text == u"Title\n" && runs[0].format.bold && runs[0].format.size == 18 && runs[0].format.font == "Helvetica" &&
                runs[0].format.alignment == 1 && !runs[1].format.bold && runs[1].format.alignment == 1,
            "RTF runs keep the font, size, weight and alignment");
    const std::string coloured = "{\\rtf1\\ansi{\\fonttbl\\f0 Times;}{\\colortbl;\\red255\\green0\\blue0;}\\f0\\fs24 plain \\cf1\\ul\\i red\\ulnone\\i0\\cf0  \\pard\\qr\\li720\\fi-360 right}";
    const auto coloured_runs = rtf::runs(std::vector<std::uint8_t>(coloured.begin(), coloured.end()));
    require(coloured_runs.size() == 4 && coloured_runs[1].text == u"red" && coloured_runs[1].format.color &&
                (*coloured_runs[1].format.color)[0] == 1.0 && coloured_runs[1].format.underline && coloured_runs[1].format.italic &&
                !coloured_runs[2].format.color && coloured_runs[3].format.alignment == 2 && coloured_runs[3].format.left_indent == 36 &&
                coloured_runs[3].format.first_indent == -18 && coloured_runs[3].format.font == "Times",
            "RTF colours, underline, italic, alignment and indents");
    for (const auto* source : {&runs, &coloured_runs}) {
        const auto written = rtf::write(*source);
        const auto again = rtf::runs(std::vector<std::uint8_t>(written.begin(), written.end()));
        std::u16string before, after;
        for (const auto& run : *source) before += run.text;
        for (const auto& run : again) after += run.text;
        bool same = before == after && again.size() == source->size();
        for (std::size_t i = 0; same && i < again.size(); ++i) same = again[i].format == (*source)[i].format && again[i].text == (*source)[i].text;
        require(same, "RTF written and read back keeps the text and its formatting: " + written);
    }
}

// QLab's license agreement (read only; skipped when absent): it parses and starts with its title.
void qlab_rtf() {
    std::filesystem::path app = "C:/dev/transmute/artifacts/dmg/qlab/QLab 5.6.3/QLab.app";
    if (const char* override_path = std::getenv("ALCHEMY_QLAB_APP")) app = override_path;
    const auto file = app / "Contents/Resources/Base.lproj/eula.rtf";
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) { std::cout << "QLab RTF skipped: " << file.string() << " is absent\n"; return; }
    std::ifstream stream(file, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const auto text = alchemy::gui::rtf::plain_text(bytes);
    require(text.find(u"QLab Software License Agreement") != std::u16string::npos, "QLab's eula.rtf has its title");
    std::cout << "QLab RTF: " << text.size() << " characters\n";
}

// QLab's compiled asset catalog (read only; skipped when absent).
void qlab_asset_catalog() {
    std::filesystem::path app = "C:/dev/transmute/artifacts/dmg/qlab/QLab 5.6.3/QLab.app";
    if (const char* override_path = std::getenv("ALCHEMY_QLAB_APP")) app = override_path;
    const auto file = app / "Contents/Resources/Assets.car";
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) {
        std::cout << "QLab asset catalog skipped: " << file.string() << " is absent\n";
        return;
    }
    std::ifstream stream(file, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const auto catalog = alchemy::gui::car::AssetCatalog::parse(bytes);
    require(catalog.color_count() > 100, "QLab names its colors in Assets.car");
    const auto background = catalog.color("darkGrayBackgroundColor", "NSAppearanceNameDarkAqua");
    require(background && background->components.size() == 4 && std::abs(background->components[0] - 0.15) < 1e-6, "darkGrayBackgroundColor");
    // yellowTextColor also has an increased-contrast variant; the System one is chosen.
    const auto yellow = catalog.color("yellowTextColor", "NSAppearanceNameDarkAqua");
    require(yellow && yellow->appearance == "NSAppearanceNameSystem", "yellowTextColor resolves to the System appearance");
    std::size_t unknown_spaces{};
    for (const auto& name : catalog.color_names())
        for (const auto& variant : *catalog.color_variants(name))
            unknown_spaces += variant.color_space != 1 && variant.color_space != 4 && variant.color_space != 6;
    require(unknown_spaces == 0, "every QLab named color uses a known color space");
    std::cout << "QLab asset catalog: " << catalog.color_count() << " named colors\n";
}

// ---- AppKit over the fake window server ---------------------------------------------------
struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> bytes) {
        return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices darwin{process};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    std::unique_ptr<alchemy::FoundationBridge> foundation;
    gui::testing::FakeWindowServer server;
    Address app{};

    // AppKit with the Quartz 2D layer installed after it, as in
    // install_darwin_gui (NSGraphicsContext, NSFont, NSBezierPath, NSColor
    // -set and NSString / NSImage drawing are Quartz 2D's).
    Fixture() {
        alchemy::install_darwin_numeric(process);
        foundation = std::make_unique<alchemy::FoundationBridge>(process, objc, cf);
        alchemy::install_darwin_frameworks(process, objc, cf);
        gui::appkit::set_window_server_override(&server);
        gui::InstallContext context{process, objc, cf};
        gui::install_appkit_gui(context);
        gui::install_quartzcore(context);
        gui::install_quartz2d(context);
        app = id(cls("NSApplication"), "sharedApplication");
        static_cast<void>(id(app, "finishLaunching"));
        // Deterministic colors: the light appearance.
        static_cast<void>(id(app, "setAppearance:", {id(cls("NSAppearance"), "appearanceNamed:", {str("NSAppearanceNameAqua")})}));
    }
    ~Fixture() { gui::appkit::set_window_server_override(nullptr); }

    Address cls(std::string_view name) {
        const auto value = objc.find_class(name);
        require(value != 0, "missing class " + std::string(name));
        return value;
    }
    CallResult send(Address receiver, std::string_view selector, CallRegs registers = {}) {
        return objc.send(receiver, objc.selector(selector), std::move(registers));
    }
    Address id(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        CallRegs registers;
        std::size_t index = 2;
        for (const auto value : arguments) registers.x[index++] = value;
        return send(receiver, selector, registers).x0;
    }
    Address str(std::string_view text) { return cf.string(text); }
    std::string text(Address object) { return object ? cf.utf8(object) : std::string("(nil)"); }
    Address subclass(std::string name, std::string_view super) { return objc.register_native_class(std::move(name), cls(super), 8); }
    void add(Address target, std::string name, transmute::GuestCallHandler handler, std::string types = "v@:@") {
        static_cast<void>(objc.add_native_method(target, std::move(name), std::move(handler), false, std::move(types)));
    }
    Address next_event() { return id(app, "nextEventMatchingMask:untilDate:inMode:dequeue:", {~0ULL, 0, 0, 1}); }
    void pump() { while (const auto event = next_event()) static_cast<void>(id(app, "sendEvent:", {event})); }
    Address with_rect(Address receiver, std::string_view selector, gui::Rect rect) {
        CallRegs registers;
        gui::appkit::put_rect(registers, 0, rect);
        return send(receiver, selector, registers).x0;
    }
    Address make_window(gui::Rect content) {
        CallRegs init;
        gui::appkit::put_rect(init, 0, content);
        init.x[2] = gui::style::titled | gui::style::closable;
        init.x[3] = 2;
        init.x[4] = 0;
        const auto made = send(id(cls("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", init).x0;
        static_cast<void>(id(made, "setReleasedWhenClosed:", {0}));
        static_cast<void>(id(made, "setBackgroundColor:", {id(cls("NSColor"), "whiteColor")}));
        return made;
    }
    // A press and release of the left button at `location` (window points).
    void click(Address window, gui::Point location, std::int64_t clicks = 1) {
        const auto number = id(window, "windowNumber");
        gui::Event down;
        down.type = gui::event_type::left_mouse_down;
        down.window = number;
        down.location = location;
        down.click_count = clicks;
        down.pressed_buttons = 1;
        server.queue(down);
        auto up = down;
        up.type = gui::event_type::left_mouse_up;
        up.pressed_buttons = 0;
        server.queue(up);
        pump();
    }
    void key(Address window, std::uint16_t code, std::string characters) {
        gui::Event press;
        press.type = gui::event_type::key_down;
        press.window = id(window, "windowNumber");
        press.key_code = code;
        press.characters = characters;
        press.characters_ignoring_modifiers = characters;
        server.queue(press);
        pump();
    }
    std::array<std::uint8_t, 4> pixel(Address window, double x, double y) { return server.pixel(id(window, "windowNumber"), x, y); }
};

bool is_red(std::array<std::uint8_t, 4> bgra) { return bgra[2] > 200 && bgra[1] < 40 && bgra[0] < 40 && bgra[3] == 255; }
bool is_white(std::array<std::uint8_t, 4> bgra) { return bgra[0] > 245 && bgra[1] > 245 && bgra[2] > 245; }

void view_drawing(Fixture& f) {
    journal.clear();
    // A guest NSView subclass filling its bounds' lower-left 10x10 square red.
    const auto paint = f.subclass("TestPaintView", "NSView");
    f.add(paint, "drawRect:", [&f](CpuState& s, GuestMemory&) {
        journal.push_back("paint");
        static_cast<void>(f.id(f.id(f.cls("NSColor"), "redColor"), "set"));
        CallRegs registers;
        gui::appkit::put_rect(registers, 0, {{0, 0}, {10, 10}});
        static_cast<void>(f.send(f.cls("NSBezierPath"), "fillRect:", registers));
        static_cast<void>(s);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    const auto flipped = f.subclass("TestFlippedPaintView", "TestPaintView");
    f.add(flipped, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");

    const auto window = f.make_window({{100, 100}, {200, 100}});
    const auto content = f.id(window, "contentView");
    const auto plain = f.with_rect(f.id(paint, "alloc"), "initWithFrame:", {{20, 20}, {50, 50}});
    const auto upside = f.with_rect(f.id(flipped, "alloc"), "initWithFrame:", {{100, 20}, {50, 50}});
    static_cast<void>(f.id(content, "addSubview:", {plain}));
    static_cast<void>(f.id(content, "addSubview:", {upside}));
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    static_cast<void>(f.next_event());   // the event loop displays pending windows
    const auto number = f.id(window, "windowNumber");
    require(f.server.screens_by_window.contains(number) && f.server.screens_by_window[number].presents > 0, "the window was not presented");
    const auto& screen = f.server.screens_by_window[number];
    require(screen.width == 400 && screen.height == 200, "backing store size (scale 2)");
    require(is_red(f.pixel(window, 25, 25)) && is_white(f.pixel(window, 35, 35)), "unflipped view drew its lower-left square");
    require(is_red(f.pixel(window, 105, 65)) && is_white(f.pixel(window, 105, 25)), "flipped view drew its upper-left square");
    require(is_white(f.pixel(window, 190, 90)), "window background");
    // Dirty rectangles: only the invalidated part is redrawn and presented.
    journal.clear();
    const auto presents = screen.presents;
    CallRegs dirty;
    gui::appkit::put_rect(dirty, 0, {{0, 0}, {5, 5}});
    static_cast<void>(f.send(plain, "setNeedsDisplayInRect:", dirty));
    static_cast<void>(f.next_event());
    require(log_has("paint") && std::count(journal.begin(), journal.end(), "paint") == 1, "only the dirty view redrew");
    require(screen.presents == presents + 1 && screen.last_rect && screen.last_rect->size.width <= 6, "the dirty rect was presented");
    // Hidden views do not draw.
    static_cast<void>(f.id(upside, "setHidden:", {1}));
    static_cast<void>(f.id(content, "setNeedsDisplay:", {1}));
    static_cast<void>(f.next_event());
    require(is_white(f.pixel(window, 105, 65)), "a hidden view drew");
    // -lockFocus drawing outside -drawRect: is presented by -unlockFocus.
    static_cast<void>(f.id(plain, "lockFocus"));
    static_cast<void>(f.id(f.id(f.cls("NSColor"), "blueColor"), "set"));
    CallRegs fill;
    gui::appkit::put_rect(fill, 0, {{30, 30}, {5, 5}});
    static_cast<void>(f.send(f.cls("NSBezierPath"), "fillRect:", fill));
    static_cast<void>(f.id(plain, "unlockFocus"));
    const auto blue = f.pixel(window, 52, 52);
    require(blue[0] > 200 && blue[2] < 40, "-lockFocus drawing was not presented");
    require(f.id(f.cls("NSGraphicsContext"), "currentContext") == 0, "the current context leaked out of -unlockFocus");
    // NSString drawing measures with the font.
    const auto attributes = f.cf.dictionary({});
    const auto size = f.send(f.str("Hello"), "sizeWithAttributes:", [&] { CallRegs r; r.x[2] = attributes; return r; }());
    require(size.d(0) > 15 && size.d(0) < 60 && size.d(1) > 10 && size.d(1) < 20, "-sizeWithAttributes:");
    static_cast<void>(f.id(window, "close"));
}

void layer_hosting_window(Fixture& f) {
    // Blender's case: one view with a CAMetalLayer, drawing nothing itself
    // into the window: no backing store, no presentation, -drawRect: still runs.
    journal.clear();
    const auto metal_view = f.subclass("TestMetalView", "NSView");
    f.add(metal_view, "drawRect:", [](CpuState&, GuestMemory&) { journal.push_back("metal drawRect"); }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    const auto window = f.make_window({{0, 0}, {320, 240}});
    const auto view = f.with_rect(f.id(metal_view, "alloc"), "initWithFrame:", {{0, 0}, {320, 240}});
    static_cast<void>(f.id(view, "setWantsLayer:", {1}));
    static_cast<void>(f.id(view, "setLayer:", {f.id(f.cls("CAMetalLayer"), "layer")}));
    static_cast<void>(f.id(window, "setContentView:", {view}));
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    static_cast<void>(f.id(view, "setNeedsDisplay:", {1}));
    static_cast<void>(f.next_event());
    const auto number = f.id(window, "windowNumber");
    require(log_has("metal drawRect"), "the layer-hosting view's -drawRect: did not run");
    require(!f.server.screens_by_window.contains(number), "a layer-hosting window presented a backing store");
    static_cast<void>(f.id(window, "close"));
}

void buttons(Fixture& f) {
    journal.clear();
    const auto target_class = f.subclass("TestControlTarget", "NSObject");
    f.add(target_class, "clicked:", [&f](CpuState& s, GuestMemory&) {
        journal.push_back("clicked:" + f.text(f.id(s.x[2], "title")) + ":" + std::to_string(f.id(s.x[2], "state")));
    });
    const auto target = f.id(f.id(target_class, "alloc"), "init");
    const auto window = f.make_window({{0, 0}, {300, 200}});
    const auto content = f.id(window, "contentView");
    const auto make = [&](gui::Rect frame, std::int64_t type, std::string_view title) {
        const auto made = f.with_rect(f.id(f.cls("NSButton"), "alloc"), "initWithFrame:", frame);
        static_cast<void>(f.id(made, "setButtonType:", {static_cast<Address>(type)}));
        static_cast<void>(f.id(made, "setTitle:", {f.str(title)}));
        static_cast<void>(f.id(made, "setTarget:", {target}));
        static_cast<void>(f.id(made, "setAction:", {f.objc.selector("clicked:")}));
        static_cast<void>(f.id(content, "addSubview:", {made}));
        return made;
    };
    const auto check = make({{20, 150}, {120, 20}}, 3, "Check");
    const auto radio_a = make({{20, 120}, {120, 20}}, 4, "A");
    const auto radio_b = make({{20, 90}, {120, 20}}, 4, "B");
    const auto push = make({{150, 20}, {100, 32}}, 7, "OK");
    static_cast<void>(f.id(push, "setKeyEquivalent:", {f.str("\r")}));
    require(f.text(f.id(check, "title")) == "Check" && f.id(f.id(check, "cell"), "controlView") == check, "button cell setup");
    const auto fitting = f.send(push, "fittingSize");
    require(fitting.d(0) >= 32 && fitting.d(1) == 32, "push button fitting size");
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    static_cast<void>(f.next_event());
    // Checkbox: a click toggles the state and sends the action.
    f.click(window, {30, 160});
    require(f.id(check, "state") == 1 && log_has("clicked:Check:1"), "checkbox click");
    const auto mark = f.pixel(window, 25, 156);   // inside the box, off the check mark
    require(mark[0] > 180 && mark[2] < 120, "the checked box is drawn in the accent color");
    // A release outside the button does not click.
    journal.clear();
    gui::Event down;
    down.type = gui::event_type::left_mouse_down;
    down.window = f.id(window, "windowNumber");
    down.location = {30, 160};
    down.click_count = 1;
    down.pressed_buttons = 1;
    f.server.queue(down);
    auto away = down;
    away.type = gui::event_type::left_mouse_up;
    away.location = {280, 10};
    away.pressed_buttons = 0;
    f.server.queue(away);
    f.pump();
    require(journal.empty() && f.id(check, "state") == 1, "a release outside the checkbox clicked it");
    // Radio buttons with the same action form a group.
    f.click(window, {30, 130});
    f.click(window, {30, 100});
    require(f.id(radio_a, "state") == 0 && f.id(radio_b, "state") == 1, "radio group");
    // Return clicks the default button through the key-equivalent path.
    journal.clear();
    f.key(window, 0x24, "\r");
    require(log_has("clicked:OK:0"), "Return did not click the default button");
    // Disabled buttons ignore clicks.
    journal.clear();
    static_cast<void>(f.id(check, "setEnabled:", {0}));
    f.click(window, {30, 160});
    require(journal.empty() && f.id(check, "state") == 1, "a disabled button clicked");
    static_cast<void>(f.id(window, "close"));
}

void text_fields(Fixture& f) {
    journal.clear();
    const auto delegate_class = f.subclass("TestFieldDelegate", "NSObject");
    f.add(delegate_class, "controlTextDidChange:", [](CpuState&, GuestMemory&) { journal.push_back("didChange"); });
    f.add(delegate_class, "controlTextDidEndEditing:", [](CpuState&, GuestMemory&) { journal.push_back("didEnd"); });
    f.add(delegate_class, "submitted:", [&f](CpuState& s, GuestMemory&) { journal.push_back("submitted:" + f.text(f.id(s.x[2], "stringValue"))); });
    const auto delegate = f.id(f.id(delegate_class, "alloc"), "init");
    const auto window = f.make_window({{0, 0}, {300, 120}});
    const auto content = f.id(window, "contentView");
    const auto field = f.with_rect(f.id(f.cls("NSTextField"), "alloc"), "initWithFrame:", {{20, 60}, {200, 24}});
    static_cast<void>(f.id(field, "setStringValue:", {f.str("old")}));
    static_cast<void>(f.id(field, "setDelegate:", {delegate}));
    static_cast<void>(f.id(field, "setTarget:", {delegate}));
    static_cast<void>(f.id(field, "setAction:", {f.objc.selector("submitted:")}));
    static_cast<void>(f.id(content, "addSubview:", {field}));
    const auto label = f.id(f.cls("NSTextField"), "labelWithString:", {f.str("Name:")});
    require((f.id(label, "isEditable") & 0xff) == 0 && (f.id(label, "isBezeled") & 0xff) == 0 && f.text(f.id(label, "stringValue")) == "Name:",
            "+labelWithString:");
    static_cast<void>(f.id(content, "addSubview:", {label}));
    const auto secure = f.with_rect(f.id(f.cls("NSSecureTextField"), "alloc"), "initWithFrame:", {{20, 20}, {200, 24}});
    static_cast<void>(f.id(content, "addSubview:", {secure}));
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    static_cast<void>(f.next_event());
    // A click starts editing: the field editor becomes the first responder.
    f.click(window, {40, 70});
    const auto editor = f.id(field, "currentEditor");
    require(editor && f.id(window, "firstResponder") == editor && (f.id(editor, "isFieldEditor") & 0xff), "editing did not start");
    require(f.id(editor, "delegate") == field && f.text(f.id(editor, "string")) == "old", "field editor state");
    require(f.server.at(f.id(window, "windowNumber")).text_input, "text input is not enabled for the field editor");
    // Typing replaces the selection (all of it after the click... the
    // click placed the caret): select all first, then type.
    static_cast<void>(f.id(editor, "selectAll:", {0}));
    f.key(window, 0x07, "x");
    f.key(window, 0x10, "y");
    require(f.text(f.id(editor, "string")) == "xy" && log_has("didChange") && f.text(f.id(field, "stringValue")) == "xy",
            "typing into the field editor");
    // IME composition and commit through NSTextInputClient.
    CallRegs marked;
    marked.x[2] = f.str("ab"); marked.x[3] = 2; marked.x[4] = 0; marked.x[5] = gui::appkit::not_found; marked.x[6] = 0;
    static_cast<void>(f.send(editor, "setMarkedText:selectedRange:replacementRange:", marked));
    require((f.id(editor, "hasMarkedText") & 0xff) && f.text(f.id(editor, "string")) == "xyab", "marked text");
    CallRegs commit;
    commit.x[2] = f.str("Z"); commit.x[3] = gui::appkit::not_found; commit.x[4] = 0;
    static_cast<void>(f.send(editor, "insertText:replacementRange:", commit));
    require(!(f.id(editor, "hasMarkedText") & 0xff) && f.text(f.id(editor, "string")) == "xyZ", "committed text replaces the marked text");
    // Backspace, then Return ends editing and sends the action.
    f.key(window, 0x33, "\x7f");
    journal.clear();
    f.key(window, 0x24, "\r");
    require(log_has("submitted:xy") && log_has("didEnd") && f.text(f.id(field, "stringValue")) == "xy", "Return in a text field");
    // Another first responder ends editing (a click on the background does
    // not, as in AppKit).
    f.click(window, {280, 110});
    require(f.id(field, "currentEditor") != 0, "a click on the window background ended editing");
    require((f.id(window, "makeFirstResponder:", {window}) & 0xff) == 1 && f.id(field, "currentEditor") == 0 &&
                f.id(window, "firstResponder") == window, "editing did not end with the first responder change");
    // The secure field shows bullets but keeps the text.
    static_cast<void>(f.id(secure, "setStringValue:", {f.str("secret")}));
    static_cast<void>(f.next_event());
    require(f.text(f.id(secure, "stringValue")) == "secret", "secure text field value");
    static_cast<void>(f.id(window, "close"));
}

void popups_and_progress(Fixture& f) {
    const auto popup = f.with_rect(f.id(f.cls("NSPopUpButton"), "alloc"), "initWithFrame:", {{0, 0}, {150, 26}});
    static_cast<void>(f.id(popup, "addItemsWithTitles:", {f.cf.array(std::vector<Address>{f.str("One"), f.str("Two"), f.str("Three")})}));
    require(f.id(popup, "numberOfItems") == 3 && f.id(popup, "indexOfSelectedItem") == 0 && f.text(f.id(popup, "titleOfSelectedItem")) == "One",
            "popup items");
    static_cast<void>(f.id(popup, "selectItemWithTitle:", {f.str("Three")}));
    require(f.id(popup, "indexOfSelectedItem") == 2 && f.text(f.id(f.id(popup, "selectedItem"), "title")) == "Three", "popup selection");
    static_cast<void>(f.id(popup, "removeItemAtIndex:", {2}));
    require(f.id(popup, "numberOfItems") == 2 && f.id(popup, "indexOfSelectedItem") == 1, "popup removal keeps a selection");
    require(f.id(popup, "indexOfItemWithTitle:", {f.str("Two")}) == 1, "-indexOfItemWithTitle:");
    // Clicking pops the menu up (a window of its own); a release over an
    // item chooses it and sends the button's action.
    {
        journal.clear();
        const auto target_class = f.subclass("TestPopupTarget", "NSObject");
        f.add(target_class, "chose:", [&f](CpuState& s, GuestMemory&) { journal.push_back("chose:" + f.text(f.id(s.x[2], "titleOfSelectedItem"))); });
        const auto target = f.id(f.id(target_class, "alloc"), "init");
        static_cast<void>(f.id(popup, "addItemWithTitle:", {f.str("Three")}));
        static_cast<void>(f.id(popup, "selectItemAtIndex:", {0}));
        static_cast<void>(f.id(popup, "setTarget:", {target}));
        static_cast<void>(f.id(popup, "setAction:", {f.objc.selector("chose:")}));
        const auto window = f.make_window({{100, 100}, {200, 60}});
        CallRegs origin;
        gui::appkit::put_point(origin, 0, {10, 20});
        static_cast<void>(f.send(popup, "setFrameOrigin:", origin));
        static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {popup}));
        static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
        static_cast<void>(f.next_event());
        const auto windows = f.server.windows.size();
        const auto number = f.id(window, "windowNumber");
        gui::Event down;
        down.type = gui::event_type::left_mouse_down;
        down.window = number;
        down.location = {20, 30};
        down.screen_location = {120, 130};
        down.click_count = 1;
        down.pressed_buttons = 1;
        f.server.queue(down);
        auto release = down;   // released over the button: the menu stays open
        release.type = gui::event_type::left_mouse_up;
        release.pressed_buttons = 0;
        f.server.queue(release);
        // The menu's top is 5 points above the button's top (item 0 over the
        // button); item 2's row is 5 + 2 * 22 points below the menu's top.
        const double menu_top = 100 + 20 + 26 + 5;
        auto choose = release;
        choose.screen_location = {115, menu_top - (5 + 2 * 22 + 11)};
        f.server.queue(choose);
        f.pump();
        require(f.server.windows.size() == windows, "the popup menu window was not closed");
        require(f.id(popup, "indexOfSelectedItem") == 2 && log_has("chose:Three"), "choosing from the popup menu");
        static_cast<void>(f.id(window, "close"));
    }

    const auto progress = f.with_rect(f.id(f.cls("NSProgressIndicator"), "alloc"), "initWithFrame:", {{0, 0}, {200, 20}});
    static_cast<void>(f.id(progress, "setIndeterminate:", {0}));
    CallRegs value;
    value.set_d(0, 40);
    static_cast<void>(f.send(progress, "setDoubleValue:", value));
    require(f.send(progress, "doubleValue").d(0) == 40 && f.send(progress, "maxValue").d(0) == 100, "progress indicator values");
    CallRegs increment;
    increment.set_d(0, 80);
    static_cast<void>(f.send(progress, "incrementBy:", increment));
    require(f.send(progress, "doubleValue").d(0) == 100, "-incrementBy: is clamped to the maximum");
    // A determinate bar: 40% of the track drawn in the accent color.
    const auto window = f.make_window({{0, 0}, {200, 40}});
    static_cast<void>(f.send(progress, "setDoubleValue:", value));
    static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {progress}));
    static_cast<void>(f.id(window, "orderFront:", {0}));
    static_cast<void>(f.next_event());
    const auto filled = f.pixel(window, 40, 10), empty = f.pixel(window, 150, 10);
    require(filled[0] > 200 && filled[2] < 60 && !(empty[0] > 200 && empty[2] < 60), "progress bar fill");

    const auto box = f.with_rect(f.id(f.cls("NSBox"), "alloc"), "initWithFrame:", {{0, 0}, {200, 100}});
    const auto box_content = f.id(box, "contentView");
    require(box_content && f.id(box_content, "superview") == box, "NSBox content view");
    static_cast<void>(f.id(window, "close"));
}

void scrolling_and_tables(Fixture& f) {
    journal.clear();
    const auto window = f.make_window({{0, 0}, {300, 200}});
    const auto scroll = f.with_rect(f.id(f.cls("NSScrollView"), "alloc"), "initWithFrame:", {{0, 0}, {300, 200}});
    static_cast<void>(f.id(scroll, "setHasVerticalScroller:", {1}));
    const auto text_view = f.with_rect(f.id(f.cls("NSTextView"), "alloc"), "initWithFrame:", {{0, 0}, {280, 200}});
    std::string license;
    for (int i = 0; i < 60; ++i) license += "Line " + std::to_string(i) + " of the license agreement.\n";
    static_cast<void>(f.id(text_view, "setString:", {f.str(license)}));
    static_cast<void>(f.id(text_view, "setEditable:", {0}));
    static_cast<void>(f.id(scroll, "setDocumentView:", {text_view}));
    static_cast<void>(f.id(window, "setContentView:", {scroll}));
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    static_cast<void>(f.next_event());
    const auto clip = f.id(scroll, "contentView");
    const auto document_height = f.send(text_view, "frame").d(3);
    require(document_height > 600 && f.id(scroll, "documentView") == text_view, "the text view grew with its text");
    const auto scroller = f.id(scroll, "verticalScroller");
    require(scroller && (f.id(scroller, "isHidden") & 0xff) == 0 && f.send(scroller, "knobProportion").d(0) < 0.5, "vertical scroller");
    gui::Event wheel;
    wheel.type = gui::event_type::scroll_wheel;
    wheel.window = f.id(window, "windowNumber");
    wheel.location = {100, 100};
    wheel.delta_y = -50;
    wheel.precise_scrolling = true;
    f.server.queue(wheel);
    f.pump();
    require(f.send(clip, "bounds").d(1) == 50 && f.send(scroller, "doubleValue").d(0) > 0, "scroll wheel scrolled the document");
    CallRegs point;
    gui::appkit::put_point(point, 0, {0, 100000});
    static_cast<void>(f.send(clip, "scrollToPoint:", point));
    require(f.send(clip, "bounds").d(1) == document_height - f.send(clip, "bounds").d(3), "scrolling is constrained to the document");

    // A table with a data source and delegate.
    const auto source_class = f.subclass("TestTableSource", "NSObject");
    f.add(source_class, "numberOfRowsInTableView:", [](CpuState& s, GuestMemory&) { s.x[0] = 5; }, "q@:@");
    f.add(source_class, "tableView:objectValueForTableColumn:row:", [&f](CpuState& s, GuestMemory&) {
        s.x[0] = f.str("row " + std::to_string(s.x[4]));
    }, "@@:@@q");
    f.add(source_class, "tableViewSelectionDidChange:", [](CpuState&, GuestMemory&) { journal.push_back("selectionDidChange"); });
    const auto source = f.id(f.id(source_class, "alloc"), "init");
    const auto table = f.with_rect(f.id(f.cls("NSTableView"), "alloc"), "initWithFrame:", {{0, 0}, {300, 200}});
    const auto column = f.id(f.id(f.cls("NSTableColumn"), "alloc"), "initWithIdentifier:", {f.str("name")});
    static_cast<void>(f.id(column, "setTitle:", {f.str("Name")}));
    static_cast<void>(f.id(table, "addTableColumn:", {column}));
    static_cast<void>(f.id(table, "setDataSource:", {source}));
    static_cast<void>(f.id(table, "setDelegate:", {source}));
    const auto table_scroll = f.with_rect(f.id(f.cls("NSScrollView"), "alloc"), "initWithFrame:", {{0, 0}, {300, 200}});
    static_cast<void>(f.id(table_scroll, "setDocumentView:", {table}));
    static_cast<void>(f.id(window, "setContentView:", {table_scroll}));
    static_cast<void>(f.id(table, "reloadData"));
    require(f.id(table, "numberOfRows") == 5 && f.id(table, "tableColumnWithIdentifier:", {f.str("name")}) == column, "table rows and columns");
    require(f.id(table, "headerView") && f.id(f.id(table, "headerView"), "superview") == table_scroll, "the header view sits in the scroll view");
    static_cast<void>(f.next_event());
    // Click the third row (rows are 19 points tall below a 24-point header).
    const auto row_height = f.send(table, "rowHeight").d(0) + f.send(table, "intercellSpacing").d(1);
    f.click(window, {50, 200 - 24 - row_height * 2.5});
    require(f.id(table, "selectedRow") == 2 && log_has("selectionDidChange"), "clicking a row selects it");
    require(f.id(f.id(table, "selectedRowIndexes"), "count") == 1, "-selectedRowIndexes");
    static_cast<void>(f.id(window, "close"));
}

void image_views(Fixture& f) {
    // A red 10x10 image drawn by -lockFocus, shown scaled by an NSImageView.
    const auto image = f.send(f.id(f.cls("NSImage"), "alloc"), "initWithSize:", [] { CallRegs r; r.set_d(0, 10); r.set_d(1, 10); return r; }()).x0;
    static_cast<void>(f.id(image, "lockFocus"));
    static_cast<void>(f.id(f.id(f.cls("NSColor"), "redColor"), "set"));
    CallRegs fill;
    gui::appkit::put_rect(fill, 0, {{0, 0}, {10, 10}});
    static_cast<void>(f.send(f.cls("NSBezierPath"), "fillRect:", fill));
    static_cast<void>(f.id(image, "unlockFocus"));
    const auto window = f.make_window({{0, 0}, {120, 80}});
    const auto view = f.id(f.cls("NSImageView"), "imageViewWithImage:", {image});
    require(view && f.id(view, "image") == image, "+imageViewWithImage:");
    static_cast<void>(f.with_rect(view, "setFrame:", {{10, 10}, {40, 40}}));
    static_cast<void>(f.id(view, "setImageScaling:", {1}));   // NSImageScaleAxesIndependently
    static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {view}));
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    static_cast<void>(f.next_event());
    require(is_red(f.pixel(window, 30, 30)) && is_red(f.pixel(window, 12, 48)) && is_white(f.pixel(window, 70, 30)), "NSImageView drawing");
    static_cast<void>(f.id(window, "close"));
}

void tabs_splits_and_outlines(Fixture& f) {
    journal.clear();
    // NSTabView: only the selected item's view is shown and drawn.
    const auto page = f.subclass("TestTabPage", "NSView");
    f.add(page, "drawRect:", [](CpuState&, GuestMemory&) { journal.push_back("page"); }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    const auto window = f.make_window({{0, 0}, {300, 200}});
    const auto tabs = f.with_rect(f.id(f.cls("NSTabView"), "alloc"), "initWithFrame:", {{0, 0}, {300, 200}});
    std::array<Address, 2> items{}, pages{};
    for (std::size_t i = 0; i < 2; ++i) {
        items[i] = f.id(f.id(f.cls("NSTabViewItem"), "alloc"), "initWithIdentifier:", {f.str(i ? "two" : "one")});
        static_cast<void>(f.id(items[i], "setLabel:", {f.str(i ? "Two" : "One")}));
        pages[i] = f.with_rect(f.id(page, "alloc"), "initWithFrame:", {{0, 0}, {100, 100}});
        static_cast<void>(f.id(items[i], "setView:", {pages[i]}));
        static_cast<void>(f.id(tabs, "addTabViewItem:", {items[i]}));
    }
    static_cast<void>(f.id(window, "setContentView:", {tabs}));
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    static_cast<void>(f.next_event());
    const auto shown = [&f](Address view) { return f.id(view, "superview") != 0 && (f.id(view, "isHidden") & 0xff) == 0; };
    require(f.id(tabs, "numberOfTabViewItems") == 2 && f.id(tabs, "selectedTabViewItem") == items[0] && shown(pages[0]) && !shown(pages[1]),
            "the first tab is selected");
    const auto first_drawn = journal.size();
    require(first_drawn >= 1, "the selected tab's view was not drawn");
    static_cast<void>(f.id(tabs, "selectTabViewItemAtIndex:", {1}));
    require(f.id(tabs, "selectedTabViewItem") == items[1] && shown(pages[1]) && !shown(pages[0]), "-selectTabViewItemAtIndex:");
    require(f.text(f.id(items[1], "label")) == "Two" && f.id(items[1], "tabView") == tabs, "tab item properties");
    const auto content_frame = f.send(pages[1], "frame");
    require(content_frame.d(2) > 200 && content_frame.d(3) > 120 && content_frame.d(3) < 200, "the tab content fills the area below the tabs");

    // NSSplitView: subviews side by side with a divider; the position moves it.
    const auto split = f.with_rect(f.id(f.cls("NSSplitView"), "alloc"), "initWithFrame:", {{0, 0}, {300, 200}});
    static_cast<void>(f.id(split, "setVertical:", {1}));
    const auto left = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{0, 0}, {150, 200}});
    const auto right = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{0, 0}, {150, 200}});
    static_cast<void>(f.id(split, "addSubview:", {left}));
    static_cast<void>(f.id(split, "addSubview:", {right}));
    static_cast<void>(f.id(split, "adjustSubviews"));
    const auto divider = f.send(split, "dividerThickness").d(0);
    const auto lf = f.send(left, "frame"), rf = f.send(right, "frame");
    require(divider > 0 && lf.d(0) == 0 && std::abs(lf.d(2) + divider + rf.d(2) - 300) < 0.5 && std::abs(rf.d(0) - (lf.d(2) + divider)) < 0.5 &&
                lf.d(3) == 200, "split view layout");
    CallRegs position;
    position.set_d(0, 100);
    position.x[2] = 0;
    static_cast<void>(f.send(split, "setPosition:ofDividerAtIndex:", position));
    require(std::abs(f.send(left, "frame").d(2) - 100) < 0.5 && std::abs(f.send(right, "frame").d(0) - (100 + divider)) < 0.5,
            "-setPosition:ofDividerAtIndex:");

    // NSOutlineView over a data source: expansion changes the rows.
    static std::map<std::string, std::vector<std::string>> tree{{"", {"A", "B"}}, {"A", {"a1", "a2"}}};
    static std::map<Address, std::string> names;
    static std::map<std::string, Address> objects;
    const auto object_for = [&f](const std::string& name) {
        auto& slot = objects[name];
        if (!slot) { slot = f.str(name); names[slot] = name; }
        return slot;
    };
    const auto source_class = f.subclass("TestOutlineSource", "NSObject");
    f.add(source_class, "outlineView:numberOfChildrenOfItem:", [](CpuState& s, GuestMemory&) {
        const auto found = tree.find(s.x[3] ? names[s.x[3]] : "");
        s.x[0] = found == tree.end() ? 0 : found->second.size();
    }, "q@:@@");
    f.add(source_class, "outlineView:child:ofItem:", [object_for](CpuState& s, GuestMemory&) {
        s.x[0] = object_for(tree[s.x[4] ? names[s.x[4]] : ""].at(s.x[3]));
    }, "@@:@q@");
    f.add(source_class, "outlineView:isItemExpandable:", [](CpuState& s, GuestMemory&) { s.x[0] = tree.contains(names[s.x[3]]) ? 1 : 0; }, "B@:@@");
    f.add(source_class, "outlineView:objectValueForTableColumn:byItem:", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[4]; }, "@@:@@@");
    const auto source = f.id(f.id(source_class, "alloc"), "init");
    const auto outline = f.with_rect(f.id(f.cls("NSOutlineView"), "alloc"), "initWithFrame:", {{0, 0}, {300, 200}});
    const auto column = f.id(f.id(f.cls("NSTableColumn"), "alloc"), "initWithIdentifier:", {f.str("name")});
    static_cast<void>(f.id(outline, "addTableColumn:", {column}));
    static_cast<void>(f.id(outline, "setOutlineTableColumn:", {column}));
    static_cast<void>(f.id(outline, "setDataSource:", {source}));
    const auto outline_scroll = f.with_rect(f.id(f.cls("NSScrollView"), "alloc"), "initWithFrame:", {{0, 0}, {300, 200}});
    static_cast<void>(f.id(outline_scroll, "setDocumentView:", {outline}));
    static_cast<void>(f.id(window, "setContentView:", {outline_scroll}));
    static_cast<void>(f.id(outline, "reloadData"));
    static_cast<void>(f.next_event());
    require(f.id(outline, "numberOfRows") == 2 && f.id(outline, "itemAtRow:", {1}) == object_for("B"), "outline rows (collapsed)");
    static_cast<void>(f.id(outline, "expandItem:", {object_for("A")}));
    require(f.id(outline, "numberOfRows") == 4 && f.id(outline, "itemAtRow:", {1}) == object_for("a1") &&
                f.id(outline, "levelForRow:", {2}) == 1 && f.id(outline, "parentForItem:", {object_for("a2")}) == object_for("A") &&
                (f.id(outline, "isItemExpanded:", {object_for("A")}) & 0xff),
            "-expandItem:");
    static_cast<void>(f.next_event());
    static_cast<void>(f.id(outline, "collapseItem:", {object_for("A")}));
    require(f.id(outline, "numberOfRows") == 2 && f.id(outline, "rowForItem:", {object_for("B")}) == 1, "-collapseItem:");
    static_cast<void>(f.id(window, "close"));
}

// Expects `body` to fail with an exception (an explicitly unsupported or
// invalid operation).
template <class Body>
bool throws(Body&& body) {
    try { body(); } catch (const std::exception&) { return true; }
    return false;
}

void sliders_steppers_and_combo_boxes(Fixture& f) {
    journal.clear();
    const auto target_class = f.subclass("TestValueTarget", "NSObject");
    f.add(target_class, "changed:", [&f](CpuState& s, GuestMemory&) {
        journal.push_back("changed:" + std::to_string(static_cast<long long>(std::lround(f.send(s.x[2], "doubleValue").d(0)))));
    });
    const auto target = f.id(f.id(target_class, "alloc"), "init");
    const auto set_double = [&f](Address object, std::string_view selector, double value) {
        CallRegs registers;
        registers.set_d(0, value);
        static_cast<void>(f.send(object, selector, registers));
    };
    const auto window = f.make_window({{0, 0}, {240, 120}});
    const auto content = f.id(window, "contentView");

    // NSSlider: the value stays in range; a click moves the knob there.
    const auto slider = f.with_rect(f.id(f.cls("NSSlider"), "alloc"), "initWithFrame:", {{10, 10}, {200, 24}});
    require(f.id(f.id(slider, "cell"), "isKindOfClass:", {f.cls("NSSliderCell")}) & 0xff, "NSSlider's cell class");
    set_double(slider, "setMaxValue:", 100);
    set_double(slider, "setDoubleValue:", 150);
    require(f.send(slider, "doubleValue").d(0) == 100, "the slider value is clamped to the maximum");
    set_double(slider, "setDoubleValue:", 25);
    require(f.send(slider, "doubleValue").d(0) == 25 && !(f.id(slider, "isVertical") & 0xff), "slider value");
    static_cast<void>(f.id(slider, "setTarget:", {target}));
    static_cast<void>(f.id(slider, "setAction:", {f.objc.selector("changed:")}));
    static_cast<void>(f.id(content, "addSubview:", {slider}));
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    static_cast<void>(f.next_event());
    // 25%: the filled track left of the knob, the knob at x = 10 + 10 + 0.25 * 180.
    const auto fill = f.pixel(window, 30, 22), rest = f.pixel(window, 180, 22);
    require(fill[0] > 180 && fill[2] < 120 && !(rest[0] > 180 && rest[2] < 120), "slider track fill");
    f.click(window, {110, 22});
    require(std::abs(f.send(slider, "doubleValue").d(0) - 50) < 0.6 && log_has("changed:50"), "a click on the slider track");
    // Tick values only.
    journal.clear();
    static_cast<void>(f.id(slider, "setNumberOfTickMarks:", {5}));
    static_cast<void>(f.id(slider, "setAllowsTickMarkValuesOnly:", {1}));
    f.click(window, {20 + 0.3 * 180, 22});
    require(f.send(slider, "doubleValue").d(0) == 25 && log_has("changed:25"), "a click snaps to the closest tick mark");
    CallRegs probe;
    probe.set_d(0, 60);
    require(f.send(slider, "closestTickMarkValueToValue:", probe).d(0) == 50 &&
                f.send(slider, "tickMarkValueAtIndex:", [] { CallRegs r; r.x[2] = 4; return r; }()).d(0) == 100,
            "tick mark values");
    const auto tall = f.with_rect(f.id(f.cls("NSSlider"), "alloc"), "initWithFrame:", {{0, 0}, {24, 120}});
    require(f.id(tall, "isVertical") & 0xff, "a tall slider is vertical");
    require(throws([&] { static_cast<void>(f.id(slider, "setSliderType:", {1})); }), "circular sliders must fail explicitly");

    // NSStepper: presses in the upper / lower half step up / down, wrapping.
    journal.clear();
    const auto stepper = f.with_rect(f.id(f.cls("NSStepper"), "alloc"), "initWithFrame:", {{220, 10}, {19, 28}});
    require(f.send(stepper, "maxValue").d(0) == 59 && (f.id(stepper, "valueWraps") & 0xff), "stepper defaults");
    set_double(stepper, "setMaxValue:", 3);
    set_double(stepper, "setDoubleValue:", 2);
    static_cast<void>(f.id(stepper, "setTarget:", {target}));
    static_cast<void>(f.id(stepper, "setAction:", {f.objc.selector("changed:")}));
    static_cast<void>(f.id(content, "addSubview:", {stepper}));
    static_cast<void>(f.next_event());
    f.click(window, {229, 32});   // upper half
    require(f.send(stepper, "doubleValue").d(0) == 3 && log_has("changed:3"), "stepper up");
    f.click(window, {229, 32});
    require(f.send(stepper, "doubleValue").d(0) == 0, "the stepper wraps past its maximum");
    static_cast<void>(f.id(stepper, "setValueWraps:", {0}));
    f.click(window, {229, 15});   // lower half
    require(f.send(stepper, "doubleValue").d(0) == 0, "a non-wrapping stepper stops at its minimum");
    set_double(stepper, "setIncrement:", 2);
    f.click(window, {229, 32});
    require(f.send(stepper, "doubleValue").d(0) == 2, "stepper increment");

    // NSComboBox: the item list, selection, notifications and the popup list.
    journal.clear();
    const auto delegate_class = f.subclass("TestComboDelegate", "NSObject");
    f.add(delegate_class, "comboBoxSelectionDidChange:", [](CpuState&, GuestMemory&) { journal.push_back("selectionDidChange"); });
    f.add(delegate_class, "comboBoxWillPopUp:", [](CpuState&, GuestMemory&) { journal.push_back("willPopUp"); });
    f.add(delegate_class, "comboBoxWillDismiss:", [](CpuState&, GuestMemory&) { journal.push_back("willDismiss"); });
    f.add(delegate_class, "picked:", [&f](CpuState& s, GuestMemory&) { journal.push_back("picked:" + f.text(f.id(s.x[2], "stringValue"))); });
    const auto delegate = f.id(f.id(delegate_class, "alloc"), "init");
    const auto combo = f.with_rect(f.id(f.cls("NSComboBox"), "alloc"), "initWithFrame:", {{10, 60}, {150, 24}});
    static_cast<void>(f.id(combo, "setDelegate:", {delegate}));
    static_cast<void>(f.id(combo, "setTarget:", {delegate}));
    static_cast<void>(f.id(combo, "setAction:", {f.objc.selector("picked:")}));
    static_cast<void>(f.id(combo, "addItemsWithObjectValues:", {f.cf.array(std::vector<Address>{f.str("Alpha"), f.str("Beta"), f.str("Gamma")})}));
    require(f.id(combo, "numberOfItems") == 3 && f.id(combo, "indexOfSelectedItem") == static_cast<Address>(-1) &&
                f.id(combo, "indexOfItemWithObjectValue:", {f.str("Gamma")}) == 2, "combo box items");
    static_cast<void>(f.id(combo, "selectItemAtIndex:", {2}));
    require(f.text(f.id(combo, "stringValue")) == "Gamma" && f.id(combo, "objectValueOfSelectedItem") != 0 &&
                log_has("selectionDidChange"), "combo box selection");
    static_cast<void>(f.id(combo, "removeItemAtIndex:", {0}));
    require(f.id(combo, "numberOfItems") == 2 && f.id(combo, "indexOfSelectedItem") == 1, "removal keeps the selection");
    static_cast<void>(f.id(combo, "deselectItemAtIndex:", {1}));
    static_cast<void>(f.id(content, "addSubview:", {combo}));
    static_cast<void>(f.next_event());
    {
        journal.clear();
        const auto windows = f.server.windows.size();
        gui::Event down;
        down.type = gui::event_type::left_mouse_down;
        down.window = f.id(window, "windowNumber");
        down.location = {150, 70};          // the button at the trailing edge
        down.screen_location = {150, 70};
        down.click_count = 1;
        down.pressed_buttons = 1;
        f.server.queue(down);
        auto release = down;                // released over the field: the list stays open
        release.type = gui::event_type::left_mouse_up;
        release.pressed_buttons = 0;
        f.server.queue(release);
        // The list's top is at the field's top (no selection); item 1's row
        // is 5 + 22 points below it.
        const double list_top = 0 + 60 + 24;
        auto choose = release;
        choose.screen_location = {40, list_top - (5 + 22 + 11)};
        f.server.queue(choose);
        f.pump();
        require(f.server.windows.size() == windows, "the combo box list window was not closed");
        require(log_has("willPopUp") && log_has("willDismiss") && log_has("picked:Gamma") && f.id(combo, "indexOfSelectedItem") == 1,
                "choosing from the combo box list");
    }
    // A click on the text part edits; the field editor leaves the button free.
    f.click(window, {30, 70});
    const auto editor = f.id(combo, "currentEditor");
    require(editor != 0, "combo box editing did not start");
    require(f.send(editor, "frame").d(2) <= 150 - 8 - 20 + 0.5, "the field editor covers the combo box button");
    static_cast<void>(f.id(window, "makeFirstResponder:", {window}));
    // A data source replaces the internal list.
    const auto source_class = f.subclass("TestComboSource", "NSObject");
    f.add(source_class, "numberOfItemsInComboBox:", [](CpuState& s, GuestMemory&) { s.x[0] = 4; }, "q@:@");
    f.add(source_class, "comboBox:objectValueForItemAtIndex:", [&f](CpuState& s, GuestMemory&) {
        s.x[0] = f.str("item" + std::to_string(s.x[3]));
    }, "@@:@q");
    const auto source = f.id(f.id(source_class, "alloc"), "init");
    static_cast<void>(f.id(combo, "setUsesDataSource:", {1}));
    static_cast<void>(f.id(combo, "setDataSource:", {source}));
    require(f.id(combo, "numberOfItems") == 4 && f.text(f.id(combo, "itemObjectValueAtIndex:", {3})) == "item3", "combo box data source");
    require(throws([&] { static_cast<void>(f.id(combo, "addItemWithObjectValue:", {f.str("x")})); }),
            "the internal list of a data-source combo box must fail");
    static_cast<void>(f.id(window, "close"));
}

void panels_and_models(Fixture& f) {
    journal.clear();
    // NSPanel.
    CallRegs init;
    gui::appkit::put_rect(init, 0, {{0, 0}, {200, 100}});
    init.x[2] = gui::style::titled;
    init.x[3] = 2;
    const auto panel = f.send(f.id(f.cls("NSPanel"), "alloc"), "initWithContentRect:styleMask:backing:defer:", init).x0;
    require(f.id(panel, "isKindOfClass:", {f.cls("NSWindow")}) & 0xff, "NSPanel is a window");
    static_cast<void>(f.id(panel, "setFloatingPanel:", {1}));
    require((f.id(panel, "isFloatingPanel") & 0xff) && f.id(panel, "level") == 3 && !(f.id(panel, "canBecomeMainWindow") & 0xff),
            "floating panel");
    static_cast<void>(f.id(panel, "setBecomesKeyOnlyIfNeeded:", {1}));
    require(f.id(panel, "becomesKeyOnlyIfNeeded") & 0xff, "-setBecomesKeyOnlyIfNeeded:");
    static_cast<void>(f.id(panel, "setReleasedWhenClosed:", {0}));
    static_cast<void>(f.id(panel, "close"));

    // NSToolbar: items come from the delegate; windows refuse to show one.
    const auto toolbar_delegate = f.subclass("TestToolbarDelegate", "NSObject");
    f.add(toolbar_delegate, "toolbar:itemForItemIdentifier:willBeInsertedIntoToolbar:", [&f](CpuState& s, GuestMemory&) {
        const auto item = f.id(f.id(f.cls("NSToolbarItem"), "alloc"), "initWithItemIdentifier:", {s.x[3]});
        static_cast<void>(f.id(item, "setLabel:", {f.str("Run")}));
        s.x[0] = f.id(item, "autorelease");
    }, "@@:@@B");
    f.add(toolbar_delegate, "toolbarWillAddItem:", [](CpuState&, GuestMemory&) { journal.push_back("willAdd"); });
    const auto toolbar = f.id(f.id(f.cls("NSToolbar"), "alloc"), "initWithIdentifier:", {f.str("main")});
    static_cast<void>(f.id(toolbar, "setDelegate:", {f.id(f.id(toolbar_delegate, "alloc"), "init")}));
    static_cast<void>(f.id(toolbar, "insertItemWithItemIdentifier:atIndex:", {f.str("run"), 0}));
    static_cast<void>(f.id(toolbar, "insertItemWithItemIdentifier:atIndex:", {f.str("NSToolbarFlexibleSpaceItem"), 1}));
    const auto toolbar_items = f.id(toolbar, "items");
    require(f.id(toolbar_items, "count") == 2 && log_has("willAdd"), "toolbar items");
    const auto first = f.id(toolbar_items, "objectAtIndex:", {0});
    require(f.text(f.id(first, "label")) == "Run" && f.id(first, "toolbar") == toolbar && f.text(f.id(first, "itemIdentifier")) == "run",
            "toolbar item");
    static_cast<void>(f.id(toolbar, "removeItemAtIndex:", {1}));
    require(f.id(f.id(toolbar, "items"), "count") == 1, "-removeItemAtIndex:");
    const auto window = f.make_window({{0, 0}, {100, 100}});
    // A window shows its toolbar above the content, which keeps its size.
    static_cast<void>(f.id(window, "setToolbar:", {toolbar}));
    require(f.id(window, "toolbar") == toolbar && f.send(f.id(window, "contentView"), "frame").d(3) == 100, "a window toolbar keeps the content size");
    static_cast<void>(f.id(window, "setToolbar:", {0}));

    // NSSavePanel / NSOpenPanel: configuration, no dialog.
    const auto open = f.id(f.cls("NSOpenPanel"), "openPanel");
    static_cast<void>(f.id(open, "setCanChooseDirectories:", {1}));
    static_cast<void>(f.id(open, "setMessage:", {f.str("Choose Blender")}));
    require((f.id(open, "canChooseDirectories") & 0xff) && (f.id(open, "canChooseFiles") & 0xff) &&
                f.text(f.id(open, "message")) == "Choose Blender", "open panel configuration");
    // Running it shows the Windows file dialog (a test presenter here); a
    // cancelled dialog answers NSModalResponseCancel and no URLs.
    gui::appkit::set_file_dialog_override([](const gui::appkit::FileDialogRequest& request) {
        require(!request.save && request.message == "Choose Blender", "file dialog request");
        return std::optional<std::vector<std::string>>{};
    });
    require(f.id(open, "runModal") == 0 && f.id(f.id(open, "URLs"), "count") == 0, "a cancelled open panel");
    gui::appkit::set_file_dialog_override({});
    require(throws([&] { static_cast<void>(f.id(f.cls("NSColorPanel"), "sharedColorPanel")); }), "the color panel must fail explicitly");
    require(!(f.id(f.cls("NSColorPanel"), "sharedColorPanelExists") & 0xff), "+sharedColorPanelExists");

    // NSPrintInfo.
    const auto info = f.id(f.cls("NSPrintInfo"), "sharedPrintInfo");
    const auto paper = f.send(info, "paperSize");
    require(paper.d(0) == 612 && paper.d(1) == 792 && f.id(f.cls("NSPrintInfo"), "sharedPrintInfo") == info, "shared print info");
    static_cast<void>(f.id(info, "setOrientation:", {1}));
    require(f.send(info, "paperSize").d(0) == 792 && f.id(info, "orientation") == 1, "landscape print info");
    static_cast<void>(f.id(info, "setOrientation:", {0}));

    // Gesture recognizers attach to views.
    const auto view = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{0, 0}, {50, 50}});
    const auto pan = f.id(f.id(f.cls("NSPanGestureRecognizer"), "alloc"), "initWithTarget:action:", {view, f.objc.selector("pan:")});
    static_cast<void>(f.id(view, "addGestureRecognizer:", {pan}));
    require(f.id(f.id(view, "gestureRecognizers"), "count") == 1 && f.id(pan, "view") == view && f.id(pan, "state") == 0 &&
                f.id(pan, "buttonMask") == 1, "gesture recognizer on a view");
    static_cast<void>(f.id(view, "removeGestureRecognizer:", {pan}));
    require(f.id(f.id(view, "gestureRecognizers"), "count") == 0 && f.id(pan, "view") == 0, "-removeGestureRecognizer:");

    // NSPasteboardItem.
    const auto item = f.id(f.id(f.cls("NSPasteboardItem"), "alloc"), "init");
    require(f.id(item, "setString:forType:", {f.str("hello"), f.str("public.utf8-plain-text")}) & 0xff, "-setString:forType:");
    require(f.text(f.id(item, "stringForType:", {f.str("public.utf8-plain-text")})) == "hello" && f.id(f.id(item, "types"), "count") == 1 &&
                f.id(item, "stringForType:", {f.str("public.png")}) == 0, "pasteboard item values");

    // NSViewAnimation: frames reach their end, fades show / hide; the delegate hears the end.
    const auto rect_value_class = f.subclass("TestRectValue", "NSObject");
    static gui::Rect end_frame{{20, 10}, {30, 30}};
    f.add(rect_value_class, "rectValue", [](CpuState& s, GuestMemory&) { gui::appkit::return_rect(s, end_frame); }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    const auto end_value = f.id(f.id(rect_value_class, "alloc"), "init");
    const std::vector<std::pair<Address, Address>> entries{
        {f.str("NSViewAnimationTargetKey"), view}, {f.str("NSViewAnimationEndFrameKey"), end_value},
        {f.str("NSViewAnimationEffectKey"), f.str("NSViewAnimationFadeOutEffect")}};
    const auto entry = f.cf.dictionary(entries);
    const auto animation_delegate = f.subclass("TestAnimationDelegate", "NSObject");
    f.add(animation_delegate, "animationDidEnd:", [](CpuState&, GuestMemory&) { journal.push_back("animationDidEnd"); });
    const auto animation = f.id(f.id(f.cls("NSViewAnimation"), "alloc"), "initWithViewAnimations:", {f.cf.array(std::vector<Address>{entry})});
    CallRegs duration;
    duration.set_d(0, 0.05);
    static_cast<void>(f.send(animation, "setDuration:", duration));
    static_cast<void>(f.id(animation, "setDelegate:", {f.id(f.id(animation_delegate, "alloc"), "init")}));
    static_cast<void>(f.id(animation, "startAnimation"));
    const auto frame = f.send(view, "frame");
    require(frame.d(0) == 20 && frame.d(1) == 10 && frame.d(2) == 30 && (f.id(view, "isHidden") & 0xff) && log_has("animationDidEnd") &&
                !(f.id(animation, "isAnimating") & 0xff), "view animation");

    // NSTextView's text container.
    const auto text_view = f.with_rect(f.id(f.cls("NSTextView"), "alloc"), "initWithFrame:", {{0, 0}, {200, 100}});
    const auto container = f.id(text_view, "textContainer");
    require(container && f.id(text_view, "textContainer") == container && f.id(container, "textView") == text_view &&
                (f.id(container, "widthTracksTextView") & 0xff), "-[NSTextView textContainer]");
    CallRegs container_size;
    container_size.set_d(0, 1e7); container_size.set_d(1, 1e7);
    static_cast<void>(f.send(container, "setContainerSize:", container_size));
    static_cast<void>(f.id(container, "setWidthTracksTextView:", {0}));
    require(f.send(container, "containerSize").d(0) == 1e7 && !(f.id(container, "widthTracksTextView") & 0xff), "text container size");
    require(throws([&] { static_cast<void>(f.id(container, "setLineBreakMode:", {1})); }), "character wrapping must fail explicitly");

    // NSWindowController / NSViewController without nibs.
    const auto controlled = f.make_window({{0, 0}, {120, 80}});
    const auto controller = f.id(f.id(f.cls("NSWindowController"), "alloc"), "initWithWindow:", {controlled});
    require(f.id(controller, "window") == controlled && f.id(controlled, "windowController") == controller &&
                (f.id(controller, "isWindowLoaded") & 0xff), "-[NSWindowController initWithWindow:]");
    static_cast<void>(f.id(controller, "showWindow:", {0}));
    require(f.id(controlled, "isVisible") & 0xff, "-showWindow:");
    static_cast<void>(f.id(controller, "close"));
    const auto view_controller = f.id(f.id(f.cls("NSViewController"), "alloc"), "initWithNibName:bundle:", {0, 0});
    require(throws([&] { static_cast<void>(f.id(view_controller, "view")); }), "a view controller without a view or nib must fail");
    const auto own_view = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{0, 0}, {10, 10}});
    static_cast<void>(f.id(view_controller, "setView:", {own_view}));
    const auto child_controller = f.id(f.id(f.cls("NSViewController"), "alloc"), "init");
    static_cast<void>(f.id(view_controller, "addChildViewController:", {child_controller}));
    require(f.id(view_controller, "view") == own_view && f.id(own_view, "nextResponder") == view_controller &&
                f.id(f.id(view_controller, "childViewControllers"), "count") == 1 &&
                f.id(child_controller, "parentViewController") == view_controller, "view controller");
    static_cast<void>(f.id(child_controller, "removeFromParentViewController"));
    require(f.id(f.id(view_controller, "childViewControllers"), "count") == 0, "-removeFromParentViewController");

    // NSLayoutManager line metrics follow the font.
    const auto layout = f.id(f.id(f.cls("NSLayoutManager"), "alloc"), "init");
    CallRegs font_size;
    font_size.set_d(0, 13);
    const auto font = f.send(f.cls("NSFont"), "systemFontOfSize:", font_size).x0;
    const auto line = f.send(layout, "defaultLineHeightForFont:", [&] { CallRegs r; r.x[2] = font; return r; }()).d(0);
    require(line >= 13 && line <= 20, "-defaultLineHeightForFont:");

    // An installer's (wxOSX) AppKit imports that were missing all resolve.
    for (const auto* symbol : {"_NSAccessibilityUnignoredDescendant", "_NSCalibratedWhiteColorSpace", "_NSGeneralPboard",
             "_NSPatternColorSpace", "_NSToolbarFlexibleSpaceItemIdentifier", "_NSToolbarSpaceItemIdentifier",
             "_NSViewAnimationEffectKey", "_NSViewAnimationEndFrameKey", "_NSViewAnimationFadeInEffect", "_NSViewAnimationFadeOutEffect",
             "_NSViewAnimationStartFrameKey", "_NSViewAnimationTargetKey", "_OBJC_CLASS_$_NSColorPanel", "_OBJC_CLASS_$_NSComboBox",
             "_OBJC_CLASS_$_NSDraggingItem", "_OBJC_CLASS_$_NSFontPanel", "_OBJC_CLASS_$_NSLayoutManager",
             "_OBJC_CLASS_$_NSMagnificationGestureRecognizer", "_OBJC_CLASS_$_NSOpenPanel", "_OBJC_CLASS_$_NSPageLayout",
             "_OBJC_CLASS_$_NSPanGestureRecognizer", "_OBJC_CLASS_$_NSPanel", "_OBJC_CLASS_$_NSPasteboardItem",
             "_OBJC_CLASS_$_NSPressGestureRecognizer", "_OBJC_CLASS_$_NSPrintInfo", "_OBJC_CLASS_$_NSPrintPanel",
             "_OBJC_CLASS_$_NSRotationGestureRecognizer", "_OBJC_CLASS_$_NSSavePanel", "_OBJC_CLASS_$_NSSlider",
             "_OBJC_CLASS_$_NSStepper", "_OBJC_CLASS_$_NSToolbar", "_OBJC_CLASS_$_NSToolbarItem", "_OBJC_CLASS_$_NSViewAnimation",
             "_OBJC_METACLASS_$_NSComboBox", "_OBJC_METACLASS_$_NSPanel", "_OBJC_METACLASS_$_NSSlider", "_OBJC_METACLASS_$_NSStepper",
             "_OBJC_METACLASS_$_NSToolbar", "_OBJC_METACLASS_$_NSToolbarItem"})
        require(f.process.resolve(gui::appkit::appkit_library, symbol).has_value(), std::string("unresolved AppKit import ") + symbol);
    static_cast<void>(f.id(window, "close"));
}

void nib_loading(Fixture& f, const std::vector<std::uint8_t>& bytes) {
    journal.clear();
    const auto existing = f.objc.find_class("TestNibOwner");
    const auto owner_class = existing ? existing : f.subclass("TestNibOwner", "NSObject");
    static Address outlet{};
    outlet = 0;
    f.add(owner_class, "setButton:", [](CpuState& s, GuestMemory&) { outlet = s.x[2]; journal.push_back("setButton:"); });
    f.add(owner_class, "pressed:", [](CpuState&, GuestMemory&) { journal.push_back("pressed:"); });
    f.add(owner_class, "awakeFromNib", [](CpuState&, GuestMemory&) { journal.push_back("owner awakeFromNib"); }, "v@:");
    const auto owner = f.id(f.id(owner_class, "alloc"), "init");
    const auto block = f.process.allocate_guest(bytes.size());
    f.engine.write_bytes(block, bytes);
    const auto data = f.id(f.cls("NSData"), "dataWithBytes:length:", {block, bytes.size()});
    const auto loaded = f.id(f.id(f.cls("NSNib"), "alloc"), "initWithNibData:bundle:", {data, 0});
    const auto slot = f.process.allocate_guest(8);
    const auto windows_before = f.server.windows.size();
    require((f.id(loaded, "instantiateWithOwner:topLevelObjects:", {owner, slot}) & 0xff) == 1, "-instantiateWithOwner:topLevelObjects:");
    const auto top_level = f.engine.read64(slot);
    require(top_level && f.id(top_level, "count") == 1, "top-level objects");
    const auto window = f.id(top_level, "objectAtIndex:", {0});
    require(f.text(f.id(window, "title")) == "Nib Window" && f.server.windows.size() == windows_before + 1 && (f.id(window, "isVisible") & 0xff),
            "the nib window was created and shown");
    require(log_has("setButton:") && outlet && f.text(f.id(outlet, "title")) == "Press" && log_has("owner awakeFromNib"), "outlet and awakeFromNib");
    require(f.id(outlet, "window") == window && f.send(outlet, "frame").d(0) == 20, "the button is in the window's view hierarchy");
    static_cast<void>(f.id(outlet, "performClick:", {0}));
    require(log_has("pressed:"), "the action connection");
    static_cast<void>(f.id(window, "close"));
}
// HIToolbox over the Quartz layer: HIShape regions (and their path in a
// CGContext), HITheme drawing into a CGBitmapContext, Carbon key modifiers.
void hitoolbox(Fixture& f) {
    const auto call = [&f](std::string_view library, std::string_view name, CallRegs registers) {
        const auto found = f.process.resolve(library, name);
        require(found.has_value(), "missing export " + std::string(name));
        return alchemy::EngineAdapter(f.process.calling_engine()).call_regs(found->address, registers);
    };
    constexpr std::string_view services = "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices";
    const auto carbon = gui::appkit::carbon_library;
    const auto cg = gui::appkit::core_graphics_library;
    const auto scratch = f.process.allocate_guest(64);
    const auto put = [&f, scratch](gui::Rect r) {
        f.engine.write64(scratch, std::bit_cast<std::uint64_t>(r.origin.x));
        f.engine.write64(scratch + 8, std::bit_cast<std::uint64_t>(r.origin.y));
        f.engine.write64(scratch + 16, std::bit_cast<std::uint64_t>(r.size.width));
        f.engine.write64(scratch + 24, std::bit_cast<std::uint64_t>(r.size.height));
        return scratch;
    };
    const auto shape_call = [&](std::string_view name, std::initializer_list<Address> arguments) {
        CallRegs registers;
        unsigned index = 0;
        for (const auto value : arguments) registers.x[index++] = value;
        return call(services, name, registers);
    };
    const auto bounds_of = [&](Address shape) {
        static_cast<void>(shape_call("_HIShapeGetBounds", {shape, scratch + 32}));
        return gui::Rect{{std::bit_cast<double>(f.engine.read64(scratch + 32)), std::bit_cast<double>(f.engine.read64(scratch + 40))},
                         {std::bit_cast<double>(f.engine.read64(scratch + 48)), std::bit_cast<double>(f.engine.read64(scratch + 56))}};
    };
    const auto contains = [&](Address shape, double x, double y) {
        f.engine.write64(scratch + 32, std::bit_cast<std::uint64_t>(x));
        f.engine.write64(scratch + 40, std::bit_cast<std::uint64_t>(y));
        return (shape_call("_HIShapeContainsPoint", {shape, scratch + 32}).x0 & 0xff) != 0;
    };
    // Regions.
    const auto square = shape_call("_HIShapeCreateWithRect", {put({{0, 0}, {10, 10}})}).x0;
    const auto region = shape_call("_HIShapeCreateMutableCopy", {square}).x0;
    require(shape_call("_HIShapeUnionWithRect", {region, put({{10, 0}, {10, 10}})}).x0 == 0, "HIShapeUnionWithRect");
    auto box = bounds_of(region);
    require((shape_call("_HIShapeIsRectangular", {region}).x0 & 0xff) && box.size.width == 20 && box.size.height == 10,
            "two adjacent squares form a rectangle");
    static_cast<void>(shape_call("_HIShapeUnionWithRect", {region, put({{0, 20}, {5, 5}})}));
    require(!(shape_call("_HIShapeIsRectangular", {region}).x0 & 0xff) && bounds_of(region).size.height == 25 &&
                contains(region, 2, 22) && !contains(region, 8, 22) && contains(region, 15, 5), "a region of two rectangles");
    const auto band = shape_call("_HIShapeCreateWithRect", {put({{0, 0}, {20, 10}})}).x0;
    const auto rest = shape_call("_HIShapeCreateMutable", {}).x0;
    static_cast<void>(shape_call("_HIShapeDifference", {region, band, rest}));
    box = bounds_of(rest);
    require(box.origin.x == 0 && box.origin.y == 20 && box.size.width == 5 && box.size.height == 5, "HIShapeDifference");
    const auto both = shape_call("_HIShapeCreateIntersection", {region, band}).x0;
    box = bounds_of(both);
    require(box.size.width == 20 && box.size.height == 10 && (shape_call("_HIShapeIsRectangular", {both}).x0 & 0xff), "HIShapeCreateIntersection");
    CallRegs offset;
    offset.x[0] = rest;
    offset.set_d(0, 3); offset.set_d(1, -4);
    static_cast<void>(call(services, "_HIShapeOffset", offset));
    box = bounds_of(rest);
    require(box.origin.x == 3 && box.origin.y == 16, "HIShapeOffset");
    static_cast<void>(shape_call("_HIShapeDifference", {rest, rest, rest}));
    require(shape_call("_HIShapeIsEmpty", {rest}).x0 & 0xff, "a shape minus itself is empty");
    require(throws([&] { static_cast<void>(shape_call("_HIShapeUnionWithRect", {square, put({{0, 0}, {1, 1}})})); }),
            "an immutable shape must refuse changes");

    // The region's path fills only its rectangles.
    const auto context = gui::quartz2d::create_bitmap_context(40, 40);
    static_cast<void>(shape_call("_HIShapeReplacePathInCGContext", {region, context}));
    CallRegs color;
    color.x[0] = context;
    color.set_d(0, 1); color.set_d(1, 0); color.set_d(2, 0); color.set_d(3, 1);
    static_cast<void>(call(cg, "_CGContextSetRGBFillColor", color));
    CallRegs fill;
    fill.x[0] = context;
    static_cast<void>(call(cg, "_CGContextFillPath", fill));
    const auto image = gui::quartz2d::context_image(context);
    require(image.has_value(), "context_image");
    const auto at = [&](std::uint32_t x, std::uint32_t y_up) {   // straight RGBA, top row first
        const auto row = image->height - 1 - y_up;
        const auto* p = image->rgba.data() + (static_cast<std::size_t>(row) * image->width + x) * 4;
        return std::array<std::uint8_t, 4>{p[0], p[1], p[2], p[3]};
    };
    require(at(5, 5)[0] > 200 && at(5, 5)[3] == 255 && at(15, 5)[0] > 200 && at(2, 22)[0] > 200 && at(8, 22)[3] == 0 && at(30, 30)[3] == 0,
            "HIShapeReplacePathInCGContext");

    // HITheme: a checked check box in the accent color, a focus ring.
    const auto theme_context = gui::quartz2d::create_bitmap_context(40, 40);
    const auto info = f.process.allocate_guest(32);
    for (Address i = 0; i < 32; i += 8) f.engine.write64(info + i, 0);
    f.engine.write32(info + 4, 1);       // kThemeStateActive
    f.engine.write32(info + 8, 1 | (1U << 16));   // kThemeCheckBox, kThemeButtonOn
    CallRegs button;
    button.x[0] = put({{10, 10}, {14, 14}});
    button.x[1] = info;
    button.x[2] = theme_context;
    button.x[3] = 1;                     // kHIThemeOrientationInverted (y up)
    button.x[4] = scratch + 32;
    require(call(carbon, "_HIThemeDrawButton", button).x0 == 0, "HIThemeDrawButton");
    const auto themed = gui::quartz2d::context_image(theme_context);
    require(themed.has_value(), "themed context_image");
    const auto* corner = themed->rgba.data() + (static_cast<std::size_t>(themed->height - 1 - 12) * themed->width + 12) * 4;
    require(corner[2] > 180 && corner[0] < 120 && corner[3] == 255, "the checked check box is drawn in the accent color");
    const auto* outside = themed->rgba.data() + (static_cast<std::size_t>(themed->height - 1 - 35) * themed->width + 35) * 4;
    require(outside[3] == 0, "HIThemeDrawButton drew outside the button");
    f.engine.write32(info + 8, 200 | (1U << 16));   // an unknown kind
    require(throws([&] { static_cast<void>(call(carbon, "_HIThemeDrawButton", button)); }), "unknown button kinds must fail explicitly");
    f.process.release_guest(info);

    // Carbon key modifiers follow the window server.
    f.server.flags = (1ULL << 17) | (1ULL << 20);   // shift + command
    require(call(carbon, "_GetCurrentKeyModifiers", {}).x0 == (0x200 | 0x100), "GetCurrentKeyModifiers");
    f.server.flags = 0;
    for (const auto object : {square, region, band, rest, both, context, theme_context}) f.process.objects().release(object);
    f.process.release_guest(scratch);
}

void color_space_profiles(Fixture& f) {
    const auto space = f.id(f.cls("NSColorSpace"), "sRGBColorSpace");
    const auto data = f.id(space, "ICCProfileData");
    require(data != 0, "sRGB NSColorSpace ICCProfileData returned nil");
    require(f.id(data, "length") >= 132, "sRGB ICC profile has no header and tag table");
}

// AppKit with the Quartz 2D layer installed after it (the real process):
// guest -drawRect: code reaches the Quartz layer's NSGraphicsContext over the
// window backing store with the view's CTM and clip; AppKit's controls keep
// drawing; AppKit fonts are CoreText's; a control rendered by the Quartz
// layer's -cacheDisplayInRect:toBitmapImageRep: draws into the bitmap.
void quartz_coexistence(Fixture& f) {
    journal.clear();
    const auto cg_function = [&f](std::string_view name, CallRegs registers) {
        const auto found = f.process.resolve(gui::appkit::core_graphics_library, name);
        require(found.has_value(), "missing CoreGraphics export " + std::string(name));
        return alchemy::EngineAdapter(f.process.calling_engine()).call_regs(found->address, registers);
    };
    // Lower-left (unflipped) or upper-left (flipped) 10x10: blue through the
    // CGContext, then red beside it through NSColor / NSBezierPath.
    const auto paint = f.subclass("TestQuartzView", "NSView");
    f.add(paint, "drawRect:", [&f, cg_function](CpuState&, GuestMemory&) {
        const auto context = f.id(f.cls("NSGraphicsContext"), "currentContext");
        if (!context) { journal.push_back("no context"); return; }
        const auto cg = f.id(context, "CGContext");
        CallRegs color;
        color.x[0] = cg;
        color.set_d(0, 0); color.set_d(1, 0); color.set_d(2, 1); color.set_d(3, 1);
        static_cast<void>(cg_function("_CGContextSetRGBFillColor", color));
        CallRegs fill;
        fill.x[0] = cg;
        gui::appkit::put_rect(fill, 0, {{0, 0}, {10, 10}});
        static_cast<void>(cg_function("_CGContextFillRect", fill));
        static_cast<void>(f.id(f.id(f.cls("NSColor"), "redColor"), "set"));
        CallRegs rect;
        gui::appkit::put_rect(rect, 0, {{10, 0}, {10, 10}});
        static_cast<void>(f.send(f.cls("NSBezierPath"), "fillRect:", rect));
        journal.push_back((f.id(context, "isFlipped") & 0xff) ? "drew flipped" : "drew");
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    const auto flipped = f.subclass("TestQuartzFlippedView", "TestQuartzView");
    f.add(flipped, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");

    const auto window = f.make_window({{100, 100}, {200, 100}});
    const auto content = f.id(window, "contentView");
    const auto plain = f.with_rect(f.id(paint, "alloc"), "initWithFrame:", {{20, 20}, {50, 50}});
    const auto upside = f.with_rect(f.id(flipped, "alloc"), "initWithFrame:", {{100, 20}, {50, 50}});
    static_cast<void>(f.id(content, "addSubview:", {plain}));
    static_cast<void>(f.id(content, "addSubview:", {upside}));
    const auto check = f.with_rect(f.id(f.cls("NSButton"), "alloc"), "initWithFrame:", {{160, 70}, {30, 20}});
    static_cast<void>(f.id(check, "setButtonType:", {3}));
    static_cast<void>(f.id(check, "setState:", {1}));
    static_cast<void>(f.id(content, "addSubview:", {check}));
    const auto label = f.id(f.cls("NSTextField"), "labelWithString:", {f.str("Label")});
    static_cast<void>(f.id(content, "addSubview:", {label}));
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    static_cast<void>(f.next_event());
    require(!log_has("no context") && log_has("drew") && log_has("drew flipped"), "guest -drawRect: saw no Quartz NSGraphicsContext");
    const auto blue = [](std::array<std::uint8_t, 4> p) { return p[0] > 200 && p[1] < 40 && p[2] < 40 && p[3] == 255; };
    require(blue(f.pixel(window, 25, 25)) && is_red(f.pixel(window, 35, 25)) && is_white(f.pixel(window, 25, 45)),
            "CGContext / NSBezierPath drawing of an unflipped view");
    require(blue(f.pixel(window, 105, 65)) && is_red(f.pixel(window, 115, 65)) && is_white(f.pixel(window, 105, 25)),
            "CGContext / NSBezierPath drawing of a flipped view");
    require(f.id(f.cls("NSGraphicsContext"), "currentContext") == 0, "the Quartz current context leaked out of the display pass");
    const auto mark = f.pixel(window, 165, 76);   // inside the box, off the check mark
    require(mark[0] > 180 && mark[2] < 120, "the checkbox was not drawn");
    // AppKit's fonts are CoreText's NSFont objects.
    const auto font = f.id(label, "font");
    require(font && f.send(font, "pointSize").d(0) == 13 && f.text(f.id(font, "fontName")).size() > 0, "label font through CoreText");
    // -lockFocus outside the display pass.
    static_cast<void>(f.id(plain, "lockFocus"));
    require(f.id(f.cls("NSGraphicsContext"), "currentContext") != 0, "-lockFocus made no Quartz context current");
    static_cast<void>(f.id(f.id(f.cls("NSColor"), "greenColor"), "set"));
    CallRegs fill;
    gui::appkit::put_rect(fill, 0, {{30, 30}, {5, 5}});
    static_cast<void>(f.send(f.cls("NSBezierPath"), "fillRect:", fill));
    static_cast<void>(f.id(plain, "unlockFocus"));
    const auto green = f.pixel(window, 52, 52);
    require(green[1] > 200 && green[2] < 40 && green[0] < 40, "-lockFocus drawing through the Quartz layer");
    require(f.id(f.cls("NSGraphicsContext"), "currentContext") == 0, "-unlockFocus left a Quartz context current");
    // The Quartz layer renders an AppKit view into a bitmap (an unflipped
    // NSBox: the Quartz layer's placement of flipped views is still off).
    const auto box = f.with_rect(f.id(f.cls("NSBox"), "alloc"), "initWithFrame:", {{0, 0}, {60, 40}});
    static_cast<void>(f.id(box, "setTitle:", {f.str("Box")}));
    CallRegs bounds;
    gui::appkit::put_rect(bounds, 0, {{0, 0}, {60, 40}});
    const auto rep = f.send(box, "bitmapImageRepForCachingDisplayInRect:", bounds).x0;
    require(rep != 0, "-bitmapImageRepForCachingDisplayInRect:");
    auto cache = bounds;
    cache.x[2] = rep;
    static_cast<void>(f.send(box, "cacheDisplayInRect:toBitmapImageRep:", cache));
    const auto data = f.id(rep, "bitmapData");
    const auto row = f.id(rep, "bytesPerRow");
    const auto wide = f.id(rep, "pixelsWide"), high = f.id(rep, "pixelsHigh");
    std::size_t inked{};
    std::vector<std::uint8_t> line(row);
    for (Address y = 0; y < high; ++y) {
        f.engine.read_bytes(data + y * row, line);
        for (Address x = 0; x < wide; ++x) if (line[x * 4 + 3] > 128) ++inked;
    }
    require(inked > 20, "-cacheDisplayInRect:toBitmapImageRep: drew nothing of the box (" + std::to_string(inked) + " pixels)");
    static_cast<void>(f.id(window, "close"));
}
} // namespace

int main() {
    try {
        rasterizer();
        nib_archives();
        keyed_nibs();
        qlab_nibs();
        asset_catalogs();
        qlab_asset_catalog();
        rtf_documents();
        qlab_rtf();
        {
            Fixture f;
            color_space_profiles(f);
            view_drawing(f);
            layer_hosting_window(f);
            buttons(f);
            text_fields(f);
            popups_and_progress(f);
            scrolling_and_tables(f);
            tabs_splits_and_outlines(f);
            image_views(f);
            sliders_steppers_and_combo_boxes(f);
            panels_and_models(f);
            nib_loading(f, sample_nib().encode());
            nib_loading(f, keyed_archive(sample_nib()));
        }
        Fixture with_quartz;
        color_space_profiles(with_quartz);
        quartz_coexistence(with_quartz);
        hitoolbox(with_quartz);
    } catch (const std::exception& error) {
        std::cerr << "alchemy_gui_appkit_drawing: " << error.what() << '\n';
        return 1;
    }
    std::cout << "alchemy_gui_appkit_drawing: passed\n";
    return 0;
}
