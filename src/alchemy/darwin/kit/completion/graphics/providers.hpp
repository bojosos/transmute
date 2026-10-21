#pragma once

// The providers of the QLab graphics group (G6), one install_* per file in
// this directory, called in order by install_group_graphics (install.cpp).

#include "alchemy/darwin/kit/completion/completion_kit.hpp"

namespace alchemy::gui::completion_kit::graphics {

void install_metalkit(Support& support);   // metalkit.cpp: the MetalKit identity, MTKTextureLoader
void install_glkit(Support& support);      // glkit.cpp: the GLKit identity, GLKMath values and slerp
void install_webkit(Support& support);     // webkit.cpp: the WebKit identity, configurations and views (no web engine)
void install_animation(Support& support);  // animation.cpp: CAAnimation and subclasses, kCA* names
void install_transform3d(Support& support);   // transform3d.cpp: the CATransform3D functions
void install_core_image(Support& support);   // core_image.cpp: CIImage, CIFilter, CIContext, CIVector, CIColor, CIKernel (ci_engine.cpp)
void install_crypto(Support& support);   // crypto_exports.cpp: _alchemy_cryptokit_* host primitives of the CryptoKit overlay (crypto_core.cpp)
void install_metal_display(Support& support);   // metal_display.cpp: MTLCopyAllDevicesWithObserver, CGDisplay Metal device and EDID identity
void install_appkit_drawing(Support& support);   // appkit_drawing.cpp: NSShadow, NSGradient, NSAffineTransform, NSColor/NSImage additions

} // namespace alchemy::gui::completion_kit::graphics
