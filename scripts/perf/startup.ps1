# Startup timeline of Blender.app under alchemy-fx (or native x64 Blender):
# process creation to window, first frame, first 3D viewport draw and the
# first full UI frame, with ALCHEMY_PERF's phase table, cold or warm.
#
#   scripts/perf/startup.ps1 [-Target alchemy|native] [-Exe alchemy-fx.exe] [-Cold] [-Repeat 3] [-OutDir dir]
#
# -Cold (Alchemy): ALCHEMY_FX_CACHE=0 and a fresh MSL result cache and
# VkPipelineCache file (fresh directories under -OutDir); warm runs use the
# default caches. Every run uses a fresh BLENDER_USER_RESOURCES (factory
# startup, first-start splash), like the M4 harness. Blender quits itself 3 s
# after the first viewport draw (scripts/perf/startup_probe.py), so the exit
# reports (ALCHEMY_PERF, fx translation cache) are printed. Opens a window but
# injects no input; an NSAlert is answered by ALCHEMY_GUI_AUTODISMISS_ALERTS=0.
# Prints one JSON object (and writes <OutDir>/startup.json).
param(
    [ValidateSet('alchemy', 'native')] [string] $Target = 'alchemy',
    [string] $Exe = '', [switch] $Cold, [int] $Repeat = 1, [string] $OutDir = '',
    [int] $TimeoutSeconds = 240, [double] $QuitSeconds = 3
)
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$artifacts = if (Test-Path "$root/artifacts/alchemy-blender") { "$root/artifacts" } else { 'C:/dev/transmute/artifacts' }
if (-not $OutDir) { $OutDir = "$artifacts/alchemy-blender/perf/startup-$(Get-Date -Format 'yyyyMMdd-HHmmss')-$Target$(if ($Cold) { '-cold' })" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$probe = (Resolve-Path "$PSScriptRoot/startup_probe.py").Path
if (-not $Exe -and $Target -eq 'alchemy') { $Exe = "$root/build-wf-gui/Release/alchemy-fx.exe" }

function Guest([string] $Path) { "/Volumes/$($Path.Substring(0, 1))/$($Path.Substring(3) -replace '\\', '/')" }
function Load() { [math]::Round(((Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 2).CounterSamples.CookedValue | Measure-Object -Average).Average) }

$runs = @()
for ($run = 0; $run -lt $Repeat; ++$run) {
    $dir = Join-Path $OutDir "run$run"
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force $dir | Out-Null
    $user = Join-Path $dir 'user'
    New-Item -ItemType Directory -Force $user | Out-Null
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.WorkingDirectory = $root
    $psi.EnvironmentVariables['ALCHEMY_STARTUP_QUIT'] = "$QuitSeconds"
    if ($Target -eq 'alchemy') {
        $psi.FileName = (Resolve-Path $Exe).Path
        $app = "$artifacts/alchemy-blender/extracted/Blender/Blender.app"
        $psi.Arguments = "--runtime-dir `"$((Resolve-Path "$artifacts/alchemy-llvm-arm64").Path)`" `"$((Resolve-Path $app).Path)`" --factory-startup --python `"$(Guest $probe)`""
        foreach ($pair in @{ ALCHEMY_PERF = '1'; ALCHEMY_GUI_NO_ACTIVATE = '1'; ALCHEMY_GUI_NO_CRASH_DIALOG = '1';
                             ALCHEMY_GUI_AUTODISMISS_ALERTS = '0'; ALCHEMY_FX_STATS = '1' }.GetEnumerator()) {
            $psi.EnvironmentVariables[$pair.Key] = $pair.Value
        }
        $psi.EnvironmentVariables['BLENDER_USER_RESOURCES'] = Guest $user
        if ($Cold) {
            $psi.EnvironmentVariables['ALCHEMY_FX_CACHE'] = '0'
            $psi.EnvironmentVariables['ALCHEMY_MSL_CACHE'] = Join-Path $dir 'msl-cache'
            $psi.EnvironmentVariables['ALCHEMY_METAL_PIPELINE_CACHE'] = Join-Path $dir 'pipeline-cache.bin'
        }
    } else {
        $psi.FileName = (Resolve-Path "$artifacts/x64/blender-4.5.14-windows-x64/blender.exe").Path
        $psi.Arguments = "--factory-startup --no-window-focus --python `"$probe`""
        $psi.EnvironmentVariables['BLENDER_USER_RESOURCES'] = $user
    }
    $load = Load
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $p = [Diagnostics.Process]::Start($psi)
    $startEpochMs = ([DateTimeOffset]$p.StartTime.ToUniversalTime()).ToUnixTimeMilliseconds()
    $out = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutSeconds * 1000)) { try { $p.Kill() } catch { }; $p.WaitForExit(10000) | Out-Null }
    $wall = $clock.Elapsed.TotalSeconds
    $text = $out.Result + "`n" + $err.Result
    Set-Content -Encoding utf8 (Join-Path $dir 'log.txt') $text
    $phases = [ordered]@{}
    $inPhases = $false
    foreach ($line in $text -split "`n") {
        if ($line -match '^\[alchemy perf\] startup phases') { $inPhases = $true; continue }
        if ($inPhases -and $line -match '^\[alchemy perf\]\s+([0-9]+\.[0-9])  (\S.*?)\s*$') {
            if (-not $phases.Contains($Matches[2])) { $phases[$Matches[2]] = [double]$Matches[1] }
            continue
        }
        $inPhases = $false
        if ($line -match '^ALCHEMY_PERF_PHASE (.+) ([0-9.]+)\s*$') { $phases["guest: $($Matches[1])"] = [double]$Matches[2] }
        elseif ($line -match '^ALCHEMY_PERF_PHASE (.+) @([0-9.]+)\s*$') {
            # Epoch ms (guest clock) relative to the host process creation time.
            $phases["guest: $($Matches[1])"] = [math]::Round([double]$Matches[2] - $startEpochMs, 1)
        }
    }
    $cache = ($text -split "`n" | Where-Object { $_ -match '^alchemy-fx cache:' } | Select-Object -First 1)
    $runs += [ordered]@{ run = $run; wall_s = [math]::Round($wall, 1); exit_code = $p.ExitCode; load_percent = $load
        phases_ms = $phases; fx_cache = $cache }
    Write-Host "run $run`: wall $([math]::Round($wall, 1)) s, load $load%, first viewport draw $($phases['guest: first 3D viewport draw']) ms"
}
$result = [ordered]@{ target = $Target; cold = [bool]$Cold; exe = $Exe; runs = $runs }
$result | ConvertTo-Json -Depth 6 | Tee-Object -FilePath (Join-Path $OutDir 'startup.json')
