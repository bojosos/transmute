<#
.SYNOPSIS
  Exhaustive import / Objective-C inventory of an installed macOS bundle
  (written for Google Chrome.app; docs/ALCHEMY_CHROME_GAPS.md).

.DESCRIPTION
  Runs no guest code. Steps:
   1. `alchemy-fx doctor --deps --scan-bundle --json` over the bundle.
   2. `doctor --deps --json` again for every other executable image of the
      bundle, so that its @executable_path names its own directory (a bundle
      scan expands it against the main executable).
   3. scripts/alchemy_objc_gap_scan.py: Objective-C metadata of every arm64
      image; with -Dump (an ALCHEMY_DUMP_OBJC class dump of a run) also the
      class and selector gaps against what the runtime registers.
   4. scripts/alchemy_import_inventory.py: one class per import (provided,
      provided-error/-stop by a source heuristic, unsupported, guest,
      missing-library, unresolved, weak-unresolved), per library and per area,
      plus dlopen / NSClassFromString string candidates.

.EXAMPLE
  scripts/alchemy_chrome_inventory.ps1 -Alchemy build-c/Release/alchemy-fx.exe `
    -Root C:\dev\transmute\artifacts\alchemy-root-chrome -OutDirectory $scratch\inventory `
    -Dump $scratch\runs\p2\objc-dump.json -Probe $scratch\runs\p2\stderr.txt
#>
param(
    [Parameter(Mandatory = $true)][string]$Alchemy,
    [Parameter(Mandatory = $true)][string]$Root,
    [string]$Bundle = 'Applications\Google Chrome.app',
    [string[]]$RuntimeDir = @('C:\dev\transmute\artifacts\alchemy-llvm-arm64'),
    [Parameter(Mandatory = $true)][string]$OutDirectory,
    [string]$Dump = '',
    [string[]]$Probe = @(),
    [string]$SymbolsMd = '',
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
$scripts = $PSScriptRoot
$alchemyPath = (Resolve-Path -LiteralPath $Alchemy).Path
$bundlePath = (Resolve-Path -LiteralPath (Join-Path $Root $Bundle)).Path
New-Item -ItemType Directory -Force $OutDirectory | Out-Null
$out = (Resolve-Path -LiteralPath $OutDirectory).Path
$env:ALCHEMY_GUI_NO_ACTIVATE = '1'; $env:ALCHEMY_GUI_CLOAK = '1'; $env:ALCHEMY_GUI_NO_CRASH_DIALOG = '1'
Remove-Item Env:ALCHEMY_PROBE_MISSING -ErrorAction SilentlyContinue

function Invoke-Doctor([string[]]$Arguments, [string]$Target, [string]$Output) {
    # Read doctor's stdout as UTF-8 and write it without a BOM (PowerShell 5.1
    # redirection would re-encode it). doctor exits 1 whenever anything is
    # unsupported; the JSON is complete either way.
    $runtime = ($RuntimeDir | ForEach-Object { "--runtime-dir `"$_`"" }) -join ' '
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $alchemyPath
    $info.Arguments = "doctor $($Arguments -join ' ') --json $runtime --root `"$Root`" `"$Target`""
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = New-Object Text.UTF8Encoding $false
    $process = [Diagnostics.Process]::Start($info)
    $errors = $process.StandardError.ReadToEndAsync()
    $text = $process.StandardOutput.ReadToEnd()
    $process.WaitForExit()
    $null = $errors.Result
    if (-not $text.TrimStart().StartsWith('{')) { throw "doctor wrote no JSON for $Target" }
    [IO.File]::WriteAllText($Output, $text, (New-Object Text.UTF8Encoding $false))
}

# 1. Whole bundle.
$main = Join-Path $out 'doctor.json'
Invoke-Doctor @('--deps', '--scan-bundle') $bundlePath $main
$doctor = [IO.File]::ReadAllText($main) | ConvertFrom-Json
Write-Host "doctor: $($doctor.images.Count) images"

# 2. Every other executable on its own.
$doctors = @($main)
$index = 0
foreach ($image in $doctor.images) {
    $same = [IO.Path]::GetFullPath($image.path).ToLowerInvariant() -eq [IO.Path]::GetFullPath($doctor.root).ToLowerInvariant()
    if ($image.type -ne 'executable' -or $same) { continue }
    $index += 1
    $file = Join-Path $out ("doctor-exe$index.json")
    Invoke-Doctor @('--deps') $image.path $file
    $doctors += $file
}
Write-Host "doctor: $index executables re-inspected"

# 3. Objective-C metadata.
$objc = Join-Path $out 'objc'
$scanArgs = @('-I', (Join-Path $scripts 'alchemy_objc_gap_scan.py'), $bundlePath, '--out', $objc,
              '--sources', (Join-Path $scripts '..\src\alchemy'))
if ($Dump) { $scanArgs += @('--dump', $Dump) } else { $scanArgs += '--scan-only' }
foreach ($log in $Probe) { $scanArgs += @('--probe', $log) }
$ErrorActionPreference = 'Continue'   # the scan reports progress on stderr (a native-command error in PowerShell 5.1)
& $Python @scanArgs 2>$null | Out-Host
if ($LASTEXITCODE -ne 0) { throw "alchemy_objc_gap_scan.py failed ($LASTEXITCODE)" }

# 4. Inventory.
$invArgs = @('-I', (Join-Path $scripts 'alchemy_import_inventory.py'), '--out', $out, '--bundle', $bundlePath,
             '--bundle-name', (Split-Path $bundlePath -Leaf), '--sources', (Join-Path $scripts '..\src\alchemy'),
             '--scan', (Join-Path $objc 'scan.json'), '--summary-md', (Join-Path $out 'libraries.md'),
             '--areas-md', (Join-Path $out 'areas.md'))
foreach ($file in $doctors) { $invArgs += @('--doctor', $file) }
if ($Dump) { $invArgs += @('--dump', $Dump, '--gaps', (Join-Path $objc 'gaps.json')) }
foreach ($log in $Probe) { $invArgs += @('--probe', $log) }
$invArgs += @('--symbols-md', $(if ($SymbolsMd) { $SymbolsMd } else { Join-Path $out 'symbols.md' }))
& $Python @invArgs | Out-Host
if ($LASTEXITCODE -ne 0) { throw "alchemy_import_inventory.py failed ($LASTEXITCODE)" }
Write-Host "inventory: $out"
