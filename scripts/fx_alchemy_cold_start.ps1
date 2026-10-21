# Headless cold/warm start of macOS Blender.app under alchemy-fx: time to the
# --python-expr startup script and to process exit, fx translation counters
# (ALCHEMY_FX_STATS) and the machine's CPU load while the run lasted.
#
#   scripts/fx_alchemy_cold_start.ps1 -Exe <alchemy-fx.exe> [-Mode fresh|off|warm] [-Repeat 3]
#                                     [-CacheDir <dir>] [-OutDir <dir>] [-Env @{NAME='value'}]
#
# Modes: fresh = translation cache on, empty cache directory per run (the
# first start of a new install; background AOT can write into it); off =
# ALCHEMY_FX_CACHE=0 (no persistent cache at all); warm = cache on, -CacheDir
# reused as is. Runs `Blender -b --factory-startup -noaudio --python-expr`
# with a fresh BLENDER_USER_RESOURCES; never opens a window.
param(
    [Parameter(Mandatory)] [string] $Exe,
    [ValidateSet('fresh', 'off', 'warm')] [string] $Mode = 'fresh',
    [int] $Repeat = 3,
    [string] $CacheDir = '',
    [string] $OutDir = '',
    [hashtable] $Env = @{},
    [int] $TimeoutSeconds = 600
)
$ErrorActionPreference = 'Stop'
$artifacts = 'C:/dev/transmute/artifacts'
$app = "$artifacts/alchemy-blender/extracted/Blender/Blender.app"
$runtime = "$artifacts/alchemy-llvm-arm64"
if (-not $OutDir) { $OutDir = Join-Path $env:TEMP "fx-cold-start-$(Get-Date -Format 'yyyyMMdd-HHmmss')-$Mode" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
function Guest([string] $Path) { "/Volumes/$($Path.Substring(0, 1).ToUpper())/$($Path.Substring(3) -replace '\\', '/')" }

$runs = @()
for ($run = 0; $run -lt $Repeat; ++$run) {
    $dir = Join-Path $OutDir "run$run"
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force "$dir/user" | Out-Null
    $cache = if ($Mode -eq 'warm') { $CacheDir } else { Join-Path $dir 'fx-cache' }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path $Exe).Path
    $expr = "import time; print('ALCHEMY_READY %.3f' % time.time())"
    $psi.Arguments = "--runtime-dir `"$runtime`" `"$app`" -b --factory-startup -noaudio --python-expr `"$expr`""
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    foreach ($pair in @{ ALCHEMY_FX_STATS = '1'; ALCHEMY_GUI_NO_ACTIVATE = '1'; ALCHEMY_GUI_NO_CRASH_DIALOG = '1';
                         BLENDER_USER_RESOURCES = (Guest "$dir/user") }.GetEnumerator()) {
        $psi.EnvironmentVariables[$pair.Key] = $pair.Value
    }
    $psi.EnvironmentVariables.Remove('ALCHEMY_PROBE_MISSING')
    if ($Mode -eq 'off') { $psi.EnvironmentVariables['ALCHEMY_FX_CACHE'] = '0' }
    else { $psi.EnvironmentVariables['TRANSMUTE_FX_CACHE_DIR'] = $cache }
    foreach ($pair in $Env.GetEnumerator()) { $psi.EnvironmentVariables[$pair.Key] = "$($pair.Value)" }

    # CPU load of the whole machine, sampled every second while the run lasts.
    $loadJob = Start-Job { while ($true) { (Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 1).CounterSamples.CookedValue } }
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $p = [Diagnostics.Process]::Start($psi)
    $startEpoch = ([DateTimeOffset]$p.StartTime.ToUniversalTime()).ToUnixTimeMilliseconds() / 1000.0
    $out = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutSeconds * 1000)) { try { $p.Kill() } catch { }; $p.WaitForExit(10000) | Out-Null }
    $wall = $clock.Elapsed.TotalSeconds
    Stop-Job $loadJob
    $samples = @(Receive-Job $loadJob | ForEach-Object { [double]$_ })
    Remove-Job $loadJob -Force
    $load = if ($samples.Count) { [math]::Round(($samples | Measure-Object -Average).Average) } else { -1 }
    $loadMax = if ($samples.Count) { [math]::Round(($samples | Measure-Object -Maximum).Maximum) } else { -1 }
    $text = $out.Result + "`n" + $err.Result
    Set-Content -Encoding utf8 (Join-Path $dir 'log.txt') $text
    $ready = $null
    if ($text -match 'ALCHEMY_READY ([0-9.]+)') { $ready = [math]::Round([double]$Matches[1] - $startEpoch, 2) }
    $stats = ($text -split "`n" | Where-Object { $_ -match '^alchemy-fx stats:' } | Select-Object -First 1)
    $translate = $null; $translations = $null
    if ($stats -match 'translations ([0-9]+) \(([0-9]+) guest instructions\), translate ([0-9.]+) s') {
        $translations = [int]$Matches[1]; $translate = [double]$Matches[3]
    }
    $aot = ($text -split "`n" | Where-Object { $_ -match '^alchemy-fx aot:' } | Select-Object -First 1)
    $runs += [ordered]@{ run = $run; exit_code = $p.ExitCode; ready_s = $ready; wall_s = [math]::Round($wall, 2)
        load_avg = $load; load_max = $loadMax; jit_translations = $translations; translate_s = $translate
        aot = $aot; stats = $stats }
    Write-Host ("run {0}: exit {1}, ready {2} s, wall {3:N2} s, translate {4} s ({5} translations), load avg {6}% max {7}%" -f
        $run, $p.ExitCode, $ready, $wall, $translate, $translations, $load, $loadMax)
}
$result = [ordered]@{ exe = $Exe; mode = $Mode; env = $Env; runs = $runs }
$result | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $OutDir 'cold_start.json')
Write-Host "logs: $OutDir"
