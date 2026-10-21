# MSL corpus tour (owner: A6): run inside Blender (--python) while
# ALCHEMY_MSL_DUMP=<dir> is set, so the libraries of more than the default
# workbench UI compile and are dumped. Switches every 3D view to Material
# Preview and then Rendered (EEVEE) shading, opens the shader, geometry node,
# compositor and image editors, then quits. Each step waits for EEVEE's
# asynchronous shader compilation to settle (no new dump for SETTLE seconds).
#
#   ALCHEMY_MSL_DUMP=<dir> alchemy-fx.exe ... Blender.app --factory-startup \
#       --python src/alchemy/darwin/msl/tests/corpus_tour.py
#
# ALCHEMY_CORPUS_TOUR_SETTLE (seconds, default 15) and
# ALCHEMY_CORPUS_TOUR_STEP_LIMIT (seconds per step, default 120) tune the waits.
import os
import time

import bpy

DUMP = os.environ.get("ALCHEMY_MSL_DUMP", "")
# The host path as the guest sees it under alchemy-fx (C:\x -> /Volumes/C/x).
if len(DUMP) > 2 and DUMP[1] == ":":
    DUMP = "/Volumes/" + DUMP[0].upper() + DUMP[2:].replace("\\", "/")
SETTLE = float(os.environ.get("ALCHEMY_CORPUS_TOUR_SETTLE", "15"))
STEP_LIMIT = float(os.environ.get("ALCHEMY_CORPUS_TOUR_STEP_LIMIT", "120"))


def log(message):
    print("corpus-tour: " + message, flush=True)


def dumped():
    try:
        return len([n for n in os.listdir(DUMP) if n.endswith(".metal")]) if DUMP else 0
    except OSError:
        return 0


def areas(kind=None):
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if kind is None or area.type == kind:
                yield window, area


def redraw():
    for _, area in areas():
        area.tag_redraw()


def shading(mode):
    def run():
        for _, area in areas("VIEW_3D"):
            for space in area.spaces:
                if space.type == "VIEW_3D":
                    space.shading.type = mode
        log("3D views in %s shading" % mode)
    return run


def geometry_nodes():
    # A node group on the default cube, so the geometry node editor has a tree.
    cube = bpy.data.objects.get("Cube")
    if cube is None:
        return
    modifier = cube.modifiers.new("GeometryNodes", "NODES")
    group = bpy.data.node_groups.new("Tour", "GeometryNodeTree")
    group.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    group.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    group_in = group.nodes.new("NodeGroupInput")
    group_out = group.nodes.new("NodeGroupOutput")
    subdivide = group.nodes.new("GeometryNodeSubdivideMesh")
    group.links.new(group_in.outputs[0], subdivide.inputs[0])
    group.links.new(subdivide.outputs[0], group_out.inputs[0])
    modifier.node_group = group
    log("geometry nodes modifier on Cube")


def editor(ui_type, index):
    # Turns the index-th non-3D-view area into another editor (the 3D view
    # keeps rendering).
    def run():
        candidates = [area for _, area in areas() if area.type != "VIEW_3D"]
        if not candidates:
            log("no area for " + ui_type)
            return
        area = candidates[index % len(candidates)]
        area.ui_type = ui_type
        if ui_type == "ShaderNodeTree":
            for space in area.spaces:
                if space.type == "NODE_EDITOR":
                    space.shader_type = "OBJECT"
        log("opened %s in a %dx%d area" % (ui_type, area.width, area.height))
    return run


def quit_blender():
    log("done: %d libraries dumped" % dumped())
    bpy.ops.wm.quit_blender()


STEPS = [
    ("material preview", shading("MATERIAL")),
    ("rendered eevee", shading("RENDERED")),
    ("shader editor", editor("ShaderNodeTree", 0)),
    ("geometry nodes", lambda: (geometry_nodes(), editor("GeometryNodeTree", 1)())),
    ("image editor", editor("IMAGE_EDITOR", 2)),
    ("compositor", editor("CompositorNodeTree", 3)),
    ("solid", shading("SOLID")),
    ("wireframe", shading("WIREFRAME")),
]

state = {"step": -1, "since": 0.0, "count": -1, "changed": 0.0}


def tick():
    now = time.monotonic()
    count = dumped()
    if count != state["count"]:
        state["count"] = count
        state["changed"] = now
    redraw()
    step = state["step"]
    settled = now - state["changed"] >= SETTLE
    if step >= 0 and not settled and now - state["since"] < STEP_LIMIT:
        return 1.0
    if step >= 0:
        log("step '%s' settled after %.0f s, %d libraries dumped" % (STEPS[step][0], now - state["since"], count))
    step += 1
    if step >= len(STEPS):
        quit_blender()
        return None
    state["step"] = step
    state["since"] = now
    state["changed"] = now
    try:
        STEPS[step][1]()
    except Exception as error:  # a step that fails must not stop the tour
        log("step '%s' failed: %r" % (STEPS[step][0], error))
    return 1.0


log("dumping to %s (%d libraries already there)" % (DUMP or "<ALCHEMY_MSL_DUMP unset>", dumped()))
bpy.app.timers.register(tick, first_interval=5.0)
