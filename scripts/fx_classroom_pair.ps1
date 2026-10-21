# Runs native and fx Classroom renders simultaneously, one on each P-core
# (logical 0 and 2 on the i7-1355U), swapping cores every pair, so machine
# load affects both equally. Prints Blender render time and the fx/native ratio.
param(
    [Parameter(Mandatory = $true)] [string] $Fx,
    [int] $Percent = 10,
    [int] $Samples = 16,
    [int] $Pairs = 6,
    [int] $Threads = 1,
    [long[]] $Masks = @(1, 4),
    [string] $OutDir = 'C:\dev\transmute\artifacts\fx-classroom-pair'
)
$ErrorActionPreference = 'Stop'
$arm = "C:\dev\transmute\artifacts\arm64-stage\Blender Foundation\Blender 4.5\blender.exe"
$native = "C:\Program Files\Blender Foundation\Blender 4.5\blender.exe"
$scene = "C:\dev\transmute\artifacts\scene\classroom\classroom.blend"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$expr = "import bpy; s=bpy.context.scene; s.cycles.samples=$Samples; s.render.resolution_percentage=$Percent"

function Start-Render([string]$mode, [long]$mask, [int]$pair) {
    $prefix = Join-Path $OutDir ("{0}_{1}_#####" -f $mode, $pair)
    $bargs = @('--factory-startup', '-b', $scene, '-E', 'CYCLES', '-t', "$Threads", '-o', $prefix, '-F', 'PNG',
               '--python-expr', $expr, '-f', '1', '--', '--cycles-device', 'CPU')
    if ($mode -eq 'fx') { $exe = $Fx; $argv = @($arm) + $bargs } else { $exe = $native; $argv = $bargs }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($argv | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $p.ProcessorAffinity = [IntPtr]$mask
    [pscustomobject]@{ p = $p; out = $p.StandardOutput.ReadToEndAsync(); err = $p.StandardError.ReadToEndAsync() }
}
function Render-Time($r) {
    $r.p.WaitForExit()
    $line = ($r.out.Result -split "`n" | Select-String '^Time:\s+(\d+):(\d+\.\d+)' | Select-Object -Last 1)
    if (-not $line) { return [double]::NaN }
    [double]$line.Matches[0].Groups[1].Value * 60 + [double]$line.Matches[0].Groups[2].Value
}
$ratios = @()
for ($i = 0; $i -lt $Pairs; $i++) {
    $mn = $Masks[$i % 2]; $mf = $Masks[($i + 1) % 2]
    $n = Start-Render 'native' $mn $i
    $f = Start-Render 'fx' $mf $i
    $tn = Render-Time $n; $tf = Render-Time $f
    $ratios += $tf / $tn
    'pair {0}: native {1:N2} s (mask {2})  fx {3:N2} s (mask {4})  ratio {5:N3}' -f $i, $tn, $mn, $tf, $mf, ($tf / $tn)
}
$s = $ratios | Sort-Object
'median ratio {0:N3}  geomean {1:N3}' -f $s[[int][math]::Floor($s.Count / 2)], [math]::Exp(($ratios | ForEach-Object { [math]::Log($_) } | Measure-Object -Sum).Sum / $ratios.Count)
