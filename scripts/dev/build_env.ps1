<#
.SYNOPSIS
Prepares the current PowerShell session for a Ninja + sccache build of this checkout
(docs/ALCHEMY_BUILD.md). Dot-source it: `. scripts/dev/build_env.ps1`.

.DESCRIPTION
* Imports the Visual Studio x64 developer environment (cl, link, INCLUDE, LIB), which a Ninja
  build of MSVC code needs, and puts Visual Studio's own ninja.exe on PATH.
* Points sccache at the shared cache (default C:\dev\sccache-cache, 20 GB) and at this
  checkout's root as SCCACHE_BASEDIRS, so objects compiled in one worktree are hits in every
  other one.
* Gives each checkout its own sccache server port. The base directory is a server setting read
  at server start, and worktrees come and go, so one server per checkout keeps every server's
  base directory right. All servers share the cache directory.
* Sets TRANSMUTE_SCCACHE_DIR to the main checkout's artifacts/tools/sccache, where CMake's
  TRANSMUTE_COMPILER_CACHE option finds sccache.exe.
#>
param(
    [string]$CacheDirectory = $(if ($env:TRANSMUTE_SCCACHE_CACHE) { $env:TRANSMUTE_SCCACHE_CACHE } else { 'C:\dev\sccache-cache' }),
    [string]$CacheSize = '20G'
)
# Dot-sourced: keep the caller's error preference once this script is done.
$buildEnvPreviousPreference = $ErrorActionPreference
$ErrorActionPreference = 'Stop'
try {

$root =(& git -C $PSScriptRoot rev-parse --show-toplevel).Trim() -replace '/', '\'
$common = (& git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir).Trim() -replace '/', '\'
$mainCheckout = Split-Path -Parent $common

if (-not $env:VSCMD_VER) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if (-not $vs) { throw 'No Visual Studio with the x64 C++ tools was found.' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $lines = & cmd.exe /d /s /c "`"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat failed ($LASTEXITCODE)." }
    foreach ($line in $lines) {
        $eq = $line.IndexOf('=')
        if ($eq -gt 0) { Set-Item -LiteralPath "Env:$($line.Substring(0, $eq))" -Value $line.Substring($eq + 1) }
    }
    $ninjaDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
    if (Test-Path (Join-Path $ninjaDir 'ninja.exe')) { $env:PATH = "$ninjaDir;$env:PATH" }
}

$sha = [System.Security.Cryptography.SHA256]::Create()
$hash = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($root.ToLowerInvariant()))
$port = 4300 + ([BitConverter]::ToUInt16($hash, 0) % 2000)

$env:SCCACHE_DIR = $CacheDirectory
$env:SCCACHE_CACHE_SIZE = $CacheSize
$env:SCCACHE_BASEDIRS = $root
$env:SCCACHE_SERVER_PORT = "$port"
$env:TRANSMUTE_SCCACHE_DIR = Join-Path $mainCheckout 'artifacts\tools\sccache'
New-Item -ItemType Directory -Force -Path $CacheDirectory | Out-Null

Write-Host "build env: $root (sccache port $port, cache $CacheDirectory, $CacheSize)"

} finally {
    $ErrorActionPreference = $buildEnvPreviousPreference
}
