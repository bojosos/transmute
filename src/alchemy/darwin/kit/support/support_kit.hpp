#pragma once

// Qt 6 Cocoa QPA support (owner: qt-vfb, src/alchemy/darwin/kit/support/): the macOS
// interfaces Qt's libqcocoa platform plugin and QtGui need that no other GUI
// provider implements:
//   iosurface.cpp       IOSurface.framework: host-memory surfaces (guest
//                       memory) with the IOSurface C API and the IOSurface
//                       class (toll-free bridged object usable as layer contents)
//   display_link.cpp    CoreVideo CVDisplayLink: a guest thread per link that
//                       calls the output callback at the display refresh rate
//   layer_contents.cpp  CALayer contents (IOSurface or CGImage) of view
//                       backing layers composited into the window and shown
//                       with WindowServer::present_bitmap (QCALayerBackingStore)
//   cg_extra.cpp        CGDisplayMode, CGGetDisplaysWithRect, listen-only event taps
//   the rest of Qt's bind-time surface (constants, NSThread, NSBundle
//                       localizations, shells, explicit failures): one piece per
//                       framework, in the frameworks' directories (install_*_ below)
//   notification_blocks.cpp  NSNotificationCenter block observers
//   pasteboard.cpp      Carbon Pasteboard Manager over the host clipboard (text)
//   appkit_extra.cpp    AppKit selectors Qt sends that AppKit lacks (added only when missing)
//   selector_audit.cpp  ALCHEMY_SELECTOR_AUDIT bring-up diagnostic
//   wait_trace.cpp      ALCHEMY_WAIT_DUMP hang diagnostic
//   mutable_string.cpp NSMutableString over CF mutable strings
//   accessibility.cpp   NSAccessibility names and helpers
//   objc_assoc.cpp      objc_set/getAssociatedObject (when libobjc lacks them)
//   (darwin/opengl/no_context.cpp: AGL / OpenGL identities, no OpenGL context exists)
//   install.cpp         install_support_kit
// Not a contract header; the AppKit hook is include/alchemy/gui/layer_contents.hpp.

#include "alchemy/call_regs.hpp"
#include "alchemy/core_foundation.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/gui/window_server.hpp"
#include "alchemy/object_store.hpp"
#include "alchemy/objc_runtime.hpp"
#include "alchemy/process_context.hpp"

#include <bit>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::support_kit {

using Address = std::uint64_t;
using transmute::CpuState;
using transmute::GuestMemory;
using Handler = transmute::GuestCallHandler;

inline constexpr std::string_view iosurface_library = "/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface";
inline constexpr std::string_view core_video_library = "/System/Library/Frameworks/CoreVideo.framework/Versions/A/CoreVideo";
inline constexpr std::string_view core_graphics_library = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
inline constexpr std::string_view agl_library = "/System/Library/Frameworks/AGL.framework/Versions/A/AGL";
inline constexpr std::string_view opengl_library = "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL";
inline constexpr std::string_view libsystem_library = "/usr/lib/libSystem.B.dylib";
inline constexpr std::string_view objc_library = "/usr/lib/libobjc.A.dylib";

[[noreturn]] void fail(std::string_view why);          // "Alchemy support kit: <why>"
[[noreturn]] void unsupported(std::string_view what);  // "unsupported: Alchemy support kit: <what>"
void diagnostic(std::string_view text);                 // one stderr line

inline double real(const CpuState& s, unsigned index) { return std::bit_cast<double>(s.v[index][0]); }
inline void return_real(CpuState& s, unsigned index, double value) { s.v[index] = {std::bit_cast<std::uint64_t>(value), 0}; }

struct Support : std::enable_shared_from_this<Support> {
    ProcessContext& process;
    ObjcRuntime& objc;
    CoreFoundation& cf;
    std::uint64_t main_thread_id{};   // the host thread that ran install (the guest main thread)

    Support(ProcessContext& context, ObjcRuntime& runtime, CoreFoundation& foundation)
        : process(context), objc(runtime), cf(foundation) {}

    [[nodiscard]] GuestMemory& memory() const { return process.calling_engine(); }
    [[nodiscard]] bool on_main_thread() const;
    // Exported C function; replaces an existing export of the same name.
    // `where` (here and below) attributes the registration for the registration
    // audit (alchemy/registration_audit.hpp); callers never pass it.
    void function(std::string_view library, std::string symbol, Handler handler,
                  const std::source_location& where = std::source_location::current());
    // Exported only when no provider exports `symbol` from `library` yet.
    void fill_function(std::string_view library, std::string symbol, Handler handler,
                       const std::source_location& where = std::source_location::current());
    // Data export holding a constant CFString.
    void string_constant(std::string_view library, std::string symbol, std::string_view value,
                         const std::source_location& where = std::source_location::current());
    // A host class (found or registered), published under `library` when given.
    Address declare_class(std::string_view name, std::string_view superclass, std::string_view library = {},
                          const std::source_location& where = std::source_location::current());
    void method(Address cls, std::string name, Handler handler, std::string types, bool class_method = false,
                const std::source_location& where = std::source_location::current());
    // CF-type object of host class `cls`: +1.
    Address create(Address cls, std::shared_ptr<ObjectPayload> payload, std::function<void(Address)> destroy = {});
    Address retain(Address object) { return object ? process.objects().retain(object) : 0; }
    void release(Address object) { if (object) process.objects().release(object); }
    CallResult send(Address receiver, std::string_view selector, CallRegs registers = {});
    Address send_id(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {});
    // Calls an exported function of `library` (another provider's).
    CallResult call(std::string_view library, std::string_view symbol, const CallRegs& registers);
    Address call(std::string_view library, std::string_view symbol, std::initializer_list<Address> arguments);
};

[[nodiscard]] Support* support() noexcept;   // the process instance (install_support_kit), or nullptr

// ---- IOSurface (iosurface.cpp) ---------------------------------------------------------
struct SurfaceView {
    Address base{};
    std::uint32_t width{}, height{}, bytes_per_element{};
    std::uint64_t bytes_per_row{};
    std::uint32_t pixel_format{};   // OSType ('BGRA' = 0x42475241)
    std::uint32_t seed{};
};
// The pixel memory of an IOSurface object; nullopt when `object` is not one.
[[nodiscard]] std::optional<SurfaceView> surface_view(Support& support, Address object);
// Waits (bounded) until the GPU writes submitted to the surface so far, in
// any process of the tree, have completed: the compositor's implicit
// synchronization before it reads the pages. No-op for other objects.
void wait_for_surface_gpu_writes(Support& support, Address object);
// A use of an IOSurface (IOSurfaceIncrementUseCount) held while the result
// lives, also after the surface is freed; nullptr when `object` is not one.
[[nodiscard]] std::shared_ptr<void> hold_surface_use(Support& support, Address object);
void install_iosurface(Support& support);

// ---- CVDisplayLink (display_link.cpp) ----------------------------------------------------
void install_display_link(Support& support);
// CAMetalDisplayLink, CAMetalDisplayLinkUpdate, the CADynamicRange keys
// (metal_display_link.cpp; called by install_display_link).
void install_metal_display_link(Support& support);

// ---- CALayer contents (layer_contents.cpp) --------------------------------------------------
void install_layer_contents(Support& support);

// ---- CoreGraphics display modes, event taps (cg_extra.cpp) ---------------------------------------
void install_cg_extra(Support& support);

// ---- the remaining Qt bind-time surface (qt_imports.cpp, accessibility.cpp) ----------------------
// The rest of Qt's bind-time surface, one piece per framework (the former
// qt_imports.cpp), installed in this order:
void install_appkit_constants_and_shells(Support& support);   // darwin/appkit/constants_and_shells.cpp
void install_foundation_threads_and_localizations(Support& support);   // darwin/foundation/threads_and_localizations.cpp
void install_coretext_font_data(Support& support);   // darwin/coretext/font_data.cpp
void install_quartzcore_shape_layer_shell(Support& support);   // darwin/quartzcore/shape_layer_shell.cpp
void install_metal_stage_input_output_shell(Support& support);   // darwin/metal/stage_input_output_shell.cpp
void install_coregraphics_color_space_property_list(Support& support);   // darwin/coregraphics/color_space_property_list.cpp
void install_coreservices_icon_refs(Support& support);   // darwin/coreservices/icon_refs.cpp
void install_applicationservices_icon_services(Support& support);   // darwin/applicationservices/icon_services.cpp
void install_accessibility(Support& support);
void install_mutable_string(Support& support);   // mutable_string.cpp
void install_wait_trace(Support& support);   // wait_trace.cpp (ALCHEMY_WAIT_DUMP)
void run_selector_audit(Support& support);   // selector_audit.cpp (ALCHEMY_SELECTOR_AUDIT)
void install_appkit_extra(Support& support);   // appkit_extra.cpp
void install_pasteboard(Support& support);   // pasteboard.cpp
void install_notification_blocks(Support& support);   // notification_blocks.cpp

// ---- libobjc associated objects (objc_assoc.cpp) ------------------------------------------------
void install_objc_associations(Support& support);

// ---- AGL / OpenGL identities (gl_stubs.cpp) ----------------------------------------------------
void install_opengl_no_context(Support& support);   // darwin/opengl/no_context.cpp

} // namespace alchemy::gui::support_kit
