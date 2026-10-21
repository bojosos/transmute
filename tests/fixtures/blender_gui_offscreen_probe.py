"""Diagnose front-buffer capture on the pinned Blender 4.5.14 ARM64 build."""

import ctypes
from pathlib import Path
import struct
import sys
import zlib

import bpy


def capture(directory):
    assert bpy.app.version == (4, 5, 14), "Private capture requires the pinned release"
    with Path(sys.executable).open("rb") as executable:
        executable.seek(0x3C)
        pe_offset = struct.unpack("<I", executable.read(4))[0]
        executable.seek(pe_offset + 4)
        assert struct.unpack("<H", executable.read(2))[0] == 0xAA64, "Private capture requires ARM64"

    # Matching release PDB, .text RVA 0x1000: WM_window_pixels_read_from_offscreen
    # offset 9471428; MEM_freeN offset 326288. These remain guest ARM64 calls.
    read_window = ctypes.CFUNCTYPE(
        ctypes.POINTER(ctypes.c_ubyte), ctypes.c_void_p, ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_int)
    )(0x1409095C4)
    free_pixels = ctypes.CFUNCTYPE(None, ctypes.c_void_p)(0x140050A90)
    dimensions = (ctypes.c_int * 2)()
    pixels = read_window(bpy.context.as_pointer(), bpy.context.window.as_pointer(), dimensions)
    assert pixels, "Blender could not create its offscreen UI buffer"
    try:
        width, height = dimensions
        assert 0 < width <= 8192 and 0 < height <= 8192 and width * height * 4 <= 64 * 1024 * 1024
        rgba = ctypes.string_at(pixels, width * height * 4)
    finally:
        free_pixels(pixels)

    def chunk(kind, payload):
        return struct.pack("!I", len(payload)) + kind + payload + struct.pack("!I", zlib.crc32(kind + payload))

    stride = width * 4
    rows = b"".join(b"\0" + rgba[row * stride:(row + 1) * stride] for row in range(height - 1, -1, -1))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack("!2I5B", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    (directory / "blender-gui-offscreen.png").write_bytes(png)
    print("TRANSMUTE_OFFSCREEN_UI_CAPTURE_SAVED", flush=True)
