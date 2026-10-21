// _resetstkoflw for guest stacks.
//
// After a stack overflow has been handled, the UCRT's _resetstkoflw restores
// the stack guard so the next overflow raises STATUS_STACK_OVERFLOW again
// instead of running into unmapped memory. The host UCRT routine works on the
// *host* thread's stack, so guest calls are bound to this version. A guest
// stack's overflow detection is the runtime's 64 KiB guard region (the first
// fault there commits emergency space for the handlers and raises
// STATUS_STACK_OVERFLOW; PAGE_GUARD pages are plain stack growth), so the
// reset re-arms that region through the runtime.

#include "fx/runtime/runtime.hpp"
#include "fx/seh/seh.hpp"

namespace transmute::fx {

GuestResolution guest__resetstkoflw(GuestContext& ctx) {
    ctx.x[0] = rearm_guest_stack_guard(ctx) ? 1 : 0;
    return GuestResolution::continue_at_caller;
}

} // namespace transmute::fx
