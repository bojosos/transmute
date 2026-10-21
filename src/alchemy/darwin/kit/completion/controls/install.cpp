#include "alchemy/darwin/appkit/controls/g3.hpp"

// QLab gap group G3 (controls): AppKit controls, text, tables/outlines/collections and accessibility.
// Owner and scope: docs/ALCHEMY_QLAB_GAPS.md, group G3. Each provider file in
// this directory adds one install_* function called from here; cmake/darwin/kit_completion_controls.cmake
// lists the sources and the group's tests. The shared helpers are below.
namespace alchemy::gui::completion_kit {
namespace controls {
using namespace appkit;

AppKit* appkit_of(Support& support) {
    const auto kit = installed_appkit();
    // The AppKit of this process (a test may have made others before).
    if (!kit || &kit->process != &support.process) return nullptr;
    return kit.get();
}

bool Decoder::has(std::string_view key) const {
    return kit.send_bool(coder, "containsValueForKey:", {kit.auto_string(key)});
}
Address Decoder::object(std::string_view key) const {
    return kit.send_id(coder, "decodeObjectForKey:", {kit.auto_string(key)});
}
std::int64_t Decoder::integer(std::string_view key, std::int64_t fallback) const {
    if (!has(key)) return fallback;
    return static_cast<std::int64_t>(kit.send_id(coder, "decodeIntegerForKey:", {kit.auto_string(key)}));
}
double Decoder::real(std::string_view key, double fallback) const {
    if (!has(key)) return fallback;
    CallRegs registers;
    registers.x[2] = kit.auto_string(key);
    return kit.send(coder, "decodeDoubleForKey:", registers).d(0);
}
bool Decoder::flag(std::string_view key, bool fallback) const {
    if (!has(key)) return fallback;
    return kit.send_bool(coder, "decodeBoolForKey:", {kit.auto_string(key)});
}
std::optional<std::string> Decoder::string(std::string_view key) const {
    const auto value = object(key);
    if (!value || kit.cf.kind(value) != ObjectKind::string) return std::nullopt;
    return kit.text(value);
}
std::optional<Rect> Decoder::rect(std::string_view key) const {
    if (!has(key)) return std::nullopt;
    CallRegs registers;
    registers.x[2] = kit.auto_string(key);
    return result_rect(kit.send(coder, "decodeRectForKey:", registers));
}

void forward_to_cell(AppKit& kit, Address control_class, std::string name, std::string types, bool redisplay) {
    auto* const k = &kit;
    kit.method(control_class, name, [k, name, redisplay](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto target = control_cell(*k, self);
        if (!target) { s.x[0] = 0; s.x[1] = 0; return; }
        CallRegs registers;
        for (unsigned i = 2; i < 8; ++i) registers.x[i] = s.x[i];
        for (unsigned i = 0; i < 8; ++i) registers.v[i] = s.v[i];
        const auto result = k->send(target, name, registers);
        s.x[0] = result.x0;
        s.x[1] = result.x1;
        for (unsigned i = 0; i < 4; ++i) s.v[i] = result.v[i];
        if (redisplay) k->invalidate_view(self);
    }, std::move(types));
}

Address dictionary_get(AppKit& kit, Address dictionary, std::string_view key) {
    if (!dictionary) return 0;
    const auto name = kit.string(key);
    Address value{};
    try { value = kit.cf.dictionary_value(dictionary, name); } catch (...) { kit.release(name); throw; }
    kit.release(name);
    return value;
}

Address super_init_with_coder(AppKit& kit, Address self, Address cls, Address coder) {
    CallRegs registers;
    registers.x[2] = coder;
    return kit.objc.send_super(self, cls, kit.sel("initWithCoder:"), registers).x0;
}

} // namespace controls

void install_group_controls(Support& support) {
    // The group extends AppKit: without it (installs of other providers
    // alone) there is nothing to extend.
    auto* const installed = controls::appkit_of(support);
    if (!installed) return;
    auto& kit = *installed;
    controls::install_segmented(kit);
    controls::install_path_control(kit);
    controls::install_search_field(kit);
    controls::install_date_picker(kit);
    controls::install_color_well(kit);
    controls::install_text_system(kit);
    controls::install_tables(kit);
    controls::install_collection_view(kit);
    controls::install_rulers(kit);
    controls::install_accessibility(kit);
    controls::install_small_additions(kit);
}

} // namespace alchemy::gui::completion_kit
