# Exercise the dictionary iteration used by Blender's class registration.
import types


for size in (33, 128, 1024, 4096):
    for mixed in (False, True):
        source = {f"key_{i}": i for i in range(size)}
        if mixed:
            source.update({i: -i for i in range(size)})
        for i in range(0, size, 3):
            del source[f"key_{i}"]
        expected = list(source)
        proxy = types.MappingProxyType(source)
        for repeat in range(40):
            assert list(source) == expected
            assert list(proxy) == expected
            assert dict(proxy) == source
            assert list(source.items()) == [(key, source[key]) for key in expected]

print("PYTHON_DICT_ITERATION_PASS", flush=True)
