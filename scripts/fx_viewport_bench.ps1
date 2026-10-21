# Interactive viewport benchmark: native x64 Blender vs ARM64 Blender under transmute-fx.
# Opens a real window, runs tests/fixtures/blender_viewport_bench.py, prints per-step
# times side by side. The window must stay unoccluded: DWM throttles hidden presents.
param(
    [string] $Fx = 'C:\dev\transmute\build-fx-integ\Release\transmute-fx.exe',
    [string[]] $Modes = @('native', 'fx'),
    [string] $OutDir = 'C:\dev\transmute\artifacts\fx-gui',
    [string] $Native = 'C:\Program Files\Blender Foundation\Blender 4.5\blender.exe',
    [string] $Arm = 'C:\dev\transmute\artifacts\arm64-stage\Blender Foundation\Blender 4.5\blender.exe'
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$py = (Resolve-Path (Join-Path $PSScriptRoot '..\tests\fixtures\blender_viewport_bench.py')).Path
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runtime = $Fx
if ($Modes -contains 'fx') {   # snapshot so concurrent rebuilds cannot replace the running exe
    $runtime = Join-Path $OutDir "transmute-fx-$stamp.exe"
    Copy-Item -LiteralPath $Fx -Destination $runtime
}
$table = @{}
foreach ($mode in $Modes) {
    $json = Join-Path $OutDir "$mode-$stamp.json"
    $bargs = @('--factory-startup', '-noaudio', '--gpu-backend', 'opengl',
               '--window-geometry', '80', '80', '1280', '800', '--python', "`"$py`"", '--', "`"$json`"")
    if ($mode -eq 'fx') { $exe = $runtime; $bargs = @("`"$Arm`"") + $bargs; $wd = Split-Path $Arm }
    else { $exe = $Native; $wd = Split-Path $Native }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $exe -ArgumentList $bargs -WorkingDirectory $wd -PassThru -Wait `
        -RedirectStandardOutput "$OutDir\$mode-$stamp.out.log" -RedirectStandardError "$OutDir\$mode-$stamp.err.log"
    '{0}: wall {1:N1} s, exit {2}' -f $mode, $sw.Elapsed.TotalSeconds, $p.ExitCode
    if (-not (Test-Path $json)) { "$mode produced no results; see $OutDir\$mode-$stamp.out.log"; continue }
    $r = Get-Content $json -Raw | ConvertFrom-Json
    if ($r.PSObject.Properties.Name -contains 'error') { "$mode error: $($r.error)" }
    foreach ($s in $r.steps) {
        if (-not $table.ContainsKey($s.name)) { $table[$s.name] = [ordered]@{ step = $s.name } }
        $table[$s.name][$mode] = [math]::Round($s.per_ms, 2)
    }
}
$rows = foreach ($k in $table.Keys) { $o = $table[$k]
    if ($o.Contains('native') -and $o.Contains('fx') -and $o['native'] -gt 0) { $o['ratio'] = [math]::Round($o['fx'] / $o['native'], 2) }
    [pscustomobject]$o }
$rows | Format-Table -AutoSize | Out-String -Width 200
