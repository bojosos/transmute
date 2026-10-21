# M4 scenario 06: window resize (SetWindowPos, no input, no activation).
# Expected: a frame is presented after the resize, the capture has the new
# client size and real content (not blank), and resizing back restores the
# original picture (<= 5% of the pixels differ; area splits are rescaled
# proportionally, so edges may move by a pixel). Options: see m4_common.ps1.
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
$result = Invoke-M4Scenario '06_resize_window' 'resize the window and verify the redraw' -Session $Session -Body {
    param($ctx)
    $L = $ctx.Layout
    $width = $L.width; $height = $L.height
    $smaller = @([int]($width - 320 * $L.scale), [int]($height - 200 * $L.scale))
    Step-M4Move $ctx $L.viewport_center 'park the mouse in the viewport' -NoWait
    Save-M4Shot $ctx 'before'
    Invoke-M4Step $ctx "resize the client area to $($smaller -join 'x')" { Set-AlchemyClientSize $ctx.Window $smaller[0] $smaller[1] }
    Save-M4Shot $ctx 'resized'
    Assert-M4 $ctx "the capture has the new client size $($smaller -join 'x')" {
        $size = [AlchemyGuiInput]::Size($ctx.Shots['resized']); $size[0] -eq $smaller[0] -and $size[1] -eq $smaller[1] }
    Assert-M4 $ctx 'a frame was presented after the resize (alchemy)' {
        $step = @($ctx.Records | Where-Object { $_.kind -eq 'step' })[-1]; $ctx.Target -eq 'native' -or $step.redraw -eq $true }
    Assert-M4Content $ctx 'resized' 'the resized window shows the UI'
    Invoke-M4Step $ctx "resize back to ${width}x${height}" { Set-AlchemyClientSize $ctx.Window $width $height }
    Save-M4Shot $ctx 'restored'
    Assert-M4Same $ctx 'before' 'restored' $null 5 'the original layout is back'
}
if ($Session) { return $result }
exit (Get-M4ExitCode $result)
