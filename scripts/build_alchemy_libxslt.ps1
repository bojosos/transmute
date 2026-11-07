# Builds libxslt and libexslt (MIT, vendored in third_party/libxslt) as ARM64
# Mach-O guest runtime libraries that claim Apple's install names
# /usr/lib/libxslt.1.dylib and /usr/lib/libexslt.0.dylib. They link against the
# guest libxml2 runtime (scripts/build_alchemy_libxml2.ps1), whose output
# directory supplies libxml2.2.dylib and its public headers. Pass both output
# directories to alchemy/alchemy-fx with --runtime-dir; the loader accepts the
# identities only with the provenance.json this script writes (see
# include/alchemy/runtime_libraries.hpp). No Apple SDK, header or binary is
# used: declarations come from runtime/include and runtime/libxslt/config.h.
param(
    [string]$SourceDirectory = "$PSScriptRoot/../third_party/libxslt",
    [string]$LibXml2Directory = "$PSScriptRoot/../artifacts/alchemy-libxml2-arm64",
    [string]$OutputDirectory = "$PSScriptRoot/../artifacts/alchemy-libxslt-arm64",
    [string]$Clang = 'C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/bin/clang.exe'
)
$ErrorActionPreference = 'Stop'
$upstream = 'https://gitlab.gnome.org/GNOME/libxslt'
$version = '1.1.45'
$exsltVersion = '0.8.25'
$archive = 'https://download.gnome.org/sources/libxslt/1.1/libxslt-1.1.45.tar.xz'
$archiveSha256 = '9acfe68419c4d06a45c550321b3212762d92f41465062ca4ea19e632ee5d216e'
# libtool's Darwin versions for these releases (version-info 2:45:1 and 8:25:8),
# the scheme macOS's libxslt.1 and libexslt.0 carry.
$libraries = [ordered]@{
    'libxslt.1.dylib' = @{ install = '/usr/lib/libxslt.1.dylib'; compatibility = '3.0.0'; current = '3.45.0'; directory = 'libxslt'
        files = @('attributes.c', 'attrvt.c', 'documents.c', 'extensions.c', 'extra.c', 'functions.c', 'imports.c', 'keys.c',
            'namespaces.c', 'numbers.c', 'pattern.c', 'preproc.c', 'security.c', 'templates.c', 'transform.c', 'variables.c',
            'xslt.c', 'xsltlocale.c', 'xsltutils.c') }
    'libexslt.0.dylib' = @{ install = '/usr/lib/libexslt.0.dylib'; compatibility = '9.0.0'; current = '9.25.0'; directory = 'libexslt'
        files = @('common.c', 'crypto.c', 'date.c', 'dynamic.c', 'exslt.c', 'functions.c', 'math.c', 'saxon.c', 'sets.c', 'strings.c') }
}
# Template values (upstream CMake option names, 1 = on).
$values = @{ VERSION = $version; LIBXSLT_VERSION_NUMBER = '10145'; LIBXSLT_VERSION_EXTRA = ''; WITH_XSLT_DEBUG = '0'
    WITH_TRIO = '0'; WITH_DEBUGGER = '0'; WITH_PROFILER = '1'; WITH_MODULES = '0'
    LIBXSLT_DEFAULT_PLUGINS_PATH = '/usr/lib/libxslt-plugins'; LIBEXSLT_VERSION = $exsltVersion
    LIBEXSLT_VERSION_NUMBER = '825'; LIBEXSLT_VERSION_EXTRA = ''; WITH_CRYPTO = '0' }
# Every libSystem symbol the sources may import. A different set fails the
# build so new host requirements are reviewed against Alchemy's provider.
$expectedImports = @('___error', '___exp10', '___stderrp', '___stdoutp', '_acos', '_asin', '_atan', '_atan2',
    '_bzero', '_clock_gettime', '_cos', '_exp', '_fclose', '_fmod', '_fopen', '_fprintf', '_freelocale', '_fwrite',
    '_getenv', '_gmtime_r', '_localtime_r', '_log', '_memcpy', '_memset', '_mkdir', '_newlocale', '_pow', '_rand',
    '_sin', '_snprintf', '_sscanf', '_stat', '_strlen', '_strtol', '_strxfrm_l', '_tan', '_time', '_vfprintf',
    '_vsnprintf')

$repo = (Resolve-Path "$PSScriptRoot/..").Path
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$xml = (Resolve-Path -LiteralPath $LibXml2Directory).Path
$libxml2 = Join-Path $xml 'libxml2.2.dylib'
if (!(Test-Path -LiteralPath $libxml2) -or !(Test-Path -LiteralPath (Join-Path $xml 'include/libxml/xmlversion.h'))) {
    throw "the libxml2 runtime is missing from $xml; run scripts/build_alchemy_libxml2.ps1 first"
}
function Get-Sha256([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Get-RelativeName([string]$root, [string]$path) { $path.Substring($root.Length).TrimStart('\', '/').Replace('\', '/') }

# The vendored files must be byte-identical to the release (PROVENANCE.md).
$pinned = @(Get-Content -LiteralPath (Join-Path $source 'PROVENANCE.md') | Where-Object { $_ -match '^[0-9a-f]{64}  \S' })
if ($pinned.Count -lt 50) { throw "third_party/libxslt/PROVENANCE.md lists $($pinned.Count) hashes" }
foreach ($line in $pinned) {
    $hash, $name = $line -split '  ', 2
    $path = Join-Path $source $name
    if (!(Test-Path -LiteralPath $path)) { throw "vendored libxslt file missing: $name" }
    if ((Get-Sha256 $path) -ne $hash) { throw "vendored libxslt file differs from $version`: $name" }
}

$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$out = (Resolve-Path -LiteralPath $OutputDirectory).Path
$include = Join-Path $out 'include'
$sdk = Join-Path $repo 'runtime/include'
$config = Join-Path $repo 'runtime/libxslt'
$tools = Split-Path $Clang
$linker = Join-Path $tools 'ld64.lld.exe'
$nm = Join-Path $tools 'llvm-nm.exe'
$resourceInclude = ((& $Clang -print-resource-dir).Trim()) + '/include'

# xsltconfig.h and exsltconfig.h from upstream's templates; public headers beside them.
foreach ($directory in @('libxslt', 'libexslt')) {
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $include $directory)
    foreach ($header in Get-ChildItem -LiteralPath (Join-Path $source $directory) -Filter '*.h') {
        Copy-Item -LiteralPath $header.FullName -Destination (Join-Path $include "$directory/$($header.Name)")
    }
}
foreach ($template in @('libxslt/xsltconfig.h', 'libexslt/exsltconfig.h')) {
    $text = [System.IO.File]::ReadAllText((Join-Path $source "$template.in"))
    $generated = [regex]::Replace($text, '@([A-Z0-9_]+)@', {
        param($match)
        $key = $match.Groups[1].Value
        if (!$values.ContainsKey($key)) { throw "$template.in uses unknown value $key" }
        $values[$key]
    })
    [System.IO.File]::WriteAllText((Join-Path $include $template), $generated)
}

$flags = @('--target=arm64-apple-macos11', '-mcpu=generic', '-march=armv8-a', '-O2', '-fno-stack-protector', '-nostdinc',
    '-isystem', $sdk, '-isystem', $resourceInclude, '-I', $config, '-I', $include, '-I', (Join-Path $xml 'include'),
    '-I', $source, '-I', (Join-Path $source 'libxslt'), '-DHAVE_CONFIG_H', '-DNDEBUG', '-std=gnu11', '-Wall', '-Werror')
$libxml2Exports = @(& $nm -gU $libxml2 | ForEach-Object { ($_ -split '\s+')[-1] })
$systemStub = Join-Path $out 'link/libSystem.B.tbd'
$allImports = [System.Collections.Generic.List[string]]::new()
$built = [ordered]@{}
$exportsByLibrary = [ordered]@{}
$compiled = [ordered]@{}
foreach ($name in $libraries.Keys) {
    $library = $libraries[$name]
    $objects = Join-Path $out "obj/$($library.directory)"
    $null = New-Item -ItemType Directory -Force -Path $objects
    $objectFiles = @()
    foreach ($file in $library.files) {
        $object = Join-Path $objects ($file -replace '\.c$', '.o')
        & $Clang @flags -c (Join-Path $source "$($library.directory)/$file") -o $object
        if ($LASTEXITCODE) { throw "$($library.directory) compile failed: $file" }
        $objectFiles += $object
    }
    $compiled[$name] = $objectFiles
    $undefined = @($objectFiles | ForEach-Object { & $nm -u $_ } | ForEach-Object { $_.Trim() } | Where-Object { $_ } | Sort-Object -Unique)
    $internal = @($objectFiles | ForEach-Object { & $nm -g --defined-only $_ } | ForEach-Object { ($_ -split '\s+')[-1] })
    $provided = $libxml2Exports + @($built.Values | ForEach-Object { $exportsByLibrary[$_] })
    foreach ($symbol in $undefined) {
        if ($internal -notcontains $symbol -and $provided -notcontains $symbol -and $symbol -ne '___dso_handle') { $allImports.Add($symbol) }
    }
    $built[$name] = $name
    $exportsByLibrary[$name] = @($internal | Sort-Object -Unique)
}
$systemImports = @($allImports | Sort-Object -Unique)
$difference = Compare-Object (@($expectedImports) + '' | Sort-Object) (@($systemImports) + '' | Sort-Object)
if ($difference) { throw "libxslt libSystem imports changed: $(($difference | ForEach-Object { $_.SideIndicator + $_.InputObject }) -join ', ')" }

# Link-only libSystem interface for two-level binding; the symbols are
# implemented by Alchemy's libSystem provider at run time.
$null = New-Item -ItemType Directory -Force -Path (Split-Path $systemStub)
$tbd = @('--- !tapi-tbd', 'tbd-version: 4', 'targets: [ arm64-macos ]', "install-name: '/usr/lib/libSystem.B.dylib'",
    'current-version: 1.0.0', 'exports:', '  - targets: [ arm64-macos ]', "    symbols: [ $($expectedImports -join ', ') ]", '...')
[System.IO.File]::WriteAllLines($systemStub, [string[]]$tbd)

$outputs = @()
foreach ($name in $libraries.Keys) {
    $library = $libraries[$name]
    $dylib = Join-Path $out $name
    $dependencies = @($libxml2)
    if ($name -eq 'libexslt.0.dylib') { $dependencies += (Join-Path $out 'libxslt.1.dylib') }
    & $linker -arch arm64 -platform_version macos 11.0 11.0 -fixup_chains -dylib -install_name $library.install `
        -compatibility_version $library.compatibility -current_version $library.current @($compiled[$name]) @dependencies $systemStub -o $dylib
    if ($LASTEXITCODE) { throw "$name link failed" }
    $exports = @(& $nm -gU $dylib | ForEach-Object { ($_ -split '\s+')[-1] } | Sort-Object -Unique)
    $exportsByLibrary[$name] = $exports
    $outputs += [ordered]@{ name = $name; sha256 = (Get-Sha256 $dylib) }
    Write-Host "Built $dylib from libxslt $version, $($exports.Count) exports"
}
foreach ($name in @('_xsltParseStylesheetDoc', '_xsltApplyStylesheet', '_xsltSaveResultToString', '_xsltFreeStylesheet')) {
    if ($exportsByLibrary['libxslt.1.dylib'] -notcontains $name) { throw "libxslt.1.dylib does not export $name" }
}
if ($exportsByLibrary['libexslt.0.dylib'] -notcontains '_exsltRegisterAll') { throw 'libexslt.0.dylib does not export _exsltRegisterAll' }

Copy-Item -LiteralPath (Join-Path $source 'Copyright') -Destination (Join-Path $out 'LICENSE.libxslt.txt')
Copy-Item -LiteralPath (Join-Path $repo 'runtime/THIRD_PARTY_NOTICES.md') -Destination (Join-Path $out 'THIRD_PARTY_NOTICES.md')

$provenance = [ordered]@{
    source = $upstream
    source_version = $version
    libexslt_version = $exsltVersion
    source_archive = $archive
    source_archive_sha256 = $archiveSha256
    license = 'MIT'
    install_names = @($libraries.Keys | ForEach-Object { [ordered]@{ name = $_; install_name = $libraries[$_].install
        compatibility_version = $libraries[$_].compatibility; current_version = $libraries[$_].current } })
    libxml2_runtime = [ordered]@{ name = 'libxml2.2.dylib'; sha256 = (Get-Sha256 $libxml2) }
    target = 'arm64-apple-macos11'
    architecture_flags = @('-mcpu=generic', '-march=armv8-a')
    optimization = '-O2'
    configuration = 'WITH_PROFILER=1 WITH_THREADS=1 WITH_XSLT_DEBUG=0 WITH_DEBUGGER=0 WITH_MODULES=0 WITH_CRYPTO=0 WITH_TRIO=0'
    sources = @($libraries.Keys | ForEach-Object { $library = $libraries[$_]; $library.files | ForEach-Object {
        $relative = "$($library.directory)/$_"
        [ordered]@{ name = "third_party/libxslt/$relative"; sha256 = (Get-Sha256 (Join-Path $source $relative)) } } })
    clang_version = ((& $Clang --version) | Out-String).Trim()
    linker_version = ((& $linker --version) | Out-String).Trim()
    guest_header_profile = 'Alchemy-authored Darwin LP64 C/POSIX declarations; no Apple SDK payloads'
    build_script_sha256 = (Get-Sha256 $PSCommandPath)
    configuration_header = [ordered]@{ name = 'runtime/libxslt/config.h'; sha256 = (Get-Sha256 (Join-Path $config 'config.h')) }
    guest_headers = @(Get-ChildItem -LiteralPath $sdk -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{ name = (Get-RelativeName $sdk $_.FullName); sha256 = (Get-Sha256 $_.FullName) }
    })
    exports = [ordered]@{ 'libxslt.1.dylib' = $exportsByLibrary['libxslt.1.dylib']; 'libexslt.0.dylib' = $exportsByLibrary['libexslt.0.dylib'] }
    libsystem_imports = $expectedImports
    outputs = $outputs
    licenses = @(@('LICENSE.libxslt.txt', 'THIRD_PARTY_NOTICES.md') | ForEach-Object {
        [ordered]@{ name = $_; sha256 = (Get-Sha256 (Join-Path $out $_)) }
    })
}
$provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'provenance.json') -Encoding utf8
