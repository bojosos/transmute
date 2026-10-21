#pragma once

// The AppKit test fixture of the QLab gap group G4 integration tests: a
// process with Foundation, AppKit (over the fake window server), QuartzCore
// and the G4 providers, plus a NIBArchive builder.
#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/darwin/appkit/nib_archive.hpp"
#include "alchemy/darwin/appkit/tests/fake_window_server.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/darwin/kit/completion/completion_kit.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace windows_appkit_tests {
using Address = std::uint64_t;
using alchemy::CallRegs;
using alchemy::CallResult;
using transmute::CpuState;
using transmute::GuestMemory;

inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
extern std::vector<std::string> journal;

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> bytes) {
        return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices darwin{process};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    std::unique_ptr<alchemy::FoundationBridge> foundation;
    alchemy::gui::testing::FakeWindowServer server;
    std::shared_ptr<alchemy::gui::completion_kit::Support> support;
    Address app{};

    Fixture();
    ~Fixture();
    Address cls(std::string_view name);
    CallResult send(Address receiver, std::string_view selector, CallRegs registers = {});
    Address id(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {});
    Address str(std::string_view text) { return cf.string(text); }
    Address array(const std::vector<Address>& values) { return cf.array(values); }
    std::string text(Address object) { return object ? cf.utf8(object) : std::string("(nil)"); }
    Address subclass(std::string name, std::string_view super);
    void add(Address target, std::string name, transmute::GuestCallHandler handler, std::string types = "v@:@");
    Address with_rect(Address receiver, std::string_view selector, alchemy::gui::Rect rect);
    Address view(alchemy::gui::Rect frame, std::string_view cls_name = "NSView");
    Address make_window(alchemy::gui::Rect content);
    alchemy::gui::Rect frame(Address view_object);
    // [[first anchor] <relation>Anchor:[second second_anchor] constant:], activated.
    Address anchor_constraint(Address first, std::string_view anchor, std::string_view relation, Address second,
                              std::string_view second_anchor, double constant);
    // [[first anchor] <relation>Constant:], activated.
    Address size_constraint(Address first, std::string_view anchor, std::string_view relation, double constant);
    void set_size(Address window, alchemy::gui::Size size);
    std::vector<Address> foundation_items(Address array);
    // A global block literal whose invoke function is `handler`.
    Address block(transmute::GuestCallHandler handler);
};

void expect_frame(Fixture& f, Address view_object, alchemy::gui::Rect wanted, const std::string& what);

// Builds NIBArchive files value by value (objects are declared first so
// references can point forward).
struct NibBuilder {
    alchemy::gui::nib::Archive archive;
    std::vector<std::vector<alchemy::gui::nib::Value>> pending;
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
    std::uint32_t declare(const std::string& cls) {
        archive.objects.push_back({class_index(cls), 0, 0});
        pending.emplace_back();
        return static_cast<std::uint32_t>(archive.objects.size() - 1);
    }
    void put(std::uint32_t object, const std::string& name, alchemy::gui::nib::Value value) {
        value.key = key(name);
        pending.at(object).push_back(std::move(value));
    }
    void ref(std::uint32_t object, const std::string& name, std::uint32_t target) {
        alchemy::gui::nib::Value v;
        v.type = alchemy::gui::nib::ValueType::object;
        v.integer = target;
        put(object, name, v);
    }
    void integer(std::uint32_t object, const std::string& name, std::int64_t value) {
        alchemy::gui::nib::Value v;
        v.type = alchemy::gui::nib::ValueType::int32;
        v.integer = value;
        put(object, name, v);
    }
    void real(std::uint32_t object, const std::string& name, double value) {
        alchemy::gui::nib::Value v;
        v.type = alchemy::gui::nib::ValueType::float64;
        v.real = value;
        put(object, name, v);
    }
    void boolean(std::uint32_t object, const std::string& name, bool value) {
        alchemy::gui::nib::Value v;
        v.type = value ? alchemy::gui::nib::ValueType::true_ : alchemy::gui::nib::ValueType::false_;
        v.integer = value;
        put(object, name, v);
    }
    std::uint32_t string(const std::string& text) {
        const auto object = declare("NSString");
        alchemy::gui::nib::Value v;
        v.type = alchemy::gui::nib::ValueType::data;
        v.data.assign(text.begin(), text.end());
        put(object, "NS.bytes", v);
        return object;
    }
    std::uint32_t array(const std::string& cls, const std::vector<std::uint32_t>& elements) {
        const auto object = declare(cls);
        boolean(object, "NSInlinedValue", false);
        for (const auto element : elements) ref(object, "UINibEncoderEmptyKey", element);
        return object;
    }
    alchemy::gui::nib::Archive finish() {
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

} // namespace windows_appkit_tests
