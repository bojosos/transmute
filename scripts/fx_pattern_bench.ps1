# Compile and compare Classroom-inspired patterns. Results are per timed operation.
param(
    [string]$Fx = "$PSScriptRoot/../build-fx-investigate/Release/transmute-fx.exe",
    [string]$BaselineFx = '',
    [string]$Core = '',
    [string]$BaselineCore = '',    # TRANSMUTE_FX_CORE for the baseline mode (same-binary A/B, e.g. notier2)
    [string]$Neon = '',
    [string]$TierThreshold = '',   # TRANSMUTE_FX_TIER_THRESHOLD for the fx mode (1: kernels tier up on their second call)
    [string]$OutDir = "$PSScriptRoot/../artifacts/fx-investigate/patterns",
    [int]$Runs = 5,
    [long]$Affinity = 1,
    [switch]$NoBuild,
    [switch]$BuildOnly,
    [double]$MaxRatio = 0
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/fx_toolchain.ps1"
. "$PSScriptRoot/fx_bench_control.ps1"
$tc = Get-FxToolchain
$source = Join-Path (Get-FxRepoRoot) 'tests/fx/bench/bench_patterns.c'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
if (-not $NoBuild) {
    foreach ($arch in 'arm64', 'x64') {
        $flags = @("--target=$($tc.Targets[$arch])", '/nologo', '/c', '/O2', '/GS-', '/Zl', '/Z7',
            '/clang:-ffp-contract=off', '/clang:-fno-vectorize', '/clang:-fno-slp-vectorize',
            "/Fo$OutDir/$arch.obj", $source)
        if ($arch -eq 'x64') { $flags += '/arch:AVX2' }
        if (-not (Invoke-FxTool $tc.ClangCl $flags "patterns $arch")) { throw 'compile failed' }
        $link = @('/nologo', "/machine:$arch", '/entry:guest_entry', '/subsystem:console', '/nodefaultlib',
            '/debug', '/incremental:no', "/out:$OutDir/$arch.exe", "$OutDir/$arch.obj", "$($tc.SdkUm[$arch])/kernel32.lib")
        if (-not (Invoke-FxTool $tc.LldLink $link "patterns $arch")) { throw 'link failed' }
    }
}
if ($BuildOnly) { Write-Host "Built ARM64 and x64 patterns in $OutDir"; return }
$rows = @()
$self = [Diagnostics.Process]::GetCurrentProcess()
$oldAffinity = $self.ProcessorAffinity
$scheduling = Enter-FxBenchControl
try {
    $self.ProcessorAffinity = [IntPtr]$Affinity
    for ($run = -1; $run -lt $Runs; ++$run) {
        $order = if ($BaselineFx) { @('native', 'baseline', 'fx') } else { @('native', 'fx') }
        if ($run % 2) { [array]::Reverse($order) }
        foreach ($mode in $order) {
            $psi = New-Object Diagnostics.ProcessStartInfo
            $psi.FileName = if ($mode -eq 'native') { "$OutDir/x64.exe" } elseif ($mode -eq 'baseline') { $BaselineFx } else { $Fx }
            if ($mode -ne 'native') { $psi.Arguments = '"' + "$OutDir/arm64.exe" + '"' }
            foreach ($key in @($psi.EnvironmentVariables.Keys | Where-Object { $_ -like 'TRANSMUTE_FX*' })) { $psi.EnvironmentVariables.Remove($key) }
            $psi.EnvironmentVariables['TRANSMUTE_FX_CACHE_DIR'] = "$OutDir/cache-$mode"
            $psi.EnvironmentVariables['TRANSMUTE_FX_BACKGROUND_AOT'] = '0'
            if ($mode -eq 'baseline' -and $BaselineCore) { $psi.EnvironmentVariables['TRANSMUTE_FX_CORE'] = $BaselineCore }
            if ($mode -eq 'fx') {
                if ($Core) { $psi.EnvironmentVariables['TRANSMUTE_FX_CORE'] = $Core }
                if ($Neon) { $psi.EnvironmentVariables['TRANSMUTE_FX_NEON'] = $Neon }
                if ($TierThreshold) { $psi.EnvironmentVariables['TRANSMUTE_FX_TIER_THRESHOLD'] = $TierThreshold }
            }
            $psi.UseShellExecute = $false
            $psi.RedirectStandardOutput = $true
            $psi.RedirectStandardError = $true
            $process = [Diagnostics.Process]::Start($psi)
            Set-FxBenchChildControl $process
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            $process.WaitForExit()
            if ($process.ExitCode -ne 0) { throw "$mode failed with $($process.ExitCode): $($stderr.Result)" }
            $output = $stdout.Result -split "`r?`n"
            $output | Set-Content "$OutDir/$mode-$run.txt"
            $found = 0
            foreach ($line in $output) {
                if ($line -match '^BENCH (\S+) ops=(\d+) ns=(\d+) check=(\S+) cycles=(\d+)') {
                    if ($run -ge 0) { $rows += [pscustomobject]@{ mode=$mode; run=$run; name=$Matches[1]; ops=[long]$Matches[2]; ns=[long]$Matches[3]; check=$Matches[4]; cycles=[long]$Matches[5]; ns_op=[double]$Matches[3]/[double]$Matches[2]; cycles_op=[double]$Matches[5]/[double]$Matches[2] } }
                    ++$found
                }
            }
            if ($found -ne 9) { throw "Expected 9 kernels, got $found" }
        }
    }
} finally { $self.ProcessorAffinity = $oldAffinity; Exit-FxBenchControl $scheduling }
$rows | ConvertTo-Json -Depth 4 | Set-Content "$OutDir/raw.json"
function Median($values) {
    $v = @($values | Sort-Object); $n = $v.Count
    if ($n % 2) { return $v[[int][math]::Floor($n/2)] }
    return ($v[$n/2 - 1] + $v[$n/2]) / 2
}
$summary = foreach ($name in ($rows.name | Select-Object -Unique)) {
    $r = @($rows | Where-Object name -eq $name)
    if (@($r.check | Select-Object -Unique).Count -ne 1) { throw "Checksum mismatch: $name" }
    $native = Median @($r | Where-Object mode -eq native | ForEach-Object ns_op)
    $fxTime = Median @($r | Where-Object mode -eq fx | ForEach-Object ns_op)
    $ratios = foreach ($iteration in 0..($Runs - 1)) {
        $pair = @($r | Where-Object run -eq $iteration)
        ($pair | Where-Object mode -eq fx).ns_op / ($pair | Where-Object mode -eq native).ns_op
    }
    $cycleRatios = foreach ($iteration in 0..($Runs - 1)) {
        $pair = @($r | Where-Object run -eq $iteration)
        ($pair | Where-Object mode -eq fx).cycles_op / ($pair | Where-Object mode -eq native).cycles_op
    }
    $baselineRatios = if ($BaselineFx) { foreach ($iteration in 0..($Runs - 1)) {
        $pair = @($r | Where-Object run -eq $iteration)
        ($pair | Where-Object mode -eq fx).cycles_op / ($pair | Where-Object mode -eq baseline).cycles_op
    } }
    [pscustomobject]@{ name=$name; native_ns=$native; fx_ns=$fxTime; ratio=(Median $ratios); min_ratio=($ratios | Measure-Object -Minimum).Minimum; max_ratio=($ratios | Measure-Object -Maximum).Maximum; cycle_ratio=(Median $cycleRatios); min_cycle_ratio=($cycleRatios | Measure-Object -Minimum).Minimum; max_cycle_ratio=($cycleRatios | Measure-Object -Maximum).Maximum; baseline_cycle_ratio=$(if ($BaselineFx) { Median $baselineRatios }); min_baseline_cycle_ratio=$(if ($BaselineFx) { ($baselineRatios | Measure-Object -Minimum).Minimum }); max_baseline_cycle_ratio=$(if ($BaselineFx) { ($baselineRatios | Measure-Object -Maximum).Maximum }); check=$r[0].check }
}
$summary | ConvertTo-Json | Set-Content "$OutDir/summary.json"
@{ affinity=$Affinity; runs=$Runs; qos='high'; priority='high'; fx=(Resolve-Path $Fx).Path; fx_sha256=(Get-FileHash $Fx).Hash;
    baseline=$BaselineFx; baseline_sha256=$(if ($BaselineFx) { (Get-FileHash $BaselineFx).Hash });
    source_sha256=(Get-FileHash $source).Hash; core=$Core; baseline_core=$BaselineCore; neon=$Neon } | ConvertTo-Json -Depth 4 | Set-Content "$OutDir/conditions.json"
$summary | Format-Table name,native_ns,fx_ns,ratio,cycle_ratio,baseline_cycle_ratio -AutoSize
if ($MaxRatio -gt 0 -and @($summary | Where-Object ratio -gt $MaxRatio).Count) { exit 2 }
