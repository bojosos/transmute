param(
    [string] $Exe = "$PSScriptRoot/../build-wf-gui/Release/alchemy-fx.exe",
    [string] $App = "$PSScriptRoot/../artifacts/alchemy-blender/extracted/Blender/Blender.app",
    # alchemy-fx --runtime-dir (the ARM64 LLVM runtime).
    [string] $RuntimeDir = "$PSScriptRoot/../artifacts/alchemy-llvm-arm64",
    [string] $OutRoot = "$PSScriptRoot/../artifacts/alchemy-blender/gui",
    [string[]] $BlenderArgs = @('--factory-startup'),
    [int] $TimeoutSeconds = 300,
    # Seconds to let the main window settle (after its last modal dialog
    # closed) before the screenshot.
    [int] $SettleSeconds = 5,
    # Press the default button of every modal dialog (#32770: NSAlert,
    # MessageBoxW) after recording its text. Without it the run waits.
    [switch] $DismissDialogs,
    # Keep Blender running after the screenshot (default: stop it).
    [switch] $KeepRunning,
    [ValidateSet('off', 'zero', 'trap')] [string] $Probe = 'off',
    # ALCHEMY_TRACE_OBJC class-prefix list, e.g. 'NS,CA,MTL'.
    [string] $TraceObjc = '',
    # ALCHEMY_MSL_DUMP directory for the shader corpus.
    [string] $MslDump = '',
    # Do not set ALCHEMY_TRACE_PRESENT=1 (presentation trace and at-exit totals).
    [switch] $NoTracePresent,
    # Let Blender's windows take the foreground on their own (default:
    # ALCHEMY_GUI_NO_ACTIVATE=1, polite on a shared desktop).
    [switch] $AllowActivate
)
# Launches the official macOS ARM64 Blender.app windowed (no -b) through
# alchemy-fx and reports how far the GUI got. Writes
# <OutRoot>/<timestamp>/{log.txt,status.json,screenshot.png}.
#
# - The main window is the window server's "AlchemyWindow" of the process
#   (never a modal #32770 dialog); modal dialogs are recorded with their text
#   and, with -DismissDialogs, answered with their default button.
# - The screenshot is the main window's client area (PrintWindow with
#   PW_RENDERFULLCONTENT; when that is black, QuartzCore's present capture,
#   else a screen BitBlt while the desktop is unlocked), taken
#   -SettleSeconds after the window appeared and no dialog was open.
# - status.json: exit code, stop point (guest signal > unsupported
#   Objective-C message > "alchemy:" fatal line > "unsupported" line >
#   other stderr; informational notes are skipped), the exact presented-frame
#   count (QuartzCore's ALCHEMY_PRESENT_STATS file, sampled every second into
#   present_stats.samples, and the at-exit totals when Blender exits by
#   itself; see alchemy_gui_common.ps1), the client area's non-black
#   pixel ratio, and milestone flags with the highest milestone reached:
#   started < window < presenting (QuartzCore presented a frame) < settled
#   (screenshot taken after -SettleSeconds) < content (the screenshot is the
#   window's own pixels with >= 5% non-black). The present capture is served
#   from the last presented frame, so an idle Blender can be captured.
# - Windows are created with ALCHEMY_GUI_NO_ACTIVATE=1 unless -AllowActivate.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/alchemy_gui_common.ps1"
$R = (Resolve-Path "$PSScriptRoot/..").Path
$exePath = (Resolve-Path $Exe).Path
$appPath = (Resolve-Path $App).Path
$runtimePath = (Resolve-Path $RuntimeDir).Path
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$OutDir = Join-Path $OutRoot $stamp
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
function Quote([string] $a) { if ($a -notmatch '[\s"]') { return $a }; '"' + ($a -replace '(\\*)"', '$1$1\"') + '"' }

$all = @('--runtime-dir', $runtimePath, $appPath) + $BlenderArgs
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exePath
$psi.Arguments = ($all | ForEach-Object { Quote $_ }) -join ' '
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.WorkingDirectory = $R
if ($Probe -ne 'off') { $psi.EnvironmentVariables['ALCHEMY_PROBE_MISSING'] = $Probe }
else { $psi.EnvironmentVariables.Remove('ALCHEMY_PROBE_MISSING') }
if ($TraceObjc) { $psi.EnvironmentVariables['ALCHEMY_TRACE_OBJC'] = $TraceObjc }
if (-not $NoTracePresent) { $psi.EnvironmentVariables['ALCHEMY_TRACE_PRESENT'] = '1' }
# QuartzCore's on-demand present capture (screenshot fallback for the Vulkan swapchain).
$presentCapture = Join-Path $OutDir 'present-capture'
New-Item -ItemType Directory -Force $presentCapture | Out-Null
$psi.EnvironmentVariables['ALCHEMY_PRESENT_CAPTURE'] = $presentCapture
# Exact presented-frame totals (periodic file, final at exit).
$presentStats = Join-Path $OutDir 'present-stats.json'
$psi.EnvironmentVariables['ALCHEMY_PRESENT_STATS'] = $presentStats
if (-not $AllowActivate) { $psi.EnvironmentVariables['ALCHEMY_GUI_NO_ACTIVATE'] = '1' }
if ($MslDump) {
    New-Item -ItemType Directory -Force $MslDump | Out-Null
    $psi.EnvironmentVariables['ALCHEMY_MSL_DUMP'] = (Resolve-Path $MslDump).Path
}

$p = New-Object System.Diagnostics.Process
$p.StartInfo = $psi
$sw = [System.Diagnostics.Stopwatch]::StartNew()
[void]$p.Start()
# Output is read line by line (one ordered reader thread per stream) so the
# presented-frame count is live.
$stdoutPump = [AlchemyLinePump]::new($p.StandardOutput)
$stderrPump = [AlchemyLinePump]::new($p.StandardError)

$window = [IntPtr]::Zero
$windowSeenAt = $null
$windowInfo = $null
$dialogs = New-Object System.Collections.Generic.List[object]
$dialogByHandle = @{}
$quietSince = $null          # no dialog open since
$timedOut = $false
$shot = $null
$sampler = New-AlchemyFrameSampler $presentStats 1.0
function Seconds { [math]::Round($sw.Elapsed.TotalSeconds, 1) }
function Describe-Window([IntPtr] $handle) {
    $bounds = [AlchemyGuiWin32]::Bounds($handle); $client = [AlchemyGuiWin32]::Client($handle)
    [ordered]@{ class = [AlchemyGuiWin32]::ClassName($handle); title = [AlchemyGuiWin32]::Title($handle)
        x = $bounds[0]; y = $bounds[1]; width = $bounds[2]; height = $bounds[3]
        client_width = $client[2]; client_height = $client[3]; seen_after_s = $windowSeenAt }
}
while (-not $p.HasExited) {
    Add-AlchemyFrameSample $sampler $sw.Elapsed.TotalSeconds
    # Modal dialogs: record each once; answer it with -DismissDialogs.
    $open = @([AlchemyGuiWin32]::Dialogs([uint32]$p.Id))
    foreach ($handle in $open) {
        $key = $handle.ToInt64()
        if (-not $dialogByHandle.ContainsKey($key)) {
            Start-Sleep -Milliseconds 300   # let the dialog finish laying out its text
            $entry = [ordered]@{ text = [AlchemyGuiWin32]::DialogText($handle); seen_after_s = (Seconds)
                pressed = $null; pressed_after_s = $null }
            $dialogByHandle[$key] = $entry
            $dialogs.Add($entry)
            "dialog at $($entry.seen_after_s)s: $($entry.text)"
            # What the main window showed behind the dialog (the guest may
            # not survive the answer).
            $main = [AlchemyGuiWin32]::MainWindow([uint32]$p.Id)
            if ($main -ne [IntPtr]::Zero) {
                $behind = Join-Path $OutDir "behind-dialog-$($dialogs.Count).png"
                try { $entry.behind = [ordered]@{ path = $behind; method = [AlchemyGuiWin32]::Capture($main, $behind, $true, $presentCapture)
                        nonblack_ratio = [math]::Round(([AlchemyGuiWin32]::PixelStats($behind))[0], 4) } }
                catch { $entry.behind = [ordered]@{ error = $_.Exception.Message } }
            }
        }
        $entry = $dialogByHandle[$key]
        if ($DismissDialogs -and -not $entry.pressed -and [AlchemyGuiWin32]::Alive($handle)) {
            $entry.pressed = [AlchemyGuiWin32]::PressDefault($handle)
            $entry.pressed_after_s = (Seconds)
            "dialog answered with '$($entry.pressed)'"
        }
    }
    if ($open.Count) { $quietSince = $null } elseif ($null -eq $quietSince) { $quietSince = $sw.Elapsed.TotalSeconds }

    # The main window; an AlchemyWindow replaces an earlier fallback choice.
    if ($window -eq [IntPtr]::Zero -or [AlchemyGuiWin32]::ClassName($window) -ne 'AlchemyWindow' -or
        -not [AlchemyGuiWin32]::Alive($window)) {
        $candidate = [AlchemyGuiWin32]::MainWindow([uint32]$p.Id)
        if ($candidate -ne [IntPtr]::Zero -and $candidate -ne $window) {
            $window = $candidate
            if ($null -eq $windowSeenAt) { $windowSeenAt = Seconds }
            $windowInfo = Describe-Window $window
        }
    }
    $settled = $window -ne [IntPtr]::Zero -and $null -ne $quietSince -and
        $sw.Elapsed.TotalSeconds - [math]::Max($windowSeenAt, $quietSince) -ge $SettleSeconds
    if (-not $shot -and $settled -and [AlchemyGuiWin32]::Alive($window)) {
        $path = Join-Path $OutDir 'screenshot.png'
        $shot = [ordered]@{ path = $null; method = $null; after_s = (Seconds); error = $null }
        try {
            $windowInfo = Describe-Window $window
            $shot.method = [AlchemyGuiWin32]::Capture($window, $path, $true, $presentCapture)
            $shot.path = $path
            $stats = [AlchemyGuiWin32]::PixelStats($path)
            $shot.nonblack_ratio = [math]::Round($stats[0], 4)
            $shot.mean_luma = [math]::Round($stats[1], 1)
            $shot.distinct_colours = [int]$stats[2]
        } catch { $shot.error = $_.Exception.Message }
        $shot.presented_frames = Get-AlchemyPresentedFrames '' $presentStats
        if (-not $KeepRunning) { break }
    }
    if ($sw.Elapsed.TotalSeconds -ge $TimeoutSeconds) { $timedOut = $true; break }
    Start-Sleep -Milliseconds 250
}
Add-AlchemyFrameSample $sampler $sw.Elapsed.TotalSeconds -Force
$aliveAtEnd = -not $p.HasExited
$killed = $false
if ($aliveAtEnd) {
    if ($KeepRunning -and -not $timedOut) {
        "Blender is still running (pid $($p.Id)); output after this point is not captured."
    } else {
        $p.Kill(); $killed = $true
    }
}
if ($killed -or $p.HasExited) { $p.WaitForExit() }   # also flushes the asynchronous readers
$sw.Stop()
Start-Sleep -Milliseconds 200
[void]$stdoutPump.Wait(3000); [void]$stderrPump.Wait(3000)
$out = $stdoutPump.Text; $err = $stderrPump.Text
$log = Join-Path $OutDir 'log.txt'
Set-Content -Encoding utf8 $log ("== command`n$exePath $($psi.Arguments)`n== stdout`n$out`n== stderr`n$err")

$exitCode = if ($p.HasExited) { $p.ExitCode } else { $null }
$stop = Get-AlchemyStopPoint $out $err $(if ($killed) { $null } else { $exitCode })
$presentSummary = Get-AlchemyPresentSummary $presentStats $err $sampler
$frames = $presentSummary.presented
$milestones = [ordered]@{
    started = $true
    window = $null -ne $windowSeenAt
    dialog = $dialogs.Count -gt 0
    dialogs_dismissed = $dialogs.Count -gt 0 -and -not ($dialogs | Where-Object { -not $_.pressed })
    presenting = [bool]($frames -gt 0)
    # Only a capture of the window's own pixels counts (not a blank
    # PrintWindow, not the lock screen).
    content = [bool]($shot -and ($shot.method -in @('printwindow', 'present', 'screen')) -and $shot.nonblack_ratio -ge 0.05)
    settled = [bool]$shot -and -not $shot.error
    alive_after_settle = [bool]$shot -and ($aliveAtEnd -or $KeepRunning)
    clean_exit = -not $killed -and $exitCode -eq 0
}
$highest = 'started'
foreach ($name in 'window', 'presenting', 'settled', 'content') { if ($milestones[$name]) { $highest = $name } }
$status = [ordered]@{
    timestamp = $stamp
    command = "$exePath $($psi.Arguments)"
    exit_code = $exitCode
    killed_by_script = $killed
    timed_out = $timedOut
    elapsed_ms = $sw.ElapsedMilliseconds
    milestone = $highest
    milestones = $milestones
    window = $windowInfo
    dialogs = $dialogs
    screenshot = $shot
    presented_frames = $frames
    present_stats = $presentSummary
    stop_point = $stop
    guest_errors = @(Get-AlchemyGuestErrors $out)
    desktop_visible = [AlchemyGuiWin32]::DesktopVisible()
    unsupported_count = @($err -split "`r?`n" | Where-Object { $_ -match 'unsupported' }).Count
    trace_objc = $TraceObjc
    probe = $Probe
}
$status | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $OutDir 'status.json')

$state = if ($timedOut) { 'TIMEOUT' } elseif ($killed) { 'stopped after screenshot' } elseif ($p.HasExited) { "exit=$exitCode" } else { 'running' }
'{0} in {1} ms; milestone {2}; window: {3}' -f $state, $sw.ElapsedMilliseconds, $highest,
    $(if ($windowInfo) { "$($windowInfo.class) $($windowInfo.client_width)x$($windowInfo.client_height) '$($windowInfo.title)'" } else { 'none' })
foreach ($d in $dialogs) { "dialog: $($d.text)$(if ($d.pressed) { " -> [$($d.pressed)]" })" }
if ($shot) { "screenshot: $($shot.method) non-black $($shot.nonblack_ratio) luma $($shot.mean_luma) frames $($shot.presented_frames)$(if ($shot.error) { " error: $($shot.error)" })" }
"presented frames: $(if ($null -ne $frames) { "$frames ($($presentSummary.source))" } else { 'none' })"
if ($stop.line) { "stop ($($stop.kind)): $($stop.line)"; $stop.detail | ForEach-Object { "    $_" } }
"logs: $OutDir"
