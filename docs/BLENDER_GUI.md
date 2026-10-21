# Blender GUI on Windows x64

## fx engine (current)

The ARM64 Blender 4.5.14 GUI runs interactively under `transmute-fx.exe` with
both the OpenGL and the Vulkan backends:

```powershell
.\scripts\start_blender_gui.ps1 -Stage 'artifacts\arm64-stage\Blender Foundation\Blender 4.5' `
  -GpuBackend vulkan -SnapshotRuntime -WindowGeometry 80,80,1280,800
```

`-Engine fx` (the default) uses `build-fx-integ\Release\transmute-fx.exe`;
`-Engine legacy` keeps the `transmute.exe --jit` path described below. Under
Vulkan the ARM64 `vulkan-1.dll` loader loads the native x64 ICD; it is
called through ordinary host thunks. The first Vulkan start compiles shaders
through the translated ARM64 `shaderc_shared.dll` and can take tens of
seconds before the first frame. Later starts open the window in about 2-5 s.

Measured 1 October 2026 on the i7-1355U / Iris Xe machine while other builds
loaded the CPU. A real `SendInput` middle-button orbit ran for about 9.5 s,
and `tests/fixtures/blender_frame_log.py` timestamped viewport draws:

| Scene, backend | Native x64 | ARM64 under fx |
| --- | ---: | ---: |
| Default cube, OpenGL | 51.6 fps, median 16.6 ms | 45.7 fps, median 15.9 ms |
| Cube subdivision 6, OpenGL | 51.5 fps | 45.1 fps |
| Default cube, Vulkan | 49.4 fps, median 17.0 ms | 49.5 fps, median 16.9 ms |
| Sculpt strokes, 31k-face Suzanne, Vulkan | 7.8 fps | 5.6 fps |

These results are bounded by vsync and GPU work. CPU-heavy edits, such as
sculpting, are where translation cost appears. Real-input checks passed on both
backends: orbit, wheel zoom, click select/deselect, Tab edit mode, live grab,
context menu and tooltip, F3 search with typing, add Suzanne, the shading pie
menu and the Preferences window as a second swapchain. Selection appears in
the presented frame with the same timing as native. `scripts/fx_viewport_bench.ps1`
runs the scripted `wm.redraw_timer` comparison
(`tests/fixtures/blender_viewport_bench.py`).

### Feature probe

`scripts/fx_feature_probe.ps1` opens a real window with audio enabled and runs
`tests/fixtures/blender_feature_probe.py`. It checks WASAPI and OpenAL
playback, Cycles CPU with and without OpenImageDenoise, EEVEE, Workbench, and
Material Preview. It also checks Cycles and EEVEE rendered viewport shading, H.264
encoding and decoding, and EXR/PNG/JPEG/TIFF/WebP read and write. File coverage
includes `.blend`, OBJ, STL, PLY, Alembic, USD, FBX and glTF, plus rigid-body
simulation and geometry nodes. The Python checks cover numpy, the standard
library and OpenSSL, ctypes with native callbacks, threads, a custom operator
and panel, handlers, Text Editor Run Script, and native and ARM64 child
processes. On 1 October 2026 all 27 checks passed under fx with both OpenGL and
Vulkan. The launcher defaults to Vulkan.

Fixes behind these results:

- Borrowed host threads, including thread-pool workers used by OpenAL and
  WASAPI through `msvcp140`, receive the guest activation context only for
  each outermost guest entry. A context left active fail-fasts the thread pool
  with `STATUS_SXS_INVALID_DEACTIVATION` (`src/fx/runtime/dispatcher.cpp`,
  `rt::invoke`).
- `CreateProcessW/A` starts an ARM64 image as
  `transmute-fx.exe -- <image> <arguments>`. It keeps the caller's flags,
  inherited handles, environment, directory and `STARTUPINFOEX`. An x64 host
  otherwise returns `ERROR_EXE_MACHINE_TYPE_MISMATCH`, for example for Python
  `subprocess` with Blender's ARM64 `python.exe`.

Known issues:

- OpenGL with viewport Cycles crashed with an access violation in about one
  of seven fx runs. The fault reads address `0x20` inside `igxelpicd64.dll` on
  the Intel driver's own worker thread. It occurred once while rendering and
  otherwise during quit. Native Blender passed 10 of 10 runs, so the crash is
  specific to fx. No Vulkan run reproduced it.
- A child process reads `platform.machine()` as `AMD64` because it inherits
  `PROCESSOR_ARCHITECTURE`.
- With a cold translation cache, the first EEVEE render on OpenGL took 161 s.
  The first native render took 43 s.

### Cycles GPU

The official ARM64 Windows build has no Cycles GPU backend. `blender.exe` does
not reference `nvcuda.dll`, `amdhip64` or SYCL and ships no GPU kernels.
The x64 build contains all three, plus the PTX and fatbin kernels.
A GPU path therefore needs a custom ARM64 Blender build with the CUDA or HIP
device enabled; GPU kernels do not depend on the host ISA. fx would also
need typed signatures for the driver API (`cuGetProcAddress`, the OptiX
function table) and callbacks. This is analogous to the `wglGetProcAddress`
work. The Iris Xe test machine has no Cycles GPU device even in native
Blender, because Cycles oneAPI requires Arc-class GPUs.

OpenGL entry points from `wglGetProcAddress` are driver addresses without
export names. The fx override for that function binds the signatures in
`kOpenGl` (`src/fx/abi/sigdb.cpp`). Otherwise the positional `?` signature
would pass wrong FP arguments to calls such as `glSamplerParameterf`,
`glClearBufferfi` and `glVertexAttrib4f`, because AAPCS64 numbers integer and
FP registers separately. Names absent from the table still use `?`.

The official Windows ARM64 Blender 4.5.14 LTS build opens its interface through
Transmute's JIT and the host OpenGL driver. The verified machine reported Intel
Iris Xe Graphics and OpenGL 4.6, driver 32.0.101.7085. Both Blender's offscreen UI
capture and its native window back buffer contained the complete interface,
including the viewport, splash screen, Outliner, Properties and timeline.

## Launch

Build the Release runtime and launch the staged ARM64 executable:

```powershell
cmake --build build --config Release --target transmute -j 2
.\scripts\start_blender_gui.ps1 `
  -Stage 'artifacts\arm64-stage\Blender Foundation\Blender 4.5' `
  -FactoryStartup -SnapshotRuntime -WindowGeometry 80,80,960,640
```

The script selects OpenGL, enables the JIT and records output under `artifacts`.
`-SnapshotRuntime` copies the runtime so other work can rebuild it while Blender
stays open. The GUI uses a very large instruction allowance by default
(`Int64.MaxValue`, effectively unlimited for interactive use); `-InstructionLimit N` sets a smaller
allowance for bounded probes. Initial startup on the verified machine took about
a minute. Bring the Blender window forward from the taskbar if it opens behind
another application.

Audio is disabled by default. `-Audio` enables the separate audio path, which
this GUI validation did not exercise. `-Diagnose` and `-DiagnoseSync` enable
exception and synchronization reports. `-PythonScript` runs a supplied script.

## Forwarding and overhead

Scalar Win32 imports bind to native wrappers once during import resolution.
Calls such as `GetSystemMetrics`, `GetKeyState`, `GetAsyncKeyState` and
`MapVirtualKey` avoid the general bridge dispatcher and its State mutex.
APIs involving guest memory, handles or callbacks use checked adapters. Window
procedures return to ARM64 code; Windows helper-window messages and native timer
callbacks stay on the native side.

OpenGL extension lookup returns cached ARM64 guest-call entries. Each adapter
supplies the host ABI and calls the native driver. Native function pointers,
driver strings and sync pointers never become directly callable or dereferenceable
guest pointers. Mapped GPU buffers use checked external-memory aliases, avoiding
a staging copy; unmapping or deletion revokes those aliases.

The scalar-wrapper microbenchmark used five rotated rounds of 200,000 calls:

| API | Host call | Cached wrapper | General dispatcher |
| --- | ---: | ---: | ---: |
| `GetCurrentThreadId` | 3.4 ns | 3.1 ns | 3,744.8 ns |
| `GetSystemMetrics` | 76.9 ns | 76.0 ns | 475.8 ns |

These are medians of per-round averages from
`artifacts/gui-native-import-benchmark.csv`. Sub-nanosecond differences are
measurement noise. This measures wrapper cost from native code; it excludes
the ARM64 execution transition and does not establish Blender frame rate.

## Validation

```powershell
cmake --build build --config Release --target transmute_tests arm64_fixture -j 2
ctest --test-dir build -C Release -R '^transmute_' --parallel 4 --output-on-failure
```

The Windows suite covers real ARM64 window callbacks, keyboard and Unicode
character messages, mouse messages, native OpenGL readback and buffer swaps in
both interpreter and JIT modes, including teardown with live windows. Graphics
tests cover shader reflection, indexed and instanced draws, indirect draws,
texture uploads, framebuffer readback, GPU buffer mappings, fences and queries.

The normal Blender screenshot operator returned a uniform gray front-buffer
image on this machine. That image is insufficient to judge presentation:
`artifacts/blender-gui-presented.bmp` contains the populated native back buffer
immediately before `SwapBuffers`, and `artifacts/blender-gui-offscreen.png`
contains Blender's rendered UI. Temporary back-buffer instrumentation was removed
from the runtime after collecting that evidence.

To reproduce the offscreen diagnostic on the pinned release:

```powershell
$env:TRANSMUTE_GUI_OFFSCREEN_DIAG = '1'
try {
  .\scripts\start_blender_gui.ps1 `
    -Stage 'artifacts\arm64-stage\Blender Foundation\Blender 4.5' `
    -FactoryStartup -SnapshotRuntime -WindowGeometry 80,80,960,640 `
    -PythonScript tests\fixtures\blender_gui_probe.py
} finally {
  Remove-Item Env:TRANSMUTE_GUI_OFFSCREEN_DIAG
}
```

This diagnostic calls private ARM64 functions at addresses from the matching
release PDB and verifies the Blender version and PE architecture first. It is
specific to this pinned build, not a stable Blender Python API. The matching
source is staged at `artifacts/blender-source`, tag `v4.5.14`, commit
`62c1db4208e862312bc36db5bc15d4a6dbcfa3e3`.
