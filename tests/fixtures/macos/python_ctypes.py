"""Callbacks require genuine shared Mach VM alias support in Transmute."""
from python_ctypes_calls import ctypes, library, sys

integer_callback_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int)
integer_callback = integer_callback_type(lambda value: value + 27)
library.alchemy_ctypes_callback.argtypes = [integer_callback_type, ctypes.c_int]
library.alchemy_ctypes_callback.restype = ctypes.c_int
assert library.alchemy_ctypes_callback(integer_callback, 15) == 69
floating_callback_type = ctypes.CFUNCTYPE(ctypes.c_double, ctypes.c_double)
floating_callback = floating_callback_type(lambda value: value * 2.0)
library.alchemy_ctypes_float_callback.argtypes = [floating_callback_type, ctypes.c_double]
library.alchemy_ctypes_float_callback.restype = ctypes.c_double
assert library.alchemy_ctypes_float_callback(floating_callback, 13.25) == 27.0

# A callback exception is unraisable. Its native result is unspecified, so
# validate the hook and later recovery without asserting that result.
unraisable = []
previous_hook = sys.unraisablehook
sys.unraisablehook = unraisable.append
try:
    failing_callback = integer_callback_type(lambda value: 1 / 0)
    library.alchemy_ctypes_callback(failing_callback, 15)
finally:
    sys.unraisablehook = previous_hook
assert len(unraisable) == 1
assert unraisable[0].exc_type is ZeroDivisionError
assert library.alchemy_ctypes_callback(integer_callback, 15) == 69
print("Guest CPython ctypes callbacks: 69")
