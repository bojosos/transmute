# M4 scenario 04: viewport navigation. Middle-button drag orbits, the wheel
# zooms, numpad '.' frames the selection (View Selected, smooth view).
# Expected: each step changes the viewport (orbit >= 10%, zoom >= 5%, view
# selected >= 5% of its pixels). Options: see m4_common.ps1.
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
$result = Invoke-M4Scenario '04_viewport_orbit_zoom' 'middle-drag orbit, wheel zoom, numpad . view selected' -Session $Session -Body {
    param($ctx)
    $L = $ctx.Layout
    $c = $L.viewport_center
    $s = $L.scale
    Step-M4Move $ctx $c 'hover the viewport centre' -NoWait
    Save-M4Shot $ctx 'before'
    Step-M4Drag $ctx $c @([int]($c[0] + 220 * $s), [int]($c[1] + 60 * $s)) 'orbit the view' -Button middle -Steps 24
    Save-M4Shot $ctx 'orbited'
    Assert-M4Change $ctx 'before' 'orbited' $L.viewport 1 'the view orbited'
    Step-M4Wheel $ctx $c 3 'zoom in'
    Save-M4Shot $ctx 'zoomed'
    Assert-M4Change $ctx 'orbited' 'zoomed' $L.viewport 1 'the view zoomed'
    Step-M4Key $ctx 'num.' 'View Selected (numpad .)'
    Save-M4Shot $ctx 'view_selected'
    Assert-M4Change $ctx 'zoomed' 'view_selected' $L.viewport 1 'the view framed the selected cube'
    # Zooming out around the view centre leaves the framed cube small and
    # centred again for 05 (a click at the viewport centre selects it).
    Step-M4Wheel $ctx $c -8 'zoom out'
    Save-M4Shot $ctx 'zoomed_out'
    Assert-M4Change $ctx 'view_selected' 'zoomed_out' $L.viewport 1 'the view zoomed out'
}
if ($Session) { return $result }
exit (Get-M4ExitCode $result)
