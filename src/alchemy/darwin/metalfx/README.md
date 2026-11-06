# MetalFX

`/System/Library/Frameworks/MetalFX.framework/Versions/A/MetalFX` (alchemy-fx).

* **Real:** the descriptor classes MTLFXSpatialScalerDescriptor, MTLFXTemporalScalerDescriptor,
  MTLFXTemporalDenoisedScalerDescriptor and MTLFXFrameInterpolatorDescriptor keep their properties (formats, sizes,
  modes, flags, the temporal scaler's input content scales).
* **Unavailable, as documented:** MetalFX's upscalers and frame interpolation are Apple's own algorithms and are not
  provided. `+supportsDevice:` answers NO for every device, so the scaler and interpolator factories
  (`-new...WithDevice:` and `-new...WithDevice:compiler:`) return nil. The temporal scaler's supported input content
  scale range is 1.0 both ways. Clients check `supportsDevice:` first and fall back to their own upscaling.
* **Host dependencies:** none.
* **Registration:** `install_metalfx`, one row of the installer table after the fill-only set (fill-only, through
  `gui::values::Provider`). Godot 4.7 first linked the framework: without it, Godot did not load.
* **Tests:** `tests/metalfx_tests.cpp` (ctest `alchemy_gui_metalfx`).
