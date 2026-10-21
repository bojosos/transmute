"""Record a live Blender window and screenshot after the GUI event loop starts."""

import json
import os
from pathlib import Path

import bpy
import gpu

capture_attempt = 0


def record_gui():
    global capture_attempt
    capture_attempt += 1
    directory = Path(os.getenv("TRANSMUTE_GUI_PROBE_DIR", str(Path(__file__).resolve().parents[2] / "artifacts")))
    directory.mkdir(parents=True, exist_ok=True)
    report = {
        "process_id": os.getpid(),
        "capture_attempt": capture_attempt,
        "blender_version": bpy.app.version_string,
        "background": bpy.app.background,
        "backend": gpu.platform.backend_type_get(),
        "device_type": gpu.platform.device_type_get(),
        "renderer": gpu.platform.renderer_get(),
        "vendor": gpu.platform.vendor_get(),
        "driver_version": gpu.platform.version_get(),
        "windows": [
            {
                "width": window.width,
                "height": window.height,
                "areas": [area.type for area in window.screen.areas],
            }
            for window in bpy.context.window_manager.windows
        ],
    }
    assert not report["background"] and report["windows"], "Blender has no GUI window"
    assert report["backend"] == "OPENGL", "Blender did not activate OpenGL"
    if addon := os.getenv("TRANSMUTE_GUI_EXPECT_ADDON"):
        report["addon_enabled"] = addon in bpy.context.preferences.addons
        assert report["addon_enabled"], f"Add-on is not enabled: {addon}"
    with (directory / "blender-gui-probe.json").open("w", encoding="utf-8") as output:
        json.dump(report, output, indent=2)
    print("TRANSMUTE_GUI_READY " + json.dumps(report), flush=True)
    if os.getenv("TRANSMUTE_GUI_OFFSCREEN_DIAG") == "1":
        import runpy
        runpy.run_path(str(Path(__file__).with_name("blender_gui_offscreen_probe.py")))["capture"](directory)
    else:
        bpy.ops.screen.screenshot(filepath=str(directory / "blender-gui-probe.png"))
        print("TRANSMUTE_GUI_SCREENSHOT_SAVED", flush=True)
    # Windows front-buffer reads immediately after the first redraw can be
    # stale. Capture again after the normal event loop has presented frames.
    if capture_attempt >= 2 and os.getenv("TRANSMUTE_GUI_PROBE_QUIT") == "1":
        bpy.ops.wm.quit_blender()
    return 10.0 if capture_attempt < 2 else None


bpy.app.timers.register(record_gui, first_interval=2.0)
