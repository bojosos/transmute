#pragma once

// GUI provider registration (alchemy-fx only, ALCHEMY_WITH_GUI).
//
// install_darwin_gui runs once on the main thread from run_program (and from
// `alchemy-fx doctor`, which must see the same exports, so installers never
// start anything), right after install_darwin_frameworks has registered the
// headless shells (the
// AppKit/Metal/QuartzCore/CoreGraphics classes and functions of
// darwin/identities/frameworks.cpp and darwin/foundation/foundation_extra.cpp).
// It calls the subsystem
// installers below in this fixed order:
//
//   install_runloop       src/alchemy/darwin/runloop/  (CFRunLoop, NSRunLoop, NSTimer, main pump)
//   install_windowserver  src/alchemy/darwin/windowserver/  (Win32 windows, input, cursors, clipboard, screens)
//   install_appkit_gui    src/alchemy/darwin/appkit/  (NSApplication, NSWindow, NSView, NSEvent, ...)
//   install_quartzcore    src/alchemy/darwin/quartzcore/  (CALayer, CAMetalLayer, presentation)
//   install_metal_gui     src/alchemy/darwin/metal/  (MTLDevice and the rest of Metal on Vulkan)
//   install_system_configuration src/alchemy/darwin/systemconfiguration/ (SCDynamicStoreCopyProxies over the Windows proxy settings)
//   install_quartz2d      src/alchemy/darwin/coregraphics/, coretext/ (CoreGraphics drawing, CoreText, ImageIO; AppKit drawing classes)
//   install_security      src/alchemy/darwin/security/  (Security: SecTrust, SecKey, keychain, SecureTransport; NSError/CFError)
//   install_network       src/alchemy/darwin/network/  (CFNetwork, NSURL network URLs, NSURLSession over WinHTTP, Bonjour: dns_sd, NSNetService)
//   install_gss           src/alchemy/darwin/security/  (GSS: names; Kerberos/NTLM/SPNEGO unavailable)
//   install_zlib ... install_dispatch_data  the former gap-fill group, one row per framework
//                          (libz, libSystem, IOKit, CF, Foundation, CoreServices, dispatch data gaps; fill-only)
//   install_support_kit    src/alchemy/darwin/kit/support/  (IOSurface, CVDisplayLink, CALayer contents of layer-backed views; Qt QPA; fills gaps only)
//   install_completion_kit  src/alchemy/darwin/kit/completion/  (QLab 5 launch surface; after Qt support, before the fill-only set: completes classes the providers above declare)
//   install_mach           src/alchemy/darwin/libsystem/mach/ (in-process and cross-process Mach IPC, notify(3); before the fill-only set)
//   fill::install_machsys  src/alchemy/darwin/libsystem/mach/ (libSystem/libbsm gaps: launchd, sandbox, libc; fills gaps only)
//   fill::install_fwgaps   src/alchemy/darwin/kit/fwgaps/  (CF, Foundation, AppKit, CG, CA, CT, CGL, CoreServices, Security, SC, vDSP gaps)
//   fill::install_media    src/alchemy/darwin/media/  (CoreMedia, VideoToolbox, AVFoundation, audio, MIDI; no devices or codecs)
//   fill::install_devices  src/alchemy/darwin/iokit/  (HID, power, Bluetooth, controllers, Wi-Fi, location, LocalAuthentication)
//   fill::install_printing src/alchemy/darwin/printing/  (libcups, PrintCore; no printing system)
//   fill::install_libsystem_leftovers src/alchemy/darwin/libsystem/misc/ (libSystem/libbsm/libbz2 leftovers; Chrome imported them first)
//                          (declared in src/alchemy/gui/fill_provider.hpp; the strict-mode set, last)
//   install_values_kit     src/alchemy/gui/value_provider.cpp (the value-class frameworks and additions; fill-only, last;
//                          declared in src/alchemy/gui/value_provider.hpp)
//
// Later installers may rely on earlier ones (AppKit pumps through the run loop
// and draws through the window server; QuartzCore presents to window-server
// HWNDs; Metal completion handlers run on the run loop's completion thread).
//
// Overriding rule: an installer replaces a headless shell by registering over
// it, never by editing the shell's source:
// - functions and data: ProcessContext::install_export (insert-or-assign);
// - Objective-C methods: ObjcRuntime::add_native_method on the existing class
//   (find_class("NSApplication")); a method added later replaces the earlier
//   one for the same selector;
// - new classes: ObjcRuntime::register_native_class, then publish them under
//   the framework's install name with install_export("_OBJC_CLASS_$_...")
//   like darwin/identities/frameworks.cpp's declare_class.
// Host classes keep their state in ObjcRuntime::host_state, never in the
// guest instance (guest subclasses add ivars right after the 8-byte isa).
// Methods a guest subclass may override are called through
// ObjcRuntime::send, never through the C++ handler.
//
// Every installer must leave the process able to run headless (Blender -b):
// installation may not create windows, devices or threads, and a missing
// window server / Vulkan driver must fail only when the guest actually asks
// for a window or device, with the framework's documented failure value or
// an explicit "unsupported" error.

#include "alchemy/core_foundation.hpp"
#include "alchemy/objc_runtime.hpp"
#include "alchemy/process_context.hpp"

namespace alchemy {
namespace gui {

// Everything an installer needs. The referenced objects live for the whole
// process (they outlive every guest thread), so installers may capture them;
// the struct itself is a temporary of install_darwin_gui and must not be
// kept. New members may be appended.
struct InstallContext {
    ProcessContext& process;
    ObjcRuntime& objc;
    CoreFoundation& cf;
};

void install_runloop(InstallContext& context);
void install_windowserver(InstallContext& context);
void install_appkit_gui(InstallContext& context);
void install_quartzcore(InstallContext& context);
void install_metal_gui(InstallContext& context);
void install_system_configuration(InstallContext& context);
void install_quartz2d(InstallContext& context);
void install_security(InstallContext& context);
void install_network(InstallContext& context);
void install_gss(InstallContext& context);
void install_completion_kit(InstallContext& context);
void install_support_kit(InstallContext& context);

// Not part of installation: run_program calls this when the main executable
// links Metal.framework, once that image is mapped at its fixed address
// (ProcessContext::main_image_mapped) and before its dependencies load: the
// driver's own allocations must not take the executable's range. It creates
// the system Vulkan device (instance, device, pipeline cache) on a background
// thread, so the 0.5-1.5 s of driver initialization overlaps the loader and
// MTLCreateSystemDefaultDevice finds the device ready (it waits for a creation
// in progress). ALCHEMY_METAL_PREFETCH=0 disables it. The returned handle
// joins the background threads when released: run_program keeps it until it
// returns, so they never outlive the process's static state.
[[nodiscard]] std::shared_ptr<void> prefetch_metal_device();

// `alchemy-fx msl-seed <corpus> <executable> <root> [threads]`: compiles an
// ALCHEMY_MSL_DUMP corpus into <root>/<LC_UUID of executable>/results, the
// seed set Metal consults on a cache miss (ALCHEMY_MSL_SEED=<root>, default
// %LOCALAPPDATA%\Alchemy\msl-seed). Returns the process exit status.
int seed_msl_corpus(const std::filesystem::path& corpus, const std::filesystem::path& executable,
                    const std::filesystem::path& root, unsigned threads);

} // namespace gui

// Runs the installers above in order. Requires install_darwin_frameworks.
void install_darwin_gui(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);

} // namespace alchemy
