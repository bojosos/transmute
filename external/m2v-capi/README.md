# m2v-capi

A C ABI over [metal2vulkan](https://github.com/steelbrain/metal2vulkan), which translates Apple's
Metal AIR (precompiled Metal libraries, `.metallib`) to Vulkan SPIR-V, with the pipeline layouts and
per-draw binding instructions to run them. alchemy-fx loads the built `m2v_capi.dll` at run time
for `-[MTLDevice newLibraryWithData:error:]`. Without the DLL that call fails explicitly, as before.

## License

**This directory is not MIT-licensed** like the rest of the repository.

* **m2v-capi** (this directory) is licensed under the GNU Lesser General Public License, version 3
  or (at your option) any later version: `COPYING.LESSER`, which incorporates the GNU General Public
  License, version 3 (`COPYING`).
* **metal2vulkan** is `GPL-2.0-or-later OR LGPL-3.0-or-later`. m2v-capi uses it under the
  LGPL-3.0-or-later option. Cargo fetches it at the revision `Cargo.toml` pins. None of its source is
  copied here.
* **Alchemy** stays MIT. It never links m2v-capi or metal2vulkan. It loads the DLL by name at run
  time through the C ABI in `include/m2v_capi.h`, with its own declarations of those entry points
  (`src/alchemy/darwin/metal/air_translator.cpp`).

What distributing the DLL with Alchemy requires (the DLL stays replaceable, the license texts ship
with it, and its source is offered) is listed in `THIRD_PARTY_NOTICES.md` at the repository root.

## Build

```powershell
scripts/m2v/build_capi.ps1            # -> artifacts/m2v-capi/m2v_capi.dll
```

It needs Rust 1.98.1, which `rust-toolchain.toml` selects through rustup. The first build fetches
metal2vulkan and the crates.io dependencies `Cargo.lock` pins, and takes about a quarter of an hour.
Point alchemy-fx at the DLL with `ALCHEMY_M2V_CAPI=<path>\m2v_capi.dll`, or place the DLL next to
`alchemy-fx.exe`.

## ABI

See `include/m2v_capi.h`. In short:
* opaque handles: device, library, pipeline, plan;
* plain data as JSON: descriptors in, layouts and plans out, in metal2vulkan's serialized forms;
* SPIR-V as word arrays;
* errors per thread through `m2v_last_error`.

The device description is the device the caller created. The caller passes the feature chain and
extension list it gave `vkCreateDevice`, and metal2vulkan's own query runs with those in place of
what the hardware reports.
