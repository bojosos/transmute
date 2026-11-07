# Builds a subset of ICU4C's common library (Unicode-3.0, vendored in third_party/icu) as an ARM64
# Mach-O guest runtime library that claims Apple's install name /usr/lib/libicucore.A.dylib. Pass
# the output directory to alchemy/alchemy-fx with --runtime-dir, together with the LLVM runtime
# directory (it links /usr/lib/libc++.1.dylib); the loader accepts the identity only with the
# provenance.json this script writes (include/alchemy/runtime_libraries.hpp). No Apple SDK, header
# or binary is used (docs/ALCHEMY_OSS_RUNTIMES.md).
#
# Like Apple's libicucore, the C API is exported under unversioned names (U_DISABLE_RENAMING). The
# export set is the UTF-8 conversion, case-mapping and string functions and UTS #46 IDNA, which is
# what macOS applications import from it (libpsl, raptor2). The only data is UTS #46's
# normalization (uts46.nrm), compiled in as a minimal common-data package (runtime/icu/make_icudata.ps1);
# character properties, case mapping and NFC are compiled into ICU's sources.
param(
    [string]$OutputDirectory = "$PSScriptRoot/../artifacts/alchemy-icu-arm64",
    [string]$LlvmRuntimeDirectory = "$PSScriptRoot/../artifacts/alchemy-llvm-arm64",
    [string]$LlvmSourceDirectory = "$PSScriptRoot/../artifacts/alchemy-llvm-source",
    [string]$Clang = 'C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/bin/clang.exe',
    [int]$Jobs = 8
)
$ErrorActionPreference = 'Stop'
$upstream = 'https://github.com/unicode-org/icu'
$version = '79.1'
$archive = 'https://github.com/unicode-org/icu/releases/download/release-79.1/icu4c-79.1-sources.tgz'
$archiveSha256 = '06fa63ecb521048f8082f12ad988f6b678d90cbd88aafce24cbc94902b191d99'
$installName = '/usr/lib/libicucore.A.dylib'
# The versions macOS applications record for /usr/lib/libicucore.A.dylib.
$compatibilityVersion = '1.0.0'
$currentVersion = '79.1.0'
$exports = @('_u_errorName', '_u_foldCase', '_u_getVersion', '_u_strFoldCase', '_u_strFromUTF8', '_u_strFromUTF8Lenient',
    '_u_strFromUTF8WithSub', '_u_strToLower', '_u_strToUTF8', '_u_strToUTF8WithSub', '_u_strToUpper', '_u_strcmp',
    '_u_strcpy', '_u_strlen', '_u_strncpy', '_u_tolower', '_u_toupper', '_uidna_close', '_uidna_labelToASCII',
    '_uidna_labelToASCII_UTF8', '_uidna_labelToUnicode', '_uidna_labelToUnicodeUTF8', '_uidna_nameToASCII',
    '_uidna_nameToASCII_UTF8', '_uidna_nameToUnicode', '_uidna_nameToUnicodeUTF8', '_uidna_openUTS46')
# ICU's common sources these exports need (the members a static link of all of common/ pulls in).
$modules = @('appendable', 'bmpset', 'bytesinkutil', 'bytestream', 'bytestrie', 'charstr', 'cmemory', 'cstring', 'edits',
    'fixedstring', 'loadednormalizer2impl', 'localebuilder', 'locid', 'loclikely', 'loclikelysubtags', 'locmap', 'lsr',
    'normalizer2', 'normalizer2impl', 'patternprops', 'propname', 'punycode', 'putil', 'resource', 'stringpiece', 'uarrsort',
    'ubidi_props', 'ucase', 'uchar', 'ucln_cmn', 'ucmndata', 'ucol_swp', 'ucptrie', 'udata', 'udatamem', 'udataswp', 'uenum',
    'uhash', 'uinvchar', 'uloc', 'uloc_keytype', 'uloc_tag', 'umapfile', 'umath', 'umutablecptrie', 'umutex', 'unifilt',
    'unifunct', 'uniset', 'unisetspan', 'unistr', 'uobject', 'uresbund', 'uresdata', 'uscript_props', 'ustrcase',
    'ustrcase_locale', 'ustrenum', 'ustring', 'ustrtrns', 'utf_impl', 'util', 'utrace', 'utrie2', 'utrie_swap', 'uts46',
    'utypes', 'uvector')
# Every libSystem symbol the sources may import; libc++ and libc++abi names come from libc++.1.dylib.
$expectedImports = @('___stack_chk_fail', '___stack_chk_guard', '_abort', '_bzero', '_free', '_getenv', '_malloc', '_memchr',
    '_memcmp', '_memcpy', '_memmove', '_realloc', '_setlocale', '_strcat', '_strchr', '_strcmp', '_strcpy', '_strlen',
    '_strncat', '_strncmp', '_strncpy', '_strrchr', '_strstr', '_strtol', '_strtoul')

$repo = (Resolve-Path "$PSScriptRoot/..").Path
$source = Join-Path $repo 'third_party/icu/source/common'
$config = Join-Path $repo 'runtime/icu'
$llvmRuntime = (Resolve-Path -LiteralPath $LlvmRuntimeDirectory).Path
$llvmSource = (Resolve-Path -LiteralPath $LlvmSourceDirectory).Path
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$out = (Resolve-Path -LiteralPath $OutputDirectory).Path
$objects = Join-Path $out 'obj'
$null = New-Item -ItemType Directory -Force -Path $objects
$sdk = Join-Path $repo 'runtime/include'
$tools = Split-Path $Clang
$clangxx = Join-Path $tools 'clang++.exe'
$linker = Join-Path $tools 'ld64.lld.exe'
$nm = Join-Path $tools 'llvm-nm.exe'
$resourceInclude = ((& $Clang -print-resource-dir).Trim()) + '/include'

$defines = @('-DU_COMMON_IMPLEMENTATION', '-DU_DISABLE_RENAMING=1', '-DU_CHARSET_IS_UTF8=1', '-DUCONFIG_NO_FILE_IO=1',
    '-DUCONFIG_NO_CONVERSION=1', '-DUCONFIG_NO_SERVICE=1', '-DU_ENABLE_DYLOAD=0', '-DU_USING_ICU_NAMESPACE=0',
    '-DU_HAVE_STRTOD_L=0', '-DNDEBUG')
$flags = @('--target=arm64-apple-macos11', '-mcpu=generic', '-march=armv8-a', '-O2', '-std=c++17', '-nostdinc', '-nostdinc++',
    '-isystem', "$llvmSource/libcxx/include", '-isystem', (Join-Path $config 'include'), '-isystem', $sdk,
    '-isystem', $resourceInclude, '-I', $source, '-fno-exceptions', '-fvisibility=hidden') + $defines

# Compile in parallel batches.
$objectFiles = @()
$pending = @()
foreach ($module in $modules) {
    $object = Join-Path $objects "$module.o"
    $objectFiles += $object
    $arguments = @($flags + @('-c', (Join-Path $source "$module.cpp"), '-o', $object) | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } })
    $pending += Start-Process -FilePath $clangxx -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardError "$object.err"
    if ($pending.Count -ge $Jobs) { $pending[0].WaitForExit(); $pending = @($pending | Select-Object -Skip 1) }
}
$pending | ForEach-Object { $_.WaitForExit() }
foreach ($module in $modules) {
    $object = Join-Path $objects "$module.o"
    if (!(Test-Path -LiteralPath $object) -or (Get-Item "$object.err").Length) {
        throw "ICU compile failed: $module.cpp`n$(Get-Content -Raw "$object.err")"
    }
}

# The data package: upstream's prebuilt uts46.nrm in ICU's common-data layout.
$data = Join-Path $objects 'icudata.c'
& (Join-Path $config 'make_icudata.ps1') -Output $data -DataDirectory (Join-Path $repo 'third_party/icu/source/data/in') `
    -Package 'icudt79l' -Symbol 'icudt79_dat' -Items @('uts46.nrm')
& $Clang --target=arm64-apple-macos11 -O2 -c $data -o (Join-Path $objects 'icudata.o')
if ($LASTEXITCODE) { throw 'ICU data compile failed' }
$objectFiles += Join-Path $objects 'icudata.o'

$exportList = Join-Path $out 'libicucore.exports'
[System.IO.File]::WriteAllLines($exportList, [string[]]$exports)
$stub = Join-Path $out 'link/libSystem.B.tbd'
$null = New-Item -ItemType Directory -Force -Path (Split-Path $stub)
$tbd = @('--- !tapi-tbd', 'tbd-version: 4', 'targets: [ arm64-macos ]', "install-name: '/usr/lib/libSystem.B.dylib'",
    'current-version: 1.0.0', 'exports:', '  - targets: [ arm64-macos ]', "    symbols: [ $($expectedImports -join ', ') ]", '...')
[System.IO.File]::WriteAllLines($stub, [string[]]$tbd)

$dylib = Join-Path $out 'libicucore.A.dylib'
$objectList = Join-Path $objects 'objects.txt'
[System.IO.File]::WriteAllLines($objectList, [string[]]$objectFiles)
& $linker -arch arm64 -platform_version macos 11.0 11.0 -fixup_chains -dead_strip -dylib -install_name $installName `
    -compatibility_version $compatibilityVersion -current_version $currentVersion -exported_symbols_list $exportList `
    -filelist $objectList "-L$llvmRuntime" (Join-Path $llvmRuntime 'libc++.1.dylib') (Join-Path $llvmRuntime 'libc++abi.dylib') `
    (Join-Path $llvmRuntime 'libunwind.dylib') $stub -o $dylib
if ($LASTEXITCODE) { throw 'ICU link failed' }
$exported = @(& $nm -gU $dylib | ForEach-Object { ($_ -split '\s+')[-1] } | Sort-Object -Unique)
$difference = Compare-Object ($exports | Sort-Object) $exported
if ($difference) { throw "libicucore.A.dylib export set differs: $(($difference | ForEach-Object { $_.SideIndicator + $_.InputObject }) -join ', ')" }
$imports = @(& $nm -u $dylib | ForEach-Object { $_.Trim() } | Where-Object { $_ -and $_ -notmatch '^__Z|^___cxa_|^___dynamic_cast$' } | Sort-Object -Unique)
$difference = Compare-Object ($expectedImports | Sort-Object) $imports
if ($difference) { throw "ICU libSystem imports changed: $(($difference | ForEach-Object { $_.SideIndicator + $_.InputObject }) -join ', ')" }

Copy-Item -LiteralPath (Join-Path $repo 'third_party/icu/LICENSE') -Destination (Join-Path $out 'LICENSE.icu.txt')
Copy-Item -LiteralPath (Join-Path $repo 'runtime/THIRD_PARTY_NOTICES.md') -Destination (Join-Path $out 'THIRD_PARTY_NOTICES.md')
Write-Host "Built $dylib from ICU $version, $($exports.Count) exports, $((Get-Item $dylib).Length) bytes"

function Get-Sha256([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Get-RelativeName([string]$root, [string]$path) { $path.Substring($root.Length).TrimStart('\', '/').Replace('\', '/') }
$provenance = [ordered]@{
    source = $upstream
    source_version = $version
    source_archive = $archive
    source_archive_sha256 = $archiveSha256
    vendored = 'third_party/icu'
    license = 'Unicode-3.0'
    install_name = $installName
    compatibility_version = $compatibilityVersion
    current_version = $currentVersion
    target = 'arm64-apple-macos11'
    architecture_flags = @('-mcpu=generic', '-march=armv8-a')
    optimization = '-O2'
    configuration = ($defines -join ' ')
    data = 'icudt79l: uts46.nrm only (third_party/icu/source/data/in)'
    sources = @($modules | ForEach-Object { [ordered]@{ name = "third_party/icu/source/common/$_.cpp"; sha256 = (Get-Sha256 (Join-Path $source "$_.cpp")) } }) +
        @([ordered]@{ name = 'third_party/icu/source/data/in/uts46.nrm'; sha256 = (Get-Sha256 (Join-Path $repo 'third_party/icu/source/data/in/uts46.nrm')) })
    clang_version = ((& $Clang --version) | Out-String).Trim()
    linker_version = ((& $linker --version) | Out-String).Trim()
    guest_header_profile = 'Alchemy-authored Darwin LP64 C/POSIX declarations and LLVM libc++ headers; no Apple SDK payloads'
    build_script_sha256 = (Get-Sha256 $PSCommandPath)
    configuration_files = @(Get-ChildItem -LiteralPath $config -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{ name = 'runtime/icu/' + (Get-RelativeName $config $_.FullName); sha256 = (Get-Sha256 $_.FullName) }
    })
    exports = $exports
    libsystem_imports = $expectedImports
    runtime_dependencies = @('/usr/lib/libc++.1.dylib')
    outputs = @([ordered]@{ name = 'libicucore.A.dylib'; sha256 = (Get-Sha256 $dylib) })
    licenses = @(@('LICENSE.icu.txt', 'THIRD_PARTY_NOTICES.md') | ForEach-Object {
        [ordered]@{ name = $_; sha256 = (Get-Sha256 (Join-Path $out $_)) }
    })
}
$provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'provenance.json') -Encoding utf8
