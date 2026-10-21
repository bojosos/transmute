param(
    [string]$PythonRoot = "$PSScriptRoot/../artifacts/alchemy-blender/extracted/Blender/Blender.app/Contents/Resources/4.5/python",
    [string]$Alchemy = "$PSScriptRoot/../build/alchemy-expansion/Release/alchemy.exe",
    [string]$Nm = 'C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/bin/llvm-nm.exe',
    [string]$OutputDirectory = "$PSScriptRoot/../artifacts/alchemy-python/audit",
    [string[]]$RuntimeDirectory = @(),
    [string[]]$DylibDirectory = @(),
    [string[]]$ImagePattern = @(),
    [switch]$ReuseReports
)
$ErrorActionPreference = 'Stop'
$rootPath = (Resolve-Path -LiteralPath $PythonRoot).Path
$alchemyPath = (Resolve-Path -LiteralPath $Alchemy).Path
$nmPath = (Resolve-Path -LiteralPath $Nm).Path
$pythonPath = Join-Path $rootPath 'bin/python3.11'
if (!(Test-Path -LiteralPath $pythonPath -PathType Leaf)) { throw "Bundled ARM64 Python is missing: $pythonPath" }
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$outputPath = (Resolve-Path -LiteralPath $OutputDirectory).Path
$inspectorPath = Join-Path $outputPath 'alchemy-inspector.exe'
Copy-Item -LiteralPath $alchemyPath -Destination $inspectorPath -Force
$inspectorHash = (Get-FileHash -LiteralPath $inspectorPath -Algorithm SHA256).Hash.ToLowerInvariant()
$cachedImages = @{}
$manifestPath = Join-Path $outputPath 'manifest.json'
if ($ReuseReports -and (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    $previous = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($previous.alchemy_sha256 -eq $inspectorHash -and
        (@($previous.runtime_directories) -join "`n") -eq ($RuntimeDirectory -join "`n") -and
        (@($previous.dylib_directories) -join "`n") -eq ($DylibDirectory -join "`n") -and
        (@($previous.image_patterns) -join "`n") -eq ($ImagePattern -join "`n")) {
        foreach ($image in $previous.images) { $cachedImages[$image.path] = $image }
    }
}
$reportDirectory = Join-Path $outputPath 'images'
$null = New-Item -ItemType Directory -Force -Path $reportDirectory

# This audit never runs Python or imports an extension. The stock executable's
# exported C API supplies the namespace that a Python-loaded bundle expects.
$pythonExports = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$exportText = @(& $nmPath --extern-only --defined-only --format=posix $pythonPath)
if ($LASTEXITCODE) { throw 'Could not inspect the bundled executable exports' }
foreach ($line in $exportText) {
    if ($line -match '^(\S+)\s+[A-Za-z]\s+') { $null = $pythonExports.Add($Matches[1]) }
}
$exportText | Set-Content -LiteralPath (Join-Path $outputPath 'python-exports.txt') -Encoding utf8
$doctorArguments = @('doctor', '--deps', '--json')
foreach ($directory in $RuntimeDirectory) { $doctorArguments += @('--runtime-dir', (Resolve-Path -LiteralPath $directory).Path) }
foreach ($directory in $DylibDirectory) { $doctorArguments += @('--dylib-dir', (Resolve-Path -LiteralPath $directory).Path) }
$nativeImages = @((Get-Item -LiteralPath $pythonPath)) + @(Get-ChildItem -LiteralPath (Join-Path $rootPath 'lib') -File -Recurse |
    Where-Object { $_.Extension -in @('.so', '.dylib') } | Sort-Object FullName)
$summaries = [System.Collections.Generic.List[object]]::new()
$index = 0
foreach ($image in $nativeImages) {
    $relative = [System.IO.Path]::GetRelativePath($rootPath, $image.FullName).Replace('\', '/')
    if ($ImagePattern.Count -and !@($ImagePattern | Where-Object { $relative -like $_ }).Count) { continue }
    $reportName = ('{0:d4}.json' -f $index++)
    $imageHash = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $cached = $cachedImages[$relative]
    $report = $null
    if ($null -ne $cached -and $cached.sha256 -eq $imageHash -and $cached.report -match '^images/[0-9]+\.json$') {
        $cachedPath = Join-Path $outputPath $cached.report
        if (Test-Path -LiteralPath $cachedPath -PathType Leaf) {
            $text = Get-Content -LiteralPath $cachedPath -Raw
            $candidate = $text | ConvertFrom-Json
            if ($candidate.root -eq $image.FullName) { $report = $candidate }
        }
    }
    if ($null -eq $report) {
        $text = (& $inspectorPath @doctorArguments $image.FullName) -join "`n"
        $inspectionExit = $LASTEXITCODE
        if ($inspectionExit -notin @(0, 1)) { throw "Alchemy inspection failed for ${relative}: $inspectionExit" }
        $report = $text | ConvertFrom-Json
    }
    $text | Set-Content -LiteralPath (Join-Path $reportDirectory $reportName) -Encoding utf8
    $rootImage = @($report.images | Where-Object { $_.path -eq $image.FullName })[0]
    if ($null -eq $rootImage) { throw "No root image in inspection report: $relative" }
    $imports = @($report.images | ForEach-Object { $_.imports } | Where-Object { $null -ne $_ })
    $pythonImports = @($imports | Where-Object {
        $_.ordinal -eq -2 -and $_.status -eq 'unresolved' -and $pythonExports.Contains($_.symbol)
    })
    $missingImports = @($imports | Where-Object {
        !$_.weak -and $_.status -in @('unresolved', 'invalid-ordinal', 'unsupported') -and
        !($_.ordinal -eq -2 -and $pythonExports.Contains($_.symbol))
    })
    $dependencies = @($report.images | ForEach-Object { $_.dependencies } | Where-Object { $null -ne $_ })
    $moduleExports = @(& $nmPath --extern-only --defined-only --format=posix $image.FullName)
    if ($LASTEXITCODE) { throw "Could not inspect exports: $relative" }
    $initializers = @($moduleExports | Where-Object { $_ -match '^_PyInit_' } | ForEach-Object { ($_ -split '\s+')[0] })
    $summaries.Add([ordered]@{
        path = $relative
        sha256 = $imageHash
        bytes = $image.Length
        architecture = $rootImage.architecture
        type = $rootImage.type
        error = $rootImage.error
        minimum_os = $rootImage.minimum_os
        sdk_metadata = $rootImage.sdk
        stack_size = $rootImage.stack_size
        classic_fixups = $rootImage.classic_fixups
        pointer_formats = @($rootImage.pointer_formats)
        runpaths = @($rootImage.runpaths)
        dependencies = @($rootImage.dependencies)
        requirements = @($report.images | ForEach-Object { $_.requirements } | Sort-Object -Unique)
        initialization_exports = $initializers
        flat_imports_from_python_executable = @($pythonImports.symbol | Sort-Object -Unique)
        missing_libraries = @($dependencies | Where-Object { $_.status -eq 'missing' } | Select-Object -ExpandProperty identity -Unique | Sort-Object)
        unresolved_strong_imports = @($missingImports | Select-Object library, symbol, ordinal, status -Unique)
        static_imports_resolvable = @($report.images | Where-Object { $_.error }).Count -eq 0 -and $missingImports.Count -eq 0 -and
            @($dependencies | Where-Object { $_.status -eq 'missing' -and !$_.weak }).Count -eq 0
        execution_verified = $false
        report = "images/$reportName"
    })
}
$archivePath = Join-Path $rootPath 'lib/python3.11/config-3.11-darwin/libpython3.11.a'
$archive = $null
if (Test-Path -LiteralPath $archivePath -PathType Leaf) {
    $archiveImports = @(& $nmPath --undefined-only --extern-only --format=posix $archivePath)
    if ($LASTEXITCODE) { throw 'Could not inspect the bundled CPython archive' }
    $archiveImports | Set-Content -LiteralPath (Join-Path $outputPath 'libpython-undefined.txt') -Encoding utf8
    $archiveDefinitions = @(& $nmPath --defined-only --extern-only --format=posix $archivePath)
    if ($LASTEXITCODE) { throw 'Could not inspect the bundled CPython archive definitions' }
    $defined = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($line in $archiveDefinitions) {
        if ($line -match '^(\S+)\s+[A-Za-z]\s+') { $null = $defined.Add($Matches[1]) }
    }
    $external = @($archiveImports | Where-Object { $_ -match '^(\S+)\s+U(?:\s|$)' } | ForEach-Object {
        ($_ -split '\s+')[0]
    } | Where-Object { !$defined.Contains($_) } | Sort-Object -Unique)
    $external | Set-Content -LiteralPath (Join-Path $outputPath 'libpython-external-symbols.txt') -Encoding utf8
    $archive = [ordered]@{
        path = [System.IO.Path]::GetRelativePath($rootPath, $archivePath).Replace('\', '/')
        sha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
        bytes = (Get-Item -LiteralPath $archivePath).Length
        undefined_symbols_report = 'libpython-undefined.txt'
        external_symbols_report = 'libpython-external-symbols.txt'
        external_symbols = $external
        note = 'Archive-member undefined symbols include references resolved by other members; this is not a missing-import count.'
    }
}
$resourcesPath = Split-Path (Split-Path $rootPath)
$noticePaths = @(
    (Join-Path $rootPath 'lib/python3.11/LICENSE.txt'),
    (Join-Path $resourcesPath 'text/copyright.txt'),
    (Join-Path $resourcesPath 'text/license/spdx/Python-2.0.txt')
)
$notices = @($noticePaths | ForEach-Object {
    $path = $_
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        [ordered]@{ path = $path; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
})
$manifest = [ordered]@{
    schema = 1
    audit_time_utc = [DateTime]::UtcNow.ToString('o')
    python_root = $rootPath
    alchemy_sha256 = $inspectorHash
    runtime_directories = $RuntimeDirectory
    dylib_directories = $DylibDirectory
    image_patterns = $ImagePattern
    python_c_api_exports = $pythonExports.Count
    archive = $archive
    bundled_notices = $notices
    images = $summaries.ToArray()
    scope = 'Static audit of existing guest binaries. Python C API imports are matched against the stock executable exports; no guest code executes.'
}
$manifest | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $manifestPath -Encoding utf8
$missing = @($summaries | ForEach-Object { $_.unresolved_strong_imports })
$summary = [ordered]@{
    native_images = $summaries.Count
    stdlib_extensions = @($summaries | Where-Object { $_.path.StartsWith('lib/python3.11/lib-dynload/') }).Count
    site_package_extensions = @($summaries | Where-Object { $_.path.StartsWith('lib/python3.11/site-packages/') }).Count
    static_imports_resolvable = @($summaries | Where-Object { $_.static_imports_resolvable }).Count
    unsupported_images = @($summaries | Where-Object { $_.error }).Count
    missing_libraries = @($summaries | ForEach-Object { $_.missing_libraries } | Sort-Object -Unique)
    missing_imports_by_library = @($missing | Group-Object library | Sort-Object Count -Descending | ForEach-Object {
        [ordered]@{ library = $_.Name; occurrences = $_.Count; symbols = @($_.Group.symbol | Sort-Object -Unique) }
    })
    execution_verified = $false
}
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $outputPath 'summary.json') -Encoding utf8
[pscustomobject]$summary | Select-Object native_images, stdlib_extensions, site_package_extensions, static_imports_resolvable, unsupported_images | Format-List
