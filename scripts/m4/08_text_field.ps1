# M4 scenario 08: text input. A double-click on "Cube" in the outliner
# starts renaming (the whole name is selected), typing "Hero" replaces it and
# Enter commits. Expected: the row turns into a text field (>= 1% of the
# outliner rows change), the typed text shows (>= 0.5%), and after Enter the
# outliner and the properties editor show the new name (>= 0.5% each,
# compared with the start). Options: see m4_common.ps1.
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
$result = Invoke-M4Scenario '08_text_field' "rename the cube in the outliner by double-click and typing" -Session $Session -Body {
    param($ctx)
    $L = $ctx.Layout
    Step-M4Move $ctx $L.viewport_center 'park the mouse in the viewport' -NoWait
    Save-M4Shot $ctx 'before'
    Step-M4Click $ctx $L.outliner_cube 'double-click the Cube name in the outliner' -Count 2
    Save-M4Shot $ctx 'editing'
    Assert-M4Change $ctx 'before' 'editing' $L.outliner_rows 1 'the name became a text field'
    Step-M4Text $ctx 'Hero' 'type the new name'
    Save-M4Shot $ctx 'typed'
    Assert-M4Change $ctx 'editing' 'typed' $L.outliner_rows 0.5 'the typed text is shown'
    Step-M4Key $ctx 'enter' 'commit the name'
    Step-M4Move $ctx $L.viewport_center 'park the mouse in the viewport'
    Save-M4Shot $ctx 'renamed'
    Assert-M4Change $ctx 'before' 'renamed' $L.outliner_rows 0.5 'the outliner shows the new name'
    Assert-M4Change $ctx 'before' 'renamed' $L.properties_name 0.5 'the properties editor shows the new name'
    # 'H' (hide) or 'E' reaching the viewport would change the cube.
    # (from 'editing': the double-click itself selects the cube)
    Assert-M4Same $ctx 'editing' 'typed' $L.cube_region 1 'the keys went to the text field, not the viewport'
}
if ($Session) { return $result }
exit (Get-M4ExitCode $result)
