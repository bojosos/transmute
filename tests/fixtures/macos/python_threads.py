"""Exercise real guest CPython workers, GIL transitions and thread-local state."""
import threading

local = threading.local()
local.value = -1
barrier = threading.Barrier(4, timeout=10)
lock = threading.Lock()
results = []
errors = []


def worker(value):
    try:
        local.value = value
        barrier.wait()
        total = sum(number * number for number in range(100))
        assert local.value == value
        with lock:
            results.append((value, total))
    except BaseException as error:
        with lock:
            errors.append(error)


workers = [threading.Thread(target=worker, args=(value,)) for value in range(3)]
for thread in workers:
    thread.start()
barrier.wait()
for thread in workers:
    thread.join(timeout=10)
    assert not thread.is_alive()
assert not errors, errors
assert local.value == -1
assert sorted(results) == [(value, 328350) for value in range(3)]
print("Guest CPython threads: 27")
