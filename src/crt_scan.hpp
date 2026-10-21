#pragma once

#include "transmute/a64.hpp"
#include <locale.h>

namespace transmute {

// UCRT common string-scan ABI: options, input, count, format, locale, va_list.
// Host scanf receives only owned buffers and translated argument slots.
int scan_guest_string(const CpuState& state, GuestMemory& memory,
                      bool wide, _locale_t locale);

} // namespace transmute
