# Feature probe in a real Blender window (audio enabled): native x64 and/or ARM64 under transmute-fx.
# Runs tests/fixtures/blender_feature_probe.py, screenshots the viewport during the
# viewport-render steps, and prints a pass/fail table per mode.
param(
    [string] $Fx = 'C:\dev\transmute\build-fx-integ\Release\transmute-fx.exe',
    [string[]] $Modes = @('native', 'fx'),
    [string] $Backend = 'opengl',
    [string] $OutDir = 'C:\dev\transmute\artifacts\fx-features',
    [string] $Native = 'C:\Program Files\Blender Foundation\Blender 4.5\blender.exe',
    [string] $Arm = 'C:\dev\transmute\artifacts\arm64-stage\Blender Foundation\Blender 4.5\blender.exe',
    [int] $TimeoutSeconds = 1200
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class FpWin {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO i);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize, dwTime; }
  public static uint IdleMs() { var i = new LASTINPUTINFO(); i.cbSize = 8; GetLastInputInfo(ref i); return (uint)Environment.TickCount - i.dwTime; }
}
"@
function Shot-Process([int]$ProcessId, [string]$Path) {
    # Screenshots need the window in front; never take focus from someone using the machine.
    if ([FpWin]::IdleMs() -lt 30000) { Write-Warning "skipped $Path (user active)"; return }
    $script:hw = [IntPtr]::Zero
    [FpWin]::EnumWindows({ param($h, $l) $q = 0; [void][FpWin]::GetWindowThreadProcessId($h, [ref]$q)
        if ($q -eq $ProcessId -and [FpWin]::IsWindowVisible($h)) { $r = New-Object FpWin+RECT; [void][FpWin]::GetWindowRect($h, [ref]$r)
            if (($r.R - $r.L) -gt 600) { $script:hw = $h; return $false } }
        return $true }, [IntPtr]::Zero) | Out-Null
    if ($script:hw -eq [IntPtr]::Zero) { return }
    [void][FpWin]::SetForegroundWindow($script:hw); Start-Sleep -Milliseconds 300
    $r = New-Object FpWin+RECT; [void][FpWin]::GetWindowRect($script:hw, [ref]$r)
    try {   # best effort: a minimized or resizing window has no usable rect
        $bmp = New-Object Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T); $g = [Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size); $bmp.Save($Path); $g.Dispose(); $bmp.Dispose()
    } catch { Write-Warning "screenshot $Path failed: rect $($r.L),$($r.T),$($r.R),$($r.B)" }
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
$py = (Resolve-Path (Join-Path $PSScriptRoot '..\tests\fixtures\blender_feature_probe.py')).Path
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ($Modes -contains 'fx') { $runtime = Join-Path $OutDir "transmute-fx-$stamp.exe"; Copy-Item -LiteralPath $Fx -Destination $runtime }
$all = @{}
foreach ($mode in $Modes) {
    $tag = "$mode-$Backend-$stamp"
    $json = Join-Path $OutDir "$tag.json"; $work = Join-Path $OutDir "$tag-work"
    $outLog = Join-Path $OutDir "$tag.out.log"; $errLog = Join-Path $OutDir "$tag.err.log"
    $bargs = @('--factory-startup', '--gpu-backend', $Backend, '--window-geometry', '80', '80', '1280', '800',
               '--python', "`"$py`"", '--', "`"$json`"", "`"$work`"")
    if ($mode -eq 'fx') { $exe = $runtime; $bargs = @("`"$Arm`"") + $bargs; $wd = Split-Path $Arm } else { $exe = $Native; $wd = Split-Path $Native }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $exe -ArgumentList $bargs -WorkingDirectory $wd -PassThru -RedirectStandardOutput $outLog -RedirectStandardError $errLog
    $null = $p.Handle   # keeps the exit code readable after exit
    $shot = @{}
    while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        Start-Sleep -Milliseconds 500
        $cur = (Select-String -Path $outLog -Pattern '^\[probe\] step (viewport_\S+)' -ErrorAction SilentlyContinue | Select-Object -Last 1)
        if ($cur) { $name = $cur.Matches[0].Groups[1].Value
            if (-not $shot.ContainsKey($name)) { $shot[$name] = $sw.Elapsed.TotalSeconds }
            elseif ($shot[$name] -gt 0 -and $sw.Elapsed.TotalSeconds - $shot[$name] -gt 6) { Shot-Process $p.Id (Join-Path $OutDir "$tag-$name.png"); $shot[$name] = -1 } }
    }
    if (-not $p.HasExited) { "$mode timed out; current step: $((Get-Content $json -Raw | ConvertFrom-Json).current)"; Stop-Process -Id $p.Id -Force }
    '{0}: wall {1:N1} s, exit {2}' -f $mode, $sw.Elapsed.TotalSeconds, $(if ($p.HasExited) { '0x{0:x8}' -f $p.ExitCode } else { 'killed' })
    if (Test-Path $json) {
        $r = Get-Content $json -Raw | ConvertFrom-Json
        "  audio devices: $($r.audio_devices -join ', ')  engines: $($r.engines -join ', ')"
        foreach ($c in $r.checks) {
            if (-not $all.ContainsKey($c.name)) { $all[$c.name] = [ordered]@{ check = $c.name } }
            $all[$c.name][$mode] = '{0} {1:N1}s' -f $(if ($c.ok) { 'ok' } else { 'FAIL' }), $c.seconds
            if (-not $c.ok) { "  FAIL $($c.name): $($c.detail)" }
        }
    }
}
$all.Values | ForEach-Object { [pscustomobject]$_ } | Format-Table -AutoSize | Out-String -Width 200
