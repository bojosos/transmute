#pragma once

#include "alchemy/core_foundation.hpp"
#include "alchemy/objc_runtime.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace alchemy {

// Apple framework and /usr/lib identities that Alchemy provides for a headless
// process. Every symbol a provider exports has headless semantics: no display,
// GPU, audio device, input source or IOKit registry exists, so queries answer
// "none" and operations that would need a device return the framework's
// documented failure code. Absent symbols stay unresolved and fail at bind.
inline constexpr std::array<std::string_view, 52> darwin_framework_libraries{
    "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit",
    "/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa",
    "/System/Library/Frameworks/Metal.framework/Versions/A/Metal",
    "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore",
    "/System/Library/Frameworks/OpenAL.framework/Versions/A/OpenAL",
    "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL",
    "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices",
    "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics",
    "/System/Library/Frameworks/ColorSync.framework/Versions/A/ColorSync",
    "/System/Library/Frameworks/CoreVideo.framework/Versions/A/CoreVideo",
    "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit",
    "/System/Library/Frameworks/Carbon.framework/Versions/A/Carbon",
    "/System/Library/Frameworks/AudioUnit.framework/Versions/A/AudioUnit",
    "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox",
    "/System/Library/Frameworks/CoreAudio.framework/Versions/A/CoreAudio",
    "/usr/lib/libiconv.2.dylib",
    // Linked by Blender without imported symbols (zlib/bzip2 are bundled).
    // The identities load; any symbol bound to them is unresolved.
    "/usr/lib/libz.1.dylib",
    "/usr/lib/libbz2.1.0.dylib",
    // Proxy settings for CPython's _scproxy. The exports are installed by the
    // GUI build (src/alchemy/sysconfig, alchemy-fx); without it the identity
    // loads and every symbol bound to it is unresolved.
    "/System/Library/Frameworks/SystemConfiguration.framework/Versions/A/SystemConfiguration",
    // Identities an installer links. The headless additions
    // (darwin/headless_additions.hpp) install headless exports and
    // placeholder explicit failures for them; the GUI
    // build replaces the placeholders with real providers (CoreGraphics
    // drawing, CoreText and ImageIO in src/alchemy/quartz2d, coretext and
    // imageio; Security in src/alchemy/security).
    "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices",
    "/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText",
    "/System/Library/Frameworks/ImageIO.framework/Versions/A/ImageIO",
    "/System/Library/Frameworks/Security.framework/Versions/A/Security",
    // Linked without imported symbols.
    "/System/Library/Frameworks/AGL.framework/Versions/A/AGL",
    // Security (above) and CFNetwork (src/alchemy/security, src/alchemy/network). The
    // exports are installed by the GUI build (alchemy-fx); without it the
    // identities load and every symbol bound to them is unresolved.
    "/System/Library/Frameworks/CFNetwork.framework/Versions/A/CFNetwork",
    // The BIND resolver's res_9_n* (src/alchemy/darwin/network/resolver.cpp, GUI build).
    "/usr/lib/libresolv.9.dylib",
    // GSS-API for Qt Network's Negotiate authentication (src/alchemy/gss, GUI
    // build): names work, every mechanism (Kerberos, NTLM, SPNEGO) is
    // unavailable. Without the GUI build the identity loads and every symbol
    // bound to it is unresolved.
    "/System/Library/Frameworks/GSS.framework/Versions/A/GSS",
    // Strict mode: identities Qt WebEngine links. The
    // GUI build installs their exports (src/alchemy/media, devices, printing,
    // machsys, fwgaps): no capture, audio, MIDI, codec, Bluetooth, HID,
    // location, Wi-Fi, biometric or printing hardware or service exists, and
    // the documented failure values say so. Without the GUI build the
    // identities load and every symbol bound to them is unresolved.
    "/usr/lib/libbsm.0.dylib",
    "/usr/lib/libpmenergy.dylib",
    "/usr/lib/libpmsample.dylib",
    "/usr/lib/libcups.2.dylib",
    // terminfo without a database (a shader compiler's terminal colours; src/alchemy/machsys).
    "/usr/lib/libncurses.5.4.dylib",
    "/System/Library/Frameworks/AVFoundation.framework/Versions/A/AVFoundation",
    "/System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia",
    "/System/Library/Frameworks/VideoToolbox.framework/Versions/A/VideoToolbox",
    "/System/Library/Frameworks/CoreMIDI.framework/Versions/A/CoreMIDI",
    "/System/Library/Frameworks/MediaAccessibility.framework/Versions/A/MediaAccessibility",
    "/System/Library/Frameworks/ImageCaptureCore.framework/Versions/A/ImageCaptureCore",
    "/System/Library/Frameworks/CoreImage.framework/Versions/A/CoreImage",
    "/System/Library/Frameworks/IOBluetooth.framework/Versions/A/IOBluetooth",
    "/System/Library/Frameworks/CoreBluetooth.framework/Versions/A/CoreBluetooth",
    "/System/Library/Frameworks/GameController.framework/Versions/A/GameController",
    "/System/Library/Frameworks/ForceFeedback.framework/Versions/A/ForceFeedback",
    "/System/Library/Frameworks/CoreWLAN.framework/Versions/A/CoreWLAN",
    "/System/Library/Frameworks/CoreLocation.framework/Versions/A/CoreLocation",
    "/System/Library/Frameworks/LocalAuthentication.framework/Versions/A/LocalAuthentication",
    "/System/Library/Frameworks/OpenDirectory.framework/Versions/A/OpenDirectory",
    "/System/Library/Frameworks/SecurityInterface.framework/Versions/A/SecurityInterface",
    // Linked without imported symbols (the sandbox calls bind through libSystem).
    "/System/Library/Frameworks/Quartz.framework/Versions/A/Quartz",
    "/usr/lib/libsandbox.1.dylib",
    // Linked without imported symbols by a renderer SDK's image and material
    // libraries. Any symbol bound to them stays unresolved.
    "/System/Library/Frameworks/GLUT.framework/Versions/A/GLUT",
    "/usr/lib/libxml2.2.dylib"};

[[nodiscard]] inline bool darwin_framework_library(std::string_view library) {
    return std::find(darwin_framework_libraries.begin(), darwin_framework_libraries.end(), library) !=
        darwin_framework_libraries.end();
}

// Installs the framework providers. Requires the Objective-C runtime (NSObject)
// and CoreFoundation, so it runs after FoundationBridge.
void install_darwin_frameworks(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf);

} // namespace alchemy
