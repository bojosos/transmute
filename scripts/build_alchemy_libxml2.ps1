# Builds libxml2 (MIT, vendored in third_party/libxml2) as an ARM64 Mach-O guest
# runtime library that claims Apple's install name /usr/lib/libxml2.2.dylib. Pass
# the output directory to alchemy/alchemy-fx with --runtime-dir; the loader
# accepts the identity only with the provenance.json this script writes (see
# include/alchemy/runtime_libraries.hpp). No Apple SDK, header or binary is used:
# declarations come from runtime/include and runtime/libxml2/config.h.
#
# The output's include/ holds libxml2's public headers with the generated
# libxml/xmlversion.h, for runtimes built on top of it (libxslt).
param(
    [string]$SourceDirectory = "$PSScriptRoot/../third_party/libxml2",
    [string]$OutputDirectory = "$PSScriptRoot/../artifacts/alchemy-libxml2-arm64",
    [string]$Clang = 'C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/bin/clang.exe'
)
$ErrorActionPreference = 'Stop'
$upstream = 'https://gitlab.gnome.org/GNOME/libxml2'
$version = '2.15.4'
$archive = 'https://download.gnome.org/sources/libxml2/2.15/libxml2-2.15.4.tar.xz'
$archiveSha256 = '98087fd181d9070724f3fbc65c7377db03038eb92bd882374daff44940138821'
$installName = '/usr/lib/libxml2.2.dylib'
# The versions macOS's libxml2.2 carries; consumers record compatibility 10.0.0.
$compatibilityVersion = '10.0.0'
$currentVersion = '10.9.0'

# Upstream CMake options (1 = on). LEGACY stays off: in 2.15 it forces zlib.
$features = [ordered]@{
    WITH_C14N = 1; WITH_CATALOG = 1; WITH_DEBUG = 1; WITH_HTML = 1; WITH_HTTP = 1; WITH_ICONV = 1; WITH_ICU = 0
    WITH_ISO8859X = 1; WITH_MODULES = 1; WITH_OUTPUT = 1; WITH_PATTERN = 1; WITH_PUSH = 1; WITH_READER = 1
    WITH_REGEXPS = 1; WITH_RELAXNG = 1; WITH_SAX1 = 1; WITH_SCHEMAS = 1; WITH_SCHEMATRON = 1; WITH_THREADS = 1
    WITH_THREAD_ALLOC = 0; WITH_VALID = 1; WITH_WRITER = 1; WITH_XINCLUDE = 1; WITH_XPATH = 1; WITH_XPTR = 1
    WITH_ZLIB = 0
}
# The library sources upstream's CMakeLists.txt lists for those options.
$files = @('buf.c', 'chvalid.c', 'dict.c', 'encoding.c', 'entities.c', 'error.c', 'globals.c', 'hash.c', 'list.c',
    'parser.c', 'parserInternals.c', 'SAX2.c', 'threads.c', 'tree.c', 'uri.c', 'valid.c', 'xmlIO.c', 'xmlmemory.c',
    'xmlstring.c', 'c14n.c', 'catalog.c', 'debugXML.c', 'HTMLparser.c', 'HTMLtree.c', 'nanohttp.c', 'xmlmodule.c',
    'xmlsave.c', 'pattern.c', 'xmlreader.c', 'xmlregexp.c', 'relaxng.c', 'xmlschemas.c', 'xmlschemastypes.c',
    'schematron.c', 'xmlwriter.c', 'xinclude.c', 'xpath.c', 'xlink.c', 'xpointer.c')
# Every symbol the sources may import, by library. A different set fails the
# build so new host requirements are reviewed against Alchemy's providers.
$iconvImports = @('_iconv', '_iconv_close', '_iconv_open')
$expectedImports = @('___cxa_atexit', '___error', '___exp10', '___stderrp', '___stdinp', '___stdoutp', '_abort',
    '_bsearch', '_bzero', '_calloc', '_close', '_dlclose', '_dlerror', '_dlopen', '_dlsym', '_dup', '_exit',
    '_fclose', '_ferror', '_fflush', '_fmod', '_fopen', '_fprintf', '_fputc', '_fputs', '_fread', '_free',
    '_fwrite', '_getentropy', '_getenv', '_log10', '_malloc', '_memchr', '_memcpy', '_memmove', '_memset', '_open',
    '_pthread_cond_destroy', '_pthread_cond_init', '_pthread_cond_signal', '_pthread_cond_wait', '_pthread_equal',
    '_pthread_getspecific', '_pthread_key_create', '_pthread_key_delete', '_pthread_mutex_destroy',
    '_pthread_mutex_init', '_pthread_mutex_lock', '_pthread_mutex_unlock', '_pthread_once', '_pthread_self',
    '_pthread_setspecific', '_read', '_realloc', '_snprintf', '_sscanf', '_stat', '_strcat', '_strchr', '_strcmp',
    '_strlen', '_strncmp', '_strncpy', '_strstr', '_strtoul', '_time', '_toupper', '_vfprintf', '_vsnprintf',
    '_write')

$repo = (Resolve-Path "$PSScriptRoot/..").Path
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
function Get-Sha256([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Get-RelativeName([string]$root, [string]$path) { $path.Substring($root.Length).TrimStart('\', '/').Replace('\', '/') }

# The vendored files must be byte-identical to the release (PROVENANCE.md).
$pinned = @(Get-Content -LiteralPath (Join-Path $source 'PROVENANCE.md') | Where-Object { $_ -match '^[0-9a-f]{64}  \S' })
if ($pinned.Count -lt 100) { throw "third_party/libxml2/PROVENANCE.md lists $($pinned.Count) hashes" }
foreach ($line in $pinned) {
    $hash, $name = $line -split '  ', 2
    $path = Join-Path $source $name
    if (!(Test-Path -LiteralPath $path)) { throw "vendored libxml2 file missing: $name" }
    if ((Get-Sha256 $path) -ne $hash) { throw "vendored libxml2 file differs from $version`: $name" }
}

$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$out = (Resolve-Path -LiteralPath $OutputDirectory).Path
$objects = Join-Path $out 'obj'
$include = Join-Path $out 'include'
$null = New-Item -ItemType Directory -Force -Path $objects, (Join-Path $include 'libxml')
$sdk = Join-Path $repo 'runtime/include'
$config = Join-Path $repo 'runtime/libxml2'
$tools = Split-Path $Clang
$linker = Join-Path $tools 'ld64.lld.exe'
$nm = Join-Path $tools 'llvm-nm.exe'
$resourceInclude = ((& $Clang -print-resource-dir).Trim()) + '/include'

# libxml/xmlversion.h from upstream's template and the feature table.
$values = @{ VERSION = $version; LIBXML_VERSION_NUMBER = '21504'; LIBXML_VERSION_EXTRA = ''; MODULE_EXTENSION = '.so' }
foreach ($key in $features.Keys) { $values[$key] = [string]$features[$key] }
$template = [System.IO.File]::ReadAllText((Join-Path $source 'include/libxml/xmlversion.h.in'))
$generated = [regex]::Replace($template, '@([A-Z0-9_]+)@', {
    param($match)
    $key = $match.Groups[1].Value
    if (!$values.ContainsKey($key)) { throw "xmlversion.h.in uses unknown value $key" }
    $values[$key]
})
[System.IO.File]::WriteAllText((Join-Path $include 'libxml/xmlversion.h'), $generated)
foreach ($header in Get-ChildItem -LiteralPath (Join-Path $source 'include/libxml') -Filter '*.h') {
    Copy-Item -LiteralPath $header.FullName -Destination (Join-Path $include "libxml/$($header.Name)")
}

$flags = @('--target=arm64-apple-macos11', '-mcpu=generic', '-march=armv8-a', '-O2', '-fno-stack-protector', '-nostdinc',
    '-isystem', $sdk, '-isystem', $resourceInclude, '-I', $config, '-I', $include, '-I', (Join-Path $source 'include'),
    '-I', $source, '-DNDEBUG', '-std=gnu11', '-Wall', '-Werror')
$objectFiles = @()
foreach ($file in $files) {
    $object = Join-Path $objects ($file -replace '\.c$', '.o')
    & $Clang @flags -c (Join-Path $source $file) -o $object
    if ($LASTEXITCODE) { throw "libxml2 compile failed: $file" }
    $objectFiles += $object
}

$undefined = @($objectFiles | ForEach-Object { & $nm -u $_ } | ForEach-Object { $_.Trim() } | Where-Object { $_ } | Sort-Object -Unique)
$internal = @($objectFiles | ForEach-Object { & $nm -g --defined-only $_ } | ForEach-Object { ($_ -split '\s+')[-1] } | Sort-Object -Unique)
# ___dso_handle is defined by the linker for __cxa_atexit (the destructor attribute).
$imports = @($undefined | Where-Object { $internal -notcontains $_ -and $_ -ne '___dso_handle' })
$systemImports = @($imports | Where-Object { $iconvImports -notcontains $_ })
$difference = Compare-Object ($expectedImports | Sort-Object) $systemImports
if ($difference) { throw "libxml2 libSystem imports changed: $(($difference | ForEach-Object { $_.SideIndicator + $_.InputObject }) -join ', ')" }
$missingIconv = @($iconvImports | Where-Object { $imports -notcontains $_ })
if ($missingIconv) { throw "libxml2 no longer imports $($missingIconv -join ', '); review the iconv stub" }

# Link-only interfaces for two-level binding; the symbols are implemented by
# Alchemy's libSystem and libiconv providers at run time.
function Write-Stub([string]$path, [string]$identity, [string]$current, [string[]]$symbols) {
    $null = New-Item -ItemType Directory -Force -Path (Split-Path $path)
    $tbd = @('--- !tapi-tbd', 'tbd-version: 4', 'targets: [ arm64-macos ]', "install-name: '$identity'",
        "current-version: $current", "compatibility-version: $current", 'exports:', '  - targets: [ arm64-macos ]', "    symbols: [ $($symbols -join ', ') ]", '...')
    [System.IO.File]::WriteAllLines($path, [string[]]$tbd)
}
$systemStub = Join-Path $out 'link/libSystem.B.tbd'
$iconvStub = Join-Path $out 'link/libiconv.2.tbd'
Write-Stub $systemStub '/usr/lib/libSystem.B.dylib' '1.0.0' $expectedImports
Write-Stub $iconvStub '/usr/lib/libiconv.2.dylib' '7.0.0' $iconvImports

$dylib = Join-Path $out 'libxml2.2.dylib'
& $linker -arch arm64 -platform_version macos 11.0 11.0 -fixup_chains -dylib -install_name $installName `
    -compatibility_version $compatibilityVersion -current_version $currentVersion @objectFiles $systemStub $iconvStub -o $dylib
if ($LASTEXITCODE) { throw 'libxml2 link failed' }
$exports = @(& $nm -gU $dylib | ForEach-Object { ($_ -split '\s+')[-1] } | Sort-Object -Unique)
# The names the toolchains and libxslt bind must be among them.
foreach ($name in @('_xmlReadMemory', '_xmlDocDumpFormatMemoryEnc', '_xmlNewDoc', '_xmlNewNs', '_xmlSetGenericErrorFunc',
        '_xmlXPathEval', '_xmlParseFile', '_xmlSAXVersion', '_htmlParseDoc', '_xmlTextReaderRead', '_xmlNewTextWriterMemory')) {
    if ($exports -notcontains $name) { throw "libxml2.2.dylib does not export $name" }
}

Copy-Item -LiteralPath (Join-Path $source 'Copyright') -Destination (Join-Path $out 'LICENSE.libxml2.txt')
Copy-Item -LiteralPath (Join-Path $repo 'runtime/THIRD_PARTY_NOTICES.md') -Destination (Join-Path $out 'THIRD_PARTY_NOTICES.md')
Write-Host "Built $dylib from libxml2 $version, $($exports.Count) exports"

$provenance = [ordered]@{
    source = $upstream
    source_version = $version
    source_archive = $archive
    source_archive_sha256 = $archiveSha256
    license = 'MIT'
    install_name = $installName
    compatibility_version = $compatibilityVersion
    current_version = $currentVersion
    target = 'arm64-apple-macos11'
    architecture_flags = @('-mcpu=generic', '-march=armv8-a')
    optimization = '-O2'
    configuration = ($features.Keys | ForEach-Object { "$_=$($features[$_])" }) -join ' '
    sources = @($files | ForEach-Object { [ordered]@{ name = "third_party/libxml2/$_"; sha256 = (Get-Sha256 (Join-Path $source $_)) } })
    clang_version = ((& $Clang --version) | Out-String).Trim()
    linker_version = ((& $linker --version) | Out-String).Trim()
    guest_header_profile = 'Alchemy-authored Darwin LP64 C/POSIX declarations; no Apple SDK payloads'
    build_script_sha256 = (Get-Sha256 $PSCommandPath)
    configuration_header = [ordered]@{ name = 'runtime/libxml2/config.h'; sha256 = (Get-Sha256 (Join-Path $config 'config.h')) }
    guest_headers = @(Get-ChildItem -LiteralPath $sdk -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{ name = (Get-RelativeName $sdk $_.FullName); sha256 = (Get-Sha256 $_.FullName) }
    })
    exports = $exports
    libsystem_imports = $expectedImports
    libiconv_imports = $iconvImports
    outputs = @([ordered]@{ name = 'libxml2.2.dylib'; sha256 = (Get-Sha256 $dylib) })
    licenses = @(@('LICENSE.libxml2.txt', 'THIRD_PARTY_NOTICES.md') | ForEach-Object {
        [ordered]@{ name = $_; sha256 = (Get-Sha256 (Join-Path $out $_)) }
    })
}
$provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'provenance.json') -Encoding utf8
