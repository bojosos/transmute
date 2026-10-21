# Builds upstream libexpat (MIT) as an ARM64 Mach-O guest runtime library that
# claims Apple's install name /usr/lib/libexpat.1.dylib. Pass the output
# directory to alchemy/alchemy-fx with --runtime-dir; the loader accepts the
# identity only with the provenance.json this script writes (see
# include/alchemy/runtime_libraries.hpp). No Apple SDK, header or binary is used:
# declarations come from runtime/include and runtime/expat/expat_config.h.
param(
    [string]$SourceDirectory = "$PSScriptRoot/../artifacts/expat-source",
    [string]$OutputDirectory = "$PSScriptRoot/../artifacts/alchemy-expat-arm64",
    [string]$Clang = 'C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/bin/clang.exe',
    [switch]$Fetch
)
$ErrorActionPreference = 'Stop'
$expatRepository = 'https://github.com/libexpat/libexpat'
$expatTag = 'R_2_8_5'
$expatCommit = '4b3f0b06f39fb5529cead381694f8929901bc273'
$installName = '/usr/lib/libexpat.1.dylib'
# The versions Apple's libexpat.1 carries; consumers record compatibility 7.0.0.
$compatibilityVersion = '7.0.0'
$currentVersion = '8.0.0'
# Every libSystem symbol the pinned sources may import. A different set fails
# the build so new host requirements are reviewed against Alchemy's provider.
$expectedImports = @('___error', '___stderrp', '_arc4random_buf', '_bzero', '_fprintf', '_fputs', '_free', '_fwrite',
    '_getenv', '_malloc', '_memcmp', '_memcpy', '_memmove', '_memset', '_realloc', '_strlen', '_strncmp', '_strtoul')

$repo = (Resolve-Path "$PSScriptRoot/..").Path
if (!(Test-Path -LiteralPath $SourceDirectory)) {
    if (!$Fetch) { throw 'expat source is missing. Pass -Fetch to retrieve the pinned MIT-licensed release.' }
    & git -c advice.detachedHead=false clone --depth 1 --branch $expatTag "$expatRepository.git" $SourceDirectory
    if ($LASTEXITCODE) { throw 'expat source fetch failed' }
}
$actual = (& git -C $SourceDirectory rev-parse HEAD).Trim()
if ($actual -ne $expatCommit) { throw "Expected expat $expatTag commit $expatCommit; found $actual" }
$dirty = & git -C $SourceDirectory status --porcelain --untracked-files=no
if ($dirty) { throw "expat checkout has local modifications; restore $expatTag before building" }
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$lib = Join-Path $source 'expat/lib'
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$out = (Resolve-Path -LiteralPath $OutputDirectory).Path
$objects = Join-Path $out 'obj'
$null = New-Item -ItemType Directory -Force -Path $objects
$sdk = Join-Path $repo 'runtime/include'
$config = Join-Path $repo 'runtime/expat'
$tools = Split-Path $Clang
$linker = Join-Path $tools 'ld64.lld.exe'
$nm = Join-Path $tools 'llvm-nm.exe'
$resourceInclude = ((& $Clang -print-resource-dir).Trim()) + '/include'
$flags = @('--target=arm64-apple-macos11', '-mcpu=generic', '-march=armv8-a', '-O2', '-fno-stack-protector', '-nostdinc',
    '-isystem', $sdk, '-isystem', $resourceInclude, '-I', $config, '-DHAVE_EXPAT_CONFIG_H', '-DNDEBUG',
    '-DXML_ENABLE_VISIBILITY=1', '-fvisibility=hidden', '-std=c99', '-Wall', '-Werror')

# expat's library sources for this profile (expat/CMakeLists.txt): the parser,
# role and token modules, and the arc4random_buf entropy source.
$files = @('xmlparse.c', 'xmlrole.c', 'xmltok.c', 'random_arc4random_buf.c')
$objectFiles = @()
foreach ($file in $files) {
    $object = Join-Path $objects ($file -replace '\.c$', '.o')
    & $Clang @flags -c (Join-Path $lib $file) -o $object
    if ($LASTEXITCODE) { throw "expat compile failed: $file" }
    $objectFiles += $object
}

# expat's public API from its own export definition, without
# XML_GetAttributeInfo (XML_ATTR_INFO is off, as in upstream's default build).
$exports = @(Get-Content -LiteralPath (Join-Path $lib 'libexpat.def.cmake') | ForEach-Object {
    if ($_ -match '^\s*(@(?<guard>\w+)@)?\s*(?<name>XML_\w+)\s+@\d+\s*$') {
        if ($Matches['guard'] -ne '_EXPAT_COMMENT_ATTR_INFO') { '_' + $Matches['name'] }
    }
} | Sort-Object -Unique)
if ($exports.Count -lt 70) { throw "unexpected expat export definition ($($exports.Count) names)" }
$exportList = Join-Path $out 'libexpat.exports'
[System.IO.File]::WriteAllLines($exportList, [string[]]$exports)

$undefined = @($objectFiles | ForEach-Object { & $nm -u $_ } | ForEach-Object { $_.Trim() } | Where-Object { $_ } | Sort-Object -Unique)
$internal = @($objectFiles | ForEach-Object { & $nm -g --defined-only $_ } | ForEach-Object { ($_ -split '\s+')[-1] } | Sort-Object -Unique)
$imports = @($undefined | Where-Object { $internal -notcontains $_ })
$difference = Compare-Object ($expectedImports | Sort-Object) $imports
if ($difference) { throw "expat libSystem imports changed: $(($difference | ForEach-Object { $_.SideIndicator + $_.InputObject }) -join ', ')" }

# Link-only libSystem interface for two-level binding; the symbols are
# implemented by Alchemy's libSystem provider at run time.
$stub = Join-Path $out 'link/libSystem.B.tbd'
$null = New-Item -ItemType Directory -Force -Path (Split-Path $stub)
$tbd = @('--- !tapi-tbd', 'tbd-version: 4', 'targets: [ arm64-macos ]', "install-name: '/usr/lib/libSystem.B.dylib'",
    'current-version: 1.0.0', 'exports:', '  - targets: [ arm64-macos ]', "    symbols: [ $($expectedImports -join ', ') ]", '...')
[System.IO.File]::WriteAllLines($stub, [string[]]$tbd)

$dylib = Join-Path $out 'libexpat.1.dylib'
& $linker -arch arm64 -platform_version macos 11.0 11.0 -fixup_chains -dylib -install_name $installName `
    -compatibility_version $compatibilityVersion -current_version $currentVersion -exported_symbols_list $exportList `
    @objectFiles $stub -o $dylib
if ($LASTEXITCODE) { throw 'expat link failed' }
$exported = @(& $nm -gU $dylib | ForEach-Object { ($_ -split '\s+')[-1] } | Sort-Object -Unique)
$difference = Compare-Object $exports $exported
if ($difference) { throw "libexpat.1.dylib export set differs from expat's API: $(($difference | ForEach-Object { $_.SideIndicator + $_.InputObject }) -join ', ')" }

$include = Join-Path $out 'include'
$null = New-Item -ItemType Directory -Force -Path $include
foreach ($header in @('expat.h', 'expat_external.h')) { Copy-Item -LiteralPath (Join-Path $lib $header) -Destination (Join-Path $include $header) }
Copy-Item -LiteralPath (Join-Path $source 'expat/COPYING') -Destination (Join-Path $out 'LICENSE.expat.txt')
Copy-Item -LiteralPath (Join-Path $repo 'runtime/THIRD_PARTY_NOTICES.md') -Destination (Join-Path $out 'THIRD_PARTY_NOTICES.md')
Write-Host "Built $dylib from expat $expatTag ($expatCommit), $($exports.Count) exports"

function Get-Sha256([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Get-RelativeName([string]$root, [string]$path) { $path.Substring($root.Length).TrimStart('\', '/').Replace('\', '/') }
$provenance = [ordered]@{
    source = $expatRepository
    source_tag = $expatTag
    source_commit = $expatCommit
    license = 'MIT'
    install_name = $installName
    compatibility_version = $compatibilityVersion
    current_version = $currentVersion
    target = 'arm64-apple-macos11'
    architecture_flags = @('-mcpu=generic', '-march=armv8-a')
    optimization = '-O2'
    configuration = 'XML_DTD XML_GE XML_NS XML_CONTEXT_BYTES=1024; XML_ATTR_INFO off; entropy arc4random_buf'
    sources = @($files | ForEach-Object { [ordered]@{ name = "expat/lib/$_"; sha256 = (Get-Sha256 (Join-Path $lib $_)) } })
    clang_version = ((& $Clang --version) | Out-String).Trim()
    linker_version = ((& $linker --version) | Out-String).Trim()
    guest_header_profile = 'Alchemy-authored Darwin LP64 C/POSIX declarations; no Apple SDK payloads'
    build_script_sha256 = (Get-Sha256 $PSCommandPath)
    configuration_header = [ordered]@{ name = 'runtime/expat/expat_config.h'; sha256 = (Get-Sha256 (Join-Path $config 'expat_config.h')) }
    guest_headers = @(Get-ChildItem -LiteralPath $sdk -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{ name = (Get-RelativeName $sdk $_.FullName); sha256 = (Get-Sha256 $_.FullName) }
    })
    exports = $exports
    libsystem_imports = $expectedImports
    outputs = @([ordered]@{ name = 'libexpat.1.dylib'; sha256 = (Get-Sha256 $dylib) })
    licenses = @(@('LICENSE.expat.txt', 'THIRD_PARTY_NOTICES.md') | ForEach-Object {
        [ordered]@{ name = $_; sha256 = (Get-Sha256 (Join-Path $out $_)) }
    })
}
$provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'provenance.json') -Encoding utf8
