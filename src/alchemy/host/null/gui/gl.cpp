// Null host backend, area "gl": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/gl.hpp"

namespace alchemy::host::gl {

Result<Context*> create_context(const Format&, Context*) { return Error::unsupported; }
void destroy_context(Context*) noexcept {}
Status make_current(Context* context) { return context ? Status(Error::unsupported) : Status(); }
Context* current() noexcept { return nullptr; }
Format format(const Context*) noexcept { return {}; }
void swap_buffers(Context*) noexcept {}
void* proc(const char*) noexcept { return nullptr; }
Result<Renderer> renderer() { return Error::unsupported; }

} // namespace alchemy::host::gl
