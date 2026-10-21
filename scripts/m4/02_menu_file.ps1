# M4 scenario 02: the File menu opens on a click, is captured, and closes
# with Esc. Expected: the area below the menu bar changes (>= 5%) while the
# menu is open and returns to the original picture (<= 1% differs) after Esc.
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
$result = Invoke-M4Scenario '02_menu_file' 'open the File menu, capture it, close it with Esc' -Session $Session -Body {
    param($ctx)
    $L = $ctx.Layout
    Step-M4Move $ctx $L.viewport_center 'park the mouse in the viewport' -NoWait
    Save-M4Shot $ctx 'before'
    # Hover first: Blender activates a button on the move, then the press opens it.
    Step-M4Move $ctx $L.file_menu 'hover the File menu'
    Step-M4Click $ctx $L.file_menu 'open the File menu'
    Save-M4Shot $ctx 'menu_open'
    Assert-M4Change $ctx 'before' 'menu_open' $L.file_dropdown 5 'the File menu dropped down'
    Step-M4Key $ctx 'esc' 'close the menu'
    Step-M4Move $ctx $L.viewport_center 'park the mouse in the viewport'
    Save-M4Shot $ctx 'menu_closed'
    Assert-M4Same $ctx 'before' 'menu_closed' $L.file_dropdown 1 'Esc closed the menu'
}
if ($Session) { return $result }
exit (Get-M4ExitCode $result)
