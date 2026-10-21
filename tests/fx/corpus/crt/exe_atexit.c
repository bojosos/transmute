// fx corpus ARM64 CRT: atexit for EXEs registers with the UCRT process table,
// which exit() runs.
#include <stdlib.h>

#include "crt_internal.h"

int __cdecl atexit(_PVFV func) {
    return _crt_atexit(func);
}

int __cdecl at_quick_exit(_PVFV func) {
    return _crt_at_quick_exit(func);
}

_onexit_t __cdecl _onexit(_onexit_t func) {
    return _crt_atexit((_PVFV)func) == 0 ? func : NULL;
}
