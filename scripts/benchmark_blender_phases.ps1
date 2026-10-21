param(
    [Parameter(Mandatory = $true)] [string]$Blender,
    [Parameter(Mandatory = $true)]
    [ValidateSet('startup', 'render', 'save', 'whole')] [string]$Workload,
    [string]$TransmuteExecutable,
    [string[]]$DllDirectory = @(),
    [ValidateRange(1, 20)] [int]$Repeats = 3,
    [ValidateRange(1, 256)] [int]$Threads = 1,
    [ValidateRange(1, 4096)] [int]$Width = 16,
    [ValidateRange(1, 4096)] [int]$Height = 16,
    [ValidateRange(1, 1024)] [int]$Samples = 1,
    [switch]$VerifyImage,
    [long]$InstructionLimit = 16000000000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'benchmark_power.ps1')
if ($InstructionLimit -lt 1) { throw 'InstructionLimit must be positive' }
$blenderPath = (Resolve-Path -LiteralPath $Blender).Path
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$script = Join-Path $PSScriptRoot 'blender_phase_probe.py'
$label = if ($TransmuteExecutable) { 'arm64' } else { 'native' }
$prefix = Join-Path $repository ("artifacts/phase-{0}-{1}-{2}" -f
    $label, $Workload, (Get-Date -Format 'yyyyMMdd-HHmmss'))
$log = $prefix + '.log'
$arguments = @('--factory-startup', '-b', '-t', "$Threads", '--python-exit-code', '1',
    '--python', $script, '--', '--workload', $Workload, '--repeats', "$Repeats",
    '--width', "$Width", '--height', "$Height", '--samples', "$Samples", '--output', $prefix)
if ($VerifyImage) { $arguments += '--verify-image' }
$runtime = if ($TransmuteExecutable) { (Resolve-Path -LiteralPath $TransmuteExecutable).Path } else { $null }
$runtimeArguments = @('--jit', '--instruction-limit', "$InstructionLimit")
foreach ($directory in $DllDirectory) {
    $runtimeArguments += @('--dll-dir', (Resolve-Path -LiteralPath $directory).Path)
}
$started = [DateTime]::UtcNow
$powerBefore = Get-ProbePower
$processStartedUnix = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() / 1000.0
$watch = [Diagnostics.Stopwatch]::StartNew()
if ($runtime) { & $runtime @runtimeArguments $blenderPath @arguments *> $log }
else { & $blenderPath @arguments *> $log }
$code = $LASTEXITCODE
$watch.Stop()
$processFinishedUnix = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() / 1000.0
if ($code -ne 0) { throw "Phase probe exited with $code. See $log" }
$markers = @(Get-Content -LiteralPath $log | Where-Object {
    $_.StartsWith('TRANSMUTE_PHASE ')
} | ForEach-Object { $_.Substring(16) | ConvertFrom-Json })
if (-not ($markers | Where-Object { $_.event -eq 'finished' })) {
    throw "Phase probe produced no completion marker. See $log"
}
$ready = $markers | Where-Object { $_.event -eq 'ready' } | Select-Object -First 1
$finished = $markers | Where-Object { $_.event -eq 'finished' } | Select-Object -Last 1
foreach ($complete in @($markers | Where-Object { $_.event -in @('complete', 'verification') })) {
    if ($complete.image -and
        (-not (Test-Path -LiteralPath $complete.image) -or
         (Get-Item -LiteralPath $complete.image).LastWriteTimeUtc -lt $started)) {
        throw "Missing fresh phase output: $($complete.image)"
    }
}
$record = [pscustomobject]@{
    workload = $Workload; repeats = $Repeats; threads = $Threads
    width = $Width; height = $Height; samples = $Samples
    blender = $blenderPath; blender_sha256 = (Get-FileHash -LiteralPath $blenderPath).Hash
    transmute = $runtime
    transmute_sha256 = if ($runtime) { (Get-FileHash -LiteralPath $runtime).Hash } else { $null }
    wall_seconds = $watch.Elapsed.TotalSeconds
    before_ready_seconds = $ready.wall_unix - $processStartedUnix
    after_finished_seconds = $processFinishedUnix - $finished.wall_unix
    power_before = $powerBefore; power_after = Get-ProbePower
    markers = $markers; log = $log
}
$record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath ($prefix + '.json')
$record
