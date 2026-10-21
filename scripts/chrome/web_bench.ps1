# Browser benchmarks (browserbench.org's Speedometer 3, JetStream 2.2 and MotionMark 1.3) in Chrome under alchemy-fx
# and in a native Windows Chrome on the same machine, interleaved: for each run and benchmark, native first, then
# Alchemy (web_visit.ps1; Alchemy with V8's JIT unless -Jitless, and Chrome's software compositor (--disable-gpu), or
# with -SwiftShader the GPU process with ANGLE and Graphite on SwiftShader, which reaches about 6.5 frames per second
# under Alchemy against about 58 for the software compositor: ALCHEMY_CHROME_AUDIT.md). Before each,
# it waits (up to 20 min) for a quiet machine: three samples 5 s apart under -MaxCpu percent. The CPU load during
# each run is sampled every 10 s into <Out>\<side>-run<N>\<benchmark>-cpu.csv.
# Usage: web_bench.ps1 -Exe <alchemy-fx.exe> -Out <scratch dir> [-Which speedometer,jetstream,motionmark] [-Runs 3]
#          [-FirstRun 1] [-Native <chrome.exe>] [-NoNative] [-Jitless] [-SwiftShader] [-SpeedometerIterations N] [-MaxCpu 30]
# -SpeedometerIterations N runs Speedometer with ?iterationCount=N on both sides (its official score uses 10; under
# Alchemy with the JIT a renderer runs out of fx code arena after about two minutes of Speedometer: see the audit).
# Results: <Out>\<side>-run<N>\<benchmark>\results.json (sites[0].action_result).
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Out,
    [string]$Which = 'speedometer,jetstream,motionmark',
    [int]$Runs = 3,
    [int]$FirstRun = 1,
    [string]$Native = 'C:\Program Files\Google\Chrome\Application\chrome.exe',
    [switch]$NoNative,
    [switch]$Jitless,
    [switch]$SwiftShader,
    [int]$SpeedometerIterations = 0,
    [int]$MaxCpu = 30,
    [string]$Root = '',
    [string]$RuntimeDir = ''
)
# The checkout's artifacts by default ($PSScriptRoot is empty in parameter defaults under powershell -File).
$checkout = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Root) { $Root = Join-Path $checkout 'artifacts\alchemy-root-chrome' }
if (-not $RuntimeDir) { $RuntimeDir = Join-Path $checkout 'artifacts\alchemy-llvm-arm64' }
$ErrorActionPreference = 'Stop'
$urls = @{ speedometer = 'https://browserbench.org/Speedometer3.0/'; jetstream = 'https://browserbench.org/JetStream2.2/';
           motionmark = 'https://browserbench.org/MotionMark1.3/' }
if ($SpeedometerIterations -gt 0) { $urls.speedometer += "?iterationCount=$SpeedometerIterations" }
function Get-Load { (Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average }
for ($run = $FirstRun; $run -lt $FirstRun + $Runs; $run++) {
    foreach ($benchmark in ($Which -split ',')) {
        foreach ($side in @($(if (-not $NoNative) { 'native' }), 'alchemy')) {
            if (-not $side) { continue }
            $waited = [Diagnostics.Stopwatch]::StartNew()
            do { $samples = @(1..3 | ForEach-Object { Start-Sleep 5; Get-Load }) }
            while (($samples | Measure-Object -Maximum).Maximum -ge $MaxCpu -and $waited.Elapsed.TotalMinutes -lt 20)
            $dir = Join-Path $Out "$side-run$run"
            New-Item -ItemType Directory -Force $dir | Out-Null
            "=== $side $benchmark run $run $(Get-Date -Format HH:mm:ss) cpu before: $($samples -join '/') (waited $([math]::Round($waited.Elapsed.TotalMinutes, 1)) min)"
            $arguments = @{ Exe = $Exe; OutDir = (Join-Path $dir $benchmark); Url = @($urls[$benchmark]); Name = @($benchmark);
                Action = @($benchmark); Settle = 2; NoScreenshot = $true; LoadTimeout = 300; Root = $Root; RuntimeDir = $RuntimeDir;
                Jit = (-not $Jitless) }
            if ($side -eq 'native') { $arguments.Native = $Native } elseif ($SwiftShader) { $arguments.SwiftShader = $true }
            $cpuLog = Join-Path $dir "$benchmark-cpu.csv"
            $sampler = Start-Job -ScriptBlock { param($file) while ($true) {
                "$(Get-Date -Format HH:mm:ss),$((Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average)" | Add-Content $file
                Start-Sleep 10 } } -ArgumentList $cpuLog
            try { & (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'web_visit.ps1') @arguments 2>&1 | Select-Object -Last 4 }
            finally { Stop-Job $sampler; Remove-Job $sampler }
        }
    }
}
