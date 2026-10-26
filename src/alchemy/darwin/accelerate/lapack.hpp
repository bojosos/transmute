#pragma once

#include "alchemy/process_context.hpp"

#include <functional>

// Accelerate's LAPACK decomposition drivers (lapack.cpp; both builds): ?gesdd, ?syevd/?heevd,
// ?geev, ?geqrf, ?orgqr/?ungqr under their `$NEWLAPACK` names. Installed by
// install_darwin_numeric (libsystem/misc/numeric.cpp) with its floating-point environment
// wrapper `wrap`.
namespace alchemy::accelerate {

void install_accelerate_lapack(ProcessContext& context, const std::function<transmute::GuestCallHandler(transmute::GuestCallHandler)>& wrap);

} // namespace alchemy::accelerate
