# Startup milestones for scripts/perf/startup.ps1, run with --python.
#
# Prints "ALCHEMY_PERF_PHASE <name> [ms]" lines:
#   - under Alchemy (ALCHEMY_PERF=1) the runtime records each line as a
#     startup phase with its own clock (ms since process creation);
#   - on native Windows Blender the line carries the milliseconds since
#     process creation itself (GetProcessTimes through ctypes).
# Milestones: this script runs (Python, bpy and the startup file are up),
# the first event-loop timer (the window manager loop runs), the first 3D
# viewport draw, and then the first event-loop timer after that draw (the
# first full UI frame went out). ALCHEMY_STARTUP_QUIT=<seconds> quits Blender
# that long after the first viewport draw (a clean exit prints the exit
# reports and flushes the caches).
import os
import sys

import bpy


def _since_creation_ms():
    if sys.platform != "win32":
        return None
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.windll.kernel32
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE   # the pseudo handle (-1) must stay 64-bit
    times = [wintypes.FILETIME() for _ in range(4)]
    kernel32.GetProcessTimes(wintypes.HANDLE(kernel32.GetCurrentProcess()), *[ctypes.byref(t) for t in times])
    now = wintypes.FILETIME()
    kernel32.GetSystemTimePreciseAsFileTime(ctypes.byref(now))

    def ticks(value):
        return (value.dwHighDateTime << 32) | value.dwLowDateTime

    return (ticks(now) - ticks(times[0])) / 10000.0


def mark(name):
    # "<ms since creation>" on native Windows; "@<Unix epoch ms>" otherwise
    # (startup.ps1 converts it with the process start time, which also works
    # for builds without ALCHEMY_PERF).
    import time
    ms = _since_creation_ms()
    suffix = (" %.1f" % ms) if ms is not None else (" @%.1f" % (time.time() * 1000.0))
    print("ALCHEMY_PERF_PHASE %s%s" % (name, suffix), flush=True)


mark("python startup script runs")

_state = {"drawn": False, "handle": None}


def _quit():
    bpy.ops.wm.quit_blender()
    return None


def _after_draw():
    mark("first event-loop timer after the first viewport draw")
    seconds = os.environ.get("ALCHEMY_STARTUP_QUIT")
    if seconds:
        bpy.app.timers.register(_quit, first_interval=float(seconds))
    return None


def _draw():
    if _state["drawn"]:
        return
    _state["drawn"] = True
    mark("first 3D viewport draw")
    bpy.types.SpaceView3D.draw_handler_remove(_state["handle"], "WINDOW")
    bpy.app.timers.register(_after_draw, first_interval=0.0)


def _first_timer():
    mark("first event-loop timer")
    return None


_state["handle"] = bpy.types.SpaceView3D.draw_handler_add(_draw, (), "WINDOW", "POST_PIXEL")
bpy.app.timers.register(_first_timer, first_interval=0.0)
