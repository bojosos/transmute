# Builds the fx perf suite's guest fixtures (docs/FX_PERF.md):
#   <Out>\fxbench           arm64 Mach-O, LSE atomics (-mcpu=apple-m1, as Apple-silicon builds are)
#   <Out>\fxbench_llsc      arm64 Mach-O, ARMv8.0 (-mcpu=cortex-a53: Apple targets keep LSE under -march=armv8-a): atomics as LL/SC loops
#   <Out>\fxbench_native.exe x86-64 Windows build of the same source: reference times and checksums
# The guest builds use Visual Studio's clang and ld64.lld (as cmake/Alchemy.cmake does for its
# macOS fixtures), linked against a project-authored link-only libSystem stub.
# Usage: scripts/perf/build_fixtures.ps1 [-Out <dir>]   (default: <checkout>\build-perf-fixtures)
param([string]$Out = '')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
if (-not $Out) { $Out = Join-Path $repo 'build-perf-fixtures' }
New-Item -ItemType Directory -Force $Out | Out-Null

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$llvm = Join-Path $vs 'VC\Tools\Llvm\x64\bin'
$clang = Join-Path $llvm 'clang.exe'
$lld = Join-Path $llvm 'ld64.lld.exe'
foreach ($t in $clang, $lld) { if (-not (Test-Path $t)) { throw "missing $t" } }

$src = Join-Path $here 'fixtures\fxbench.c'
$stub = Join-Path $here 'fixtures\fxbench_link.c'
function Invoke-Tool([string]$exe, [string[]]$arguments) {
    $ErrorActionPreference = 'Continue'   # compiler warnings on stderr are not failures
    & $exe @arguments 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE) { throw "$([IO.Path]::GetFileName($exe)) failed ($LASTEXITCODE): $($arguments -join ' ')" }
}

# Common flags: -fno-builtin keeps the guest's own copy loops in guest code; -ffp-contract=off makes
# floating-point results identical across the arm64 and x64 builds (fused ops are explicit fma calls).
$common = @('-O2', '-fno-builtin', '-ffp-contract=off', '-fno-stack-protector', '-Wall', '-Wno-unused-function')
$guest = @('--target=arm64-apple-macos11', '-ffreestanding') + $common
$link = @('-arch', 'arm64', '-platform_version', 'macos', '11.0', '11.0', '-fixup_chains')

Invoke-Tool $clang ($guest + @('-c', $stub, '-o', "$Out\fxbench_link.o"))
Invoke-Tool $lld ($link + @('-dylib', '-install_name', '/usr/lib/libSystem.B.dylib', "$Out\fxbench_link.o", '-o', "$Out\libSystem.B.dylib"))
foreach ($v in @(@{ Name = 'fxbench'; Cpu = @('-mcpu=apple-m1') }, @{ Name = 'fxbench_llsc'; Cpu = @('-mcpu=cortex-a53', '-mno-outline-atomics') })) {
    Invoke-Tool $clang ($guest + $v.Cpu + @('-c', $src, '-o', "$Out\$($v.Name).o"))
    Invoke-Tool $lld ($link + @('-e', '_main', "$Out\$($v.Name).o", "$Out\libSystem.B.dylib", '-o', "$Out\$($v.Name)"))
}
# Native reference. legacy_stdio_definitions provides printf as a linkable symbol (the fixture
# includes no headers).
Invoke-Tool $clang (@('--target=x86_64-pc-windows-msvc', '-march=x86-64-v3') + $common + @($src, '-o', "$Out\fxbench_native.exe",
    '-Wno-microsoft-anon-tag', '-llegacy_stdio_definitions', '-lkernel32'))
Get-ChildItem $Out -Include fxbench, fxbench_llsc, fxbench_native.exe -Recurse | ForEach-Object { '{0,-20} {1,8} bytes' -f $_.Name, $_.Length }
