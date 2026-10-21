# M4 scenario 09: viewport shading modes through the 3D view header buttons:
# Solid -> Material Preview (EEVEE) -> Rendered (EEVEE) -> Solid. Each EEVEE
# mode compiles its shaders asynchronously, so every switch waits until no
# frame was presented for 3 s. Expected: Material Preview and Rendered each
# change the viewport (>= 5%; the world HDRI / lit scene replace the grey
# Solid background), and Solid looks as before afterwards (<= 1% differs).
# Runs last in run_all.ps1: EEVEE under Alchemy is the riskiest path (sampler
# argument buffers and framebuffer fetch are not implemented yet).
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
$result = Invoke-M4Scenario '09_viewport_shading' 'Solid, Material Preview, Rendered, Solid via the header buttons' -Session $Session -Body {
    param($ctx)
    $L = $ctx.Layout
    Step-M4Move $ctx $L.viewport_center 'park the mouse in the viewport' -NoWait
    Save-M4Shot $ctx 'solid'
    Step-M4Click $ctx $L.shading_material 'switch to Material Preview' -NoWait
    Wait-M4Quiet $ctx -NativeMs 30000
    Step-M4Move $ctx $L.viewport_center 'park the mouse in the viewport' -NoWait
    Save-M4Shot $ctx 'material_preview'
    Assert-M4Change $ctx 'solid' 'material_preview' $L.viewport 0.5 'Material Preview replaced the Solid view'
    Assert-M4Content $ctx 'material_preview' 'Material Preview drew a picture'
    Step-M4Click $ctx $L.shading_rendered 'switch to Rendered' -NoWait
    Wait-M4Quiet $ctx -NativeMs 30000
    Step-M4Move $ctx $L.viewport_center 'park the mouse in the viewport' -NoWait
    Save-M4Shot $ctx 'rendered'
    # The default world (grey 0.05) is close to the Solid background: a small
    # channel threshold sees the lit background.
    Assert-M4Change $ctx 'solid' 'rendered' $L.viewport 20 'Rendered replaced the Solid view' -Threshold 2
    Assert-M4Content $ctx 'rendered' 'Rendered drew a picture'
    Step-M4Click $ctx $L.shading_solid 'switch back to Solid' -NoWait
    Wait-M4Quiet $ctx -QuietMs 1500
    Step-M4Move $ctx $L.viewport_center 'park the mouse in the viewport' -NoWait
    Save-M4Shot $ctx 'solid_again'
    Assert-M4Same $ctx 'solid' 'solid_again' $L.viewport 1 'Solid looks as before'
}
if ($Session) { return $result }
exit (Get-M4ExitCode $result)
