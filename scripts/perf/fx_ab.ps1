# fx perf suite: interleaved A/B runs of two alchemy-fx builds (docs/FX_PERF.md).
#
#   scripts/perf/fx_ab.ps1 -A <build dir | alchemy-fx.exe> -B <build dir | alchemy-fx.exe> -Out <scratch dir>
#       [-LabelA main] [-LabelB new] [-Bench micro,fxunit] [-Rounds 5] [-MaxLoad 25] [-QuietMinutes 30] [-NoQuietWait]
#       [-MicroScale 100] [-MicroReps 5] [-JetStreamTests richards,...] [-SpeedometerIterations 1]
#       [-EnvA @{...}] [-EnvB @{...}]
# -EnvA/-EnvB: extra environment for every run of that side, e.g. a codegen knob A/B on one binary:
#   -A <build> -B <build> -LabelA base -LabelB globals -EnvB @{ TRANSMUTE_FX_GLOBALS = 'sp,fp,lr,x19,x20,x21,x22,x26,x28' }
#
# Benchmarks (-Bench, comma-separated; "all" = every one, "quick" = micro,fxunit):
#   micro        fxbench guest kernels (scripts/perf/fixtures), translated, pinned to two P-cores; checksums are
#                checked against the native x64 build of the same source (a mismatch fails the run)
#   fxunit       the in-process translated-loop tests in fx_translator_tests (core_perf_*); needs build dirs
#   blender      Blender.app headless: process CPU of -b --factory-startup + one operator (startup-dominated)
#   cycles       Blender.app headless Cycles render of the factory cube (640x360, 64 samples, 2 threads; about 8 s of render)
#   plugin       a Blender renderer plug-in's headless cube render, on a scratch copy of the plug-in's Alchemy root
#                (-PluginRoot: a folder under -Artifacts; -PluginScript: the cube script's guest path)
#   qlab         QLab 5 to its first captured window (ALCHEMY_CONTENTS_CAPTURE) on a scratch copy of alchemy-root
#   sp-jitless, sp-jit   Speedometer 3 (browserbench.org), -SpeedometerIterations iterations, software compositor
#   js-jitless, js-jit   JetStream 2.2, one fresh browser per test in -JetStreamTests (?test=<name>)
#   fps          scripts/perf/pages/raf_box.html, rAF callbacks per second over 20 s, SwiftShader GPU process
#   fps-sw       the same page with the software compositor (--disable-gpu)
#
# Method: both binaries (exe, pdb, test exes) are copied into <Out>\bin-<label> and run from there. Every round runs
# every benchmark on both sides, A first on odd rounds and B first on even ones. Before each sample the script waits
# (up to -QuietMinutes) for a quiet machine: no compiler, linker or ninja process, and CPU load under -MaxLoad on three
# samples 2 s apart; the load is recorded with every sample. While it runs, <LockDir>\perf.lock (-LockDir or
# ALCHEMY_PERF_LOCK_DIR) tells a build-lock script that honours it not to start new builds. Each side has its own fx cache
# (TRANSMUTE_FX_CACHE_DIR=<Out>\cache-<label>): the cache key does not change with codegen changes, so a shared
# cache would run one build's translations under the other. Round 0 (not counted) warms both caches.
# Guest runs are cloaked; ALCHEMY_NETWORK=loopback except the Chrome benchmarks, which use web_visit.ps1's
# real-web safeguards (ALCHEMY_NETWORK=full with the update/crash/metrics deny list, logged).
#
# Output: <Out>\samples.csv (round, side, bench, metric, value, unit, better, load, status) and <Out>\summary.md from
# scripts/perf/summarize.py: per metric the median of each side, the spread, B/A and a bootstrap 95 % interval.
param(
    [Parameter(Mandatory)][string]$A,
    [Parameter(Mandatory)][string]$B,
    [Parameter(Mandatory)][string]$Out,
    [string]$LabelA = 'A',
    [string]$LabelB = 'B',
    [hashtable]$EnvA = @{},
    [hashtable]$EnvB = @{},
    [string[]]$Bench = @('quick'),
    [int]$Rounds = 5,
    [switch]$NoWarmup,
    [int]$MaxLoad = 25,
    [int]$QuietMinutes = 30,
    [switch]$NoQuietWait,
    [int]$MicroScale = 100,
    [int]$MicroReps = 5,
    [string]$Fixtures = '',
    [int]$SpeedometerIterations = 1,
    [string]$JetStreamTests = 'richards,delta-blue,crypto,navier-stokes,splay,Box2D',
    [string]$Artifacts = '',                                    # default: the main checkout's artifacts
    [string]$LockDir = $env:ALCHEMY_PERF_LOCK_DIR,              # where perf.lock lives; default %TEMP%\alchemy-perf
    [string]$PluginRoot = $env:ALCHEMY_PERF_PLUGIN_ROOT,        # 'plugin': the Alchemy root folder under -Artifacts
    [string]$PluginScript = $env:ALCHEMY_PERF_PLUGIN_SCRIPT,    # 'plugin': guest path of the headless cube script
    [string]$PluginUser = $env:USERNAME                         # 'plugin': the root's Users\<name> with its settings
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$checkout = (Resolve-Path (Join-Path $here '..\..')).Path
. (Join-Path $checkout 'scripts\fx_bench_control.ps1')
if (-not $Artifacts) {
    # A worktree has no artifacts of its own: use <git common dir>\..\artifacts, the main checkout's.
    $common = (& git -C $checkout rev-parse --path-format=absolute --git-common-dir).Trim()
    $Artifacts = Join-Path (Split-Path -Parent $common) 'artifacts'
}
if (-not $LockDir) { $LockDir = Join-Path $env:TEMP 'alchemy-perf' }
$LockDir = (New-Item -ItemType Directory -Force -Path $LockDir).FullName   # one spelling for every check

$all = @('micro', 'fxunit', 'blender', 'cycles', 'plugin', 'qlab', 'sp-jitless', 'sp-jit', 'js-jitless', 'js-jit', 'fps', 'fps-sw')
$benches = @($Bench | ForEach-Object { $_ -split ',' } | Where-Object { $_ } | ForEach-Object {
    if ($_ -eq 'all') { $all } elseif ($_ -eq 'quick') { 'micro', 'fxunit' } else { $_ } })
foreach ($name in $benches) { if ($all -notcontains $name) { throw "unknown benchmark $name (known: $($all -join ', '))" } }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
if (-not $Fixtures) { $Fixtures = Join-Path $checkout 'build-perf-fixtures' }
$llvm = Join-Path $Artifacts 'alchemy-llvm-arm64'
$swift = Join-Path $Artifacts 'alchemy-swift-arm64'
$expat = Join-Path $Artifacts 'alchemy-expat-arm64'
$blenderApp = Join-Path $Artifacts 'alchemy-blender\extracted\Blender\Blender.app'
$log = Join-Path $Out 'run.log'
$script:sideEnv = @{}   # the running side's -EnvA/-EnvB (Invoke-Bench sets it)
function Say([string]$text) { $line = "$(Get-Date -Format HH:mm:ss) $text"; Write-Host $line; Add-Content $log $line }
function Guest([string]$path) { $f = [IO.Path]::GetFullPath($path); '/Volumes/' + $f.Substring(0, 1).ToUpperInvariant() + ($f.Substring(2) -replace '\\', '/') }

# ---- the two sides --------------------------------------------------------------------------------------------
function Freeze([string]$spec, [string]$label, [hashtable]$sideEnv) {
    $dir = Join-Path $Out "bin-$label"
    New-Item -ItemType Directory -Force $dir | Out-Null
    if (Test-Path $spec -PathType Container) {
        $release = Join-Path $spec 'Release'
        $exe = Join-Path $release 'alchemy-fx.exe'
        foreach ($f in 'alchemy-fx.exe', 'alchemy-fx.pdb', 'fx_translator_tests.exe') {
            if (Test-Path (Join-Path $release $f)) { Copy-Item -Force (Join-Path $release $f) $dir }
        }
        $rev = if (Test-Path (Join-Path $spec '.s3-last-built-rev')) { (Get-Content (Join-Path $spec '.s3-last-built-rev') -TotalCount 1).Trim() } else { '' }
    } else {
        $exe = $spec
        Copy-Item -Force $spec $dir
        foreach ($f in 'alchemy-fx.pdb', 'fx_translator_tests.exe') {   # next to the exe, if there
            $sibling = Join-Path (Split-Path -Parent $spec) $f
            if (Test-Path $sibling) { Copy-Item -Force $sibling $dir }
        }
        $rev = ''
    }
    if (-not (Test-Path $exe)) { throw "no alchemy-fx.exe for $label ($spec)" }
    $frozen = Join-Path $dir 'alchemy-fx.exe'
    $sha = (Get-FileHash $frozen -Algorithm SHA256).Hash
    $tests = Join-Path $dir 'fx_translator_tests.exe'
    [pscustomobject]@{ label = $label; exe = $frozen; tests = $(if (Test-Path $tests) { $tests } else { '' }); sha = $sha; rev = $rev
        cache = (Join-Path $Out "cache-$label"); env = $sideEnv }
}
if ($LabelA -eq $LabelB) { throw 'the labels must differ' }
$sides = @((Freeze $A $LabelA $EnvA), (Freeze $B $LabelB $EnvB))
foreach ($s in $sides) {
    New-Item -ItemType Directory -Force $s.cache | Out-Null
    Say "side $($s.label): $($s.exe) sha256 $($s.sha.Substring(0, 16))... rev $($s.rev) env $(($s.env.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ')"
}
$samplesCsv = Join-Path $Out 'samples.csv'
if (-not (Test-Path $samplesCsv)) { 'round,side,bench,metric,value,unit,better,load,status' | Set-Content $samplesCsv }

# ---- quiet machine and the perf lock -----------------------------------------------------------------------
$lockFile = Join-Path $LockDir 'perf.lock'
New-Item -ItemType Directory -Force $LockDir | Out-Null
# -NoQuietWait (measuring on a busy machine anyway) takes no lock: it would only hold builds back. A lock held by
# another live process (a window.ps1 window) is left alone: the window already holds builds back.
function Test-ForeignLock {
    if (-not (Test-Path -LiteralPath $lockFile)) { return $false }
    $owner = ((Get-Content $lockFile -TotalCount 1 -ErrorAction SilentlyContinue) -split ' ')[0]
    return $owner -and $owner -ne "$PID" -and [bool](Get-Process -Id $owner -ErrorAction SilentlyContinue)
}
function Set-PerfLock { if (-not $NoQuietWait -and -not (Test-ForeignLock)) { "$PID $(Get-Date -Format o) $Out" | Set-Content -LiteralPath $lockFile } }
function Clear-PerfLock {
    try {
        if ((Test-Path -LiteralPath $lockFile) -and ((Get-Content -LiteralPath $lockFile -TotalCount 1) -like "$PID *")) { Remove-Item -LiteralPath $lockFile -Force }
    } catch { Say "  (perf lock not removed: $_)" }
}
# WMI queries can fail transiently ("call cancelled"): a failed sample reads as busy, never ends the run.
function Get-Load { try { [math]::Round((Get-CimInstance Win32_Processor -ErrorAction Stop | Measure-Object -Property LoadPercentage -Average).Average) } catch { 100 } }
function Get-Builders {
    # Compilers and linkers, and guest app runs of other sessions (no alchemy-fx of this script is running here).
    try {
        @(Get-CimInstance Win32_Process -ErrorAction Stop -Filter "Name='cl.exe' OR Name='link.exe' OR Name='ninja.exe' OR Name='lld-link.exe' OR Name='MSBuild.exe' OR Name='alchemy-fx.exe' OR Name='transmute-fx.exe'").Count
    } catch { 99 }
}
function Wait-Quiet {
    if ($NoQuietWait) { return "$(Get-Load)" }
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        $builders = Get-Builders
        $loads = @(1..3 | ForEach-Object { Start-Sleep -Seconds 2; Get-Load })
        $max = ($loads | Measure-Object -Maximum).Maximum
        if ($builders -eq 0 -and $max -lt $MaxLoad) { return ($loads -join '/') }
        if ($clock.Elapsed.TotalMinutes -ge $QuietMinutes) { Say "  not quiet after $QuietMinutes min ($builders build or guest processes, load $($loads -join '/')): measuring anyway"; return "busy:$($loads -join '/')" }
        Start-Sleep -Seconds 20
    }
}

# ---- process runner ---------------------------------------------------------------------------------------
# Runs one process with extra environment, optional CPU affinity, high priority and QoS; returns its output, exit
# code, wall and CPU seconds. -Until: a regex; once the output matches it, waits -After seconds and ends the process
# tree (only processes this script started). Output goes to files so it can be watched while the process runs.
function Stop-Tree([int]$id) {
    $children = try { @(Get-CimInstance Win32_Process -ErrorAction Stop -Filter "ParentProcessId=$id") } catch { @() }
    foreach ($c in $children) { Stop-Tree $c.ProcessId }
    Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
}
function Invoke-Measured([string]$exe, [string]$arguments, [hashtable]$environment, [string]$logBase, [int]$timeout = 600,
                         [long]$affinity = 0, [string]$until = '', [int]$after = 0) {
    $base = @{ ALCHEMY_GUI_CLOAK = '1'; ALCHEMY_GUI_NO_ACTIVATE = '1'; ALCHEMY_GUI_NO_CRASH_DIALOG = '1'; ALCHEMY_NETWORK = 'loopback' }
    foreach ($k in $environment.Keys) { $base[$k] = $environment[$k] }
    foreach ($k in $script:sideEnv.Keys) { $base[$k] = $script:sideEnv[$k] }   # the side's -EnvA/-EnvB
    $saved = @{}
    foreach ($k in $base.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $base[$k]) }
    try {
        $outFile = "$logBase.out.txt"; $errFile = "$logBase.err.txt"
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $p = Start-Process -FilePath $exe -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
        $null = $p.Handle
        try { if ($affinity) { $p.ProcessorAffinity = [IntPtr]$affinity }; Set-FxBenchChildControl $p } catch { Say "  (process controls: $_)" }
        $ended = $false; $matchedAt = $null; $killed = $false
        while (-not $ended) {
            $ended = $p.WaitForExit(500)
            if ($ended) { break }
            if ($until -and -not $matchedAt) {
                $text = (Get-Content -Raw $errFile -ErrorAction SilentlyContinue) + (Get-Content -Raw $outFile -ErrorAction SilentlyContinue)
                if ($text -and $text -match $until) { $matchedAt = $clock.Elapsed.TotalSeconds }
            }
            if (($matchedAt -and $clock.Elapsed.TotalSeconds -ge $matchedAt + $after) -or $clock.Elapsed.TotalSeconds -ge $timeout) {
                $killed = $true; Stop-Tree $p.Id; $p.WaitForExit(15000) | Out-Null; break
            }
        }
        $p.WaitForExit()
        $cpu = try { $p.TotalProcessorTime.TotalSeconds } catch { -1 }
        $text = (Get-Content -Raw $outFile -ErrorAction SilentlyContinue) + "`n" + (Get-Content -Raw $errFile -ErrorAction SilentlyContinue)
        [pscustomobject]@{ exit = $(if ($killed) { 'killed' } else { $p.ExitCode }); matched = [bool]$matchedAt
            wall = $clock.Elapsed.TotalSeconds; cpu = $cpu; text = $text }
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
}
function Sample([string]$metric, $value, [string]$unit, [string]$better, [string]$status = 'ok') {
    [pscustomobject]@{ metric = $metric; value = $value; unit = $unit; better = $better; status = $status }
}

# ---- the native reference for micro ------------------------------------------------------------------------
# The native x64 build of the fixture gives the reference checksums (once, before the rounds) and a reference time
# per round (interleaved with the two sides, like them: a single run at the start is at the mercy of that moment).
$native = @{}
function Invoke-Native([int]$round) {
    $r = Invoke-Measured (Join-Path $Fixtures 'fxbench_native.exe') "-reps $MicroReps -scale $MicroScale" @{} (Join-Path $Out "native-micro-r$round") 900 0x5
    $n = 0
    foreach ($m in [regex]::Matches($r.text, 'FXBENCH kernel=(\S+) iters=\d+ reps=\d+ median_ps=(\d+) min_ps=\d+ checksum=([0-9a-f]+)')) {
        $k = $m.Groups[1].Value; $n++
        if (-not $native.ContainsKey($k)) { $native[$k] = @{ checksum = $m.Groups[3].Value } }
        elseif ($native[$k].checksum -ne $m.Groups[3].Value) { Say "  native checksum of $k changed: $($m.Groups[3].Value) vs $($native[$k].checksum)" }
        if ($round -gt 0) { "$round,native,micro,$k,$([math]::Round([double]$m.Groups[2].Value / 1000, 3)),ns/iter,lower,,ok" | Add-Content $samplesCsv }
    }
    $n
}
if ($benches -contains 'micro') {
    foreach ($f in 'fxbench', 'fxbench_llsc', 'fxbench_native.exe') { if (-not (Test-Path (Join-Path $Fixtures $f))) { throw "missing fixture $f in $Fixtures (run scripts/perf/build_fixtures.ps1)" } }
    $count = Invoke-Native 0
    if ($count -lt 10) { throw "native fxbench gave $count kernels" }
    Say "native reference: $count kernels"
}

# ---- benchmarks ------------------------------------------------------------------------------------------------
function Bench-Micro($side, [string]$dir) {
    $env = @{ TRANSMUTE_FX_CACHE_DIR = $side.cache }
    $results = @()
    foreach ($v in @(@{ fixture = 'fxbench'; prefix = ''; extra = '' }, @{ fixture = 'fxbench_llsc'; prefix = 'llsc.'; extra = ' -only atomics,atomics_contended' })) {
        $r = Invoke-Measured $side.exe "`"$(Join-Path $Fixtures $v.fixture)`" -reps $MicroReps -scale $MicroScale$($v.extra)" $env (Join-Path $dir $v.fixture) 1800 0x5
        $n = 0
        foreach ($m in [regex]::Matches($r.text, 'FXBENCH kernel=(\S+) iters=\d+ reps=\d+ median_ps=(\d+) min_ps=\d+ checksum=([0-9a-f]+)')) {
            $k = $m.Groups[1].Value; $n++
            $ok = $native.ContainsKey($k) -and $native[$k].checksum -eq $m.Groups[3].Value
            if (-not $ok) { Say "  CHECKSUM MISMATCH $($side.label) $($v.prefix)$k`: $($m.Groups[3].Value) vs native $($native[$k].checksum)" }
            $results += Sample "$($v.prefix)$k" ([math]::Round([double]$m.Groups[2].Value / 1000, 3)) 'ns/iter' 'lower' $(if ($ok) { 'ok' } else { 'checksum' })
        }
        if ($r.exit -ne 0 -or $n -eq 0) { $results += Sample "$($v.fixture).exit" 1 'fail' 'lower' 'fail'; Say "  $($v.fixture) exit $($r.exit) ($n kernels)" }
    }
    $results
}
function Bench-FxUnit($side, [string]$dir) {
    if (-not $side.tests) { Say "  fxunit: no fx_translator_tests.exe for $($side.label)"; return @() }
    $r = Invoke-Measured $side.tests '--filter core_perf_' @{} (Join-Path $dir 'fxunit') 900
    $results = @()
    foreach ($p in @(@{ re = 'LCG loop: native [0-9.]+ ns/iter, translated ([0-9.]+) ns/iter'; m = 'lcg_loop' },
                     @{ re = 'memory loop: native [0-9.]+ ns/iter, translated ([0-9.]+) ns/iter'; m = 'memory_loop' },
                     @{ re = 'call loop: native [0-9.]+ ns/call, translated ([0-9.]+) ns/call'; m = 'call_loop' },
                     @{ re = 'translate_region: ([0-9]+) thread cycles per 12-instruction region'; m = 'translate_cycles_12' })) {
        $m = [regex]::Match($r.text, $p.re)
        if ($m.Success) { $results += Sample $p.m ([double]$m.Groups[1].Value) $(if ($p.m -like 'translate*') { 'cycles' } else { 'ns/iter' }) 'lower' }
    }
    if ($r.exit -ne 0) { $results += Sample 'fxunit.exit' 1 'fail' 'lower' 'fail' }
    $results
}
function Blender-Args([string]$expr) {
    "--runtime-dir `"$llvm`" `"$blenderApp`" -b --factory-startup -noaudio --python-expr `"$expr`""
}
function Bench-Blender($side, [string]$dir) {
    $user = Join-Path $Out 'blender-user'; New-Item -ItemType Directory -Force $user | Out-Null
    $env = @{ TRANSMUTE_FX_CACHE_DIR = $side.cache; BLENDER_USER_RESOURCES = (Guest $user) }
    $r = Invoke-Measured $side.exe (Blender-Args 'import bpy; bpy.ops.mesh.primitive_monkey_add()') $env (Join-Path $dir 'blender') 600
    @((Sample 'cpu_s' ([math]::Round($r.cpu, 3)) 's' 'lower' $(if ($r.exit -eq 0) { 'ok' } else { 'fail' })),
      (Sample 'wall_s' ([math]::Round($r.wall, 3)) 's' 'lower' $(if ($r.exit -eq 0) { 'ok' } else { 'fail' })))
}
function Bench-Cycles($side, [string]$dir) {
    $user = Join-Path $Out 'blender-user'; New-Item -ItemType Directory -Force $user | Out-Null
    $env = @{ TRANSMUTE_FX_CACHE_DIR = $side.cache; BLENDER_USER_RESOURCES = (Guest $user) }
    $expr = "import bpy, time; s = bpy.context.scene; s.render.engine = 'CYCLES'; s.cycles.device = 'CPU'; s.cycles.samples = 64; " +
            "s.cycles.use_denoising = False; s.render.resolution_x = 640; s.render.resolution_y = 360; s.render.resolution_percentage = 100; " +
            "s.render.threads_mode = 'FIXED'; s.render.threads = 2; t = time.perf_counter(); bpy.ops.render.render(); " +
            "print('FXPERF render_s=%.3f' % (time.perf_counter() - t)); px = bpy.data.images['Render Result']; print('FXPERF done')"
    $r = Invoke-Measured $side.exe (Blender-Args $expr) $env (Join-Path $dir 'cycles') 900 0x5
    $m = [regex]::Match($r.text, 'FXPERF render_s=([0-9.]+)')
    if (-not $m.Success -or $r.exit -ne 0) { Say "  cycles $($side.label): exit $($r.exit), no render time"; return @(Sample 'render_s' -1 's' 'lower' 'fail') }
    @((Sample 'render_s' ([double]$m.Groups[1].Value) 's' 'lower'), (Sample 'cpu_s' ([math]::Round($r.cpu, 3)) 's' 'lower'))
}
function Copy-Root([string]$name) {
    $dst = Join-Path $Out "roots\$name"
    if (-not (Test-Path (Join-Path $dst '.alchemy-root'))) {
        Say "copying $name root (once)"
        & robocopy.exe (Join-Path $Artifacts $name) $dst /MIR /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "robocopy $name failed ($LASTEXITCODE)" }
    }
    $dst
}
function Bench-Plugin($side, [string]$dir) {
    if (-not $PluginRoot -or -not $PluginScript) { throw 'plugin: pass -PluginRoot and -PluginScript (or set ALCHEMY_PERF_PLUGIN_ROOT and _SCRIPT)' }
    $root = Copy-Root $PluginRoot
    $blender = Join-Path $blenderApp 'Contents\MacOS\Blender'
    $r = Invoke-Measured $side.exe "--runtime-dir `"$llvm`" --runtime-dir `"$expat`" --root `"$root`" `"$blender`" -b -noaudio --python $PluginScript" `
        @{ TRANSMUTE_FX_CACHE_DIR = $side.cache; HOME = "/Users/$PluginUser" } (Join-Path $dir 'plugin') 400
    # The cube script prints "<TAG>: enabled ... in <s> s", "render returned in <s> s", and "<TAG>: FAILED" on failure.
    $render = [regex]::Match($r.text, 'render returned in ([0-9.]+) s')
    $enabled = [regex]::Match($r.text, '[A-Z]+: enabled .* in ([0-9.]+) s')
    $ok = $render.Success -and -not ($r.text -match '[A-Z]+: FAILED')
    @((Sample 'render_s' $(if ($render.Success) { [double]$render.Groups[1].Value } else { -1 }) 's' 'lower' $(if ($ok) { 'ok' } else { 'fail' })),
      (Sample 'enable_s' $(if ($enabled.Success) { [double]$enabled.Groups[1].Value } else { -1 }) 's' 'lower' $(if ($enabled.Success) { 'ok' } else { 'fail' })))
}
function Bench-Qlab($side, [string]$dir) {
    $root = Copy-Root 'alchemy-root'
    $user = "fxperf-$($side.label)"
    New-Item -ItemType Directory -Force (Join-Path $root "Users\$user") | Out-Null
    # Cloaked, QLab presents no frame for ALCHEMY_PERF's first-frame table; its windows' contents are captured
    # (ALCHEMY_CONTENTS_CAPTURE), so the metric is the time from launch to the first captured window (the welcome
    # window), as the wave's earlier QLab checks measured it. It runs 45 s, then its process tree is ended.
    $capture = Join-Path $dir 'capture'
    Remove-Item -Recurse -Force $capture -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $capture | Out-Null
    $launched = Get-Date
    $r = Invoke-Measured $side.exe "--runtime-dir `"$llvm`" --runtime-dir `"$swift`" --root `"$root`" /Applications/QLab.app" `
        @{ TRANSMUTE_FX_CACHE_DIR = $side.cache; HOME = "/Users/$user"; ALCHEMY_CONTENTS_CAPTURE = $capture } (Join-Path $dir 'qlab') 45
    $first = Get-ChildItem $capture -Filter *.bmp -ErrorAction SilentlyContinue | Sort-Object CreationTime | Select-Object -First 1
    $windows = @(Get-ChildItem $capture -Filter *.bmp -ErrorAction SilentlyContinue).Count
    $ok = [bool]$first -and $r.text -notmatch 'alchemy: stopping'
    @((Sample 'first_window_s' $(if ($first) { [math]::Round(($first.CreationTime - $launched).TotalSeconds, 2) } else { -1 }) 's' 'lower' $(if ($ok) { 'ok' } else { 'fail' })),
      (Sample 'windows_captured' $windows 'count' 'higher' $(if ($windows) { 'ok' } else { 'fail' })))
}
function Invoke-Chrome($side, [string]$dir, [string]$url, [string]$action, [bool]$jit, [bool]$swiftshader, [int]$play = 5) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    $arguments = @{ Exe = $side.exe; OutDir = $dir; Url = @($url); Name = @($action); Action = @($action); Settle = 2; NoScreenshot = $true
        LoadTimeout = 300; Root = (Join-Path $Artifacts 'alchemy-root-chrome'); RuntimeDir = $llvm; User = 'fxperf'; Jit = $jit
        SwiftShader = $swiftshader; Play = $play; Env = @{ TRANSMUTE_FX_CACHE_DIR = $side.cache } }
    foreach ($k in $side.env.Keys) { $arguments.Env[$k] = $side.env[$k] }
    $script = Join-Path $checkout 'scripts\chrome\web_visit.ps1'
    $saved = $ErrorActionPreference
    try { $ErrorActionPreference = 'Continue'; & $script @arguments *> (Join-Path $dir 'web_visit.log') } catch { Say "  web_visit: $_" }
    finally { $ErrorActionPreference = $saved }
    $json = Join-Path $dir 'results.json'
    if (-not (Test-Path $json)) { return $null }
    $site = @((Get-Content -Raw $json | ConvertFrom-Json).sites)[0]
    if (-not $site.action_result) { return $null }
    if ($site.action_result -is [string]) { try { $site.action_result | ConvertFrom-Json } catch { $null } }
    else { $site.action_result }   # web_visit stores a JSON result already parsed
}
function Bench-Speedometer($side, [string]$dir, [bool]$jit) {
    $url = 'https://browserbench.org/Speedometer3.0/' + $(if ($SpeedometerIterations -gt 0) { "?iterationCount=$SpeedometerIterations" })
    $res = Invoke-Chrome $side $dir $url 'speedometer' $jit $false
    if (-not $res -or -not $res.PSObject.Properties['score'] -or "$($res.score)" -notmatch '^[0-9.]+$') { return @(Sample 'score' -1 'score' 'higher' 'fail') }
    @(Sample 'score' ([double]$res.score) 'score' 'higher')
}
function Bench-JetStream($side, [string]$dir, [bool]$jit) {
    $results = @(); $logs = @()
    foreach ($t in ($JetStreamTests -split ',')) {
        $res = Invoke-Chrome $side (Join-Path $dir $t) "https://browserbench.org/JetStream2.2/?test=$t" 'jetstream' $jit $false
        $v = $null
        if ($res -and $res.PSObject.Properties['scores']) {
            $p = $res.scores.PSObject.Properties[$t]
            if ($p -and $null -ne $p.Value -and "$($p.Value)" -match '^[0-9.]+$') { $v = [double]$p.Value }
        }
        if (-not $v -and $res -and $res.PSObject.Properties['score'] -and "$($res.score)" -match '^[0-9.]+$') { $v = [double]$res.score }
        if ($v -and $v -gt 0) { $results += Sample $t $v 'score' 'higher'; $logs += [math]::Log($v) } else { $results += Sample $t -1 'score' 'higher' 'fail' }
    }
    if ($logs.Count -eq ($JetStreamTests -split ',').Count) { $results += Sample 'geomean' ([math]::Round([math]::Exp(($logs | Measure-Object -Average).Average), 3)) 'score' 'higher' }
    $results
}
function Bench-Fps($side, [string]$dir, [bool]$swiftshader) {
    $res = Invoke-Chrome $side $dir "file://$(Guest (Join-Path $here 'pages\raf_box.html'))" 'fps' $false $swiftshader 20
    if (-not $res -or -not $res.PSObject.Properties['fps']) { return @(Sample 'fps' -1 'fps' 'higher' 'fail') }
    @(Sample 'fps' ([double]$res.fps) 'fps' 'higher')
}
function Invoke-Bench([string]$name, $side, [string]$dir) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    $script:sideEnv = $side.env
    switch ($name) {
        'micro' { Bench-Micro $side $dir }
        'fxunit' { Bench-FxUnit $side $dir }
        'blender' { Bench-Blender $side $dir }
        'cycles' { Bench-Cycles $side $dir }
        'plugin' { Bench-Plugin $side $dir }
        'qlab' { Bench-Qlab $side $dir }
        'sp-jitless' { Bench-Speedometer $side $dir $false }
        'sp-jit' { Bench-Speedometer $side $dir $true }
        'js-jitless' { Bench-JetStream $side $dir $false }
        'js-jit' { Bench-JetStream $side $dir $true }
        'fps' { Bench-Fps $side $dir $true }
        'fps-sw' { Bench-Fps $side $dir $false }
    }
}

# ---- rounds ----------------------------------------------------------------------------------------------------
$enter = Enter-FxBenchControl
try {
    $first = if ($NoWarmup) { 1 } else { 0 }
    for ($round = $first; $round -le $Rounds; $round++) {
        $order = if ($round % 2) { $sides } else { @($sides[1], $sides[0]) }
        foreach ($bench in $benches) {
            if ($bench -eq 'micro' -and $round -gt 0) { $null = Wait-Quiet; $null = Invoke-Native $round }
            foreach ($side in $order) {
                $load = Wait-Quiet
                Set-PerfLock
                $dir = Join-Path $Out "r$round\$($side.label)\$bench"
                $clock = [Diagnostics.Stopwatch]::StartNew()
                $samples = @(try { Invoke-Bench $bench $side $dir } catch { Say "  $bench $($side.label) error: $_"; Sample "$bench.error" 1 'fail' 'lower' 'fail' })
                Clear-PerfLock
                $bad = @($samples | Where-Object { $_.status -ne 'ok' }).Count
                Say ("round $round $($side.label) $bench`: $($samples.Count) metrics, $bad not ok, $([math]::Round($clock.Elapsed.TotalSeconds)) s, load $load" +
                     $(if ($round -eq 0) { ' (warm-up)' }))
                if ($round -gt 0) {
                    $samples | ForEach-Object { "$round,$($side.label),$bench,$($_.metric),$($_.value),$($_.unit),$($_.better),$load,$($_.status)" } | Add-Content $samplesCsv
                }
            }
        }
    }
} finally {
    Clear-PerfLock
    Exit-FxBenchControl $enter
}
& python -I (Join-Path $here 'summarize.py') $samplesCsv --a $LabelA --b $LabelB --out (Join-Path $Out 'summary.md')
Get-Content -Encoding utf8 (Join-Path $Out 'summary.md')
