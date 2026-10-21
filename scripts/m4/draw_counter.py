# Counts 3D viewport redraws for scripts/m4/perf_orbit.ps1: a POST_PIXEL
# draw handler increments a counter and a 0.25 s timer prints
# "M4DRAWS <count> <seconds>" (time.perf_counter) to stdout, so the harness
# can turn an input interval into viewport frames per second on native
# Blender and under Alchemy alike.
import bpy, time

state = {"draws": 0}


def count():
    state["draws"] += 1


def report():
    print("M4DRAWS %d %.4f" % (state["draws"], time.perf_counter()), flush=True)
    return 0.25


bpy.types.SpaceView3D.draw_handler_add(count, (), 'WINDOW', 'POST_PIXEL')
bpy.app.timers.register(report, first_interval=0.25, persistent=True)
