# Dot-source to capture benchmark conditions outside the measured process.
if (-not ('TransmuteProbePower' -as [type])) {
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Sequential)]
public struct TransmuteProbePowerStatus {
    public byte ACLineStatus, BatteryFlag, BatteryLifePercent, SystemStatus;
    public uint BatteryLifeTime, BatteryFullLifeTime;
}
public static class TransmuteProbePower {
    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetSystemPowerStatus(out TransmuteProbePowerStatus status);
}
'@
}
function Get-ProbePower {
    $status = [TransmuteProbePowerStatus]::new()
    if (-not [TransmuteProbePower]::GetSystemPowerStatus([ref]$status)) { return $null }
    [pscustomobject]@{
        ac_line_status = [int]$status.ACLineStatus
        battery_percent = [int]$status.BatteryLifePercent
        battery_saver = [bool]$status.SystemStatus
        active_scheme = (& powercfg /getactivescheme | Out-String).Trim()
    }
}
