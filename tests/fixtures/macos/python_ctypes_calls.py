"""Exercise the bundled guest ctypes/libffi against an authored Mach-O dylib."""
import ctypes
import os
import sys


class Pair(ctypes.Structure):
    _fields_ = [("integer", ctypes.c_int), ("real", ctypes.c_double)]


assert ctypes.sizeof(Pair) == 16
assert Pair.real.offset == 8
pair = Pair(27, 1.5)
assert pair.integer == 27 and pair.real == 1.5
values = (ctypes.c_int * 4)(1, 2, 3, 21)
assert list(values) == [1, 2, 3, 21]
buffer = ctypes.create_string_buffer(b"Alchemy", 16)
assert ctypes.string_at(ctypes.addressof(buffer)) == b"Alchemy"
ctypes.memmove(ctypes.addressof(buffer) + 1, b"lchemy", 6)
assert buffer.value == b"Alchemy"
ctypes.memset(ctypes.addressof(buffer) + 7, 0, 9)
assert buffer.raw == b"Alchemy" + bytes(9)

library = ctypes.CDLL(os.path.join(sys.argv[1], "python_ctypes_helper.dylib"))
library.alchemy_ctypes_add.argtypes = [ctypes.c_longlong, ctypes.c_longlong]
library.alchemy_ctypes_add.restype = ctypes.c_longlong
assert library.alchemy_ctypes_add(2**40, 27) == 2**40 + 27
library.alchemy_ctypes_scale.argtypes = [ctypes.c_double, ctypes.c_double]
library.alchemy_ctypes_scale.restype = ctypes.c_double
assert library.alchemy_ctypes_scale(1.5, 18.0) == 27.0
library.alchemy_ctypes_pair.argtypes = [Pair]
library.alchemy_ctypes_pair.restype = Pair
result = library.alchemy_ctypes_pair(Pair(0, 13.5))
assert result.integer == 27 and result.real == 27.0
library.alchemy_ctypes_sum.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_uint]
library.alchemy_ctypes_sum.restype = ctypes.c_int
assert library.alchemy_ctypes_sum(values, len(values)) == 27
exported_value = ctypes.c_int.in_dll(library, "alchemy_ctypes_value")
assert exported_value.value == 27
exported_value.value = 42
assert ctypes.c_int.in_dll(library, "alchemy_ctypes_value").value == 42
exported_value.value = 27
assert ctypes.CDLL(None).printf is not None

if __name__ == "__main__":
    print("Guest CPython ctypes: 69")
