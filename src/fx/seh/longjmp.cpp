// longjmp over the guest's ARM64 _JUMP_BUFFER (as produced by _setjmp /
// _setjmpex / __intrinsic_setjmpex). With a Frame (setjmpex) it runs the
// intervening unwind handlers first (RtlUnwind with STATUS_LONGJUMP), then
// transfers to the saved return address with the saved register state.

#include "fx/seh/dispatch.hpp"

#include <cstring>

namespace transmute::fx {
namespace {

using namespace transmute::fx::seh;

// Offsets in the ARM64 _JUMP_BUFFER (setjmp.h).
struct JumpBuffer {
    std::uint64_t Frame;
    std::uint64_t Reserved;
    std::uint64_t X[10];   // x19..x28
    std::uint64_t Fp;
    std::uint64_t Lr;
    std::uint64_t Sp;
    std::uint32_t Fpcr;
    std::uint32_t Fpsr;
    double D[8];           // d8..d15
};
static_assert(sizeof(JumpBuffer) == 24 * 8);

} // namespace

GuestResolution guest_longjmp(GuestContext& ctx) {
    const std::uint64_t buffer_address = ctx.x[0];
    std::int32_t value = static_cast<std::int32_t>(ctx.x[1]);
    if (value == 0) value = 1;

    JumpBuffer buffer{};
    if (!read_guest(buffer_address, &buffer, sizeof(buffer))) return GuestResolution::continue_at_caller;

    // The setjmp-time register state.
    Arm64Context resume{};
    guest_to_arm64_context(ctx, resume, kCtxFull);
    for (unsigned i = 0; i < 10; ++i) resume.X[19 + i] = buffer.X[i];
    resume.Fp = buffer.Fp;
    resume.Lr = buffer.Lr;
    resume.Sp = buffer.Sp;
    resume.Pc = buffer.Lr;
    resume.Fpcr = buffer.Fpcr;
    resume.Fpsr = buffer.Fpsr;
    for (unsigned i = 0; i < 8; ++i) std::memcpy(&resume.V[8 + i].Low, &buffer.D[i], sizeof(double));
    resume.X[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value));

    return run_entry(ctx, [&](EntryScope& scope) {
        if (buffer.Frame == 0) return transfer_to(scope, resume);   // _setjmp: no unwinding
        UnwindRequest request;
        request.target_frame = buffer.Frame;
        request.target_pc = buffer.Lr;
        request.return_value = resume.X[0];
        request.resume_context = &resume;
        request.record.code = kStatusLongjump;
        request.record.address = ctx.x[30];
        request.record.parameter_count = 1;
        request.record.parameters[0] = buffer_address;
        UnwindOutcome outcome = unwind_to_target(scope, caller_context(ctx), request);
        // Even if the frame was not found the jump buffer fully describes the target.
        return transfer_to(scope, outcome.status == UnwindOutcome::Status::transferred ? outcome.target : resume);
    });
}

} // namespace transmute::fx
