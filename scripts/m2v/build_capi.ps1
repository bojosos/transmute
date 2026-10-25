# Builds external/m2v-capi (LGPL-3.0-or-later, a C ABI over metal2vulkan) into m2v_capi.dll.
#
# alchemy-fx loads the DLL at run time to translate precompiled Metal libraries
# (newLibraryWithData:); without it they fail as before. Nothing of it is linked into Alchemy.
#
# Usage: scripts/m2v/build_capi.ps1 [-Out <dir>] [-Jobs N] [-Debug]
#   -Out   where m2v_capi.dll (and .pdb) land; default <repo>/artifacts/m2v-capi
#   -Jobs  cargo -j (default 2)
# Needs Rust 1.98.1 (rustup reads external/m2v-capi/rust-toolchain.toml). The first build fetches
# metal2vulkan at the revision Cargo.toml pins and its crates.io dependencies (Cargo.lock), and
# takes about a quarter of an hour; later ones only rebuild the wrapper. Cargo's own output
# (target/) stays under <Out>/target.
param([string]$Out = '', [int]$Jobs = 2, [switch]$Debug)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$crate = Join-Path $repo 'external\m2v-capi'
if (-not $Out) { $Out = Join-Path $repo 'artifacts\m2v-capi' }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) { throw 'cargo not found: install rustup (Rust 1.98.1 is selected by rust-toolchain.toml)' }
$profile = if ($Debug) { 'debug' } else { 'release' }
$cargoArgs = @('build', '--manifest-path', (Join-Path $crate 'Cargo.toml'), '--target-dir', (Join-Path $Out 'target'), '--locked', '-j', "$Jobs")
if (-not $Debug) { $cargoArgs += '--release' }
# Cargo writes progress to stderr; run it through cmd so Windows PowerShell does not turn that into errors.
$quoted = ($cargoArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
Push-Location $crate
try { & cmd /c "cargo $quoted 2>&1"; $code = $LASTEXITCODE } finally { Pop-Location }
if ($code -ne 0) { throw "cargo build failed ($code)" }
foreach ($file in 'm2v_capi.dll', 'm2v_capi.pdb') {
    $built = Join-Path $Out "target\$profile\$file"
    if (Test-Path $built) { Copy-Item -Force $built (Join-Path $Out $file) }
}
Write-Host "m2v_capi.dll -> $(Join-Path $Out 'm2v_capi.dll')"
