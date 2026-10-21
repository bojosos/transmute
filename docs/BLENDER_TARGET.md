# Blender compatibility target

Pin the first end-to-end target to **Blender 4.5.14 LTS for Windows**, released on 15 September 2026. Blender publishes matching ARM64 and x64 MSI installers, MSIX packages, and portable ZIPs. The MSI is the installation target; the ZIP is useful for diagnosing application startup without installation. These are official release artifacts, not development builds. [Blender 4.5 release index](https://download.blender.org/release/Blender4.5/), [Blender 4.5 LTS page](https://www.blender.org/releases/4-5/)

| Artifact | Official download | SHA-256 |
| --- | --- | --- |
| ARM64 MSI | [blender-4.5.14-windows-arm64.msi](https://download.blender.org/release/Blender4.5/blender-4.5.14-windows-arm64.msi) | `b74818273c454c9901e21d4e0029d1cee73dd0c5c6a334639984186bf9f1dbc5` |
| x64 MSI | [blender-4.5.14-windows-x64.msi](https://download.blender.org/release/Blender4.5/blender-4.5.14-windows-x64.msi) | `5e04b3f587250d611bdd40d41fd9e3f75207a44e7d554d495dfe81ff6345937c` |
| ARM64 ZIP | [blender-4.5.14-windows-arm64.zip](https://download.blender.org/release/Blender4.5/blender-4.5.14-windows-arm64.zip) | `0153ecefc96a0e23985e6edcdbe04e909ba2078258f4ff6a05c21d4b7c756911` |
| x64 ZIP | [blender-4.5.14-windows-x64.zip](https://download.blender.org/release/Blender4.5/blender-4.5.14-windows-x64.zip) | `b9533d2397ac1984db4466fb23a7a4649391cca93f6e84209f9bcc60d071c8b9` |

The hashes come from [Blender's 4.5.14 SHA-256 manifest](https://download.blender.org/release/Blender4.5/blender-4.5.14.sha256). Check each downloaded file against that manifest before testing. The ARM64 MSI is about 221 MB and the x64 MSI about 360 MB; the official download endpoint serves both directly. [ARM64 MSI](https://download.blender.org/release/Blender4.5/blender-4.5.14-windows-arm64.msi), [x64 MSI](https://download.blender.org/release/Blender4.5/blender-4.5.14-windows-x64.msi)

## Full-scene render

Use [Classroom](https://www.blender.org/download/demo-files/) as the first full-scene Cycles workload. Blender lists it under Cycles, credits Christophe Seux, and marks it CC0. The [official archive](https://download.blender.org/demo/test/classroom.zip) is about 70 MB. Its SHA-256 is `0f0ecc58d45f12b2f4272a53e2d8e69135518e16e956b118f10c20d8dcfdf5e6`. Extract the whole archive, including its linked assets; the scene is `classroom\classroom.blend`. Blender's own performance-test inventory also includes `cycles/classroom`, so it is a useful common workload, although the run below is a full frame rather than Blender Open Data's fixed-time score. [Blender performance tests](https://developer.blender.org/docs/handbook/testing/performance/), [Blender Open Data benchmark method](https://opendata.blender.org/about/)

Run the same extracted `.blend` file with each architecture. The fixed comparison
uses the complete 1920 × 1080 frame, 16 Cycles samples, eight CPU threads, and
PNG output. The 16-sample override differs from the scene's original
300-sample setting; apply it identically to both architectures. CPU rendering
keeps GPU driver differences out of this translation measurement. The Blender
manual documents `-b`, `-E CYCLES`, `-o`, `-F`, `-f`, and the Cycles options
after `--`; it also warns that argument order matters. [Blender 4.5 command-line rendering](https://docs.blender.org/manual/en/4.5/advanced/command_line/render.html), [Blender 4.5 command-line arguments](https://docs.blender.org/manual/en/4.5/advanced/command_line/arguments.html)

```text
blender.exe --factory-startup -b classroom\classroom.blend -E CYCLES -t 8 -o output\classroom_##### -F PNG --python-expr "import bpy; bpy.context.scene.cycles.samples = 16; bpy.context.scene.render.resolution_percentage = 100" -f 1 -- --cycles-device CPU
```

The planned acceptance run has two parts. First, install the ARM64 MSI through Transmute, launch the installed Blender executable, and open the scene in the GUI. Then render frame 1 in the background with the command above, passing the same arguments to the translated ARM64 Blender and native x64 Blender. Confirm both produce a complete image. Record exit status, output path, image dimensions, Blender's render time, and wall-clock time from process start. Use one warm-up and the median of at least three measured runs per architecture on an otherwise idle machine. Keep the Blender version, scene archive, settings, CPU thread count, and output format identical. This procedure is a proposed project acceptance test, not Blender's official Open Data score.

Blender binaries are distributed under the GPL; the Classroom scene has a separate CC0 license. Keep their provenance and licenses distinct if test artifacts are redistributed. [Blender license](https://www.blender.org/about/license/), [Blender demo files](https://www.blender.org/download/demo-files/)
