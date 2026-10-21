param(
    [string[]]$Binary = @(),
    [string[]]$Directory = @(),
    [string[]]$GuestDllDirectory = @(),
    [string]$Objdump = 'llvm-objdump',
    [int]$Top = 40
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Top -lt 1) { throw 'Top must be positive' }
if ($Binary.Count -eq 0 -and $Directory.Count -eq 0) {
    throw 'Supply -Binary, -Directory, or both'
}

# PE headers and import tables (llvm-objdump -p) of ARM64 binaries, matched against
# Transmute's Win32 bridge.

# Visual Studio's LLVM tools are usually installed but absent from PATH in a
# regular PowerShell session. Keep an explicit -Objdump override for other SDKs.
if (-not (Get-Command $Objdump -ErrorAction SilentlyContinue)) {
    $visualStudioRoot = Join-Path $env:ProgramFiles 'Microsoft Visual Studio'
    $candidate = Get-ChildItem -LiteralPath $visualStudioRoot -Filter llvm-objdump.exe `
        -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\VC\\Tools\\Llvm\\x64\\bin\\' } |
        Select-Object -First 1
    if ($null -eq $candidate) {
        throw "llvm-objdump was not found; supply its full path with -Objdump"
    }
    $Objdump = $candidate.FullName
}

$paths = [System.Collections.Generic.List[string]]::new()
$seenPaths = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
$guestDirectories = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($item in $Binary) {
    $path = (Resolve-Path -LiteralPath $item).Path
    if ($seenPaths.Add($path)) { $paths.Add($path) }
    [void]$guestDirectories.Add((Split-Path -Parent $path))
}
foreach ($item in $Directory) {
    $directoryPath = (Resolve-Path -LiteralPath $item).Path
    [void]$guestDirectories.Add($directoryPath)
    foreach ($file in Get-ChildItem -LiteralPath $directoryPath -File |
        Where-Object { $_.Extension -in @('.dll', '.exe') }) {
        if ($seenPaths.Add($file.FullName)) { $paths.Add($file.FullName) }
    }
}
foreach ($item in $GuestDllDirectory) {
    [void]$guestDirectories.Add((Resolve-Path -LiteralPath $item).Path)
}

$bridgePath = Join-Path $PSScriptRoot '..\src\win32_bridge.cpp'
$bridgeSource = Get-Content -LiteralPath $bridgePath -Raw
$bridgeNames = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::Ordinal)
foreach ($match in [regex]::Matches($bridgeSource, '"([^"\r\n]+)"')) {
    [void]$bridgeNames.Add($match.Groups[1].Value)
}
$systemDirectory = [Environment]::SystemDirectory
$results = foreach ($path in $paths) {
    $stream = [System.IO.File]::OpenRead($path)
    try {
        $reader = [System.IO.BinaryReader]::new($stream)
        if ($stream.Length -lt 0x40) { throw "truncated PE header: $path" }
        $stream.Position = 0x3c
        $peOffset = $reader.ReadUInt32()
        if ($peOffset -gt $stream.Length - 24) { throw "invalid PE offset: $path" }
        $stream.Position = $peOffset + 4
        $machine = '0x{0:X4}' -f $reader.ReadUInt16()
        $sectionCount = $reader.ReadUInt16()
        $stream.Position = $peOffset + 20
        $optionalHeaderSize = $reader.ReadUInt16()
        $sectionTable = [long]$peOffset + 24 + $optionalHeaderSize
        if ($sectionCount -gt 96 -or $sectionTable + 40 * $sectionCount -gt
            $stream.Length) {
            throw "invalid PE section table: $path"
        }
        $sections = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::OrdinalIgnoreCase)
        for ($sectionIndex = 0; $sectionIndex -lt $sectionCount; ++$sectionIndex) {
            $stream.Position = $sectionTable + 40 * $sectionIndex
            [void]$sections.Add([Text.Encoding]::ASCII.GetString(
                $reader.ReadBytes(8)).TrimEnd([char]0))
        }
    } finally {
        $stream.Dispose()
    }
    $imports = [System.Collections.Generic.List[object]]::new()
    $inImports = $false
    $library = ''
    $format = ''
    $directoryEntries = [System.Collections.Generic.Dictionary[int, bool]]::new()
    & $Objdump -p $path | ForEach-Object {
        $line = $_
        if ($line -match 'file format\s+(\S+)') { $format = $Matches[1] }
        if ($line -match '^Entry\s+([0-9a-f])\s+([0-9a-f]+)\s+([0-9a-f]+)') {
            $directoryEntries[[Convert]::ToInt32($Matches[1], 16)] =
                [Convert]::ToUInt64($Matches[3], 16) -ne 0
        }
        if ($line -match '^The Import Tables:') { $inImports = $true }
        if ($line -match '^(Export Table:|Delay Import Tables:)') { $inImports = $false }
        if ($inImports -and $line -match '^\s*DLL Name:\s*(.+)$') {
            $library = $Matches[1]
        }
        if ($inImports -and $line -match '^\s*\d+\s+([A-Za-z_?@][^\s]*)\s*$') {
            $imports.Add([pscustomobject]@{
                Library = $library
                Function = $Matches[1]
            })
        }
        if ($inImports -and $library -ne '' -and
            $line -match '^\s*(\d+)\s*$') {
            $imports.Add([pscustomobject]@{
                Library = $library
                Function = "#$($Matches[1])"
            })
        }
    }
    if ($LASTEXITCODE -ne 0) { throw "objdump failed to inspect imports: $path" }

    $hostLibraries = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $guestLibraries = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $seenLibraries = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $imports) {
        if (-not $seenLibraries.Add($entry.Library)) { continue }
        $packaged = $false
        foreach ($directoryPath in $guestDirectories) {
            if (Test-Path -LiteralPath (Join-Path $directoryPath $entry.Library) `
                -PathType Leaf) {
                $packaged = $true
                break
            }
        }
        if ($packaged) {
            [void]$guestLibraries.Add($entry.Library)
            continue
        }
        if ($entry.Library -match '^(?i:api-ms-win-|ext-ms-)' -or
            $entry.Library -in @('kernel32.dll', 'kernelbase.dll') -or
            (Test-Path -LiteralPath (Join-Path $systemDirectory $entry.Library))) {
            [void]$hostLibraries.Add($entry.Library)
        }
    }
    $unmatched = @($imports | Where-Object {
        $hostLibraries.Contains($_.Library) -and
        -not $bridgeNames.Contains($_.Function)
    } | ForEach-Object { "$($_.Library)!$($_.Function)" })
    $byLibrary = @($imports | Group-Object Library | Sort-Object Count -Descending |
        ForEach-Object {
            [pscustomobject]@{
                Library = $_.Name
                ImportCount = $_.Count
                Resolution = $(if ($guestLibraries.Contains($_.Name)) { 'Guest DLL' }
                    elseif ($hostLibraries.Contains($_.Name)) { 'Host bridge' }
                    else { 'Missing' })
                NamesAbsentFromBridge = @($_.Group | Where-Object {
                    $hostLibraries.Contains($_.Library) -and
                    -not $bridgeNames.Contains($_.Function)
                }).Count
            }
        })
    [pscustomobject]@{
        Binary = $path
        Format = $format
        RawCoffMachine = $machine
        HasArm64XFixupSection = $sections.Contains('.a64xrm')
        HasHybridThunkSection = $sections.Contains('.hexpthk')
        HasExceptionDirectory = $directoryEntries.ContainsKey(3) -and $directoryEntries[3]
        HasBaseRelocationDirectory = $directoryEntries.ContainsKey(5) -and $directoryEntries[5]
        HasTlsDirectory = $directoryEntries.ContainsKey(9) -and $directoryEntries[9]
        HasLoadConfigDirectory = $directoryEntries.ContainsKey(10) -and $directoryEntries[10]
        HasDelayImportDirectory = $directoryEntries.ContainsKey(13) -and $directoryEntries[13]
        ImportCount = $imports.Count
        HostImportCount = @($imports | Where-Object {
            $hostLibraries.Contains($_.Library)
        }).Count
        GuestImportCount = @($imports | Where-Object {
            $guestLibraries.Contains($_.Library)
        }).Count
        MissingImportCount = @($imports | Where-Object {
            -not $hostLibraries.Contains($_.Library) -and
            -not $guestLibraries.Contains($_.Library)
        }).Count
        HostImportsNotNamedInBridgeCount = $unmatched.Count
        ImportLibraries = $byLibrary
        HostImportsNotNamedInBridge = @($unmatched | Select-Object -First $Top)
    }
}

$results | ConvertTo-Json -Depth 5
