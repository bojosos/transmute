# M4 scenario 07: a second window. Edit > Preferences opens the Preferences
# window (the Edit menu's 'P' accelerator; when no window appears, F3 search
# "preferences" + Enter), it is captured, then closed (WM_CLOSE, as its
# title-bar close button). Expected: the Edit menu drops down (>= 5%), a new
# top-level window of the process appears, presents frames (alchemy) and shows
# UI content, closes, and the main window is unchanged afterwards (<= 2%).
# Options: see m4_common.ps1.
param(
    [ValidateSet('alchemy', 'native')] [string] $Target = 'alchemy',
    [switch] $WhatIf,
    [string] $OutRoot = '', [string] $ReferenceRoot = '',
    [string] $Exe = '', [string] $App = '', [string] $RuntimeDir = '', [string] $NativeExe = '',
    [int] $ClientWidth = 0, [int] $ClientHeight = 0,
    [switch] $KeepRunning, [switch] $NoReferenceCheck, [int] $RefTolerance = 8, [double] $RefPercent = 3.0,
    $Session = $null
)
. "$PSScriptRoot/m4_common.ps1"
$result = Invoke-M4Scenario '07_preferences_window' 'Edit > Preferences opens a second window; capture it; close it' -Session $Session -Body {
    param($ctx)
    $L = $ctx.Layout
    $known = @(Get-AlchemyWindows $ctx.Session | ForEach-Object { $_.ToInt64() })
    # The first visible top-level window that did not exist before.
    $newWindow = {
        param([int] $TimeoutMs)
        if ($ctx.WhatIf) { Write-M4 "[plan] wait up to $TimeoutMs ms for a new top-level window of the process"; return [IntPtr]::Zero }
        $clock = [System.Diagnostics.Stopwatch]::StartNew()
        while ($clock.ElapsedMilliseconds -lt $TimeoutMs) {
            $fresh = @(Get-AlchemyWindows $ctx.Session | Where-Object { $known -notcontains $_.ToInt64() })
            if ($fresh.Count) { return $fresh[0] }
            Start-Sleep -Milliseconds 100
        }
        [IntPtr]::Zero
    }
    Step-M4Move $ctx $L.viewport_center 'park the mouse in the viewport' -NoWait
    Save-M4Shot $ctx 'before'
    Step-M4Move $ctx $L.edit_menu 'hover the Edit menu'
    Step-M4Click $ctx $L.edit_menu 'open the Edit menu'
    Save-M4Shot $ctx 'edit_menu'
    Assert-M4Change $ctx 'before' 'edit_menu' $L.edit_dropdown 5 'the Edit menu dropped down'
    Step-M4Key $ctx 'p' 'Preferences... (menu accelerator P)' -NoWait
    $prefs = & $newWindow 6000
    if (-not $ctx.WhatIf -and $prefs -eq [IntPtr]::Zero -and -not $ctx.Failure) {
        Add-M4Note $ctx "the 'P' accelerator opened no window: closing the menu and using F3 search"
        Step-M4Key $ctx 'esc' 'close the Edit menu'
        Step-M4Key $ctx 'f3' 'open the menu search'
        Step-M4Text $ctx 'preferences' 'search for Preferences'
        Step-M4Key $ctx 'enter' 'run Edit > Preferences' -NoWait
        $prefs = & $newWindow 8000
    }
    if ($ctx.WhatIf) { Write-M4 "[plan] fallback when no window appears: key esc; key f3; type 'preferences'; key enter; wait again" }
    Assert-M4 $ctx 'a second (Preferences) top-level window opened' { $prefs -ne [IntPtr]::Zero }
    if (-not $ctx.WhatIf -and -not $ctx.Failure) {
        Add-M4Note $ctx "Preferences window 0x$('{0:X}' -f $prefs.ToInt64()) class $([AlchemyGuiWin32]::ClassName($prefs)) title '$([AlchemyGuiWin32]::Title($prefs))'"
        $wait = Wait-Frame $ctx.Session -Since 0 -Window $prefs -TimeoutMs 8000
        Assert-M4 $ctx 'the Preferences window presented frames (alchemy)' { $ctx.Target -eq 'native' -or $wait.advanced }
    } elseif ($ctx.WhatIf) { Write-M4 '[plan] wait for the Preferences window''s first presented frame (per-window count in the stats file)' }
    # Blender asks for a 660x520 window: macOS (Alchemy) gives the content
    # that size, Win32 Blender the whole frame (client 660x489).
    Save-M4Shot $ctx 'preferences' -Window $prefs -SizeMayDiffer
    Assert-M4Content $ctx 'preferences' 'the Preferences window drew its UI'
    Invoke-M4Step $ctx 'close the Preferences window (WM_CLOSE)' { Close-AlchemyWindow $prefs } -NoWait
    Assert-M4 $ctx 'the Preferences window closed and the main window is alive' {
        $clock = [System.Diagnostics.Stopwatch]::StartNew()
        while ([AlchemyGuiWin32]::Alive($prefs) -and $clock.ElapsedMilliseconds -lt 5000) { Start-Sleep -Milliseconds 100 }
        -not [AlchemyGuiWin32]::Alive($prefs) -and [AlchemyGuiWin32]::Alive($ctx.Window)
    }
    Step-M4Move $ctx $L.viewport_center 'park the mouse in the viewport'
    Save-M4Shot $ctx 'after_close'
    Assert-M4Same $ctx 'before' 'after_close' $null 2 'the main window looks as before'
}
if ($Session) { return $result }
exit (Get-M4ExitCode $result)
