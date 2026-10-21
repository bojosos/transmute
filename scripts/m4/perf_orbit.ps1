# M4 measurement: continuous viewport orbit frame rate with real input, on
# native x64 Blender and on Blender.app under Alchemy, plus input-to-frame
# latency samples (Alchemy: exact presented-frame counts).
#
#   scripts/m4/perf_orbit.ps1 -Target native|alchemy [-Exe alchemy-fx.exe] [-DurationMs 6000] [-Repeat 3]
#
# Blender runs with scripts/m4/draw_counter.py, which prints the number of
# 3D viewport redraws every 0.25 s; the orbit is a middle-button drag that
# circles the viewport centre for -DurationMs. Prints one JSON object.
# Needs the unlocked desktop (real input), like the M4 scenarios.
param(
    [ValidateSet('alchemy', 'native')] [string] $Target = 'alchemy',
    [string] $Exe = '', [string] $OutDir = '',
    [int] $DurationMs = 6000, [int] $Repeat = 3, [int] $StepMs = 8
)
. "$PSScriptRoot/m4_common.ps1"
if (-not $OutDir) { $OutDir = "$script:M4Artifacts/alchemy-blender/m4/perf/$(Get-Date -Format 'yyyyMMdd-HHmmss')-$Target" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$counter = (Resolve-Path "$PSScriptRoot/draw_counter.py").Path
if ($Target -eq 'alchemy') { $counter = "/Volumes/$($counter.Substring(0, 1))/$($counter.Substring(3) -replace '\\', '/')" }

function Get-Draws($Session) {
    $m = [regex]::Matches($Session.Stdout.Text, 'M4DRAWS (\d+) ([0-9.]+)')
    if (-not $m.Count) { return $null }
    $last = $m[$m.Count - 1]
    [ordered]@{ draws = [long]$last.Groups[1].Value; t = [double]$last.Groups[2].Value; lines = $m.Count }
}
function Wait-NextDraws($Session, $After) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ($clock.ElapsedMilliseconds -lt 3000) {
        $now = Get-Draws $Session
        if ($now -and (-not $After -or $now.lines -gt $After.lines)) { return $now }
        Start-Sleep -Milliseconds 20
    }
    throw 'draw_counter.py printed nothing (is --python honoured?)'
}

$launch = [Diagnostics.Stopwatch]::StartNew()
$session = Start-AlchemyGuiTarget -Target $Target -OutDir $OutDir -Exe $Exe -Arguments @('--factory-startup', '--python', $counter)
$result = [ordered]@{ target = $Target; settle_s = [math]::Round($launch.Elapsed.TotalSeconds, 1); orbits = @(); latency_ms = @() }
try {
    Set-M4ClientSize $session 1600 900
    $ctx = [ordered]@{ Name = 'perf'; Target = $Target; WhatIf = $false; Session = $session; Window = $session.Window; OutDir = $OutDir
        ReferenceDir = "$OutDir/ref"; Records = New-Object System.Collections.Generic.List[object]; Shots = @{}; Failure = $null
        NoReferenceCheck = $true; RefTolerance = 8; RefPercent = 3.0 }
    [void](Activate-AlchemyWindow $ctx.Window)
    $client = Get-AlchemyClient $ctx.Window
    $ctx.Layout = Get-BlenderLayout $client.Width $client.Height $client.Scale
    Invoke-M4DismissSplash $ctx
    Start-Sleep -Milliseconds 1500
    $c = $ctx.Layout.viewport_center
    $center = ConvertTo-AlchemyScreenPoint $ctx.Window $c[0] $c[1]
    for ($run = 0; $run -lt $Repeat; ++$run) {
        [AlchemyGuiInput]::MoveTo($center.X, $center.Y)
        Start-Sleep -Milliseconds 300
        $before = Wait-NextDraws $session $null
        $presentedBefore = Get-AlchemyFrameCount $session
        $clock = [Diagnostics.Stopwatch]::StartNew()
        [AlchemyGuiInput]::Button($center.X, $center.Y, 'middle', $true)
        try {
            $moves = 0
            while ($clock.ElapsedMilliseconds -lt $DurationMs) {
                $angle = 2 * [math]::PI * $clock.ElapsedMilliseconds / 3000.0
                [AlchemyGuiInput]::MoveTo([int]($center.X + 150 * [math]::Cos($angle)), [int]($center.Y + 80 * [math]::Sin($angle)))
                ++$moves
                Start-Sleep -Milliseconds $StepMs
            }
        } finally { [AlchemyGuiInput]::Button($center.X, $center.Y, 'middle', $false) }
        $elapsed = $clock.Elapsed.TotalSeconds
        $after = Wait-NextDraws $session $before
        $presentedAfter = Get-AlchemyFrameCount $session
        $orbit = [ordered]@{
            seconds = [math]::Round($elapsed, 2); moves = $moves
            viewport_draws = $after.draws - $before.draws
            viewport_fps = [math]::Round(($after.draws - $before.draws) / [math]::Max(0.001, $after.t - $before.t), 1)
        }
        if ($null -ne $presentedBefore) {
            $orbit.presented = $presentedAfter - $presentedBefore
            $orbit.presented_fps = [math]::Round(($presentedAfter - $presentedBefore) / $elapsed, 1)
        }
        $result.orbits += $orbit
        Write-Host "orbit $run`: $($orbit | ConvertTo-Json -Compress)"
        Start-Sleep -Milliseconds 1000
    }
    # Input-to-frame latency (Alchemy): a small move over a header button
    # toggles its highlight; time until the next presented frame.
    if ($session.StatsPath) {
        $a = ConvertTo-AlchemyScreenPoint $ctx.Window $ctx.Layout.file_menu[0] $ctx.Layout.file_menu[1]
        $b = ConvertTo-AlchemyScreenPoint $ctx.Window $c[0] $c[1]
        for ($i = 0; $i -lt 10; ++$i) {
            $point = if ($i % 2) { $b } else { $a }
            Start-Sleep -Milliseconds 400
            $since = Get-AlchemyFrameCount $session
            $clock = [Diagnostics.Stopwatch]::StartNew()
            [AlchemyGuiInput]::MoveTo($point.X, $point.Y)
            while ((Get-AlchemyFrameCount $session) -le $since -and $clock.ElapsedMilliseconds -lt 2000) { Start-Sleep -Milliseconds 1 }
            $result.latency_ms += $clock.ElapsedMilliseconds
        }
    }
} finally {
    $stopped = Stop-AlchemyGuiTarget $session
}
$result | ConvertTo-Json -Depth 5 | Tee-Object -FilePath (Join-Path $OutDir 'perf.json')
