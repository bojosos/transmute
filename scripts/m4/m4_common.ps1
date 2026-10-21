# Shared runner for the M4 input scenarios (scripts/m4/NN_*.ps1). Dot-source
# it from a scenario script whose param block declares the common options:
#   -Target alchemy|native   Blender.app under alchemy-fx, or native x64 Blender
#                            (its captures become the reference images)
#   -WhatIf                  print the planned inputs, captures and checks; launch
#                            nothing and inject nothing
#   -OutRoot / -ReferenceRoot / -Exe / -App / -RuntimeDir / -NativeExe
#   -ClientWidth/-ClientHeight  client area size both targets are resized to first
#                            (default 1600x900, which fits a 1920x1080 screen)
#   -KeepRunning             leave Blender running after a standalone scenario
#   -NoReferenceCheck        do not compare captures with the reference images
#   -RefTolerance / -RefPercent  reference comparison (default: channel diff 8 on
#                            at most 3% of the channels)
#   -Session                 run inside an existing session (run_all.ps1)
#
# Each scenario prints "PASS <name>" or "FAIL <name>: <first failure>" and
# writes <out>/<name>/result.json with every step (input, frames before and
# after), capture (method, size, reference comparison) and check (expected
# visible change or sameness in a named region, with the measured share of
# changed pixels). Standalone scenarios other than 01 first dismiss the
# splash (01's steps) without checking it.
#
# Real input requires the phase-2 driver: an unlocked desktop that no other
# agent uses. Unattended agents run scenarios only with -WhatIf.
. "$PSScriptRoot/../alchemy_gui_input.ps1"

$script:M4Root = (Resolve-Path "$PSScriptRoot/../..").Path
# Both targets run at this client size (unless -ClientWidth/-ClientHeight), so
# captures and references match pixel for pixel.
$script:M4DefaultClient = @(1600, 900)
$script:M4Artifacts = if (Test-Path "$script:M4Root/artifacts/alchemy-blender") { "$script:M4Root/artifacts" } else { 'C:/dev/transmute/artifacts' }

# ---- Blender 4.5 layout ---------------------------------------------------------------
# Points and regions of the factory-startup screen, measured on Blender 4.5.14
# (native x64 and Blender.app under Alchemy draw it identically) at a
# 1904x1113 client area and 96 DPI. Header rows and widget offsets scale with
# the UI scale (window DPI / 96); area splits scale with the window, as
# Blender rescales screen areas proportionally. Points are @(x, y), regions
# @(x, y, width, height), client pixels.
function Get-BlenderLayout([int] $Width, [int] $Height, [double] $Scale = 1.0) {
    $s = $Scale
    function LPoint([double] $x, [double] $y) { , @([int][math]::Round($x), [int][math]::Round($y)) }
    function LRegion([double] $x, [double] $y, [double] $w, [double] $h) {
        $x0 = [math]::Max(0, [math]::Round($x)); $y0 = [math]::Max(0, [math]::Round($y))
        $x1 = [math]::Min($Width, [math]::Round($x + $w)); $y1 = [math]::Min($Height, [math]::Round($y + $h))
        , @([int]$x0, [int]$y0, [int]($x1 - $x0), [int]($y1 - $y0))
    }
    $top = 26 * $s                                      # top bar (menus, workspaces)
    $status = 22 * $s                                   # status bar
    $middle = $Height - $top - $status
    $split = $Width * 1574 / 1904                       # 3D viewport | outliner + properties
    $timeline = $top + $middle * (1008 - 26) / 1065     # 3D viewport / timeline
    $propsTop = $top + $middle * (368 - 26) / 1065      # outliner / properties
    $viewTop = 50 * $s                                  # below the 3D view header
    $cx = $split / 2; $cy = ($viewTop + $timeline) / 2
    [ordered]@{
        width = $Width; height = $Height; scale = $s
        # Quick Setup splash (first start): centred in the window.
        splash = LRegion ($Width / 2 - 250 * $s) ($Height / 2 - 232 * $s) (500 * $s) (464 * $s)
        splash_continue = LPoint ($Width / 2) ($Height / 2 + 198 * $s)
        # Probes telling the Quick Setup splash (Continue button, grey 84) from
        # the regular one (background 24 there).
        splash_continue_probes = @((LPoint ($Width / 2 - 110 * $s) ($Height / 2 + 198 * $s)), (LPoint ($Width / 2 + 110 * $s) ($Height / 2 + 198 * $s)))
        file_menu = LPoint (44 * $s) (13 * $s)
        edit_menu = LPoint (79 * $s) (13 * $s)
        file_dropdown = LRegion 0 ($top + 2 * $s) (380 * $s) (520 * $s)
        edit_dropdown = LRegion (30 * $s) ($top + 2 * $s) (380 * $s) (480 * $s)
        viewport = LRegion 0 $viewTop $split ($timeline - $viewTop)
        viewport_header = LRegion 0 $top $split ($viewTop - $top)
        viewport_center = LPoint $cx $cy
        # Viewport shading buttons, right-aligned in the 3D view header.
        shading_wireframe = LPoint ($split - 96 * $s) ($top + 11 * $s)
        shading_solid = LPoint ($split - 78 * $s) ($top + 11 * $s)
        shading_material = LPoint ($split - 59 * $s) ($top + 11 * $s)
        shading_rendered = LPoint ($split - 40 * $s) ($top + 11 * $s)
        # Around the default cube (the view looks at the origin).
        cube_region = LRegion ($cx - 160 * $s) ($cy - 160 * $s) (320 * $s) (320 * $s)
        # Floor grid only: bottom right of the viewport, clear of the gizmos.
        viewport_empty = LPoint ($split - 140 * $s) ($timeline - 70 * $s)
        # F3 menu search: the popup opens around the mouse.
        search_popup = LRegion ($cx - 360 * $s) ($cy - 90 * $s) (720 * $s) (520 * $s)
        outliner_rows = LRegion $split (90 * $s) ($Width - $split) (70 * $s)
        outliner_cube = LPoint ($split + 99 * $s) (123 * $s)
        properties_name = LRegion ($split + 30 * $s) ($propsTop + 30 * $s) ($Width - $split - 30 * $s) (100 * $s)
    }
}

# ---- context ------------------------------------------------------------------------------
function Get-M4Option([string] $Name, $Default = $null) {
    $v = Get-Variable -Name $Name -Scope Script -ErrorAction SilentlyContinue
    if ($v -and $null -ne $v.Value -and "$($v.Value)" -ne '') { return $v.Value }
    $Default
}

function Write-M4([string] $Text) { Write-Host $Text }

function Add-M4Record($ctx, [string] $Kind, $Entry) {
    $Entry.kind = $Kind
    $ctx.Records.Add($Entry)
    if ($Entry.Contains('ok') -and $Entry.ok -eq $false -and -not $ctx.Failure) { $ctx.Failure = "$($Entry.what): $($Entry.detail)" }
}

# Runs `$Action` (Send-Alchemy* calls) as one step: records the presented
# frame count before, then waits for the redraw (Wait-Frame) unless -NoWait.
function Invoke-M4Step($ctx, [string] $What, [scriptblock] $Action, [switch] $NoWait, [IntPtr] $FrameWindow = [IntPtr]::Zero,
                       [int] $TimeoutMs = 5000) {
    if ($ctx.Failure -and -not $ctx.WhatIf) { return }
    $entry = [ordered]@{ kind = ''; what = $What; frames_before = $null; frames_after = $null; redraw = $null; ms = $null }
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    if ($ctx.WhatIf) { Write-M4 "[plan] step: $What" }
    else { $entry.frames_before = Get-AlchemyFrameCount $ctx.Session $FrameWindow }
    try {
        & $Action
        if (-not $NoWait) {
            $since = if ($null -ne $entry.frames_before) { [long]$entry.frames_before } else { -1 }
            $wait = Wait-Frame $ctx.Session -Since $since -Window $FrameWindow -TimeoutMs $TimeoutMs
            $entry.frames_after = $wait.frames
            $entry.redraw = $wait.advanced
            if ($wait.advanced -eq $false) { $entry.note = "no frame was presented after this input (within $TimeoutMs ms)" }
        }
    } catch {
        $entry.ok = $false; $entry.detail = $_.Exception.Message
    }
    $entry.ms = $clock.ElapsedMilliseconds
    Add-M4Record $ctx 'step' $entry
}
function Step-M4Click($ctx, $Point, [string] $Why, [string] $Button = 'left', [int] $Count = 1, [switch] $NoWait) {
    Invoke-M4Step $ctx "$Why ($Button click$(if ($Count -gt 1) { " x$Count" }) at $($Point -join ','))" {
        Send-AlchemyClick $ctx.Window $Point[0] $Point[1] -Button $Button -Count $Count } -NoWait:$NoWait
}
# A move redraws only when it leaves or enters a highlighted widget: a short wait.
function Step-M4Move($ctx, $Point, [string] $Why, [switch] $NoWait) {
    Invoke-M4Step $ctx "$Why (move to $($Point -join ','))" { Send-AlchemyMove $ctx.Window $Point[0] $Point[1] } -NoWait:$NoWait -TimeoutMs 1500
}
function Step-M4Drag($ctx, $From, $To, [string] $Why, [string] $Button = 'left', [int] $Steps = 20, [string[]] $Modifiers = @()) {
    Invoke-M4Step $ctx "$Why ($Button drag $($From -join ',') -> $($To -join ','))" {
        Send-AlchemyDrag $ctx.Window $From $To -Button $Button -Steps $Steps -Modifiers $Modifiers }
}
function Step-M4Wheel($ctx, $Point, [int] $Notches, [string] $Why) {
    Invoke-M4Step $ctx "$Why (wheel $Notches at $($Point -join ','))" { Send-AlchemyWheel $ctx.Window $Point[0] $Point[1] $Notches }
}
function Step-M4Key($ctx, [string] $Chord, [string] $Why, [switch] $NoWait) {
    Invoke-M4Step $ctx "$Why (key $Chord)" { Send-AlchemyKey $Chord -Window $ctx.Window } -NoWait:$NoWait
}
function Step-M4Text($ctx, [string] $Text, [string] $Why) {
    Invoke-M4Step $ctx "$Why (type '$Text')" { Send-AlchemyText $Text -Window $ctx.Window }
}

# Captures the client area of -Window (default: the main window) as
# <name>.png. Native targets also store it as the reference; Alchemy
# captures are compared with an existing reference (-NoReferenceCheck: not).
# -SizeMayDiffer: a window whose size the platform defines differently (the
# Preferences window: macOS sizes its content rect, Win32 Blender its frame)
# is compared with the reference only when the sizes agree.
function Save-M4Shot($ctx, [string] $Name, [IntPtr] $Window = [IntPtr]::Zero, [switch] $SizeMayDiffer) {
    $path = Join-Path $ctx.OutDir "$Name.png"
    $ctx.Shots[$Name] = $path
    if ($ctx.WhatIf) { Write-M4 "[plan] capture '$Name'$(if ($Window -ne [IntPtr]::Zero) { ' (second window)' })"; return }
    if ($ctx.Failure) { return }
    $entry = [ordered]@{ kind = ''; what = "capture $Name"; path = $path; method = $null; size = $null; frames = $null; reference = $null }
    try {
        $entry.method = Capture-AlchemyWindow $ctx.Session $path -Window $(if ($Window -ne [IntPtr]::Zero) { $Window } else { $ctx.Window })
        $entry.size = [AlchemyGuiInput]::Size($path)
        $entry.frames = Get-AlchemyFrameCount $ctx.Session
        if ($entry.method -eq 'printwindow-blank') { $entry.ok = $false; $entry.detail = 'only a blank capture was possible (locked desktop and no present capture)' }
        $reference = Join-Path $ctx.ReferenceDir "$Name.png"
        if ($ctx.Target -eq 'native') {
            New-Item -ItemType Directory -Force $ctx.ReferenceDir | Out-Null
            Copy-Item -Force $path $reference
            $entry.reference = [ordered]@{ stored = $reference }
        } elseif (-not $ctx.NoReferenceCheck -and (Test-Path $reference) -and $SizeMayDiffer -and
                  (($entry.size -join 'x') -ne ([AlchemyGuiInput]::Size($reference) -join 'x'))) {
            $entry.reference = [ordered]@{ ok = $null; summary = "not compared: size $($entry.size -join 'x') vs the reference's $([AlchemyGuiInput]::Size($reference) -join 'x') (platform window sizing)" }
        } elseif (-not $ctx.NoReferenceCheck -and (Test-Path $reference)) {
            $cmp = Compare-AlchemyCapture $path $reference -Tolerance $ctx.RefTolerance -Percent $ctx.RefPercent
            $entry.reference = $cmp
            if ($cmp.ok -eq $false) { $entry.ok = $false; $entry.detail = "differs from the native reference: $($cmp.summary)" }
        }
    } catch { $entry.ok = $false; $entry.detail = $_.Exception.Message }
    Add-M4Record $ctx 'capture' $entry
}

function Format-M4Region($Region) { if ($Region) { "($($Region -join ','))" } else { '(whole image)' } }

# Expected visible change: at least -MinPercent % of the region's pixels
# changed (largest channel difference > 24) between two captures.
function Assert-M4Change($ctx, [string] $Before, [string] $After, $Region, [double] $MinPercent, [string] $Why, [int] $Threshold = 24) {
    $what = "${Why}: >= $MinPercent% of $(Format-M4Region $Region) changes$(if ($Threshold -ne 24) { " (channel diff > $Threshold)" }) from '$Before' to '$After'"
    if ($ctx.WhatIf) { Write-M4 "[plan] expect $what"; return }
    if ($ctx.Failure) { return }
    $entry = [ordered]@{ kind = ''; what = $what; ok = $false; measured = $null; detail = '' }
    try {
        $m = Measure-AlchemyChange $ctx.Shots[$Before] $ctx.Shots[$After] -Region $Region -Threshold $Threshold
        $entry.measured = $m
        $entry.ok = $m.percent -ge $MinPercent
        $entry.detail = "$($m.percent)% changed"
    } catch { $entry.detail = $_.Exception.Message }
    Add-M4Record $ctx 'check' $entry
}
# Expected sameness: at most -MaxPercent % of the region's pixels changed.
function Assert-M4Same($ctx, [string] $Before, [string] $After, $Region, [double] $MaxPercent, [string] $Why) {
    $what = "${Why}: <= $MaxPercent% of $(Format-M4Region $Region) differs between '$Before' and '$After'"
    if ($ctx.WhatIf) { Write-M4 "[plan] expect $what"; return }
    if ($ctx.Failure) { return }
    $entry = [ordered]@{ kind = ''; what = $what; ok = $false; measured = $null; detail = '' }
    try {
        $m = Measure-AlchemyChange $ctx.Shots[$Before] $ctx.Shots[$After] -Region $Region
        $entry.measured = $m
        $entry.ok = $m.percent -le $MaxPercent
        $entry.detail = "$($m.percent)% differs"
    } catch { $entry.detail = $_.Exception.Message }
    Add-M4Record $ctx 'check' $entry
}
# Waits until the target presented no frame for -QuietMs (EEVEE compiles its
# shaders asynchronously and redraws until done), at most -TimeoutMs.
# Native targets: sleeps -NativeMs.
function Wait-M4Quiet($ctx, [int] $QuietMs = 3000, [int] $TimeoutMs = 120000, [int] $NativeMs = 8000) {
    if ($ctx.WhatIf) { Write-M4 "[plan] wait until no frame was presented for $QuietMs ms (at most $TimeoutMs ms)"; return }
    if ($ctx.Failure) { return }
    $entry = [ordered]@{ kind = ''; what = "wait for $QuietMs ms without a frame"; frames_before = $null; frames_after = $null; ms = $null }
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    if (-not $ctx.Session.StatsPath) { Start-Sleep -Milliseconds $NativeMs }
    else {
        $count = Get-AlchemyFrameCount $ctx.Session; $entry.frames_before = $count; $changedAt = 0
        while ($clock.ElapsedMilliseconds - $changedAt -lt $QuietMs -and $clock.ElapsedMilliseconds -lt $TimeoutMs) {
            if ($ctx.Session.Process.HasExited) { $entry.ok = $false; $entry.detail = "the target exited (status $($ctx.Session.Process.ExitCode))"; break }
            Start-Sleep -Milliseconds 100
            $now = Get-AlchemyFrameCount $ctx.Session
            if ($now -ne $count) { $count = $now; $changedAt = $clock.ElapsedMilliseconds }
        }
        $entry.frames_after = $count
    }
    $entry.ms = $clock.ElapsedMilliseconds
    Add-M4Record $ctx 'step' $entry
}
# A condition evaluated now (-Plan: what it means in -WhatIf mode).
function Assert-M4($ctx, [string] $What, [scriptblock] $Condition, [string] $Detail = '') {
    if ($ctx.WhatIf) { Write-M4 "[plan] expect $What"; return }
    if ($ctx.Failure) { return }
    $entry = [ordered]@{ kind = ''; what = $What; ok = $false; detail = $Detail }
    try { $entry.ok = [bool](& $Condition) } catch { $entry.detail = $_.Exception.Message }
    Add-M4Record $ctx 'check' $entry
}
# Image content check: non-black share and distinct colours of a capture.
function Assert-M4Content($ctx, [string] $Name, [string] $Why, [double] $MinNonBlack = 0.5, [int] $MinColours = 24) {
    $what = "${Why}: '$Name' has >= $([int]($MinNonBlack * 100))% non-black pixels and >= $MinColours colours"
    if ($ctx.WhatIf) { Write-M4 "[plan] expect $what"; return }
    if ($ctx.Failure) { return }
    $entry = [ordered]@{ kind = ''; what = $what; ok = $false; detail = '' }
    try {
        $stats = [AlchemyGuiWin32]::PixelStats($ctx.Shots[$Name])
        $entry.ok = $stats[0] -ge $MinNonBlack -and $stats[2] -ge $MinColours
        $entry.detail = "non-black $([math]::Round($stats[0], 3)), colours $($stats[2])"
    } catch { $entry.detail = $_.Exception.Message }
    Add-M4Record $ctx 'check' $entry
}
function Add-M4Note($ctx, [string] $Text) {
    if ($ctx.WhatIf) { Write-M4 "[plan] note: $Text"; return }
    $ctx.Records.Add([ordered]@{ kind = 'note'; what = $Text })
}

# True when the capture shows the Quick Setup splash (its Continue button).
function Test-M4QuickSetup($ctx, [string] $Name) {
    if ($ctx.WhatIf) { return $true }
    $bitmap = New-Object System.Drawing.Bitmap $ctx.Shots[$Name]
    try {
        foreach ($p in $ctx.Layout.splash_continue_probes) {
            $c = $bitmap.GetPixel($p[0], $p[1])
            if ([math]::Abs($c.R - 84) -gt 14 -or [math]::Abs($c.G - 84) -gt 14 -or [math]::Abs($c.B - 84) -gt 14) { return $false }
        }
        $true
    } finally { $bitmap.Dispose() }
}

# 01's steps: dismiss the first-start splash. Quick Setup: its Continue
# button; the regular splash: Esc (a click there could open a web link).
function Invoke-M4DismissSplash($ctx, [switch] $Check) {
    Save-M4Shot $ctx 'splash'
    if (Test-M4QuickSetup $ctx 'splash') {
        if ($ctx.WhatIf) { Write-M4 '[plan] (the Quick Setup splash is detected by its grey Continue button; otherwise Esc dismisses the regular splash)' }
        Step-M4Click $ctx $ctx.Layout.splash_continue 'press Continue on the Quick Setup splash'
        # Blender 4.5 then opens the regular splash (Quick Setup saved the
        # preferences): Esc over the viewport closes it, and is harmless when
        # it did not open. Without this the next click only closes the splash.
        if (-not $ctx.WhatIf -and -not $ctx.Session.StatsPath) { Start-Sleep -Milliseconds 1500 }
        Step-M4Move $ctx $ctx.Layout.viewport_center 'hover the viewport' -NoWait
        Step-M4Key $ctx 'esc' 'dismiss the regular splash that follows Quick Setup'
    } else {
        Add-M4Note $ctx 'the regular splash is shown (no Quick Setup): dismissing it with Esc'
        Step-M4Move $ctx $ctx.Layout.viewport_center 'hover the viewport' -NoWait
        Step-M4Key $ctx 'esc' 'dismiss the splash'
    }
    Save-M4Shot $ctx 'splash_dismissed'
    if ($Check) {
        Assert-M4Change $ctx 'splash' 'splash_dismissed' $ctx.Layout.splash 20 'the splash closed'
        Assert-M4Content $ctx 'splash_dismissed' 'the full UI is drawn'
    }
    if (-not $ctx.Failure -and $ctx.Session) { $ctx.Session.SplashDismissed = $true }
}

# ---- runner ------------------------------------------------------------------------------------
# Resizes the session's main window (restored first when maximized, without
# activation) and waits for the redraw.
function Set-M4ClientSize($Session, [int] $Width, [int] $Height) {
    $client = Get-AlchemyClient $Session.Window
    if ($client.Width -eq $Width -and $client.Height -eq $Height) { return }
    $before = Get-AlchemyFrameCount $Session
    Set-AlchemyClientSize $Session.Window $Width $Height
    [void](Wait-Frame $Session -Since $(if ($null -ne $before) { $before } else { -1 }) -TimeoutMs 8000)
    $client = Get-AlchemyClient $Session.Window
    if ($client.Width -ne $Width -or $client.Height -ne $Height) {
        throw "the client area is $($client.Width)x$($client.Height) after resizing to ${Width}x${Height} (screen too small?)"
    }
}

# Runs one scenario body ({ param($ctx) ... }) and returns its result
# (status PASS / FAIL / PLANNED). Without -Session it launches the target,
# dismisses the splash (scenarios other than 01) and stops it afterwards.
function Invoke-M4Scenario([string] $Name, [string] $Title, [scriptblock] $Body, $Session = $null) {
    $target = Get-M4Option 'Target' 'alchemy'
    $whatIf = [bool](Get-M4Option 'WhatIf' $false)
    $script:AlchemyInputWhatIf = $whatIf
    $mode = if ($Session) { 'batch' } else { 'single' }
    $outRoot = Get-M4Option 'OutRoot' "$script:M4Artifacts/alchemy-blender/m4/$(Get-Date -Format 'yyyyMMdd-HHmmss')-$target"
    $referenceRoot = Get-M4Option 'ReferenceRoot' "$script:M4Artifacts/alchemy-blender/m4-reference"
    $ctx = [ordered]@{
        Name = $Name; Title = $Title; Target = $target; WhatIf = $whatIf; Mode = $mode
        OutDir = Join-Path $outRoot $Name; ReferenceDir = Join-Path (Join-Path $referenceRoot $mode) $Name
        Session = $Session; Window = [IntPtr]::Zero; Layout = $null
        Records = New-Object System.Collections.Generic.List[object]; Shots = @{}; Failure = $null
        NoReferenceCheck = [bool](Get-M4Option 'NoReferenceCheck' $false)
        RefTolerance = [int](Get-M4Option 'RefTolerance' 8); RefPercent = [double](Get-M4Option 'RefPercent' 3.0)
    }
    $owned = $false
    $width = [int](Get-M4Option 'ClientWidth' 0); $height = [int](Get-M4Option 'ClientHeight' 0)
    if ($width -le 0 -or $height -le 0) { $width = $script:M4DefaultClient[0]; $height = $script:M4DefaultClient[1] }
    Write-M4 "== $Name ($target$(if ($whatIf) { ', what-if' })): $Title"
    try {
        if ($whatIf) {
            $ctx.Layout = Get-BlenderLayout $width $height 1.0
            $launch = if ($target -eq 'native') { 'launch native Blender (--no-window-focus, fresh BLENDER_USER_RESOURCES), wait for its window' }
                      else { 'launch Blender.app under alchemy-fx (ALCHEMY_GUI_NO_ACTIVATE=1, present stats + capture), wait for its window and first frame' }
            $resize = "; resize the client area to ${width}x${height} (SetWindowPos, no activation)"
            Write-M4 "[plan] $(if ($Session) { 'use the running session' } else { "$launch$resize" }); client ${width}x${height} at 96 DPI assumed"
            Write-M4 '[plan] activate the main window before input (Activate-AlchemyWindow)'
            if ($Name -ne '01_dismiss_splash' -and -not $Session) { Write-M4 '[plan] prelude: dismiss the splash'; $null = Invoke-M4DismissSplash $ctx }
        } else {
            New-Item -ItemType Directory -Force $ctx.OutDir | Out-Null
            if (-not $Session) {
                $Session = Start-AlchemyGuiTarget -Target $target -OutDir $ctx.OutDir -Exe (Get-M4Option 'Exe' '') -App (Get-M4Option 'App' '') `
                    -RuntimeDir (Get-M4Option 'RuntimeDir' '') -NativeExe (Get-M4Option 'NativeExe' '')
                $Session.SplashDismissed = $false
                $owned = $true
                Set-M4ClientSize $Session $width $height
            }
            $ctx.Session = $Session
            $ctx.Window = $Session.Window
            [void](Activate-AlchemyWindow $ctx.Window)
            $client = Get-AlchemyClient $ctx.Window
            $ctx.Layout = Get-BlenderLayout $client.Width $client.Height $client.Scale
            if ($Name -ne '01_dismiss_splash' -and -not $Session.SplashDismissed) {
                $prelude = $ctx.Shots.Clone()
                $null = Invoke-M4DismissSplash $ctx
                $ctx.Shots = $prelude
            }
        }
        if (-not $ctx.Failure -or $whatIf) { $null = & $Body $ctx }
    } catch {
        if (-not $ctx.Failure) { $ctx.Failure = "error: $($_.Exception.Message)" }
        $ctx.Records.Add([ordered]@{ kind = 'error'; what = $_.Exception.Message; at = "$($_.InvocationInfo.ScriptName):$($_.InvocationInfo.ScriptLineNumber)" })
    }
    $stopped = $null
    if ($owned -and -not [bool](Get-M4Option 'KeepRunning' $false)) { $stopped = Stop-AlchemyGuiTarget $Session }
    $status = if ($ctx.Failure) { 'FAIL' } elseif ($whatIf) { 'PLANNED' } else { 'PASS' }
    $result = [ordered]@{
        scenario = $Name; title = $Title; target = $target; mode = $mode; status = $status; failure = $ctx.Failure
        window = $(if ($ctx.Layout) { "$($ctx.Layout.width)x$($ctx.Layout.height) scale $($ctx.Layout.scale)" } else { $null })
        records = $ctx.Records; out_dir = $ctx.OutDir; reference_dir = $ctx.ReferenceDir; process = $stopped
        dialogs = $(if ($Session -and $Session.Dialogs) { [object[]]$Session.Dialogs } else { @() })
    }
    if (-not $whatIf) {
        New-Item -ItemType Directory -Force $ctx.OutDir | Out-Null
        $result | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path $ctx.OutDir 'result.json')
    }
    switch ($status) {
        'PASS' { Write-M4 "PASS $Name ($target): $(@($ctx.Records | Where-Object { $_.kind -eq 'check' }).Count) checks; $($ctx.OutDir)" }
        'FAIL' { Write-M4 "FAIL $Name ($target): $($ctx.Failure); $($ctx.OutDir)" }
        default { Write-M4 "PLANNED $Name (what-if: no input injected)" }
    }
    $result
}

# Exit code of a standalone scenario script.
function Get-M4ExitCode($Result) { if ($Result.status -eq 'FAIL') { 1 } else { 0 } }
