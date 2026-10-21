# Startup-dominated benchmark: default cube, 16x16, 1 sample, 1 thread.
# Prints process wall time per run (native x64 Blender vs ARM64 Blender under fx).
param(
    [string] $Fx = "",
    [string[]] $Modes = @('native', 'fx'),
    [int] $Runs = 5,
    [string] $OutDir = 'C:\dev\transmute\artifacts\fx-tiny-bench',
    [switch] $Stats
)
$ErrorActionPreference = 'Stop'
$arm = "C:\dev\transmute\artifacts\arm64-stage\Blender Foundation\Blender 4.5\blender.exe"
$native = "C:\Program Files\Blender Foundation\Blender 4.5\blender.exe"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$expr = 'import bpy; s=bpy.context.scene; s.render.resolution_x=16; s.render.resolution_y=16; s.render.resolution_percentage=100; s.cycles.samples=1'
if ($Stats) { $env:TRANSMUTE_FX_STATS = '1' }
foreach ($mode in $Modes) {
    $times = @()
    for ($r = 0; $r -lt $Runs; $r++) {
        $prefix = Join-Path $OutDir "${mode}_${r}_"
        $log = Join-Path $OutDir "${mode}_${r}.log"
        $bargs = @('--factory-startup', '-b', '-E', 'CYCLES', '-t', '1', '--python-expr', $expr,
                   '-o', "$prefix#", '-F', 'PNG', '-f', '1', '--', '--cycles-device', 'CPU')
        $sw = [Diagnostics.Stopwatch]::StartNew()
        if ($mode -eq 'fx') { & $Fx $arm @bargs *> $log } else { & $native @bargs *> $log }
        $sw.Stop()
        $times += $sw.Elapsed.TotalSeconds
        if ($LASTEXITCODE -ne 0) { "{0} run {1}: exit code {2} (see {3})" -f $mode, $r, $LASTEXITCODE, $log }
    }
    $sorted = $times | Sort-Object
    '{0,-7} runs: {1}  median {2:N3} s  best {3:N3} s' -f $mode, (($times | ForEach-Object { $_.ToString('N3') }) -join ' '),
        $sorted[[int][math]::Floor($sorted.Count / 2)], $sorted[0]
}
if ($Stats) { Remove-Item Env:TRANSMUTE_FX_STATS }
