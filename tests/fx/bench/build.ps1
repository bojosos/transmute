# Builds the fx benchmarks (bench.c) for ARM64 and x64 (owner: corpus/bench).
#   powershell -ExecutionPolicy Bypass -File tests\fx\bench\build.ps1 [-OutDir <dir>] [-Rebuild]
# Output: <OutDir>\arm64\bench_<group>.exe (plus bench_threads_lse.exe) and
# <OutDir>\x64\bench_<group>.exe. Default OutDir: build-fx-corpus\fx\bench.
# scripts\fx_bench.ps1 calls this before running.
[CmdletBinding()]
param([string]$OutDir, [switch]$Rebuild)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "..\..\..\scripts\fx_toolchain.ps1")
$repo = Get-FxRepoRoot
if (-not $OutDir) { $OutDir = Join-Path $repo "build-fx-corpus\fx\bench" }
$source = Join-Path $PSScriptRoot "bench.c"
$toolchain = Get-FxToolchain

# name, define, arch list, extra ARM64 flags, extra libs (per arch)
$variants = @(
    @{ Name = "compute";     Define = "B_COMPUTE" },
    @{ Name = "mem";         Define = "B_MEM" },
    @{ Name = "fma";         Define = "B_FMA" },
    @{ Name = "calls";       Define = "B_CALLS" },
    @{ Name = "heap";        Define = "B_HEAP"; Ucrt = $true },
    @{ Name = "threads";     Define = "B_THREADS" },
    @{ Name = "threads_lse"; Define = "B_THREADS"; Arm64Only = $true; Arm64Flags = @("/clang:-march=armv8.1-a") },
    @{ Name = "icall";       Define = "B_ICALL" },
    @{ Name = "recursion";   Define = "B_RECURSION" },
    @{ Name = "memcpy";      Define = "B_MEMCPY"; Vcruntime = $true },
    @{ Name = "interp";      Define = "B_INTERP" },
    # SIMD kernels (bench_simd.c, owner: neon/fp): NEON intrinsics vs SSE4.1/AVX2.
    @{ Name = "simd";        Define = "B_SIMD"; Source = "bench_simd.c"; X64Flags = @("/arch:AVX2") }
)

$crt = $null
$failed = 0
foreach ($v in $variants) {
    foreach ($arch in "arm64", "x64") {
        if ($v["Arm64Only"] -and $arch -ne "arm64") { continue }
        $target = $toolchain.Targets[$arch]
        $src = if ($v["Source"]) { Join-Path $PSScriptRoot $v["Source"] } else { $source }
        $obj = Join-Path $OutDir "obj\$arch\bench_$($v.Name).obj"
        $exe = Join-Path $OutDir "$arch\bench_$($v.Name).exe"
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $obj), (Split-Path -Parent $exe) | Out-Null
        # fp-contract=fast so the ARM64 FMA loop uses FMLA; -ffreestanding keeps
        # the compiler from treating memcpy as a builtin it may fold away.
        $flags = @("--target=$target", "/nologo", "/c", "/O2", "/GS-", "/Zl", "/Z7", "/W3", "-D$($v.Define)",
                   "/clang:-ffp-contract=fast", "/clang:-ffreestanding", "/Fo$obj", $src)
        if ($arch -eq "arm64" -and $v["Arm64Flags"]) { $flags += $v["Arm64Flags"] }
        if ($arch -eq "x64" -and $v["X64Flags"]) { $flags += $v["X64Flags"] }
        $libs = @("$($toolchain.SdkUm[$arch])\kernel32.lib")
        if ($v["Ucrt"]) { $libs += "$($toolchain.SdkUcrt[$arch])\ucrt.lib" }
        if ($v["Vcruntime"]) {
            if ($arch -eq "arm64") {
                if (-not $crt) { $crt = Build-FxArm64Crt -Toolchain $toolchain -OutDir (Join-Path $repo "build-fx-corpus\fx\crt") }
                $libs += Join-Path $repo "build-fx-corpus\fx\crt\vcruntime140.lib"
            } else {
                $libs += Join-Path $toolchain.MsvcLib.x64 "vcruntime.lib"
            }
        }
        if ($Rebuild -or (Test-FxStale $exe @($src, $PSCommandPath))) {
            if (-not (Invoke-FxTool $toolchain.ClangCl $flags "compile bench_$($v.Name) ($arch)")) { $failed++; continue }
            $link = @("/nologo", "/machine:$arch", "/entry:guest_entry", "/subsystem:console", "/nodefaultlib",
                      "/debug", "/incremental:no", "/out:$exe", $obj) + $libs
            if (-not (Invoke-FxTool $toolchain.LldLink $link "link bench_$($v.Name) ($arch)")) { $failed++; continue }
        }
    }
}
if ($crt -or (Test-Path -LiteralPath (Join-Path $OutDir "arm64\bench_memcpy.exe"))) {
    $dll = Join-Path $toolchain.Arm64Crt "vcruntime140.dll"
    if (Test-Path -LiteralPath $dll) { Copy-Item -LiteralPath $dll -Destination (Join-Path $OutDir "arm64") -Force }
}
Write-Host "bench build: $failed failure(s), output in $OutDir"
exit $failed
