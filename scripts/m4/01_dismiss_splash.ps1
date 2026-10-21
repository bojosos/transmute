# M4 scenario 01: the first-start Quick Setup splash closes with its
# Continue button (a real click). Expected: the splash region changes (>= 20%
# of its pixels) and the full UI is drawn. Options: see m4_common.ps1.
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
$result = Invoke-M4Scenario '01_dismiss_splash' 'click Continue on the Quick Setup splash' -Session $Session -Body {
    param($ctx)
    Invoke-M4DismissSplash $ctx -Check
}
if ($Session) { return $result }
exit (Get-M4ExitCode $result)
