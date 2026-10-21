#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/shells.hpp"
#include "alchemy/objc_runtime.hpp"

#include <stdexcept>

// The headless framework set, in every build (include/alchemy/darwin_frameworks.hpp):
// independently authored headless providers for the Apple frameworks in a
// background (no window server, no GPU, no audio hardware) process, each in
// its framework's directory. Numbers and structure layouts follow the public
// SDK headers; no Apple code is used.
namespace alchemy {

void install_darwin_frameworks(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    if (!objc.find_class("NSObject")) throw std::runtime_error("Alchemy frameworks require the NSObject runtime class");
    darwin_headless::install_appkit_base_classes(context, objc, cf);
    darwin_headless::install_metal_base_classes(context, objc, cf);
    darwin_headless::install_quartzcore_base_layers(context, objc, cf);
    darwin_headless::install_openal(context, objc, cf);
    darwin_headless::install_coregraphics_no_displays(context, objc, cf);
    darwin_headless::install_colorsync(context, objc, cf);
    darwin_headless::install_media_pixel_buffers(context, objc, cf);
    darwin_headless::install_opengl_cgl(context, objc, cf);
    darwin_headless::install_iokit_empty_registry(context, objc, cf);
    darwin_headless::install_appkit_input_sources(context, objc, cf);
    darwin_headless::install_coreservices_shared_file_lists(context, objc, cf);
    darwin_headless::install_coreservices_carbon_core_math(context, objc, cf);
    darwin_headless::install_media_audio_components(context, objc, cf);
    darwin_headless::install_libiconv(context, objc, cf);
    darwin_headless::install_additions(context, objc, cf);   // darwin/headless_additions.hpp
    // The headless shells of the frameworks whose GUI providers own these
    // names in alchemy-fx (darwin/shells.hpp; empty there).
    darwin_shells::install_appkit_shell(context, objc, cf);
    darwin_shells::install_coregraphics_shell(context, objc, cf);
    darwin_shells::install_coretext_shell(context, objc, cf);
    darwin_shells::install_metal_shell(context, objc, cf);
    darwin_shells::install_iokit_shell(context, objc, cf);
    darwin_shells::install_media_shell(context, objc, cf);
}

} // namespace alchemy
