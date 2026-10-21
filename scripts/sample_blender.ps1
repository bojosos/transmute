param(
    [Parameter(Mandatory = $true)] [int]$ProcessId,
    [Parameter(Mandatory = $true)] [string]$ProbeLog,
    [int]$Windows = 16,
    [int]$Attempts = 400,
    [int]$IntervalMilliseconds = 10,
    [string]$OutputPrefix = 'artifacts/blender-current-cpu'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'benchmark_power.ps1')
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$sampler = Join-Path $repository 'build/RelWithDebInfo/transmute_debug_host_crash.exe'
if ($Windows -lt 1 -or $Attempts -lt 1 -or $IntervalMilliseconds -lt 1) {
    throw 'Sampling counts and interval must be positive'
}
$powerBefore = Get-ProbePower
$records = @()

function Get-SampleBoundary {
    $target = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if (-not $target) { return $null }
    $threads = @($target.Threads | ForEach-Object {
        try {
            [pscustomobject]@{
                id = $_.Id
                cpu_seconds = $_.TotalProcessorTime.TotalSeconds
                state = [string]$_.ThreadState
            }
        } catch { }
    })
    $lastLine = if (Test-Path -LiteralPath $ProbeLog) {
        Get-Content -LiteralPath $ProbeLog -Tail 12 |
            Where-Object { $_ -match 'Fra:|Time:|Saved:|Blender' } |
            Select-Object -Last 1
    } else { $null }
    [pscustomobject]@{
        utc = [DateTime]::UtcNow.ToString('o')
        process_elapsed_seconds = ([DateTime]::Now - $target.StartTime).TotalSeconds
        process_cpu_seconds = $target.TotalProcessorTime.TotalSeconds
        last_blender_line = $lastLine
        threads = $threads
    }
}

try {
    for ($window = 0; $window -lt $Windows; ++$window) {
        $before = Get-SampleBoundary
        if (-not $before) { break }
        $sampleLog = '{0}-{1:D2}.txt' -f $OutputPrefix, $window
        & $sampler --sample $ProcessId $Attempts $IntervalMilliseconds 2>&1 |
            Out-File -LiteralPath $sampleLog -Encoding utf8
        $sampleCode = $LASTEXITCODE
        $after = Get-SampleBoundary
        $records += [pscustomobject]@{
            window = $window
            before = $before
            after = $after
            sample_log = $sampleLog
            sampler_exit = $sampleCode
        }
        Write-Output ("Window {0}: process elapsed {1:N2}s, {2}" -f
            $window, $before.process_elapsed_seconds, $before.last_blender_line)
        if (-not $after) { break }
    }
} finally {
    [pscustomobject]@{
        process_id = $ProcessId
        probe_log = $ProbeLog
        attempts_per_window = $Attempts
        interval_milliseconds = $IntervalMilliseconds
        power_before = $powerBefore
        power_after = Get-ProbePower
        limitation = 'Each window samples one recently busiest thread; suspending it perturbs wall time. Shares describe captured active-thread CPU, not all process CPU or waits.'
        windows = $records
    } | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath ($OutputPrefix + '.json')
}
