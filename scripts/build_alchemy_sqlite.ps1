# Builds the SQLite amalgamation (public domain, vendored in third_party/sqlite) as an ARM64 Mach-O
# guest runtime library that claims Apple's install name /usr/lib/libsqlite3.dylib. Pass the output
# directory to alchemy/alchemy-fx with --runtime-dir; the loader accepts the identity only with the
# provenance.json this script writes (include/alchemy/runtime_libraries.hpp). No Apple SDK, header
# or binary is used: declarations come from runtime/include and runtime/sqlite, the feature set from
# runtime/sqlite/sqlite_cfg.h (docs/ALCHEMY_OSS_RUNTIMES.md).
param(
    [string]$OutputDirectory = "$PSScriptRoot/../artifacts/alchemy-sqlite-arm64",
    [string]$Clang = 'C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/bin/clang.exe'
)
$ErrorActionPreference = 'Stop'
$upstream = 'https://www.sqlite.org'
$version = '3.54.1'
$archive = 'https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip'
$archiveSha256 = '0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9'
$archiveSha3 = '92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30'
$installName = '/usr/lib/libsqlite3.dylib'
# The versions macOS applications record for /usr/lib/libsqlite3.dylib.
$compatibilityVersion = '9.0.0'
$currentVersion = '380.0.0'
# Every libSystem symbol the amalgamation may import with this configuration. A different set
# fails the build so new host requirements are reviewed against Alchemy's provider.
$expectedImports = @('___error', '___stack_chk_fail', '___stack_chk_guard', '_access', '_acos', '_acosh', '_asin',
    '_asinh', '_atan', '_atan2', '_atanh', '_bzero', '_close', '_cos', '_cosh', '_dlclose', '_dlerror', '_dlopen', '_dlsym',
    '_exp', '_fchmod', '_fchown', '_fcntl', '_fmod', '_free', '_fstat', '_fstatfs', '_fsync', '_ftruncate', '_getcwd',
    '_getenv', '_geteuid', '_getpid', '_gettimeofday', '_localtime_r', '_log', '_log10', '_log2', '_lstat', '_malloc',
    '_memchr', '_memcmp', '_memcpy', '_memmove', '_memset', '_mkdir', '_mmap', '_munmap', '_nanosleep', '_open', '_pow',
    '_pread', '_pthread_create', '_pthread_join', '_pthread_mutex_destroy', '_pthread_mutex_init', '_pthread_mutex_lock',
    '_pthread_mutex_trylock', '_pthread_mutex_unlock', '_pthread_mutexattr_destroy', '_pthread_mutexattr_init',
    '_pthread_mutexattr_settype', '_pwrite', '_qsort', '_read', '_readlink', '_realloc', '_rmdir', '_sin', '_sinh', '_sqrt',
    '_stat', '_strchr', '_strcmp', '_strcspn', '_strlen', '_strncmp', '_strrchr', '_strspn', '_sysconf', '_tan', '_tanh',
    '_time', '_trunc', '_unlink', '_utimes', '_write')

$repo = (Resolve-Path "$PSScriptRoot/..").Path
$source = Join-Path $repo 'third_party/sqlite'
$config = Join-Path $repo 'runtime/sqlite'
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

# runtime/sqlite/include comes first: its headers add to the project's guest headers (#include_next).
$flags = @('--target=arm64-apple-macos11', '-mcpu=generic', '-march=armv8-a', '-O2', '-nostdinc',
    '-isystem', (Join-Path $config 'include'), '-isystem', $sdk, '-isystem', $resourceInclude, '-I', $config,
    '-D_HAVE_SQLITE_CONFIG_H', '-DNDEBUG', '-Werror')
$files = @('sqlite3.c')
$objectFiles = @()
foreach ($file in $files) {
    $object = Join-Path $objects ($file -replace '\.c$', '.o')
    & $Clang @flags -c (Join-Path $source $file) -o $object
    if ($LASTEXITCODE) { throw "SQLite compile failed: $file" }
    $objectFiles += $object
}

# The C interface: every global the amalgamation defines (SQLITE_API functions and data such as
# sqlite3_version and sqlite3_temp_directory; internal routines are static).
$exports = @($objectFiles | ForEach-Object { & $nm -g --defined-only $_ } | ForEach-Object { ($_ -split '\s+')[-1] } |
    Where-Object { $_ -match '^_sqlite3' } | Sort-Object -Unique)
if ($exports.Count -lt 250) { throw "unexpected SQLite export list ($($exports.Count) names)" }
$exportList = Join-Path $out 'libsqlite3.exports'
[System.IO.File]::WriteAllLines($exportList, [string[]]$exports)

$undefined = @($objectFiles | ForEach-Object { & $nm -u $_ } | ForEach-Object { $_.Trim() } | Where-Object { $_ } | Sort-Object -Unique)
$difference = Compare-Object ($expectedImports | Sort-Object) $undefined
if ($difference) { throw "SQLite libSystem imports changed: $(($difference | ForEach-Object { $_.SideIndicator + $_.InputObject }) -join ', ')" }

$stub = Join-Path $out 'link/libSystem.B.tbd'
$null = New-Item -ItemType Directory -Force -Path (Split-Path $stub)
$tbd = @('--- !tapi-tbd', 'tbd-version: 4', 'targets: [ arm64-macos ]', "install-name: '/usr/lib/libSystem.B.dylib'",
    'current-version: 1.0.0', 'exports:', '  - targets: [ arm64-macos ]', "    symbols: [ $($expectedImports -join ', ') ]", '...')
[System.IO.File]::WriteAllLines($stub, [string[]]$tbd)

$dylib = Join-Path $out 'libsqlite3.dylib'
& $linker -arch arm64 -platform_version macos 11.0 11.0 -fixup_chains -dylib -install_name $installName `
    -compatibility_version $compatibilityVersion -current_version $currentVersion -exported_symbols_list $exportList `
    @objectFiles $stub -o $dylib
if ($LASTEXITCODE) { throw 'SQLite link failed' }
$exported = @(& $nm -gU $dylib | ForEach-Object { ($_ -split '\s+')[-1] } | Sort-Object -Unique)
$difference = Compare-Object $exports $exported
if ($difference) { throw "libsqlite3.dylib export set differs: $(($difference | ForEach-Object { $_.SideIndicator + $_.InputObject }) -join ', ')" }

foreach ($header in @('sqlite3.h', 'sqlite3ext.h')) { Copy-Item -LiteralPath (Join-Path $source $header) -Destination (Join-Path $include $header) }
Copy-Item -LiteralPath (Join-Path $repo 'runtime/THIRD_PARTY_NOTICES.md') -Destination (Join-Path $out 'THIRD_PARTY_NOTICES.md')
Write-Host "Built $dylib from SQLite $version, $($exports.Count) exports"

function Get-Sha256([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Get-RelativeName([string]$root, [string]$path) { $path.Substring($root.Length).TrimStart('\', '/').Replace('\', '/') }
$provenance = [ordered]@{
    source = $upstream
    source_version = $version
    source_archive = $archive
    source_archive_sha256 = $archiveSha256
    source_archive_sha3_256 = $archiveSha3
    vendored = 'third_party/sqlite'
    license = 'public domain (SQLite blessing)'
    install_name = $installName
    compatibility_version = $compatibilityVersion
    current_version = $currentVersion
    target = 'arm64-apple-macos11'
    architecture_flags = @('-mcpu=generic', '-march=armv8-a')
    optimization = '-O2'
    configuration = 'runtime/sqlite/sqlite_cfg.h'
    sources = @($files | ForEach-Object { [ordered]@{ name = "third_party/sqlite/$_"; sha256 = (Get-Sha256 (Join-Path $source $_)) } })
    clang_version = ((& $Clang --version) | Out-String).Trim()
    linker_version = ((& $linker --version) | Out-String).Trim()
    guest_header_profile = 'Alchemy-authored Darwin LP64 C/POSIX declarations; no Apple SDK payloads'
    build_script_sha256 = (Get-Sha256 $PSCommandPath)
    configuration_headers = @(Get-ChildItem -LiteralPath $config -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{ name = 'runtime/sqlite/' + (Get-RelativeName $config $_.FullName); sha256 = (Get-Sha256 $_.FullName) }
    })
    guest_headers = @(Get-ChildItem -LiteralPath $sdk -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{ name = (Get-RelativeName $sdk $_.FullName); sha256 = (Get-Sha256 $_.FullName) }
    })
    exports = $exports
    libsystem_imports = $expectedImports
    outputs = @([ordered]@{ name = 'libsqlite3.dylib'; sha256 = (Get-Sha256 $dylib) })
    licenses = @(@('THIRD_PARTY_NOTICES.md') | ForEach-Object {
        [ordered]@{ name = $_; sha256 = (Get-Sha256 (Join-Path $out $_)) }
    })
}
$provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'provenance.json') -Encoding utf8
