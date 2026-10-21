# Feature probe for an interactive (windowed) Blender session.
# Usage: blender --python blender_feature_probe.py -- <out.json> <work_dir>
# Checks audio playback per device, Cycles CPU / EEVEE / Workbench renders,
# viewport rendered shading, video encoding, file formats and a Python sweep.
# Every check records ok/detail/seconds; the run ends by quitting Blender.
import bpy, json, os, sys, time, traceback

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
out_path = argv[0] if argv else "feature_probe.json"
work = argv[1] if len(argv) > 1 else os.path.join(bpy.app.tempdir, "probe")
os.makedirs(work, exist_ok=True)
results = {"version": bpy.app.version_string, "checks": []}


def record(name, fn):
    t0 = time.perf_counter()
    try:
        detail = fn()
        ok = True
    except Exception as e:
        ok, detail = False, f"{type(e).__name__}: {e}\n{traceback.format_exc(limit=3)}"
    dt = time.perf_counter() - t0
    results["checks"].append({"name": name, "ok": ok, "detail": str(detail)[:600], "seconds": round(dt, 3)})
    print(f"[probe] {'OK  ' if ok else 'FAIL'} {name} ({dt:.2f}s): {str(detail)[:200]}", flush=True)
    save()
    return ok


def save():
    with open(out_path, "w") as f:
        json.dump(results, f, indent=1)


def need(cond, msg):
    if not cond:
        raise AssertionError(msg)


def view3d():
    win = bpy.context.window_manager.windows[0]
    area = next(a for a in win.screen.areas if a.type == 'VIEW_3D')
    return win, area, next(r for r in area.regions if r.type == 'WINDOW')


def engines():
    return [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties['engine'].enum_items]


def eevee_id():
    return next(e for e in engines() if e.startswith("BLENDER_EEVEE"))


def render_still(engine, name, setup=None):
    sc = bpy.context.scene
    sc.render.engine = engine
    sc.render.resolution_x, sc.render.resolution_y, sc.render.resolution_percentage = 160, 90, 100
    sc.render.image_settings.file_format = 'PNG'
    if setup:
        setup(sc)
    path = os.path.join(work, name + ".png")
    if os.path.exists(path):
        os.remove(path)
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    need(os.path.exists(path) and os.path.getsize(path) > 200, "no image written")
    img = bpy.data.images.load(path)
    px = list(img.pixels)
    bpy.data.images.remove(img)
    # Mean luminance and spread: a black or uniform frame means the engine did nothing useful.
    lum = [0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2] for i in range(0, len(px), 4)]
    mean = sum(lum) / len(lum)
    spread = max(lum) - min(lum)
    need(spread > 0.05, f"flat image (mean {mean:.3f}, spread {spread:.3f})")
    return f"{path} mean {mean:.3f} spread {spread:.3f}"


# ---------------------------------------------------------------- audio
def audio_devices():
    # The enum is filled at runtime (RNA's static item list only has None),
    # so probe the device names Blender can offer on Windows.
    prefs = bpy.context.preferences.system
    initial, found = prefs.audio_device, []
    for d in ('WASAPI', 'OpenAL', 'SDL', 'CoreAudio', 'PulseAudio', 'JACK', 'None'):
        try:
            prefs.audio_device = d
            found.append(d)
        except TypeError:
            pass
    prefs.audio_device = initial
    return found


def play_tone(seconds=0.6):
    import aud
    dev = aud.Device()
    h = dev.play(aud.Sound.sine(440).limit(0, 2.0).volume(0.05))
    time.sleep(seconds)
    pos = h.position
    status = h.status
    h.stop()
    need(pos > seconds * 0.5, f"playback position did not advance: {pos:.3f}s after {seconds}s (status {status})")
    return f"position {pos:.3f}s after {seconds}s"


# ---------------------------------------------------------------- python sweep
def py_numpy():
    import numpy as np
    a = np.random.default_rng(1).standard_normal((300, 300))
    inv = np.linalg.inv(a)
    err = float(np.abs(a @ inv - np.eye(300)).max())
    need(err < 1e-8, f"inverse error {err}")
    f = np.fft.fft(np.sin(np.linspace(0, 20 * np.pi, 1024)))
    return f"numpy {np.__version__} inv err {err:.2e} fft peak {int(np.argmax(np.abs(f[:512])))}"


def py_stdlib():
    import hashlib, zlib, lzma, bz2, sqlite3, json as js, decimal, ssl, ctypes, datetime, re
    h = hashlib.sha256(b"transmute").hexdigest()[:16]
    z = zlib.decompress(zlib.compress(b"x" * 10000)) == b"x" * 10000
    l = lzma.decompress(lzma.compress(b"y" * 10000)) == b"y" * 10000
    b = bz2.decompress(bz2.compress(b"z" * 10000)) == b"z" * 10000
    db = sqlite3.connect(":memory:")
    db.execute("create table t(x)"); db.executemany("insert into t values (?)", [(i,) for i in range(1000)])
    s = db.execute("select sum(x) from t").fetchone()[0]
    ctx = ssl.create_default_context()
    d = decimal.Decimal(1) / decimal.Decimal(7)
    need(z and l and b and s == 499500, "stdlib mismatch")
    return f"sha256 {h} sqlite sum {s} {ssl.OPENSSL_VERSION} dec {str(d)[:8]} now {datetime.datetime.now():%H:%M}"


def py_ctypes():
    import ctypes
    k32 = ctypes.WinDLL("kernel32")
    tick = k32.GetTickCount64
    tick.restype = ctypes.c_uint64
    t = tick()
    buf = ctypes.create_unicode_buffer(260)
    n = k32.GetWindowsDirectoryW(buf, 260)
    # A Python callback invoked by native code (qsort through msvcrt/ucrt).
    crt = ctypes.CDLL("ucrtbase")
    CMP = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int))
    arr = (ctypes.c_int * 6)(5, 3, 9, 1, 7, 2)
    crt.qsort(arr, 6, ctypes.sizeof(ctypes.c_int), CMP(lambda a, b: a[0] - b[0]))
    need(list(arr) == [1, 2, 3, 5, 7, 9], f"qsort via callback gave {list(arr)}")
    need(n > 0 and t > 0, "kernel32 calls failed")
    return f"tick {t} windir {buf.value} qsort {list(arr)}"


def py_threads():
    import threading, concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        r = list(ex.map(lambda i: sum(range(i * 10000)), range(16)))
    lock = threading.Lock(); n = [0]
    def bump():
        for _ in range(20000):
            with lock: n[0] += 1
    ts = [threading.Thread(target=bump) for _ in range(4)]
    [t.start() for t in ts]; [t.join() for t in ts]
    need(n[0] == 80000, f"lock counter {n[0]}")
    return f"pool ok ({r[3]}), locked counter {n[0]}"


def py_subprocess():
    import subprocess
    a = subprocess.run(["cmd", "/c", "echo native-child"], capture_output=True, text=True, timeout=30)
    need("native-child" in a.stdout, f"cmd: {a.stdout!r} {a.stderr!r}")
    return f"cmd ok: {a.stdout.strip()}"


def py_subprocess_python():
    # sys.executable is the ARM64 python.exe shipped with Blender: a child ARM64 process.
    import subprocess
    exe = sys.executable
    b = subprocess.run([exe, "-c", "import sys, platform; print(platform.machine(), sys.version.split()[0])"],
                       capture_output=True, text=True, timeout=120)
    need(b.returncode == 0 and b.stdout.strip(), f"{exe}: rc {b.returncode} out {b.stdout!r} err {b.stderr[-300:]!r}")
    return f"{os.path.basename(exe)} -> {b.stdout.strip()}"


def py_blender_api():
    import bmesh, mathutils
    # Custom operator + panel registration, an operator call, bmesh, mathutils, handlers.
    class PROBE_OT_hello(bpy.types.Operator):
        bl_idname = "probe.hello"; bl_label = "Probe Hello"
        def execute(self, context):
            context.scene["probe"] = 42
            return {'FINISHED'}
    class PROBE_PT_panel(bpy.types.Panel):
        bl_label = "Probe"; bl_space_type = 'VIEW_3D'; bl_region_type = 'UI'; bl_category = "Probe"
        def draw(self, context):
            self.layout.operator("probe.hello")
    bpy.utils.register_class(PROBE_OT_hello); bpy.utils.register_class(PROBE_PT_panel)
    bpy.ops.probe.hello()
    need(bpy.context.scene.get("probe") == 42, "operator did not run")
    me = bpy.data.meshes.new("probe_mesh"); bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=4, radius=1.0)
    bm.to_mesh(me); bm.free()
    q = mathutils.Quaternion((0, 0, 1), 1.0) @ mathutils.Vector((1, 0, 0))
    calls = []
    h = lambda scene, *a: calls.append(scene.frame_current)
    bpy.app.handlers.frame_change_post.append(h)
    bpy.context.scene.frame_set(5); bpy.context.scene.frame_set(1)
    bpy.app.handlers.frame_change_post.remove(h)
    need(calls == [5, 1], f"handler calls {calls}")
    return f"icosphere {len(me.vertices)} verts, rotated {q.x:.3f},{q.y:.3f}, handlers {calls}"


def py_text_run_script():
    txt = bpy.data.texts.new("probe_script.py")
    txt.write("import bpy\nbpy.context.scene['probe_text'] = sum(i*i for i in range(100))\n")
    win, area, region = view3d()
    old = area.type
    area.type = 'TEXT_EDITOR'
    area.spaces.active.text = txt
    try:
        with bpy.context.temp_override(window=win, area=area, region=area.regions[-1]):
            bpy.ops.text.run_script()
    finally:
        area.type = old
    need(bpy.context.scene.get("probe_text") == 328350, "text block did not run")
    return "text editor Run Script ok"


def geometry_nodes():
    bpy.ops.mesh.primitive_plane_add(location=(0, 0, -1))
    ob = bpy.context.object
    mod = ob.modifiers.new("gn", 'NODES')
    tree = bpy.data.node_groups.new("probe_gn", 'GeometryNodeTree')
    tree.interface.new_socket("Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
    tree.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
    gi = tree.nodes.new("NodeGroupInput"); go = tree.nodes.new("NodeGroupOutput")
    sub = tree.nodes.new("GeometryNodeSubdivideMesh"); sub.inputs["Level"].default_value = 5
    tree.links.new(gi.outputs[0], sub.inputs[0]); tree.links.new(sub.outputs[0], go.inputs[0])
    mod.node_group = tree
    dg = bpy.context.evaluated_depsgraph_get()
    n = len(ob.evaluated_get(dg).data.vertices)
    need(n == 33 * 33, f"{n} vertices")
    bpy.data.objects.remove(ob)
    return f"geometry nodes subdivide -> {n} verts"


# ---------------------------------------------------------------- formats
def io_round_trips():
    done = []
    p = os.path.join(work, "probe.blend")
    bpy.ops.wm.save_as_mainfile(filepath=p, copy=True); done.append("blend save")
    bpy.ops.wm.obj_export(filepath=os.path.join(work, "probe.obj")); done.append("obj")
    bpy.ops.wm.stl_export(filepath=os.path.join(work, "probe.stl")); done.append("stl")
    bpy.ops.wm.ply_export(filepath=os.path.join(work, "probe.ply")); done.append("ply")
    bpy.ops.wm.alembic_export(filepath=os.path.join(work, "probe.abc"), start=1, end=2); done.append("abc")
    bpy.ops.wm.usd_export(filepath=os.path.join(work, "probe.usdc")); done.append("usd")
    try:
        bpy.ops.export_scene.fbx(filepath=os.path.join(work, "probe.fbx")); done.append("fbx")
        bpy.ops.export_scene.gltf(filepath=os.path.join(work, "probe.glb")); done.append("gltf")
    except AttributeError as e:
        done.append(f"(addon missing: {e})")
    n0 = len(bpy.data.objects)
    bpy.ops.wm.usd_import(filepath=os.path.join(work, "probe.usdc")); done.append("usd import")
    bpy.ops.wm.obj_import(filepath=os.path.join(work, "probe.obj")); done.append("obj import")
    need(len(bpy.data.objects) > n0, "imports added nothing")
    sizes = {f: os.path.getsize(os.path.join(work, f)) for f in os.listdir(work) if f.startswith("probe.")}
    need(all(v > 0 for v in sizes.values()), f"empty file in {sizes}")
    return ", ".join(done)


def image_formats():
    out = []
    for fmt, ext in (('OPEN_EXR', 'exr'), ('PNG', 'png'), ('JPEG', 'jpg'), ('TIFF', 'tif'), ('WEBP', 'webp')):
        # Saving re-points a generated image at its file, so build a fresh one per format.
        img = bpy.data.images.new("probe_img_" + ext, 64, 64, float_buffer=True)
        img.pixels = [((i // 4) % 64) / 64.0 if i % 4 != 3 else 1.0 for i in range(64 * 64 * 4)]
        p = os.path.join(work, f"img.{ext}")
        img.filepath_raw = p; img.file_format = fmt; img.save()
        back = bpy.data.images.load(p); px = back.pixels[4 * 32]; bpy.data.images.remove(back)
        # Mid-row gray: 0.5 linear, or about 0.74 where the reader applies a view transform.
        need(0.3 < px < 0.8, f"{fmt} read back {px}")
        out.append(f"{ext}={px:.3f}")
    return "saved+loaded " + " ".join(out)


def video_encode():
    sc = bpy.context.scene
    sc.render.engine = 'BLENDER_WORKBENCH'
    sc.render.resolution_x, sc.render.resolution_y = 160, 96
    sc.frame_start, sc.frame_end = 1, 12
    sc.render.image_settings.file_format = 'FFMPEG'
    sc.render.ffmpeg.format = 'MPEG4'; sc.render.ffmpeg.codec = 'H264'
    sc.render.filepath = os.path.join(work, "probe_video_")
    bpy.ops.render.render(animation=True)
    vids = [f for f in os.listdir(work) if f.startswith("probe_video_") and f.endswith(".mp4")]
    need(vids, "no mp4")
    size = os.path.getsize(os.path.join(work, vids[0]))
    sc.render.image_settings.file_format = 'PNG'; sc.frame_end = 250
    need(size > 1000, f"mp4 only {size} bytes")
    # Read it back through the movie-clip (ffmpeg decode) path.
    clip = bpy.data.movieclips.load(os.path.join(work, vids[0]))
    n = clip.frame_duration
    return f"{vids[0]} {size} bytes, decoded {n} frames"


def physics_bake():
    bpy.ops.mesh.primitive_cube_add(location=(0, 0, 5))
    ob = bpy.context.object
    bpy.ops.rigidbody.object_add()
    bpy.ops.mesh.primitive_plane_add(size=10, location=(0, 0, 0))
    bpy.ops.rigidbody.object_add(type='PASSIVE')
    sc = bpy.context.scene
    for f in range(1, 41):
        sc.frame_set(f)
    z = ob.matrix_world.translation.z
    sc.frame_set(1)
    need(z < 4.0, f"cube did not fall (z {z:.2f})")
    return f"rigid body cube z at frame 40: {z:.2f}"


# ---------------------------------------------------------------- viewport engines
def set_shading(kind):
    _, area, _ = view3d()
    area.spaces.active.shading.type = kind


steps = []          # (name, fn, wait_seconds_after)


def add(name, fn, wait=0.0):
    steps.append((name, fn, wait))


def build():
    devs = audio_devices()
    results["audio_devices"] = devs
    results["engines"] = engines()
    prefs = bpy.context.preferences.system
    results["audio_device_initial"] = prefs.audio_device
    add("audio_default_device:" + prefs.audio_device, play_tone)
    for d in devs:
        if d == 'None' or d == 'NONE':
            continue
        def switch(d=d):
            prefs.audio_device = d
            return f"switched to {d}"
        add("audio_switch:" + d, switch, 0.5)
        add("audio_play:" + d, play_tone)
    def restore():
        prefs.audio_device = results["audio_device_initial"]
        return results["audio_device_initial"]
    add("audio_restore", restore, 0.3)
    add("python_numpy", py_numpy)
    add("python_stdlib", py_stdlib)
    add("python_ctypes", py_ctypes)
    add("python_threads", py_threads)
    add("python_subprocess_native", py_subprocess)
    add("python_subprocess_arm64_python", py_subprocess_python)
    add("python_blender_api", py_blender_api)
    add("python_text_editor_run", py_text_run_script)
    add("geometry_nodes", geometry_nodes)
    add("render_workbench", lambda: render_still('BLENDER_WORKBENCH', "workbench"))
    add("render_eevee", lambda: render_still(eevee_id(), "eevee"))
    def cyc(sc):
        sc.cycles.device = 'CPU'; sc.cycles.samples = 16; sc.cycles.use_denoising = False
    add("render_cycles_cpu", lambda: render_still('CYCLES', "cycles", cyc))
    def cyc_dn(sc):
        sc.cycles.device = 'CPU'; sc.cycles.samples = 8; sc.cycles.use_denoising = True
        sc.cycles.denoiser = 'OPENIMAGEDENOISE'
    add("render_cycles_cpu_oidn", lambda: render_still('CYCLES', "cycles_oidn", cyc_dn))
    def vp(kind, engine):
        def go():
            bpy.context.scene.render.engine = engine
            set_shading(kind)
            return f"{kind} with {engine}"
        return go
    add("viewport_material_preview_start", vp('MATERIAL', eevee_id()), 8.0)
    add("viewport_cycles_rendered_start", vp('RENDERED', 'CYCLES'), 10.0)
    add("viewport_eevee_rendered_start", vp('RENDERED', eevee_id()), 8.0)
    add("viewport_back_to_solid", vp('SOLID', eevee_id()), 1.0)
    add("image_formats", image_formats)
    add("video_ffmpeg_h264", video_encode)
    add("io_formats", io_round_trips)
    add("physics_rigid_body", physics_bake)


state = {"i": 0}


def tick():
    if state["i"] == 0 and not steps:
        build()
    if state["i"] >= len(steps):
        results["done"] = True
        save()
        print("[probe] DONE", flush=True)
        bpy.ops.wm.quit_blender()
        return None
    name, fn, wait = steps[state["i"]]
    state["i"] += 1
    print(f"[probe] step {name}", flush=True)
    results["current"] = name
    record(name, fn)
    return max(wait, 0.2)


bpy.app.timers.register(tick, first_interval=3.0)
