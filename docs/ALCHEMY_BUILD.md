# Building Alchemy: Ninja and the shared compiler cache

Many sessions build at once on one machine, and each worktree compiles nearly the same
sources. The shared compiler cache compiles each object once, and makes it a cache hit in every
other worktree.

It is opt-in. Without `TRANSMUTE_COMPILER_CACHE` the build is the same as before: the Visual
Studio generator, `/MP`, and `/Zi`.

## One-time setup (per machine)

* **sccache:** `sccache.exe` (Mozilla sccache, Apache-2.0) lives in the main checkout's
  `artifacts/tools/sccache/`, which git ignores.
  * Version 0.18.0 came from the official GitHub release
    (`sccache-v0.18.0-x86_64-pc-windows-msvc.zip`, SHA-256
    `8965c74d5e8a225244f741e18ad2f3f504f48228dc1bac948fc22761a348363d`).
  * Install it again only with the user's OK, because that is a download.
* **Ninja:** the copy Visual Studio ships, `Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja`.
  `build_env.ps1` puts it on PATH.
* **The cache:** `C:\dev\sccache-cache`, capped at 20 GB. Override it with
  `TRANSMUTE_SCCACHE_CACHE`.

## Each PowerShell session that builds

```powershell
. scripts/dev/build_env.ps1        # VS x64 environment, ninja, sccache settings for this checkout
```

Every build command, configure included, must run in a session that has dot-sourced it. Ninja
calls `cl.exe` directly, so it needs the developer environment.

## Configure (once per build directory)

```powershell
cmake -S . -B build -G "Ninja Multi-Config" -DCMAKE_CONFIGURATION_TYPES=Release `
  -DTRANSMUTE_COMPILER_CACHE=ON -DTRANSMUTE_WITH_FX=ON <the usual -DALCHEMY_* values>
```

* "Ninja Multi-Config" keeps the outputs in `build\Release\`, as with the Visual Studio
  generator. So `build\Release\alchemy-fx.exe`, `ctest -C Release` and the run scripts work
  unchanged.
* A build directory made by the Visual Studio generator can't switch generators. Use a new
  directory.

## Build

```powershell
cmake --build build --config Release --target alchemy-fx <test targets> -- -j 4
```

* `-j` is Ninja's job count: one compiler per job. On the shared machine use `-j 4`, or
  `-j 2` when three or more builds are running. The wave build locks still apply.
* Never build `alchemy_llvm_runtimes`, `alchemy_swift_runtime` or `alchemy_expat_runtime`
  from a worktree. They rewrite shared artifacts.
* `sccache --show-stats` (in `$env:TRANSMUTE_SCCACHE_DIR`) shows the hits and misses for this
  checkout's server.

## How sharing works

* **Base directories.** sccache hashes the preprocessed source and the arguments, and both
  contain absolute paths. `SCCACHE_BASEDIRS` strips the checkout root from both, so the same
  source in two worktrees hashes the same.
* **One server per checkout.** The base directory is a server setting, read when the server
  starts, and worktrees come and go. So `build_env.ps1` gives each checkout its own server port,
  derived from its root path. All the servers share the one cache directory.
* **Header dependencies.** On a cache hit sccache replays the `/showIncludes` notes of the
  compile that filled the cache, and those name that compile's checkout. Ninja records them as
  the object's header dependencies, so on its own, a header change in this checkout (an edit, a
  merge or a branch switch) would not rebuild the objects that include it, and the binary would
  mix two layouts of one structure. The launcher is therefore `tools/sccache_shim.cpp`, which
  CMake builds at configure time into `<build>\tools\sccache_shim.exe`.
  * It runs sccache and rewrites each include note naming a file in another checkout (a
    directory holding `.git`) to the same file in this one, when that file exists.
  * It leaves a generated header in another checkout's build directory as it is, unless this
    build directory has the same name.
  * A build directory configured before the shim existed keeps the wrong dependencies until it
    is reconfigured and its `.ninja_deps` is deleted once. Deleting it sends every object back
    through the cache, where unchanged objects are hits.
* **Debug information.** The fx components use `/Z7` with the cache, which embeds the debug
  information in the objects, instead of `/Zi`. sccache can't cache objects that write a
  shared PDB. Executables still get a PDB at link time (`/DEBUG`).

## Measured (2026-10-18)

`alchemy-fx` (529 compiles), `-j 4`, on a loaded machine with five or six other builds running:

| Build | Wall time | sccache |
| --- | --- | --- |
| First, cold (fills the cache) | 42.3 min | 0 hits / 529 misses |
| Same commit, a different worktree | 2.9 min | 529 hits / 0 misses |

* **The cache is small:** 72 MiB for one full set of objects.
* **The cache-built binary works:** Chrome strict headless `--dump-dom` with it exits 0 with
  `data-js="ran"`.
* **The executables differ only in the link timestamp.**
