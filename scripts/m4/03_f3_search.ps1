# M4 scenario 03: F3 menu search over the viewport, typing 'cube', Esc.
# Expected: the search popup appears around the mouse (>= 3% of the popup
# region changes), typing filters the results (>= 0.3% changes again), and
# Esc restores the viewport (<= 1% differs). Options: see m4_common.ps1.
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
$result = Invoke-M4Scenario '03_f3_search' "F3 menu search, type 'cube', Esc" -Session $Session -Body {
    param($ctx)
    $L = $ctx.Layout
    Step-M4Move $ctx $L.viewport_center 'hover the viewport centre' -NoWait
    Save-M4Shot $ctx 'before'
    Step-M4Key $ctx 'f3' 'open the menu search'
    Save-M4Shot $ctx 'search_open'
    Assert-M4Change $ctx 'before' 'search_open' $L.search_popup 3 'the search popup opened at the mouse'
    Step-M4Text $ctx 'cube' 'type a query'
    Save-M4Shot $ctx 'search_typed'
    Assert-M4Change $ctx 'search_open' 'search_typed' $L.search_popup 0.3 'the query and its results are shown'
    Step-M4Key $ctx 'esc' 'cancel the search'
    Save-M4Shot $ctx 'search_closed'
    Assert-M4Same $ctx 'before' 'search_closed' $L.search_popup 1 'Esc closed the popup'
}
if ($Session) { return $result }
exit (Get-M4ExitCode $result)
