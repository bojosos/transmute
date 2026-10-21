# M4 scenario 05: selection and mode switching. A click on empty floor
# deselects everything, a click on the cube selects it again, Tab enters
# Edit Mode and Tab returns to Object Mode. Expected: the outliner's Cube row
# highlight and the cube's outline change with the selection (outliner rows
# >= 2%, cube region >= 0.2%), the viewport header and mesh overlay change in
# Edit Mode (>= 1% each), and Object Mode looks as before (<= 1% differs).
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
$result = Invoke-M4Scenario '05_select_and_edit' 'click the cube, Tab into Edit Mode and back' -Session $Session -Body {
    param($ctx)
    $L = $ctx.Layout
    Step-M4Move $ctx $L.viewport_center 'hover the cube' -NoWait
    Save-M4Shot $ctx 'before'
    Step-M4Click $ctx $L.viewport_empty 'click empty floor (deselect all)'
    Save-M4Shot $ctx 'deselected'
    Assert-M4Change $ctx 'before' 'deselected' $L.outliner_rows 0.3 'the outliner Cube row lost its highlight'
    Assert-M4Change $ctx 'before' 'deselected' $L.viewport 0.05 'the cube lost its selection outline'
    Step-M4Click $ctx $L.viewport_center 'click the cube'
    Save-M4Shot $ctx 'selected'
    Assert-M4Change $ctx 'deselected' 'selected' $L.outliner_rows 0.3 'the outliner Cube row is highlighted again'
    Assert-M4Change $ctx 'deselected' 'selected' $L.viewport 0.05 'the cube has its selection outline again'
    Step-M4Key $ctx 'tab' 'enter Edit Mode'
    Save-M4Shot $ctx 'edit_mode'
    Assert-M4Change $ctx 'selected' 'edit_mode' $L.viewport_header 1 'the header shows Edit Mode'
    Assert-M4Change $ctx 'selected' 'edit_mode' $L.cube_region 1 'the mesh edit overlay is drawn'
    Step-M4Key $ctx 'tab' 'return to Object Mode'
    Save-M4Shot $ctx 'object_mode'
    Assert-M4Same $ctx 'selected' 'object_mode' $L.viewport 1 'Object Mode looks as before'
}
if ($Session) { return $result }
exit (Get-M4ExitCode $result)
