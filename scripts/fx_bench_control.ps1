# Benchmark process controls. Windows does not inherit these settings into children.
if (-not ('FxStudyQos' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Sequential)]
public struct FxStudyPowerState { public uint Version, ControlMask, StateMask; }
public static class FxStudyQos {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool GetProcessInformation(IntPtr process, int kind, ref FxStudyPowerState state, uint size);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool SetProcessInformation(IntPtr process, int kind, ref FxStudyPowerState state, uint size);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool QueryProcessCycleTime(IntPtr process, out ulong cycles);
}
'@
}
function Enter-FxBenchControl {
    $p = [Diagnostics.Process]::GetCurrentProcess()
    $state = New-Object FxStudyPowerState
    $state.Version = 1
    if (-not [FxStudyQos]::GetProcessInformation($p.Handle, 4, [ref]$state, 12)) { throw 'Cannot read process QoS' }
    $saved = [pscustomobject]@{ process=$p; power=$state; priority=$p.PriorityClass }
    $high = New-Object FxStudyPowerState
    $high.Version=1; $high.ControlMask=1; $high.StateMask=0
    if (-not [FxStudyQos]::SetProcessInformation($p.Handle, 4, [ref]$high, 12)) { throw 'Cannot set benchmark QoS' }
    $p.PriorityClass = [Diagnostics.ProcessPriorityClass]::High
    return $saved
}
function Exit-FxBenchControl($saved) {
    $state = $saved.power
    [void][FxStudyQos]::SetProcessInformation($saved.process.Handle, 4, [ref]$state, 12)
    $saved.process.PriorityClass = $saved.priority
}
function Set-FxBenchChildControl($process) {
    $state = New-Object FxStudyPowerState
    $state.Version=1; $state.ControlMask=1; $state.StateMask=0
    if (-not [FxStudyQos]::SetProcessInformation($process.Handle, 4, [ref]$state, 12)) { throw 'Cannot set child benchmark QoS' }
    $process.PriorityClass = [Diagnostics.ProcessPriorityClass]::High
    $actual = New-Object FxStudyPowerState
    $actual.Version = 1
    if (-not [FxStudyQos]::GetProcessInformation($process.Handle, 4, [ref]$actual, 12) -or
        $actual.ControlMask -band 1 -ne 1 -or $actual.StateMask -band 1 -ne 0 -or
        $process.PriorityClass -ne [Diagnostics.ProcessPriorityClass]::High) { throw 'Child scheduling verification failed' }
}
function Get-FxBenchProcessCycles($process) {
    [uint64]$cycles = 0
    if (-not [FxStudyQos]::QueryProcessCycleTime($process.Handle, [ref]$cycles)) { throw 'Cannot read benchmark process cycles' }
    return $cycles
}
