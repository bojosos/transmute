# Viewport responsiveness probe for interactive Blender runs.
# Usage: blender --factory-startup --python blender_viewport_bench.py -- <out.json>
# Times full-window redraw+swap in several shading modes and mesh sizes,
# plus the CPU side of common interactive edits, then quits.
import bpy, json, sys, time

out_path = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "viewport_bench.json"
results = {"version": bpy.app.version_string, "steps": []}


def ctx():
    win = bpy.context.window_manager.windows[0]
    area = next(a for a in win.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    return win, area, region


def timed(name, fn, n=1):
    t0 = time.perf_counter()
    fn()
    dt = time.perf_counter() - t0
    results["steps"].append({"name": name, "total_ms": dt * 1e3, "per_ms": dt * 1e3 / n, "n": n})
    print(f"[bench] {name}: {dt * 1e3 / n:.2f} ms x{n}", flush=True)


def redraw(name, n=30, kind='DRAW_WIN_SWAP'):
    win, area, region = ctx()
    with bpy.context.temp_override(window=win, area=area, region=region):
        bpy.ops.wm.redraw_timer(type=kind, iterations=1)  # warm (shader compile etc.)
        timed(name, lambda: bpy.ops.wm.redraw_timer(type=kind, iterations=n), n)


def orbit(name, n=30):
    from mathutils import Quaternion
    win, area, region = ctx()
    rv3d = region.data
    step = Quaternion((0, 0, 1), 0.05)
    with bpy.context.temp_override(window=win, area=area, region=region):
        def go():
            for _ in range(n):
                rv3d.view_rotation = step @ rv3d.view_rotation
                bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)
        timed(name, go, n)


def shading(kind):
    _, area, _ = ctx()
    area.spaces.active.shading.type = kind


def run():
    try:
        redraw("idle_cube_solid")
        redraw("idle_cube_region_noswap", kind='DRAW')
        orbit("orbit_cube_solid")
        shading('WIREFRAME'); redraw("cube_wire")
        shading('SOLID')
        win, area, region = ctx()
        with bpy.context.temp_override(window=win, area=area, region=region):
            timed("add_monkey_subsurf3", lambda: (
                bpy.ops.mesh.primitive_monkey_add(location=(3, 0, 0)),
                bpy.ops.object.modifier_add(type='SUBSURF'),
                setattr(bpy.context.object.modifiers[-1], "levels", 3)))
        redraw("monkey31k_solid")
        orbit("orbit_monkey31k_solid")
        with bpy.context.temp_override(window=win, area=area, region=region):
            timed("modifier_apply", lambda: bpy.ops.object.modifier_apply(modifier="Subdivision"))
            timed("edit_mode_enter", lambda: bpy.ops.object.mode_set(mode='EDIT'))
        redraw("monkey31k_editmode")
        with bpy.context.temp_override(window=win, area=area, region=region):
            timed("edit_translate", lambda: bpy.ops.transform.translate(value=(0, 0, 0.1)), 1)
            timed("edit_mode_exit", lambda: bpy.ops.object.mode_set(mode='OBJECT'))
        with bpy.context.temp_override(window=win, area=area, region=region):
            timed("object_translate_x10", lambda: [bpy.ops.transform.translate(value=(0.01, 0, 0)) for _ in range(10)], 10)
        shading('MATERIAL');
        timed("material_preview_first", lambda: redraw("material_preview", n=10))
        shading('SOLID')
        timed("frame_step_x20", lambda: [bpy.context.scene.frame_set(f) for f in range(1, 21)], 20)
    except Exception as e:
        import traceback
        results["error"] = traceback.format_exc()
        print(results["error"], flush=True)
    with open(out_path, "w") as f:
        json.dump(results, f, indent=1)
    bpy.ops.wm.quit_blender()
    return None


# Let the window come up and draw once before measuring.
bpy.app.timers.register(run, first_interval=2.0)
