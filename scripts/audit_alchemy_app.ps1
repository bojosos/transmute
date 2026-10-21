<#
.SYNOPSIS
  Per-image dependency, Objective-C, Swift and framework-usage audit of a
  macOS .app bundle (used for docs/ALCHEMY_QLAB_AUDIT.md).

.DESCRIPTION
  Static only: no guest instruction executes. For every Mach-O image it runs
  `alchemy doctor --json` (one image, non-recursive) and LLVM's objdump/nm on
  the arm64 slice, then aggregates:
    * dependencies and their Alchemy status (guest / provider / missing);
    * unresolved strong imports per library, with symbols;
    * Objective-C classes referenced/subclassed per framework, defined
      classes/categories, selector references;
    * Swift: /usr/lib/swift dependencies, imported Swift symbols per library,
      defined Swift symbols and type descriptors vs. Objective-C methods;
    * Interface Builder classes archived in compiled .nib files.
  Writes summary.json (and per-image doctor JSON) under -OutputDirectory.
#>
param(
    [Parameter(Mandatory = $true)][string]$Bundle,
    [string]$Alchemy = "$PSScriptRoot/../build-q/Release/alchemy-fx.exe",
    [string]$RuntimeDirectory = 'C:/dev/transmute/artifacts/alchemy-llvm-arm64',
    [string]$LlvmBin = 'C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/bin',
    [string]$OutputDirectory = "$PSScriptRoot/../artifacts/alchemy-qlab/audit"
)
$ErrorActionPreference = 'Stop'
$env:ALCHEMY_GUI_NO_ACTIVATE = '1'
$bundlePath = (Resolve-Path -LiteralPath $Bundle).Path
$alchemyPath = (Resolve-Path -LiteralPath $Alchemy).Path
$objdump = Join-Path $LlvmBin 'llvm-objdump.exe'
$nm = Join-Path $LlvmBin 'llvm-nm.exe'
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory

function Test-MachO([string]$path) {
    $stream = [IO.File]::OpenRead($path)
    try { $bytes = New-Object byte[] 4; if ($stream.Read($bytes, 0, 4) -ne 4) { return $false } } finally { $stream.Close() }
    $magic = [BitConverter]::ToString($bytes)
    return $magic -in @('CF-FA-ED-FE', 'CA-FE-BA-BE', 'CA-FE-BA-BF')
}

function Get-Relative([string]$path) { return $path.Substring($bundlePath.Length).TrimStart('\').Replace('\', '/') }

function Get-Short([string]$identity) {
    if ($identity -match '/([^/]+)\.framework/') { return $Matches[1] }
    return [IO.Path]::GetFileName($identity)
}

# Real Mach-O files only: framework link aliases (junctions/hard links) are
# skipped by resolving each candidate's final path and de-duplicating.
$linkedDirectory = @{}
function Test-ViaLink([string]$path) {
    # True when any directory between the bundle and path is a reparse point.
    $directory = Split-Path $path -Parent
    while ($directory.Length -gt $bundlePath.Length) {
        if (-not $linkedDirectory.ContainsKey($directory)) {
            $linkedDirectory[$directory] = [bool]([IO.File]::GetAttributes($directory) -band [IO.FileAttributes]::ReparsePoint)
        }
        if ($linkedDirectory[$directory]) { return $true }
        $directory = Split-Path $directory -Parent
    }
    return $false
}
$images = New-Object System.Collections.Generic.List[string]
Get-ChildItem -LiteralPath $bundlePath -Recurse -File -Force | Sort-Object FullName | ForEach-Object {
    if ($_.Length -lt 4096 -or -not (Test-MachO $_.FullName)) { return }
    # Framework top-level aliases (Foo.framework/Foo) are links to Versions/*.
    if ($_.FullName -match '\.framework\\[^\\]+$' -or (Test-ViaLink $_.FullName)) { return }
    $images.Add($_.FullName)
}

$appkitControls = @('NSTableView','NSOutlineView','NSSplitView','NSButton','NSTextField','NSPopUpButton','NSSlider',
    'NSTabView','NSToolbar','NSScrollView','NSSegmentedControl','NSStepper','NSColorWell','NSDatePicker','NSComboBox',
    'NSSearchField','NSTokenField','NSMatrix','NSBox','NSProgressIndicator','NSLevelIndicator','NSImageView',
    'NSTextView','NSCollectionView','NSBrowser','NSStackView','NSGridView','NSPathControl','NSSecureTextField',
    'NSSwitch','NSMenu','NSMenuItem','NSPanel','NSWindow','NSView','NSViewController','NSWindowController',
    'NSTableCellView','NSTableRowView','NSTableHeaderView','NSClipView','NSScroller','NSVisualEffectView','NSPopover',
    'NSDrawer','NSRuler','NSForm','NSOpenPanel','NSSavePanel','NSAlert','NSColorPanel','NSFontPanel','NSStatusBar','NSTouchBar')
$notableSelectors = @('drawRect:','viewForTableColumn','tableView:viewForTableColumn:row:','outlineView:viewForTableColumn:item:',
    'makeViewWithIdentifier:owner:','tableView:objectValueForTableColumn:row:','outlineView:child:ofItem:',
    'setWantsLayer:','updateLayer','makeBackingLayer','beginSheet:completionHandler:','runModal',
    'registerForDraggedTypes:','draggingSession:sourceOperationMaskForDraggingContext:','undoManager',
    'bind:toObject:withKeyPath:options:','observeValueForKeyPath:ofObject:change:context:','performSelector:withObject:afterDelay:',
    'scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:','addLocalMonitorForEventsMatchingMask:handler:',
    'addGlobalMonitorForEventsMatchingMask:handler:','evaluateJavaScript:completionHandler:','loadHTMLString:baseURL:',
    'requestAccessForMediaType:completionHandler:','addPeriodicTimeObserverForInterval:queue:usingBlock:',
    'copyPixelBufferForItemTime:itemTimeForDisplay:','newTextureWithDescriptor:','newLibraryWithURL:error:',
    'newDefaultLibraryWithBundle:error:','newLibraryWithSource:options:error:')

$summaryImages = New-Object System.Collections.Generic.List[object]
foreach ($image in $images) {
    $relative = Get-Relative $image
    $safe = ($relative -replace '[^A-Za-z0-9._-]', '_')
    $jsonPath = Join-Path $OutputDirectory "doctor-$safe.json"
    $doctorArguments = @('doctor', '--json')
    if ($RuntimeDirectory -and (Test-Path -LiteralPath $RuntimeDirectory)) { $doctorArguments += @('--runtime-dir', $RuntimeDirectory) }
    & $alchemyPath @doctorArguments $image | Set-Content -LiteralPath $jsonPath -Encoding utf8
    $report = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
    $item = $report.images[0]
    $imports = @($item.imports)
    $strongMissing = @($imports | Where-Object { -not $_.weak -and $_.status -in @('unresolved', 'invalid-ordinal', 'unsupported') })

    # Section sizes of the arm64 slice.
    $sections = @{}
    foreach ($line in (& $objdump --macho --arch=arm64 --section-headers $image)) {
        if ($line -match '^\s*\d+\s+(\S+)\s+([0-9a-f]{8,16})\s') {
            $name = $Matches[1]; $size = [Convert]::ToInt64($Matches[2], 16)
            if ($sections.ContainsKey($name)) { $sections[$name] += $size } else { $sections[$name] = $size }
        }
    }
    $section = { param($name) if ($sections.ContainsKey($name)) { $sections[$name] } else { 0 } }

    # Symbols: defined Objective-C methods vs Swift; undefined by library.
    $defined = & $nm --arch=arm64 --defined-only $image 2>$null
    $objcMethods = @($defined | Where-Object { $_ -match ' [tT] [-+]\[' }).Count
    $swiftDefined = @($defined | Where-Object { $_ -match ' _?\$s' }).Count
    $undefined = & $nm --arch=arm64 -m --undefined-only $image 2>$null
    $undefinedByLibrary = @{}
    foreach ($line in $undefined) {
        if ($line -match 'external (\S+) \(from ([^)]+)\)') {
            $symbol = $Matches[1]; $library = $Matches[2]
            if (-not $undefinedByLibrary.ContainsKey($library)) { $undefinedByLibrary[$library] = New-Object System.Collections.Generic.List[string] }
            $undefinedByLibrary[$library].Add($symbol)
        }
    }
    $classRefs = @{}
    $subclassed = @{}
    foreach ($library in $undefinedByLibrary.Keys) {
        $classes = @($undefinedByLibrary[$library] | Where-Object { $_ -like '_OBJC_CLASS_$_*' } | ForEach-Object { $_.Substring(14) } | Sort-Object -Unique)
        $metas = @($undefinedByLibrary[$library] | Where-Object { $_ -like '_OBJC_METACLASS_$_*' } | ForEach-Object { $_.Substring(18) } | Sort-Object -Unique)
        if ($classes.Count) { $classRefs[$library] = $classes }
        if ($metas.Count) { $subclassed[$library] = $metas }
    }
    $swiftImports = [ordered]@{}
    foreach ($library in ($undefinedByLibrary.Keys | Sort-Object)) {
        $count = @($undefinedByLibrary[$library] | Where-Object { $_ -match '^_?\$s' -or $library -like 'libswift*' }).Count
        if ($library -like 'libswift*' -or $count) { $swiftImports[$library] = $count }
    }

    # Selector names referenced by the image.
    $selectors = @()
    if ((& $section '__objc_methname') -gt 0) {
        $selectors = @(& $objdump --macho --arch=arm64 --section=__TEXT,__objc_methname $image 2>$null |
            ForEach-Object { if ($_ -match '^[0-9a-f]{8,16}\s+(.+)$') { $Matches[1] } } | Sort-Object -Unique)
    }
    $selectorHits = @($notableSelectors | Where-Object { $selectors -contains $_ })

    $summaryImages.Add([ordered]@{
        path = $relative
        type = $item.type
        error = $item.error
        minimum_os = $item.minimum_os
        sdk = $item.sdk
        runpaths = @($item.runpaths)
        text_bytes = (& $section '__text')
        dependencies = @($item.dependencies | ForEach-Object { [ordered]@{ identity = $_.identity; status = $_.status; weak = $_.weak } })
        imports_by_status = (@($imports | Group-Object status) | ForEach-Object -Begin { $h = [ordered]@{} } -Process { $h[$_.Name] = $_.Count } -End { $h })
        unresolved_strong = $strongMissing.Count
        unresolved_strong_by_library = @($strongMissing | Group-Object library | Sort-Object Count -Descending | ForEach-Object {
            [ordered]@{ library = $_.Name; count = $_.Count; symbols = @($_.Group.symbol | Sort-Object -Unique) } })
        objc = [ordered]@{
            classes_defined = [int]((& $section '__objc_classlist') / 8)
            categories_defined = [int]((& $section '__objc_catlist') / 8)
            selector_refs = [int]((& $section '__objc_selrefs') / 8)
            methods_defined = $objcMethods
            selector_names = $selectors.Count
            notable_selectors = $selectorHits
            class_refs = $classRefs
            subclassed = $subclassed
        }
        swift = [ordered]@{
            type_descriptors = [int]((& $section '__swift5_types') / 4)
            conformances = [int]((& $section '__swift5_proto') / 4)
            symbols_defined = $swiftDefined
            imports_by_library = $swiftImports
            libraries = @($item.dependencies | Where-Object { $_.identity -like '/usr/lib/swift/*' } | ForEach-Object identity)
        }
        undefined_by_library = ($undefinedByLibrary.Keys | Sort-Object | ForEach-Object -Begin { $h = [ordered]@{} } -Process { $h[$_] = $undefinedByLibrary[$_].Count } -End { $h })
    })
    Write-Host ("{0}: {1} unresolved strong, {2} ObjC classes, {3} Swift types" -f $relative, $strongMissing.Count,
        [int]((& $section '__objc_classlist') / 8), [int]((& $section '__swift5_types') / 4))
}

# Interface Builder: AppKit classes archived in compiled nibs (class names are
# stored as strings in the keyed archive).
$nibClasses = @{}
$nibFiles = @(Get-ChildItem -LiteralPath $bundlePath -Recurse -File -Force | Where-Object { $_.FullName -match '\.nib(\\|$)' -and -not (Test-ViaLink $_.FullName) })
foreach ($nib in $nibFiles) {
    $text = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($nib.FullName))
    foreach ($match in [regex]::Matches($text, '(?<![A-Za-z])(NS[A-Z][A-Za-z]+|WK[A-Z][A-Za-z]+|AV[A-Z][A-Za-z]+|PDF[A-Z][A-Za-z]+|IK[A-Z][A-Za-z]+)')) {
        $name = $match.Value
        if (-not $nibClasses.ContainsKey($name)) { $nibClasses[$name] = @{} }
        $nibClasses[$name][$nib.FullName] = $true
    }
}
# Keyed-archive keys that look like class names.
$nibKeys = @('NSSuperview','NSNextKeyView','NSFirstItem','NSSecondItem','NSUsesItemFromMenu','NSControlView','NSDocView','NSContentView','NSWindowView','NSSelectedTabViewItem','NSSubmenu','NSActionCellVNSCell','NSHeaderClipView','NSCornerView','NSNibTouchBar')
$nibSummary = @($nibClasses.Keys | Where-Object { $_ -notin $nibKeys } | Where-Object { $_ -in $appkitControls -or $_ -match '^(NS.*(View|Control|Button|Field|Cell|Bar|Panel|Window|Menu|Item|Controller|Picker|Slider|Stepper|Well|Box|Indicator))$' -or $_ -match '^(WK|AV|PDF|IK)' } |
    Sort-Object | ForEach-Object { [ordered]@{ class = $_; nib_files = $nibClasses[$_].Count } })

$summary = [ordered]@{
    bundle = $bundlePath
    alchemy = $alchemyPath
    execution_verified = $false
    images = $summaryImages
    nib_files = $nibFiles.Count
    nib_classes = $nibSummary
}
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
Write-Host "Wrote $(Join-Path $OutputDirectory 'summary.json')"
