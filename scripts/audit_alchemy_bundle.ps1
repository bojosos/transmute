param(
    [Parameter(Mandatory=$true)][string]$Bundle,
    [string]$Alchemy = "$PSScriptRoot/../build/alchemy-expansion/Release/alchemy.exe",
    [string]$OutputDirectory = "$PSScriptRoot/../artifacts/alchemy-blender/report",
    [string[]]$RuntimeDirectory = @(),
    [string[]]$DylibDirectory = @()
)
$ErrorActionPreference = 'Stop'
$bundlePath = (Resolve-Path -LiteralPath $Bundle).Path
$alchemyPath = (Resolve-Path -LiteralPath $Alchemy).Path
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$arguments = @('doctor', '--deps', '--scan-bundle', '--json')
foreach ($directory in $RuntimeDirectory) { $arguments += @('--runtime-dir', (Resolve-Path -LiteralPath $directory).Path) }
foreach ($directory in $DylibDirectory) { $arguments += @('--dylib-dir', (Resolve-Path -LiteralPath $directory).Path) }
$arguments += $bundlePath
$reportText = (& $alchemyPath @arguments) -join "`n"
if ($LASTEXITCODE -notin @(0, 1)) { throw "Alchemy inspection failed: $LASTEXITCODE" }
$report = $reportText | ConvertFrom-Json
$reportText | Set-Content -LiteralPath "$OutputDirectory/dependencies.json" -Encoding utf8
$dependencies = @($report.images | ForEach-Object { $_.dependencies } | Where-Object { $null -ne $_ })
$imports = @($report.images | ForEach-Object { $_.imports } | Where-Object { $null -ne $_ })
$missing = @($imports | Where-Object { !$_.weak -and $_.status -in @('unresolved','invalid-ordinal') })
$summary = [ordered]@{
    root = $report.root
    runtime_directories = $RuntimeDirectory
    dylib_directories = $DylibDirectory
    images = @($report.images).Count
    malformed_or_unsupported_images = @($report.images | Where-Object { $_.error }).Count
    imports = $imports.Count
    unresolved_strong_imports = $missing.Count
    missing_libraries = @($dependencies | Where-Object { $_.status -eq 'missing' } |
        Select-Object -ExpandProperty identity -Unique | Sort-Object)
    unresolved_by_library = @($missing | Group-Object library | Sort-Object Count -Descending |
        ForEach-Object { [ordered]@{library=$_.Name; count=$_.Count; symbols=@($_.Group.symbol | Sort-Object -Unique)} })
}
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath "$OutputDirectory/summary.json" -Encoding utf8
[pscustomobject]$summary | Select-Object root,images,imports,unresolved_strong_imports,malformed_or_unsupported_images | Format-List
