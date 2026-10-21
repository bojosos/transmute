param(
    [Parameter(Mandatory=$true)][string]$PythonDirectory,
    [string]$OutputDirectory = "$PSScriptRoot/../artifacts/alchemy-python/runtime",
    [string]$ToolDirectory = "C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/bin"
)
$ErrorActionPreference = 'Stop'
$pythonRoot = (Resolve-Path -LiteralPath $PythonDirectory).Path
$archive = Join-Path $pythonRoot 'lib/python3.11/config-3.11-darwin/libpython3.11.a'
$license = Join-Path $pythonRoot 'lib/python3.11/LICENSE.txt'
if (!(Test-Path -LiteralPath $archive) -or !(Test-Path -LiteralPath $license)) {
    throw 'Expected a CPython 3.11 static archive and its license in the supplied runtime.'
}
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path
$sourceRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/..").Path
$clang = Join-Path $ToolDirectory 'clang.exe'
$linker = Join-Path $ToolDirectory 'ld64.lld.exe'
$nm = Join-Path $ToolDirectory 'llvm-nm.exe'
foreach ($tool in @($clang, $linker, $nm)) {
    if (!(Test-Path -LiteralPath $tool)) { throw "Missing LLVM tool: $tool" }
}
function Invoke-Checked([string]$Tool, [string[]]$ToolArguments) {
    & $Tool @ToolArguments
    if ($LASTEXITCODE -ne 0) { throw "$Tool failed with status $LASTEXITCODE" }
}

# Generate declaration-only linker inputs from the non-Apple archive's imports.
# These files contain symbol names, no executable system implementations.
$undefined = @(& $nm --undefined-only --extern-only --just-symbol-name $archive)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect CPython archive imports' }
$defined = @(& $nm --defined-only --extern-only --just-symbol-name $archive)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect CPython archive exports' }
$definitions = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($symbol in $defined) { $null = $definitions.Add($symbol.Trim()) }
$imports = @($undefined | ForEach-Object { $_.Trim() } |
    Where-Object { $_ -match '^[_A-Za-z][_A-Za-z0-9.$]*$' -and !$definitions.Contains($_) } |
    Sort-Object -Unique)
function Write-LinkDeclarations([string]$Filename, [string]$Identity, [string[]]$Symbols) {
    $declarations = @('--- !tapi-tbd', 'tbd-version: 4', 'targets: [ arm64-macos ]',
        "install-name: '$Identity'", 'exports:', '  - targets: [ arm64-macos ]',
        '    symbols:')
    foreach ($symbol in $Symbols) { $declarations += "      - '$symbol'" }
    $declarations += '...'
    $declarations | Set-Content -LiteralPath $Filename -Encoding utf8
}
$cfSymbols = @($imports | Where-Object { $_ -match '^_(CF|kCF|_CF)' })
$systemSymbols = @($imports | Where-Object { $_ -notmatch '^_(CF|kCF|_CF)' })
$systemDeclarations = Join-Path $outputRoot 'libSystem.tbd'
$cfDeclarations = Join-Path $outputRoot 'CoreFoundation.tbd'
Write-LinkDeclarations $systemDeclarations '/usr/lib/libSystem.B.dylib' $systemSymbols
Write-LinkDeclarations $cfDeclarations '/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation' $cfSymbols
$compileOptions = @('--target=arm64-apple-macos11', '-march=armv8-a', '-mcpu=generic',
    '-nostdinc', '-fno-builtin', '-fno-stack-protector', '-Werror', '-O1', '-c')
foreach ($name in @('python_embed', 'python_native', 'python_native_leaf')) {
    Invoke-Checked $clang ($compileOptions + @(
        "$sourceRoot/tests/fixtures/macos/$name.c", '-o', "$outputRoot/$name.o"))
}
$linkOptions = @('-arch', 'arm64', '-platform_version', 'macos', '11.2', '11.2', '-fixup_chains')
Invoke-Checked $linker ($linkOptions + @('-e', '_main', '-export_dynamic', '-force_load', $archive,
    "$outputRoot/python_embed.o", $systemDeclarations, $cfDeclarations, '-o', "$outputRoot/python_embed"))
Invoke-Checked $linker ($linkOptions + @('-dylib', '-install_name', '@loader_path/python_native_leaf.dylib',
    "$outputRoot/python_native_leaf.o", '-o', "$outputRoot/python_native_leaf.dylib"))
Invoke-Checked $linker ($linkOptions + @('-bundle', '-undefined', 'dynamic_lookup',
    "$outputRoot/python_native.o", "$outputRoot/python_native_leaf.dylib",
    '-o', "$outputRoot/_alchemy_native.cpython-311-darwin.so"))
Copy-Item -LiteralPath $license -Destination "$outputRoot/CPython-LICENSE.txt"
$noticeRecords = @([ordered]@{ name = 'CPython-LICENSE.txt'; source = $license;
    sha256 = (Get-FileHash -LiteralPath $license).Hash.ToLowerInvariant() })
$resourceRoot = Split-Path (Split-Path $pythonRoot)
foreach ($notice in @(
    @('text/license/spdx/Python-2.0.txt', 'CPython-SPDX-license.txt'),
    @('text/license/thirdparty/python.txt', 'CPython-third-party-notices.txt'),
    @('text/copyright.txt', 'Bundle-copyright-inventory.txt')
)) {
    $noticeSource = Join-Path $resourceRoot $notice[0]
    if (Test-Path -LiteralPath $noticeSource -PathType Leaf) {
        Copy-Item -LiteralPath $noticeSource -Destination (Join-Path $outputRoot $notice[1])
        $noticeRecords += [ordered]@{ name = $notice[1]; source = $noticeSource;
            sha256 = (Get-FileHash -LiteralPath $noticeSource).Hash.ToLowerInvariant() }
    }
}
$outputs = @('python_embed', 'python_native_leaf.dylib', '_alchemy_native.cpython-311-darwin.so')
$provenance = [ordered]@{
    schema = 1
    profile = 'Bounded ARM64 CPython 3.11 embedding workload'
    external_python_root = $pythonRoot
    archive_sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    license_sha256 = (Get-FileHash -LiteralPath $license -Algorithm SHA256).Hash.ToLowerInvariant()
    clang = @(& $clang --version)[0]
    linker = @(& $linker --version)[0]
    compile_options = $compileOptions
    link_options = $linkOptions + @('-export_dynamic', '-force_load')
    notices = $noticeRecords
    stack_contract = 'Existing Transmute default; original standalone executable remains separate'
    system_link_declarations = $systemSymbols
    core_foundation_link_declarations = $cfSymbols
    inputs = @(@('python_embed', 'python_native', 'python_native_leaf') | ForEach-Object {
        [ordered]@{ source = "tests/fixtures/macos/$_.c";
            sha256 = (Get-FileHash -LiteralPath "$sourceRoot/tests/fixtures/macos/$_.c").Hash.ToLowerInvariant() }
    })
    outputs = @($outputs | ForEach-Object {
        [ordered]@{ name = $_; sha256 = (Get-FileHash -LiteralPath "$outputRoot/$_").Hash.ToLowerInvariant() }
    })
}
$provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$outputRoot/provenance.json" -Encoding utf8
Write-Output "Built guest CPython embedding workload: $outputRoot/python_embed"
