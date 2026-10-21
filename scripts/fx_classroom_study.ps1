# Serial, alternating Classroom comparisons. Do not run alongside other benchmarks.
param(
    [string]$Fx = "$PSScriptRoot/../build-fx-investigate/Release/transmute-fx.exe",
    [string]$BaselineFx = '',
    [string]$OutDir = "$PSScriptRoot/../artifacts/fx-investigate/study",
    [string[]]$Configs = @('native', 'default', 'carrydirty', 'noinline'),
    [int]$Runs = 3,
    [int]$Percent = 10,
    [int]$Samples = 64,
    [int]$Threads = 1,
    [long]$Affinity = 1,
    [hashtable]$Variants = @{}
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/fx_bench_control.ps1"
$scheduling = Enter-FxBenchControl
$repo = (Resolve-Path "$PSScriptRoot/..").Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$rows = @()
$savedCore = $env:TRANSMUTE_FX_CORE
$savedCache = $env:TRANSMUTE_FX_CACHE_DIR
$savedStats = $env:TRANSMUTE_FX_STATS
$savedNeon = $env:TRANSMUTE_FX_NEON
$env:TRANSMUTE_FX_STATS = '1'
try {
    # Iteration -1 warms each translator configuration and is excluded from summaries.
    for ($run = -1; $run -lt $Runs; ++$run) {
        $order = @($Configs)
        if ($run % 2 -eq 1) { [array]::Reverse($order) }
        foreach ($config in $order) {
            if ($config -notin @('native', 'baseline', 'default', 'carrydirty', 'noinline', 'novecpins', 'oldpins') -and -not $Variants.ContainsKey($config)) { throw "Unknown configuration $config" }
            $mode = if ($config -eq 'native') { 'native' } else { 'fx' }
            $binary = if ($config -eq 'baseline') { $BaselineFx } else { $Fx }
            if ($config -eq 'baseline' -and -not $BaselineFx) { throw 'baseline requires -BaselineFx' }
            $env:TRANSMUTE_FX_CORE = if ($Variants.ContainsKey($config)) { $Variants[$config].Core } elseif ($config -in @('native', 'baseline', 'default')) { $null } else { $config }
            $env:TRANSMUTE_FX_NEON = if ($Variants.ContainsKey($config)) { $Variants[$config].Neon } else { $null }
            # Keep old and new code generation apart even before a cache-version bump.
            $env:TRANSMUTE_FX_CACHE_DIR = "$OutDir/cache-$config"
            $destination = "$OutDir/$config-$run"
            $summary = & "$PSScriptRoot/fx_classroom.ps1" -Fx $binary -Percent $Percent -Samples $Samples -Threads $Threads -Affinity $Affinity -Modes @($mode) -OutDir $destination -ControlledScheduling
            $summary | Tee-Object -FilePath "$destination/summary.txt" | Write-Host
            if ($summary -notmatch 'exit 0x0') { throw "Render failed: $config run $run" }
            $log = Get-Content "$destination/${mode}_p${Percent}_s${Samples}_t${Threads}_r0.log" -Raw
            if ($log -notmatch '(?m)^Time:\s+(\d+):(\d+\.\d+)') { throw 'Missing render time' }
            $render = [double]$Matches[1]*60 + [double]$Matches[2]
            $start = [regex]::Match($log, 'Time:(\d+):(\d+\.\d+).*Sample 0/')
            $end = [regex]::Match($log, "Time:(\d+):(\d+\.\d+).*Sample $Samples/$Samples")
            if (-not $start.Success -or -not $end.Success) { throw 'Missing sampling phase markers' }
            $trace = ([double]$end.Groups[1].Value*60+[double]$end.Groups[2].Value) - ([double]$start.Groups[1].Value*60+[double]$start.Groups[2].Value)
            $png = "$destination/${mode}_p${Percent}_s${Samples}_t${Threads}_r0_00001.png"
            if (-not (Test-Path $png)) { throw 'Missing render output' }
            if ($summary -notmatch 'cycles (\d+)') { throw 'Missing process cycle count' }
            $cycles = [uint64]$Matches[1]
            $rows += [pscustomobject]@{ config=$config; run=$run; render_s=$render; sampling_s=$trace; process_cycles=$cycles; png=$png }
            $rows | ConvertTo-Json | Set-Content "$OutDir/raw.json"
        }
    }
} finally {
    Exit-FxBenchControl $scheduling
    $env:TRANSMUTE_FX_CORE = $savedCore
    $env:TRANSMUTE_FX_CACHE_DIR = $savedCache
    $env:TRANSMUTE_FX_STATS = $savedStats
    $env:TRANSMUTE_FX_NEON = $savedNeon
}
@{ fx=(Resolve-Path $Fx).Path; fx_sha256=(Get-FileHash $Fx).Hash; percent=$Percent;
    baseline=$BaselineFx; baseline_sha256=$(if ($BaselineFx) { (Get-FileHash $BaselineFx).Hash });
    samples=$Samples; threads=$Threads; affinity=$Affinity; qos='high'; priority='high'; runs=$Runs; configs=$Configs; variants=$Variants } |
    ConvertTo-Json -Depth 4 | Set-Content "$OutDir/conditions.json"
$rows | Where-Object run -ge 0 | Format-Table config,run,render_s,sampling_s,process_cycles
