# Builds upstream libffi (MIT, vendored in third_party/libffi) as an ARM64 Mach-O guest runtime
# library that claims Apple's install name /usr/lib/libffi.dylib. Pass the output directory to
# alchemy/alchemy-fx with --runtime-dir; the loader accepts the identity only with the
# provenance.json this script writes (include/alchemy/runtime_libraries.hpp). No Apple SDK, header
# or binary is used: declarations come from runtime/include and runtime/libffi.
#
# Closures use libffi's arm64 Darwin trampoline table (FFI_EXEC_TRAMPOLINE_TABLE): a page of
# trampolines in the library's __TEXT is vm_remap'ed next to a writable config page, so no page is
# ever writable and executable. Alchemy's vm_remap copies that immutable executable page and tells
# the translator the code is the image's (docs/ALCHEMY_OSS_RUNTIMES.md).
param(
    [string]$OutputDirectory = "$PSScriptRoot/../artifacts/alchemy-libffi-arm64",
    [string]$Clang = 'C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/bin/clang.exe'
)
$ErrorActionPreference = 'Stop'
$upstream = 'https://github.com/libffi/libffi'
$version = '3.8.0'
$versionNumber = '30800'
$archive = 'https://github.com/libffi/libffi/releases/download/v3.8.0/libffi-3.8.0.tar.gz'
$archiveSha256 = '7da3e2d9a171eb0a038f592ecad3ff2bb2550f3496d87b3b29ad0cf4430c0db4'
$installName = '/usr/lib/libffi.dylib'
# The versions macOS applications record for /usr/lib/libffi.dylib.
$compatibilityVersion = '1.0.0'
$currentVersion = '40.0.0'
# Every libSystem symbol the vendored sources may import. A different set fails the build so new
# host requirements are reviewed against Alchemy's provider.
$expectedImports = @('___stack_chk_fail', '___stack_chk_guard', '_abort', '_calloc', '_free', '_mach_task_self_',
    '_malloc', '_memcpy', '_pthread_mutex_lock', '_pthread_mutex_unlock',
    '_vm_allocate', '_vm_deallocate', '_vm_protect', '_vm_remap')

$repo = (Resolve-Path "$PSScriptRoot/..").Path
$source = Join-Path $repo 'third_party/libffi'
$config = Join-Path $repo 'runtime/libffi'
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$out = (Resolve-Path -LiteralPath $OutputDirectory).Path
$objects = Join-Path $out 'obj'
$include = Join-Path $out 'include'
$null = New-Item -ItemType Directory -Force -Path $objects, $include
$sdk = Join-Path $repo 'runtime/include'
$tools = Split-Path $Clang
$linker = Join-Path $tools 'ld64.lld.exe'
$nm = Join-Path $tools 'llvm-nm.exe'
$resourceInclude = ((& $Clang -print-resource-dir).Trim()) + '/include'

# ffi.h from upstream's template, with the values configure substitutes for aarch64-apple-darwin.
$template = Get-Content -Raw -LiteralPath (Join-Path $source 'include/ffi.h.in')
$values = [ordered]@{ '@VERSION@' = $version; '@TARGET@' = 'AARCH64'; '@HAVE_LONG_DOUBLE@' = '0';
    '@FFI_VERSION_STRING@' = $version; '@FFI_VERSION_NUMBER@' = $versionNumber; '@FFI_EXEC_TRAMPOLINE_TABLE@' = '1' }
foreach ($key in $values.Keys) { $template = $template.Replace($key, $values[$key]) }
if ($template -match '@[A-Z_]+@') { throw "ffi.h.in has an unsubstituted value: $($Matches[0])" }
[System.IO.File]::WriteAllText((Join-Path $include 'ffi.h'), $template)
Copy-Item -LiteralPath (Join-Path $source 'src/aarch64/ffitarget.h') -Destination (Join-Path $include 'ffitarget.h')

$flags = @('--target=arm64-apple-macos11', '-mcpu=generic', '-march=armv8-a', '-O2', '-nostdinc',
    '-isystem', $sdk, '-isystem', (Join-Path $config 'include'), '-isystem', $resourceInclude,
    '-I', $config, '-I', $include, '-I', (Join-Path $source 'include'), '-I', (Join-Path $source 'src/aarch64'),
    '-DNDEBUG', '-Wall', '-Wno-unused-function', '-Wno-deprecated-declarations', '-Werror')
# libffi's library sources for aarch64-apple-darwin (Makefile.am and configure.host): the common
# call/closure/type modules, the raw and Java raw APIs, the (static-trampoline-less) tramp module
# and the aarch64 port.
$files = @('src/prep_cif.c', 'src/types.c', 'src/closures.c', 'src/raw_api.c', 'src/java_raw_api.c', 'src/tramp.c',
    'src/aarch64/ffi.c', 'src/aarch64/sysv.S')
$objectFiles = @()
foreach ($file in $files) {
    $object = Join-Path $objects (($file -replace '[/\\]', '_') -replace '\.[cS]$', '.o')
    [string[]]$language = if ($file.EndsWith('.S')) { @('-x', 'assembler-with-cpp') } else { @('-std=gnu11') }
    & $Clang @flags @language -c (Join-Path $source $file) -o $object
    if ($LASTEXITCODE) { throw "libffi compile failed: $file" }
    $objectFiles += $object
}

# The public API from upstream's own version script, preprocessed with this configuration.
$map = & $Clang @flags -E -P -x c (Join-Path $source 'libffi.map.in')
if ($LASTEXITCODE) { throw 'libffi.map.in preprocessing failed' }
$exports = @($map -join "`n" -split '[;\s{}]+' | Where-Object { $_ -match '^ffi_\w+$' } | ForEach-Object { "_$_" } | Sort-Object -Unique)
if ($exports.Count -lt 30) { throw "unexpected libffi export list ($($exports.Count) names)" }
$exportList = Join-Path $out 'libffi.exports'
[System.IO.File]::WriteAllLines($exportList, [string[]]$exports)

$undefined = @($objectFiles | ForEach-Object { & $nm -u $_ } | ForEach-Object { $_.Trim() } | Where-Object { $_ } | Sort-Object -Unique)
$internal = @($objectFiles | ForEach-Object { & $nm -g --defined-only $_ } | ForEach-Object { ($_ -split '\s+')[-1] } | Sort-Object -Unique)
$imports = @($undefined | Where-Object { $internal -notcontains $_ })
$difference = Compare-Object ($expectedImports | Sort-Object) $imports
if ($difference) { throw "libffi libSystem imports changed: $(($difference | ForEach-Object { $_.SideIndicator + $_.InputObject }) -join ', ')" }

# Link-only libSystem interface for two-level binding; Alchemy's libSystem provider implements
# the symbols at run time.
$stub = Join-Path $out 'link/libSystem.B.tbd'
$null = New-Item -ItemType Directory -Force -Path (Split-Path $stub)
$tbd = @('--- !tapi-tbd', 'tbd-version: 4', 'targets: [ arm64-macos ]', "install-name: '/usr/lib/libSystem.B.dylib'",
    'current-version: 1.0.0', 'exports:', '  - targets: [ arm64-macos ]', "    symbols: [ $($expectedImports -join ', ') ]", '...')
[System.IO.File]::WriteAllLines($stub, [string[]]$tbd)

$dylib = Join-Path $out 'libffi.dylib'
& $linker -arch arm64 -platform_version macos 11.0 11.0 -fixup_chains -dylib -install_name $installName `
    -compatibility_version $compatibilityVersion -current_version $currentVersion -exported_symbols_list $exportList `
    @objectFiles $stub -o $dylib
if ($LASTEXITCODE) { throw 'libffi link failed' }
$exported = @(& $nm -gU $dylib | ForEach-Object { ($_ -split '\s+')[-1] } | Sort-Object -Unique)
$difference = Compare-Object $exports $exported
if ($difference) { throw "libffi.dylib export set differs from libffi's version script: $(($difference | ForEach-Object { $_.SideIndicator + $_.InputObject }) -join ', ')" }

Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination (Join-Path $out 'LICENSE.libffi.txt')
Copy-Item -LiteralPath (Join-Path $repo 'runtime/THIRD_PARTY_NOTICES.md') -Destination (Join-Path $out 'THIRD_PARTY_NOTICES.md')
Write-Host "Built $dylib from libffi $version, $($exports.Count) exports"

function Get-Sha256([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Get-RelativeName([string]$root, [string]$path) { $path.Substring($root.Length).TrimStart('\', '/').Replace('\', '/') }
$provenance = [ordered]@{
    source = $upstream
    source_version = $version
    source_archive = $archive
    source_archive_sha256 = $archiveSha256
    vendored = 'third_party/libffi'
    license = 'MIT'
    install_name = $installName
    compatibility_version = $compatibilityVersion
    current_version = $currentVersion
    target = 'arm64-apple-macos11'
    architecture_flags = @('-mcpu=generic', '-march=armv8-a')
    optimization = '-O2'
    configuration = 'aarch64-apple-darwin; FFI_EXEC_TRAMPOLINE_TABLE; long double = double; no arm64e pointer authentication'
    sources = @($files | ForEach-Object { [ordered]@{ name = "third_party/libffi/$_"; sha256 = (Get-Sha256 (Join-Path $source $_)) } })
    clang_version = ((& $Clang --version) | Out-String).Trim()
    linker_version = ((& $linker --version) | Out-String).Trim()
    guest_header_profile = 'Alchemy-authored Darwin LP64 C/POSIX and Mach VM declarations; no Apple SDK payloads'
    build_script_sha256 = (Get-Sha256 $PSCommandPath)
    configuration_headers = @(Get-ChildItem -LiteralPath $config -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{ name = 'runtime/libffi/' + (Get-RelativeName $config $_.FullName); sha256 = (Get-Sha256 $_.FullName) }
    })
    guest_headers = @(Get-ChildItem -LiteralPath $sdk -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{ name = (Get-RelativeName $sdk $_.FullName); sha256 = (Get-Sha256 $_.FullName) }
    })
    exports = $exports
    libsystem_imports = $expectedImports
    outputs = @([ordered]@{ name = 'libffi.dylib'; sha256 = (Get-Sha256 $dylib) })
    licenses = @(@('LICENSE.libffi.txt', 'THIRD_PARTY_NOTICES.md') | ForEach-Object {
        [ordered]@{ name = $_; sha256 = (Get-Sha256 (Join-Path $out $_)) }
    })
}
$provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'provenance.json') -Encoding utf8
