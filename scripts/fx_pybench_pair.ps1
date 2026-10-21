# Runs tests/fx/bench/pybench.py in native x64 Blender and in ARM64 Blender
# under fx simultaneously, one on each P-core (logical 0 and 2 on the
# i7-1355U), swapping cores every pair, so machine load affects both
# equally. Prints per-test seconds and fx/native ratios per pair, then the
# median ratio per test over all pairs and the geometric mean over tests.
#
# The fx process inherits the caller's environment (TRANSMUTE_FX_CACHE_DIR,
# TRANSMUTE_FX_BACKGROUND_AOT, TRANSMUTE_FX_CORE, ...); -CacheDir and
# -Env 'NAME=value' set variables for the fx process only. Run one warm-up
# pair (-Warmup, default 1, not counted) so the translation cache is warm.
# -RefFx (with -RefCacheDir, -RefEnv) replaces native Blender by another fx
# build or configuration: the ratios are then fx / reference fx (A/B runs).
param(
    [Parameter(Mandatory = $true)] [string] $Fx,
    [int] $Pairs = 4,
    [int] $Warmup = 1,
    [long[]] $Masks = @(1, 4),
    [string] $CacheDir,
    [string[]] $Env = @(),
    [string] $RefFx,
    [string] $RefCacheDir,
    [string[]] $RefEnv = @(),
    [string] $Script = 'C:\dev\transmute\tests\fx\bench\pybench.py'
)
$ErrorActionPreference = 'Stop'
$arm = "C:\dev\transmute\artifacts\arm64-stage\Blender Foundation\Blender 4.5\blender.exe"
$native = "C:\Program Files\Blender Foundation\Blender 4.5\blender.exe"

function Start-Bench([string]$mode, [long]$mask) {
    $bargs = @('--factory-startup', '-b', '--python', $Script)
    $cache = $CacheDir; $vars = $Env
    if ($mode -eq 'fx') { $exe = $Fx; $argv = @($arm) + $bargs }
    elseif ($RefFx) { $exe = $RefFx; $argv = @($arm) + $bargs; $cache = $RefCacheDir; $vars = $RefEnv }
    else { $exe = $native; $argv = $bargs }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($argv | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    if ($mode -eq 'fx' -or $RefFx) {
        if ($cache) { $psi.EnvironmentVariables['TRANSMUTE_FX_CACHE_DIR'] = $cache }
        foreach ($kv in $vars) {
            $i = $kv.IndexOf('=')
            if ($i -gt 0) { $psi.EnvironmentVariables[$kv.Substring(0, $i)] = $kv.Substring($i + 1) }
        }
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    $p.ProcessorAffinity = [IntPtr]$mask
    [pscustomobject]@{ p = $p; out = $p.StandardOutput.ReadToEndAsync(); err = $p.StandardError.ReadToEndAsync() }
}
function Bench-Times($r) {
    $r.p.WaitForExit()
    $t = [ordered]@{}
    foreach ($m in ($r.out.Result -split "`n" | Select-String '^PYBENCH\s+(\S+)\s+([0-9.]+)')) {
        $t[$m.Matches[0].Groups[1].Value] = [double]$m.Matches[0].Groups[2].Value
    }
    if ($t.Count -eq 0) { Write-Warning ("no PYBENCH lines (exit {0}): {1}" -f $r.p.ExitCode, ($r.err.Result -split "`n" | Select-Object -Last 5) -join ' | ') }
    $t
}
function Median([double[]]$v) {
    $s = $v | Sort-Object
    if ($s.Count % 2) { $s[[int][math]::Floor($s.Count / 2)] } else { ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2 }
}

$ratios = [ordered]@{}
$nat = [ordered]@{}
$fxs = [ordered]@{}
for ($i = -$Warmup; $i -lt $Pairs; $i++) {
    $k = [math]::Abs($i)
    $mn = $Masks[$k % 2]; $mf = $Masks[($k + 1) % 2]
    $n = Start-Bench 'native' $mn
    $f = Start-Bench 'fx' $mf
    $tn = Bench-Times $n; $tf = Bench-Times $f
    if ($i -lt 0) { "warm-up pair done ({0} fx tests)" -f $tf.Count; continue }
    $line = @()
    foreach ($name in $tn.Keys) {
        if (-not $tf.Contains($name)) { continue }
        $r = $tf[$name] / $tn[$name]
        if (-not $ratios.Contains($name)) { $ratios[$name] = @(); $nat[$name] = @(); $fxs[$name] = @() }
        $ratios[$name] += $r; $nat[$name] += $tn[$name]; $fxs[$name] += $tf[$name]
        $line += ('{0} {1:N2}' -f $name, $r)
    }
    'pair {0} (native mask {1}, fx mask {2}): {3}' -f $i, $mn, $mf, ($line -join '  ')
}
''
'{0,-14} {1,10} {2,10} {3,8}' -f 'test', 'native s', 'fx s', 'fx/nat'
$logs = @()
foreach ($name in $ratios.Keys) {
    $m = Median $ratios[$name]
    $logs += [math]::Log($m)
    '{0,-14} {1,10:N4} {2,10:N4} {3,8:N3}' -f $name, (Median $nat[$name]), (Median $fxs[$name]), $m
}
if ($logs.Count) { 'geomean of median ratios {0:N3}' -f [math]::Exp(($logs | Measure-Object -Sum).Sum / $logs.Count) }
