<#
.SYNOPSIS
fx performance benchmarks: native x64 vs transmute-fx (ARM64) vs the old
engine, with pinned affinity, medians of N runs, a ratio table and JSON.

.DESCRIPTION
Builds tests/fx/bench (tests/fx/bench/build.ps1), then runs every benchmark
group natively (x64), under transmute-fx (when build-fx*/Release/transmute-fx.exe
exists) and under the old engine (build/Release/transmute.exe --jit
--instruction-limit 100000000000) as a baseline.

Affinity: the script sets its own process affinity before each launch so the
child inherits it from creation. Single-thread groups run on one P-core
logical CPU; thread-scaling groups run on the E-cores (homogeneous, one
logical CPU per core). P/E cores come from GetSystemCpuSetInformation
(EfficiencyClass). Slow engines run with scale=<n> (fewer iterations); all
results are per operation.

Results: a table on stdout and artifacts/fx-bench/fx-bench-<time>.json
(plus latest.json).

.EXAMPLE
powershell -ExecutionPolicy Bypass -File scripts\fx_bench.ps1
powershell -ExecutionPolicy Bypass -File scripts\fx_bench.ps1 -Groups compute,calls -NoOld -Runs 3
#>
[CmdletBinding()]
param(
    [string[]]$Groups = @("*"),
    [int]$Runs = 5,
    [string]$FxExe,
    [string[]]$FxArgs = @(),
    [int]$FxScale = 1,
    [switch]$NoFx,
    [string]$OldEngineExe,
    [switch]$NoOld,
    [int]$OldRuns = 0,
    [int]$OldScaleMultiplier = 1,
    [int]$Cpu = -1,
    [string]$ThreadMask,
    [switch]$NoBuild,
    [switch]$NoGuestCrt,
    [switch]$NoHighPriority,
    [int]$Timeout = 900,
    [string]$OutDir
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "fx_toolchain.ps1")
$repo = Get-FxRepoRoot
$benchDir = Join-Path $repo "build-fx-corpus\fx\bench"
if (-not $OutDir) { $OutDir = Join-Path $repo "artifacts\fx-bench" }
if ($OldRuns -le 0) { $OldRuns = $Runs }

if (-not $NoBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo "tests\fx\bench\build.ps1") -OutDir $benchDir
    if ($LASTEXITCODE -ne 0) { throw "benchmark build failed" }
}
$guestCrt = Join-Path $benchDir "arm64\vcruntime140.dll"
if ($NoGuestCrt -and (Test-Path -LiteralPath $guestCrt)) { Remove-Item -LiteralPath $guestCrt -Force }

# ---- CPU topology ----------------------------------------------------------
if (-not ("FxCpuSets" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class FxCpuSets {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetSystemCpuSetInformation(IntPtr info, uint length, out uint returned, IntPtr process, uint flags);
    // Each row: logical index, core index, efficiency class, group, flags.
    public static int[][] Query() {
        uint length;
        GetSystemCpuSetInformation(IntPtr.Zero, 0, out length, IntPtr.Zero, 0);
        IntPtr buffer = Marshal.AllocHGlobal((int)length);
        try {
            if (!GetSystemCpuSetInformation(buffer, length, out length, IntPtr.Zero, 0))
                throw new System.ComponentModel.Win32Exception();
            var rows = new List<int[]>();
            int offset = 0;
            while (offset < length) {
                int size = Marshal.ReadInt32(buffer, offset);
                int type = Marshal.ReadInt32(buffer, offset + 4);
                if (type == 0) {
                    rows.Add(new int[] {
                        Marshal.ReadByte(buffer, offset + 14), Marshal.ReadByte(buffer, offset + 15),
                        Marshal.ReadByte(buffer, offset + 18), Marshal.ReadInt16(buffer, offset + 12),
                        Marshal.ReadByte(buffer, offset + 19) });
                }
                if (size <= 0) break;
                offset += size;
            }
            return rows.ToArray();
        } finally {
            Marshal.FreeHGlobal(buffer);
        }
    }
}
"@
}
$cpuSets = @([FxCpuSets]::Query() | ForEach-Object {
    [pscustomobject]@{ Logical = $_[0]; Core = $_[1]; Efficiency = $_[2]; Group = $_[3] }
})
$maxClass = ($cpuSets | Measure-Object Efficiency -Maximum).Maximum
$minClass = ($cpuSets | Measure-Object Efficiency -Minimum).Minimum
$pCpus = @($cpuSets | Where-Object { $_.Efficiency -eq $maxClass } | Sort-Object Logical)
$eCpus = @($cpuSets | Where-Object { $_.Efficiency -eq $minClass } | Sort-Object Logical)
$hybrid = $maxClass -ne $minClass
$pCores = @($pCpus | Group-Object Core | ForEach-Object { @($_.Group | Sort-Object Logical)[0] })
if ($Cpu -lt 0) {
    # First logical CPU of the last P-core (keeps CPU 0, which takes most
    # interrupts, out of the measurement when there is a choice).
    $Cpu = @($pCores)[-1].Logical
}
$singleMask = [int64]1 -shl $Cpu
if ($ThreadMask) {
    $threadMaskValue = [Convert]::ToInt64(($ThreadMask -replace '^0x', ''), 16)
} else {
    $pool = if ($hybrid -and $eCpus.Count -ge 8) { $eCpus } else {
        @($cpuSets | Group-Object Core | ForEach-Object { @($_.Group | Sort-Object Logical)[0] } | Sort-Object Logical)
    }
    $threadMaskValue = [int64]0
    foreach ($c in @($pool | Select-Object -First 8)) { $threadMaskValue = $threadMaskValue -bor ([int64]1 -shl $c.Logical) }
}
$cpuName = (Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Select-Object -First 1).Name
Write-Host "CPU: $cpuName"
Write-Host ("P-core logical CPUs: {0}; E-core logical CPUs: {1}" -f (($pCpus | ForEach-Object Logical) -join ","),
            (($eCpus | ForEach-Object Logical) -join ","))
if (-not $hybrid) { Write-Host "  (not a hybrid CPU: all CPUs have EfficiencyClass $maxClass)" }
Write-Host ("single-thread affinity: CPU {0} (mask 0x{1:x}); thread-scaling affinity: mask 0x{2:x}" -f $Cpu, $singleMask, $threadMaskValue)

# ---- engines -----------------------------------------------------------------
$fx = if ($NoFx) { $null } else { Find-FxEngine $FxExe }
$old = if ($NoOld) { $null } else { Find-FxOldEngine $OldEngineExe }
Write-Host ("transmute-fx: {0}" -f $(if ($fx) { $fx } elseif ($NoFx) { "disabled" } else { "not found (fx columns skipped)" }))
Write-Host ("old engine:   {0}" -f $(if ($old) { $old } elseif ($NoOld) { "disabled" } else { "not found" }))

# group, native exe, arm64 exe, thread-scaling, old-engine scale
$groupTable = @(
    @{ Name = "compute";     X64 = "bench_compute";   Arm = "bench_compute";     Threads = $false; OldScale = 10 },
    @{ Name = "mem";         X64 = "bench_mem";       Arm = "bench_mem";         Threads = $false; OldScale = 10 },
    @{ Name = "fma";         X64 = "bench_fma";       Arm = "bench_fma";         Threads = $false; OldScale = 10 },
    @{ Name = "calls";       X64 = "bench_calls";     Arm = "bench_calls";       Threads = $false; OldScale = 100 },
    @{ Name = "heap";        X64 = "bench_heap";      Arm = "bench_heap";        Threads = $false; OldScale = 100 },
    @{ Name = "icall";       X64 = "bench_icall";     Arm = "bench_icall";       Threads = $false; OldScale = 10 },
    @{ Name = "recursion";   X64 = "bench_recursion"; Arm = "bench_recursion";   Threads = $false; OldScale = 10 },
    @{ Name = "memcpy";      X64 = "bench_memcpy";    Arm = "bench_memcpy";      Threads = $false; OldScale = 10 },
    @{ Name = "interp";      X64 = "bench_interp";    Arm = "bench_interp";      Threads = $false; OldScale = 10 },
    @{ Name = "threads";     X64 = "bench_threads";   Arm = "bench_threads";     Threads = $true;  OldScale = 100 },
    @{ Name = "threads_lse"; X64 = "bench_threads";   Arm = "bench_threads_lse"; Threads = $true;  OldScale = 100 }
)
function Test-GroupSelected([string]$Name) {
    foreach ($g in $Groups) { foreach ($p in ($g -split ',')) { if ($Name -like $p.Trim()) { return $true } } }
    return $false
}

# Old engine versus native, measured before fx existed (reference only).
$oldReference = @{
    "compute" = 5.1; "mem" = 6.4; "fma" = 37; "GetCurrentThreadId" = 83; "CriticalSection_pair" = 473
    "SRWLock_pair" = 479; "TlsGetValue" = 1707; "SetLastError_GetLastError" = 1871; "HeapAlloc_HeapFree" = 203
    "threads_mem@8" = 91; "threads_atomic_shared@8" = 1276
}
# docs/FX_ENGINE.md performance gates. ratio = fx/native, ns = fx ns per op.
$gates = @{
    "compute" = @{ Kind = "ratio"; Limit = 2 }; "mem" = @{ Kind = "ratio"; Limit = 2 }
    "GetCurrentThreadId" = @{ Kind = "ns"; Limit = 10 }; "TlsGetValue" = @{ Kind = "ns"; Limit = 10 }
    "SetLastError_GetLastError" = @{ Kind = "ns"; Limit = 20 }
    "CriticalSection_pair" = @{ Kind = "ns"; Limit = 20 }; "SRWLock_pair" = @{ Kind = "ns"; Limit = 20 }
    "HeapAlloc_HeapFree" = @{ Kind = "ns"; Limit = 50 }; "malloc_free" = @{ Kind = "ns"; Limit = 50 }
    "threads_atomic_shared@1" = @{ Kind = "ratio"; Limit = 2 }; "threads_atomic_private@1" = @{ Kind = "ratio"; Limit = 2 }
}
$scalingGate = 1.2
# Benchmarks whose check value is deterministic and arch-independent.
$checkComparable = '^(compute|mem|indirect_call|virtual_call|recursion_.*|memcpy_.*|interpreter)$'

function Invoke-Pinned([string]$Exe, [string[]]$Arguments, [int64]$Mask) {
    $self = [System.Diagnostics.Process]::GetCurrentProcess()
    $saved = $self.ProcessorAffinity
    try {
        $self.ProcessorAffinity = [IntPtr]$Mask
        $priority = if ($NoHighPriority) { $null } else { "High" }
        return Invoke-FxProcess -Exe $Exe -Arguments $Arguments -TimeoutSeconds $Timeout -Priority $priority
    } finally {
        $self.ProcessorAffinity = $saved
    }
}

# Returns @{ name -> @{ NsPerOp = [double[]]; Check = string; Bytes = int } } and warnings.
function Invoke-BenchRuns([string]$Engine, [string]$Exe, [string[]]$Prefix, [int]$Scale, [int]$Count, [int64]$Mask) {
    $results = [ordered]@{}
    $warnings = @()
    for ($run = 1; $run -le $Count; $run++) {
        $r = Invoke-Pinned $Exe (@($Prefix) + @("scale=$Scale")) $Mask
        $text = [System.Text.Encoding]::UTF8.GetString($r.Stdout)
        if ($r.TimedOut -or $r.ExitCode -ne 0) {
            $err = ($r.Stderr -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 1)
            $warnings += "$Engine run $run exit $($r.ExitCode)$(if ($r.TimedOut) { ' (timeout)' }) $err".Trim()
        }
        foreach ($line in ($text -split "`r?`n")) {
            if ($line -match '^INFO affinity=0x([0-9a-f]+)') {
                $seen = [Convert]::ToInt64($Matches[1], 16)
                if ($seen -ne $Mask -and $run -eq 1) { $warnings += ("{0}: process affinity 0x{1:x}, expected 0x{2:x}" -f $Engine, $seen, $Mask) }
            } elseif ($line -match '^BENCH (\S+) ops=(\d+) ns=(\d+) check=([0-9a-f]+)(?: bytes=(\d+))?') {
                $name = $Matches[1]
                if (-not $results.Contains($name)) { $results[$name] = @{ NsPerOp = @(); Check = $Matches[4]; Bytes = 0 } }
                $results[$name].NsPerOp += [double]$Matches[3] / [double]$Matches[2]
                if ($Matches[5]) { $results[$name].Bytes = [int64]$Matches[5] }
            }
        }
    }
    @{ Results = $results; Warnings = $warnings }
}

function Get-Median([double[]]$Values) {
    if (-not $Values -or $Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    $mid = [int][Math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2) { return $sorted[$mid] }
    return ($sorted[$mid - 1] + $sorted[$mid]) / 2
}

$rows = New-Object System.Collections.Generic.List[object]
$allWarnings = @()
foreach ($g in $groupTable) {
    if (-not (Test-GroupSelected $g.Name)) { continue }
    $mask = if ($g.Threads) { $threadMaskValue } else { $singleMask }
    $nativeExe = Join-Path $benchDir "x64\$($g.X64).exe"
    $armExe = Join-Path $benchDir "arm64\$($g.Arm).exe"
    Write-Host ("[{0}] native x{1}" -f $g.Name, $Runs) -NoNewline
    $native = Invoke-BenchRuns "native" $nativeExe @() 1 $Runs $mask
    $fxResult = $null
    $oldResult = $null
    if ($fx) {
        Write-Host (", fx x{0}" -f $Runs) -NoNewline
        $fxResult = Invoke-BenchRuns "fx" $fx (@($FxArgs) + @($armExe)) $FxScale $Runs $mask
    }
    if ($old) {
        $oldScale = $g.OldScale * $OldScaleMultiplier
        Write-Host (", old x{0} (scale {1})" -f $OldRuns, $oldScale) -NoNewline
        $oldResult = Invoke-BenchRuns "old" $old @("--jit", "--instruction-limit", "100000000000", $armExe) $oldScale $OldRuns $mask
    }
    Write-Host ""
    foreach ($w in @($native.Warnings) + @(if ($fxResult) { $fxResult.Warnings }) + @(if ($oldResult) { $oldResult.Warnings })) {
        if ($w) { Write-Host "  warning: $w" -ForegroundColor Yellow; $allWarnings += "$($g.Name): $w" }
    }
    foreach ($name in $native.Results.Keys) {
        $n = $native.Results[$name]
        $row = [ordered]@{
            Group = $g.Name; Name = $name
            NativeNs = Get-Median $n.NsPerOp; NativeSamples = $n.NsPerOp
            FxNs = $null; FxSamples = @(); FxRatio = $null
            OldNs = $null; OldSamples = @(); OldRatio = $null
            OldReference = $oldReference[$name]; Bytes = $n.Bytes
            Gate = $null; GateLimit = $null; GatePass = $null; CheckMatch = $null
        }
        if ($fxResult -and $fxResult.Results.Contains($name)) {
            $f = $fxResult.Results[$name]
            $row.FxNs = Get-Median $f.NsPerOp
            $row.FxSamples = $f.NsPerOp
            if ($row.NativeNs) { $row.FxRatio = $row.FxNs / $row.NativeNs }
            if ($name -match $checkComparable -and $FxScale -eq 1) { $row.CheckMatch = $f.Check -eq $n.Check }
        }
        if ($oldResult -and $oldResult.Results.Contains($name)) {
            $o = $oldResult.Results[$name]
            $row.OldNs = Get-Median $o.NsPerOp
            $row.OldSamples = $o.NsPerOp
            if ($row.NativeNs) { $row.OldRatio = $row.OldNs / $row.NativeNs }
        }
        $gateName = $name
        if ($gates.ContainsKey($gateName)) {
            $gate = $gates[$gateName]
            $row.Gate = $gate.Kind
            $row.GateLimit = $gate.Limit
            if ($null -ne $row.FxNs) {
                $value = if ($gate.Kind -eq "ratio") { $row.FxRatio } else { $row.FxNs }
                $row.GatePass = $value -le $gate.Limit
            }
        }
        $rows.Add([pscustomobject]$row)
    }
}

# Weak scaling: time at 8 threads / time at 1 thread, per engine.
$scaling = @()
foreach ($group in "threads", "threads_lse") {
    foreach ($kind in "threads_compute", "threads_mem", "threads_atomic_private", "threads_atomic_shared") {
        $one = $rows | Where-Object { $_.Group -eq $group -and $_.Name -eq "$kind@1" } | Select-Object -First 1
        $eight = $rows | Where-Object { $_.Group -eq $group -and $_.Name -eq "$kind@8" } | Select-Object -First 1
        if (-not $one -or -not $eight) { continue }
        $s = [ordered]@{ Group = $group; Name = "$kind scaling 8/1"; Native = $eight.NativeNs / $one.NativeNs
                         Fx = $null; Old = $null; FxVsNative = $null; Gated = $kind -ne "threads_atomic_shared"; GatePass = $null }
        if ($null -ne $one.FxNs -and $null -ne $eight.FxNs) {
            $s.Fx = $eight.FxNs / $one.FxNs
            $s.FxVsNative = $s.Fx / $s.Native
            if ($s.Gated) { $s.GatePass = $s.FxVsNative -le $scalingGate }
        }
        if ($null -ne $one.OldNs -and $null -ne $eight.OldNs) { $s.Old = $eight.OldNs / $one.OldNs }
        $scaling += [pscustomobject]$s
    }
}

# ---- report ------------------------------------------------------------------
function Format-Num($v, [string]$fmt = "N2") { if ($null -eq $v) { return "-" } return $v.ToString($fmt) }
$table = foreach ($r in $rows) {
    $gateText = if ($r.Gate -eq "ratio") { "<= {0}x" -f $r.GateLimit } elseif ($r.Gate -eq "ns") { "<= {0} ns" -f $r.GateLimit } else { "" }
    $status = if ($null -eq $r.GatePass) { if ($r.Gate) { "n/a" } else { "" } } elseif ($r.GatePass) { "PASS" } else { "FAIL" }
    if ($r.CheckMatch -eq $false) { $status = ($status + " CHECK-MISMATCH").Trim() }
    $name = if ($r.Group -eq "threads_lse") { "$($r.Name) [lse]" } else { $r.Name }
    [pscustomobject]@{
        Benchmark = $name
        "native ns" = Format-Num $r.NativeNs "N3"
        "fx ns" = Format-Num $r.FxNs "N3"
        "fx/nat" = Format-Num $r.FxRatio
        "old ns" = Format-Num $r.OldNs "N3"
        "old/nat" = Format-Num $r.OldRatio "N1"
        "old ref" = if ($r.OldReference) { "{0}x" -f $r.OldReference } else { "" }
        Gate = $gateText
        Status = $status
    }
}
$table | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
if ($scaling.Count) {
    $scaling | ForEach-Object {
        [pscustomobject]@{
            "Weak scaling (t8/t1)" = $(if ($_.Group -eq "threads_lse") { "$($_.Name) [lse]" } else { $_.Name })
            native = Format-Num $_.Native; fx = Format-Num $_.Fx; old = Format-Num $_.Old
            "fx/native" = Format-Num $_.FxVsNative
            Gate = if ($_.Gated) { "<= $scalingGate" } else { "" }
            Status = if ($null -eq $_.GatePass) { if ($_.Gated) { "n/a" } else { "" } } elseif ($_.GatePass) { "PASS" } else { "FAIL" }
        }
    } | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
}
$memcpyRows = @($rows | Where-Object { $_.Bytes -gt 0 })
if ($memcpyRows.Count) {
    $memcpyRows | ForEach-Object {
        [pscustomobject]@{ memcpy = $_.Name
            "native GB/s" = Format-Num ($_.Bytes / $_.NativeNs)
            "fx GB/s" = $(if ($_.FxNs) { Format-Num ($_.Bytes / $_.FxNs) } else { "-" })
            "old GB/s" = $(if ($_.OldNs) { Format-Num ($_.Bytes / $_.OldNs) } else { "-" }) }
    } | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$report = [ordered]@{
    timestamp = (Get-Date).ToString("o")
    machine = [ordered]@{
        cpu = $cpuName; hybrid = $hybrid
        pCoreLogicalCpus = @($pCpus | ForEach-Object Logical); eCoreLogicalCpus = @($eCpus | ForEach-Object Logical)
        singleThreadCpu = $Cpu; singleThreadMask = ("0x{0:x}" -f $singleMask); threadMask = ("0x{0:x}" -f $threadMaskValue)
    }
    engines = [ordered]@{ fx = $fx; fxArgs = $FxArgs; fxScale = $FxScale; old = $old; oldScaleMultiplier = $OldScaleMultiplier }
    runs = $Runs; oldRuns = $OldRuns
    gates = [ordered]@{ perBenchmark = $gates; weakScaling = $scalingGate }
    results = $rows
    scaling = $scaling
    warnings = $allWarnings
}
$json = $report | ConvertTo-Json -Depth 6
$jsonPath = Join-Path $OutDir "fx-bench-$stamp.json"
[System.IO.File]::WriteAllText($jsonPath, $json, (New-Object System.Text.UTF8Encoding $false))
[System.IO.File]::WriteAllText((Join-Path $OutDir "latest.json"), $json, (New-Object System.Text.UTF8Encoding $false))
Write-Host "wrote $jsonPath"
$gateFailures = @($rows | Where-Object { $_.GatePass -eq $false }).Count + @($scaling | Where-Object { $_.GatePass -eq $false }).Count
if ($fx) { Write-Host "fx gate failures: $gateFailures" }
exit 0
