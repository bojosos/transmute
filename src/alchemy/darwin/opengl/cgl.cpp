#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"
#include "alchemy/darwin_frameworks.hpp"

// OpenGL (CGL): no CGL context can be created.
// Moved from darwin/identities/frameworks.cpp (the headless framework set);
// registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

constexpr std::string_view opengl = darwin_framework_libraries[5];

// CGLTypes.h
constexpr std::int32_t cgl_bad_context = 10004;

class Cgl final : public Provider {
public:
    using Provider::Provider;
    void install() {
        // OpenGL: no CGL context can be created.
        function(opengl, "_CGLTexImageIOSurface2D", [](CpuState& s, GuestMemory&) { s.x[0] = status(cgl_bad_context); });
    }
};

} // namespace

void install_opengl_cgl(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<Cgl>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
