"""Real Mach-O module imports plus an authored extension and guest dylib."""
import sys
sys.path.insert(0, sys.argv[1])
import _struct
import math
import _json
import _random
import binascii
import importlib
import importlib.util

previous_flags = sys.getdlopenflags()
try:
    sys.setdlopenflags(2 | 4)  # Darwin RTLD_NOW | RTLD_LOCAL.
    import _alchemy_native
finally:
    sys.setdlopenflags(previous_flags)

assert _struct.unpack("<Iq", _struct.pack("<Iq", 27, -42)) == (27, -42)
assert math.sqrt(729) == 27
assert _json.scanstring('"Alchemy"', 1)[0] == "Alchemy"
assert 0.0 <= _random.Random().random() < 1.0
assert binascii.hexlify(b"\x1b\x2a") == b"1b2a"
assert _alchemy_native.initialized == 27
assert _alchemy_native.compute(15, lambda value: value + 27) == 69
try:
    _alchemy_native.compute(15, lambda value: 1 / 0)
except ZeroDivisionError:
    pass
else:
    raise AssertionError("native callback lost Python exception")
assert __import__("_alchemy_native") is _alchemy_native
assert importlib.reload(_alchemy_native) is _alchemy_native
assert _alchemy_native.initialized == 27

missing_init = importlib.util.spec_from_file_location(
    "_alchemy_missing_init", _alchemy_native.__file__
)
assert missing_init is not None
try:
    importlib.util.module_from_spec(missing_init)
except ImportError as error:
    assert "PyInit__alchemy_missing_init" in str(error)
else:
    raise AssertionError("native extension without its PyInit export was accepted")
assert "_alchemy_missing_init" not in sys.modules
assert _alchemy_native.compute(15, lambda value: value + 27) == 69
print("Guest CPython native extensions: 69")
