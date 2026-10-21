#include "alchemy/darwin/kit/support/support_kit.hpp"

// AGL and the OpenGL entry points (Qt's Cocoa plugin first imported them). No OpenGL
// context can exist under Alchemy (NSOpenGLPixelFormat / CGL creation fail
// in the headless OpenGL provider), so these answer like OpenGL without a
// current context: glGetString returns NULL and glGetIntegerv leaves its
// output untouched. The AGL identity is linked by libqcocoa but none of its
// functions are imported.
namespace alchemy::gui::support_kit {

void install_opengl_no_context(Support& support) {
    ProcessContext::add_provider_library(std::string(agl_library));
    support.fill_function(opengl_library, "_glGetString", [](CpuState& c, GuestMemory&) { c.x[0] = 0; });
    support.fill_function(opengl_library, "_glGetIntegerv", [](CpuState&, GuestMemory&) {});
    support.fill_function(opengl_library, "_glGetError", [](CpuState& c, GuestMemory&) { c.x[0] = 0x0502; /* GL_INVALID_OPERATION */ });
    // No pixel format matches any attribute list and no context can be made:
    // the initializers release the receiver and return nil, AppKit's answer
    // for an unsatisfiable request. Qt WebEngine asks for its global share
    // context at startup and continues without one.
    const auto nil_initializer = [&support](std::string_view class_name, std::string selector, std::string types) {
        const auto cls = support.objc.find_class(class_name);
        if (!cls) return;
        const auto probe = support.objc.allocate(cls);
        const bool answers = support.objc.responds(probe, support.objc.selector(selector));
        support.process.objects().release(probe);
        if (answers) return;
        support.method(cls, std::move(selector), [&support](CpuState& c, GuestMemory&) {
            support.release(c.x[0]);
            c.x[0] = 0;
        }, std::move(types));
    };
    nil_initializer("NSOpenGLPixelFormat", "initWithAttributes:", "@@:^I");
    nil_initializer("NSOpenGLPixelFormat", "initWithCGLPixelFormatObj:", "@@:^v");
    nil_initializer("NSOpenGLContext", "initWithFormat:shareContext:", "@@:@@");
    nil_initializer("NSOpenGLContext", "initWithCGLContextObj:", "@@:^v");
}

} // namespace alchemy::gui::support_kit
