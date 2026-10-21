<#
.SYNOPSIS
  Installs the .app from a macOS disk image, or from a zip archive, into the
  Alchemy root, keeping its symbolic links.

.DESCRIPTION
  7-Zip reads the UDIF/HFS+ image but cannot create Windows symbolic links
  without a privilege; it leaves empty files instead. This script extracts the
  bundle WITHOUT its link entries, recovers every link's target text from the
  image (`7z x -so` prints a link entry's target), and hands both to
  `alchemy install --links`, which recreates the links (native symlink,
  junction or hard link) and records them in the root's link manifest.

  A zip (an app downloaded as `<Name>-darwin-arm64.zip`, made by `ditto`)
  works the same way: its link entries carry the Unix mode in the zip's
  external attributes, and its .app sits at the top of the archive instead of
  inside a volume folder.

  7-Zip is an external tool the user supplies; no image content is
  redistributed. Nothing is installed when any link cannot be represented.

.EXAMPLE
  scripts/alchemy_install_dmg.ps1 -Dmg $HOME\Downloads\QLab.dmg -Replace
.EXAMPLE
  scripts/alchemy_install_dmg.ps1 -Dmg $HOME\Downloads\VSCode-darwin-arm64.zip -Root <root>
#>
param(
    [Parameter(Mandatory = $true)][string]$Dmg,
    [string]$Root = '',
    [string]$App = '',
    [string]$Alchemy = "$PSScriptRoot/../build-q/Release/alchemy.exe",
    [string]$SevenZip = 'C:\Program Files\7-Zip\7z.exe',
    [string]$Staging = '',
    [switch]$Replace,
    [switch]$KeepStaging
)
$ErrorActionPreference = 'Stop'

$dmgPath = (Resolve-Path -LiteralPath $Dmg).Path
$alchemyPath = (Resolve-Path -LiteralPath $Alchemy).Path
if (-not (Test-Path -LiteralPath $SevenZip)) { throw "7-Zip not found at $SevenZip (pass -SevenZip)" }
if (-not $Staging) { $Staging = Join-Path ([IO.Path]::GetTempPath()) ("alchemy-dmg-" + [guid]::NewGuid().ToString('N').Substring(0, 12)) }

# 1. Catalog: path, folder flag and Unix mode of every entry.
$listing = & $SevenZip l -slt -- $dmgPath
if ($LASTEXITCODE -ne 0) { throw "7-Zip cannot list $dmgPath (exit $LASTEXITCODE)" }
$entries = New-Object System.Collections.Generic.List[object]
$current = $null
foreach ($line in $listing) {
    if ($line.StartsWith('Path = ')) {
        if ($current) { $entries.Add([pscustomobject]$current) }
        $current = @{ Path = $line.Substring(7); Folder = $false; Mode = '' }
    } elseif ($current -and $line -eq 'Folder = +') { $current.Folder = $true }
    elseif ($current -and $line.StartsWith('Mode = ')) { $current.Mode = $line.Substring(7) }
    # A zip lists the Unix mode after the DOS attribute letters: "Attributes = D drwxr-xr-x".
    elseif ($current -and -not $current.Mode -and $line -match '^Attributes = .*?([-dlbcps][-rwxsStT]{9})$') { $current.Mode = $Matches[1] }
}
if ($current) { $entries.Add([pscustomobject]$current) }
# The first records describe the image container itself, not the volume.
$entries = @($entries | Where-Object { $_.Path -and $_.Path -ne $dmgPath -and $_.Path -notmatch '\.(dmg|hfs|apfs)$' })

# 2. Select the bundle: a top-level .app folder of the volume (of the archive, for a zip).
$isZip = [IO.Path]::GetExtension($dmgPath) -ieq '.zip'
$appPattern = if ($isZip) { '^[^\\]+\.app$' } else { '^[^\\]+\\[^\\]+\.app$' }
$apps = @($entries | Where-Object { $_.Folder -and $_.Path -match $appPattern })
if ($App) { $apps = @($apps | Where-Object { (Split-Path $_.Path -Leaf) -ieq $App -or (Split-Path $_.Path -Leaf) -ieq "$App.app" }) }
if ($apps.Count -ne 1) {
    throw "expected exactly one top-level .app in the image, found: " + (($apps | ForEach-Object Path) -join ', ') + " (use -App)"
}
$appEntry = $apps[0].Path
$appName = Split-Path $appEntry -Leaf
$prefix = $appEntry + '\'
$links = @($entries | Where-Object { $_.Path.StartsWith($prefix) -and $_.Mode.StartsWith('l') })
Write-Host "Image: $dmgPath"
Write-Host "Bundle: $appName ($($links.Count) symbolic links)"

$null = New-Item -ItemType Directory -Force -Path $Staging
try {
    # 3. Extract the bundle without its link entries.
    $exclude = Join-Path $Staging 'exclude.txt'
    [IO.File]::WriteAllLines($exclude, [string[]]@($links | ForEach-Object Path), (New-Object Text.UTF8Encoding $false))
    $out = Join-Path $Staging 'volume'
    & $SevenZip x -y -bso0 -bsp0 "-o$out" "-x@$exclude" -- $dmgPath "$appEntry\*" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7-Zip extraction failed (exit $LASTEXITCODE)" }

    # 4. Recover each link's target text from the image.
    $records = New-Object System.Collections.Generic.List[string]
    $records.Add("# links recovered from $(Split-Path $dmgPath -Leaf) by alchemy_install_dmg.ps1")
    $targetFile = Join-Path $Staging 'target.bin'
    foreach ($link in $links) {
        cmd /c "`"$SevenZip`" x -so -- `"$dmgPath`" `"$($link.Path)`" > `"$targetFile`" 2>nul"
        if ($LASTEXITCODE -ne 0) { throw "cannot read link target of $($link.Path) (exit $LASTEXITCODE)" }
        $target = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($targetFile))
        if (-not $target -or $target.Contains("`n") -or $target.Contains("`t")) { throw "unsupported link target for $($link.Path)" }
        $relative = $link.Path.Substring($prefix.Length).Replace('\', '/')
        $records.Add("$relative`t$target")
    }
    $linkList = Join-Path $Staging 'links.tsv'
    [IO.File]::WriteAllLines($linkList, [string[]]$records, (New-Object Text.UTF8Encoding $false))

    # 5. Install.
    $arguments = @('install', '--links', $linkList)
    if ($Root) { $arguments += @('--root', $Root) }
    if ($Replace) { $arguments += '--replace' }
    $arguments += (Join-Path $out $appEntry)
    & $alchemyPath @arguments
    if ($LASTEXITCODE -ne 0) { throw "alchemy install failed (exit $LASTEXITCODE)" }
} finally {
    if (-not $KeepStaging) { Remove-Item -LiteralPath $Staging -Recurse -Force -ErrorAction SilentlyContinue }
}
