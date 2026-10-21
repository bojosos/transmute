"""Small Blender workloads separating startup, rendering and saving."""
import argparse
import json
import sys
import time

import bpy


def marker(event, **fields):
    print("TRANSMUTE_PHASE " + json.dumps({"event": event, "wall_unix": time.time(), **fields}), flush=True)


args = sys.argv[sys.argv.index("--") + 1 :]
parser = argparse.ArgumentParser()
parser.add_argument("--workload", choices=("startup", "render", "save", "whole"), required=True)
parser.add_argument("--repeats", type=int, default=3)
parser.add_argument("--width", type=int, default=16)
parser.add_argument("--height", type=int, default=16)
parser.add_argument("--samples", type=int, default=1)
parser.add_argument("--output", required=True)
parser.add_argument("--verify-image", action="store_true")
options = parser.parse_args(args)
if min(options.repeats, options.width, options.height, options.samples) < 1:
    parser.error("Counts and dimensions must be positive")

scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = options.samples
scene.render.resolution_x = options.width
scene.render.resolution_y = options.height
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = "PNG"
marker("ready", workload=options.workload)

if options.workload == "save":
    started = time.perf_counter()
    bpy.ops.render.render(write_still=False)
    marker("preparation_render", seconds=time.perf_counter() - started)

if options.workload != "startup":
    for repeat in range(options.repeats):
        output = f"{options.output}-{repeat:02}.png"
        scene.render.filepath = output
        marker("begin", repeat=repeat)
        started = time.perf_counter()
        if options.workload == "save":
            bpy.data.images["Render Result"].save_render(output, scene=scene)
        else:
            bpy.ops.render.render(write_still=options.workload == "whole")
        marker("complete", repeat=repeat, seconds=time.perf_counter() - started,
               image=output if options.workload in ("save", "whole") else None)
if options.workload == "render" and options.verify_image:
    output = options.output + "-verification.png"
    started = time.perf_counter()
    bpy.data.images["Render Result"].save_render(output, scene=scene)
    marker("verification", seconds=time.perf_counter() - started, image=output)
marker("finished")
