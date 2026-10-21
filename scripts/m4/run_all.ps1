# Runs the M4 input scenarios 01..08 in ONE Blender session (in order: each
# leaves Blender as the next expects it) and prints a PASS/FAIL table.
# References are kept per mode (batch here, single for standalone scenario
# scripts) because the batch scenarios start from the previous one's state.
#
#   native references:  scripts/m4/run_all.ps1 -Target native
#   Alchemy vs them:    scripts/m4/run_all.ps1 -Target alchemy
#   plan only:          scripts/m4/run_all.ps1 -WhatIf
# -Only 02,05 runs a subset (01 always runs first: the splash). Real input
# needs an unlocked desktop that nothing else uses (the phase-2 driver).
param(
    [ValidateSet('alchemy', 'native')] [string] $Target = 'alchemy',
    [switch] $WhatIf,
    [string] $OutRoot = '', [string] $ReferenceRoot = '',
    [string] $Exe = '', [string] $App = '', [string] $RuntimeDir = '', [string] $NativeExe = '',
    [int] $ClientWidth = 0, [int] $ClientHeight = 0,
    [switch] $KeepRunning, [switch] $NoReferenceCheck, [int] $RefTolerance = 8, [double] $RefPercent = 3.0,
    [string[]] $Only = @()
)
. "$PSScriptRoot/m4_common.ps1"
if (-not $OutRoot) { $OutRoot = "$script:M4Artifacts/alchemy-blender/m4/$(Get-Date -Format 'yyyyMMdd-HHmmss')-$Target-batch" }
$scenarios = @(Get-ChildItem $PSScriptRoot -Filter '0*_*.ps1' | Sort-Object Name)
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if ($Only.Count) { $scenarios = @($scenarios | Where-Object { $n = $_.BaseName; $n -like '01_*' -or ($Only | Where-Object { $n -like "$_*" }) }) }

$common = @{ Target = $Target; OutRoot = $OutRoot; RefTolerance = $RefTolerance; RefPercent = $RefPercent }
foreach ($name in 'ReferenceRoot', 'Exe', 'App', 'RuntimeDir', 'NativeExe') { $v = Get-Variable -Name $name -ValueOnly; if ($v) { $common[$name] = $v } }
if ($ClientWidth -gt 0 -and $ClientHeight -gt 0) { $common.ClientWidth = $ClientWidth; $common.ClientHeight = $ClientHeight }
if ($NoReferenceCheck) { $common.NoReferenceCheck = $true }
if ($WhatIf) { $common.WhatIf = $true }

$session = $null
$results = New-Object System.Collections.Generic.List[object]
$exitCode = 0
try {
    if ($WhatIf) {
        Write-Host "[plan] launch $Target Blender once (ALCHEMY_GUI_NO_ACTIVATE=1, present stats + capture); run $($scenarios.Count) scenarios in it"
        # A stand-in session so each scenario plans as part of the batch.
        $session = [ordered]@{ Target = $Target; SplashDismissed = $false; Dialogs = @(); WhatIf = $true }
    } else {
        $sessionDir = Join-Path $OutRoot 'session'
        $session = Start-AlchemyGuiTarget -Target $Target -OutDir $sessionDir -Exe $Exe -App $App -RuntimeDir $RuntimeDir -NativeExe $NativeExe
        $session.SplashDismissed = $false
        if ($ClientWidth -le 0 -or $ClientHeight -le 0) { $ClientWidth = $script:M4DefaultClient[0]; $ClientHeight = $script:M4DefaultClient[1] }
        Set-M4ClientSize $session $ClientWidth $ClientHeight
    }
    foreach ($file in $scenarios) {
        $result = & $file.FullName @common -Session $session
        $results.Add($result)
        if ($result.status -eq 'FAIL') { $exitCode = 1 }
        if (-not $WhatIf -and $session.Process.HasExited) {
            Write-Host "Blender exited (status $($session.Process.ExitCode)) during $($file.BaseName); stopping"
            $exitCode = 1
            break
        }
    }
} finally {
    if ($session -and -not $WhatIf -and -not $KeepRunning) {
        $stopped = Stop-AlchemyGuiTarget $session
        if ($stopped) { Write-Host "session: presented frames $($stopped.presented_frames); stop $($stopped.stop_point.kind) $($stopped.stop_point.line)" }
    }
}
Write-Host ''
Write-Host ("{0,-26} {1,-8} {2}" -f 'scenario', 'status', 'detail')
foreach ($r in $results) { Write-Host ("{0,-26} {1,-8} {2}" -f $r.scenario, $r.status, $(if ($r.failure) { $r.failure } else { '' })) }
if (-not $WhatIf) {
    New-Item -ItemType Directory -Force $OutRoot | Out-Null
    $results | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path $OutRoot 'results.json')
    Write-Host "results: $OutRoot"
}
exit $exitCode
