#include "alchemy/darwin/kit/support/support_kit.hpp"
#include "alchemy/darwin/foundation/install.hpp"

#include "alchemy/host/process.hpp"
#include "alchemy/registration_audit.hpp"

#include <cstdio>

// install_support_kit and the shared helpers of src/alchemy/darwin/kit/support.
namespace alchemy::gui::support_kit {
namespace {
std::shared_ptr<Support> process_support;
} // namespace

void fail(std::string_view why) { throw std::runtime_error("Alchemy support kit: " + std::string(why)); }
void unsupported(std::string_view what) { throw std::runtime_error("unsupported: Alchemy support kit: " + std::string(what)); }
void diagnostic(std::string_view text) {
    std::fprintf(stderr, "[alchemy support] %.*s\n", static_cast<int>(text.size()), text.data());
    std::fflush(stderr);
}

Support* support() noexcept { return process_support.get(); }

bool Support::on_main_thread() const { return host::process::current_thread_id() == main_thread_id; }

void Support::function(std::string_view library, std::string symbol, Handler handler, const std::source_location& where) {
    registration_audit::Site site(where);
    static_cast<void>(process.install_function(std::move(symbol),
        [owner = shared_from_this(), handler = std::move(handler)](CpuState& s, GuestMemory& m) { handler(s, m); },
        std::string(library)));
}

void Support::fill_function(std::string_view library, std::string symbol, Handler handler, const std::source_location& where) {
    registration_audit::Site site(where);
    const bool skipped = process.resolve(library, symbol).has_value();
    registration_audit::record_fill(library, symbol, skipped, where);
    if (skipped) return;
    function(library, std::move(symbol), std::move(handler), where);
}

void Support::string_constant(std::string_view library, std::string symbol, std::string_view value,
                              const std::source_location& where) {
    registration_audit::Site site(where);
    const auto slot = process.allocate_guest(8);
    process.main_engine().write64(slot, cf.constant_string(value));
    process.install_export(std::string(library), std::move(symbol), {slot, ExportKind::data});
}

Address Support::declare_class(std::string_view name, std::string_view superclass, std::string_view library,
                               const std::source_location& where) {
    registration_audit::Site site(where);
    auto cls = objc.find_class(name);
    if (!cls) {
        const auto parent = objc.find_class(superclass);
        if (!parent) fail("missing superclass " + std::string(superclass));
        cls = objc.register_native_class(std::string(name), parent, 8);
    }
    if (!library.empty()) objc.publish_class(name, cls, library);
    return cls;
}

void Support::method(Address cls, std::string name, Handler handler, std::string types, bool class_method,
                     const std::source_location& where) {
    registration_audit::Site site(where);
    static_cast<void>(objc.add_native_method(cls, std::move(name),
        [owner = shared_from_this(), handler = std::move(handler)](CpuState& s, GuestMemory& m) { handler(s, m); },
        class_method, std::move(types)));
}

Address Support::create(Address cls, std::shared_ptr<ObjectPayload> payload, std::function<void(Address)> destroy) {
    return process.objects().create(16, cls, ObjectKind::instance, std::move(payload), std::move(destroy));
}

CallResult Support::send(Address receiver, std::string_view selector, CallRegs registers) {
    return objc.send(receiver, objc.selector(selector), std::move(registers));
}

Address Support::send_id(Address receiver, std::string_view selector, std::initializer_list<Address> arguments) {
    CallRegs registers;
    unsigned index = 2;
    for (const auto value : arguments) registers.x.at(index++) = value;
    return send(receiver, selector, registers).x0;
}

CallResult Support::call(std::string_view library, std::string_view symbol, const CallRegs& registers) {
    const auto found = process.resolve(library, symbol);
    if (!found) unsupported(std::string(symbol.substr(1)) + " (its provider is not installed)");
    return EngineAdapter(process.calling_engine()).call_regs(found->address, registers);
}

Address Support::call(std::string_view library, std::string_view symbol, std::initializer_list<Address> arguments) {
    CallRegs registers;
    unsigned index = 0;
    for (const auto value : arguments) registers.x.at(index++) = value;
    return call(library, symbol, registers).x0;
}

} // namespace alchemy::gui::support_kit

namespace alchemy::gui {

void install_support_kit(InstallContext& context) {
    using namespace support_kit;
    auto created = std::make_shared<Support>(context.process, context.objc, context.cf);
    created->main_thread_id = host::process::current_thread_id();
    process_support = created;
    auto& value = *created;
    install_iosurface(value);
    install_display_link(value);
    install_layer_contents(value);
    install_opengl_no_context(value);
    install_foundation_associated_objects(value);
    install_cg_extra(value);
    install_appkit_constants_and_shells(value);
    install_foundation_threads_and_localizations(value);
    install_coretext_font_data(value);
    install_quartzcore_shape_layer_shell(value);
    install_metal_stage_input_output_shell(value);
    install_coregraphics_color_space_property_list(value);
    install_coreservices_icon_refs(value);
    install_applicationservices_icon_services(value);
    install_accessibility(value);
    install_appkit_extra(value);
    install_pasteboard(value);
    install_foundation_block_observers(value);
    install_wait_trace(value);
    install_foundation_mutable_strings(value);
    run_selector_audit(value);   // last: sees every provider
}

} // namespace alchemy::gui
