"""General guest CPython workload. Executed by ARM64 CPython, never host Python."""
import os
import sys
import time

assert sys.platform == "darwin"
assert sys.maxsize == 2**63 - 1
assert sys.version_info[:2] == (3, 11)
assert "\u03bb\U0001f600".encode("utf-8").decode("utf-8") == "\u03bb\U0001f600"
assert sum(value * value for value in range(10)) == 285
assert sorted({"b": 27, "a": 42}) == ["a", "b"]
os.environ["ALCHEMY_PYTHON_WORKLOAD"] = "27"
assert os.getenv("ALCHEMY_PYTHON_WORKLOAD") == "27"
before = time.monotonic_ns()
assert time.monotonic_ns() >= before
path = sys.argv[1]
payload = b"Alchemy Python\x00\xff\n" + "\u03bb".encode("utf-8")
with open(path, "wb") as output:
    assert not os.get_inheritable(output.fileno())
    os.set_inheritable(output.fileno(), True)
    duplicate = os.dup(output.fileno())
    try:
        assert not os.get_inheritable(duplicate)
        assert os.get_inheritable(output.fileno())
    finally:
        os.close(duplicate)
    os.set_inheritable(output.fileno(), False)
    assert output.write(payload) == len(payload)
with open(path, "rb") as source:
    assert source.read() == payload
os.unlink(path)
print("Guest CPython workload: 27")
