# Logs one timestamp per 3D-viewport draw for interactive frame-rate checks.
# Usage: blender --python blender_frame_log.py -- <out.txt>
# Each line: perf_counter seconds of a viewport draw. Flushed every 0.5 s.
import bpy, sys, time

out_path = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "frames.txt"
stamps = []
reported = []
out = open(out_path, "w")


def on_draw():
    if not reported:
        import gpu
        reported.append(1)
        print(f"[frame_log] backend {gpu.platform.backend_type_get()} {gpu.platform.renderer_get()}", flush=True)
    stamps.append(time.perf_counter())


def flush():
    if stamps:
        out.write("".join(f"{t:.6f}\n" for t in stamps))
        stamps.clear()
        out.flush()
    return 0.5


bpy.types.SpaceView3D.draw_handler_add(on_draw, (), 'WINDOW', 'POST_PIXEL')
bpy.app.timers.register(flush, first_interval=0.5, persistent=True)
