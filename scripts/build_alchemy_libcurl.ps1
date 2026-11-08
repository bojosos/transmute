# Builds libcurl (curl license, vendored in third_party/curl) as an ARM64 Mach-O
# guest runtime library that claims Apple's install name /usr/lib/libcurl.4.dylib.
# Pass the output directory to alchemy/alchemy-fx with --runtime-dir; the loader
# accepts the identity only with the provenance.json this script writes (see
# include/alchemy/runtime_libraries.hpp). No Apple SDK, header or binary is used:
# declarations come from runtime/include and runtime/libcurl/curl_config.h, which
# also describes the profile (HTTP and FILE; no TLS backend).
param(
    [string]$SourceDirectory = "$PSScriptRoot/../third_party/curl",
    [string]$OutputDirectory = "$PSScriptRoot/../artifacts/alchemy-libcurl-arm64",
    [string]$Clang = 'C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/bin/clang.exe'
)
$ErrorActionPreference = 'Stop'
$upstream = 'https://github.com/curl/curl'
$version = '8.23.0'
$archive = 'https://curl.se/download/curl-8.23.0.tar.gz'
$archiveSha256 = 'e3de77cfa6367083c776a26647fc9f633b538c06c3baa73d931a252ac8f72b49'
$installName = '/usr/lib/libcurl.4.dylib'
# The versions macOS's libcurl.4 carries; consumers record compatibility 7.0.0.
$compatibilityVersion = '7.0.0'
$currentVersion = '9.0.0'
# The names macOS programs bind that this build must export (cmake 4.4, cargo 1.99).
$requiredExports = @('_curl_easy_cleanup', '_curl_easy_escape', '_curl_easy_getinfo', '_curl_easy_init',
    '_curl_easy_perform', '_curl_easy_reset', '_curl_easy_setopt', '_curl_easy_strerror', '_curl_formfree',
    '_curl_free', '_curl_global_cleanup', '_curl_global_init', '_curl_global_sslset', '_curl_multi_add_handle',
    '_curl_multi_cleanup', '_curl_multi_info_read', '_curl_multi_init', '_curl_multi_perform',
    '_curl_multi_remove_handle', '_curl_multi_setopt', '_curl_multi_strerror', '_curl_multi_timeout',
    '_curl_multi_wait', '_curl_slist_append', '_curl_slist_free_all', '_curl_version_info')
# curl's macOS init (lib/macos.c) asks SystemConfiguration for the proxy
# settings once, as Apple's libcurl does, so IPv4 literals work on NAT64 networks.
$frameworkImports = [ordered]@{
    '/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation' = @('_CFRelease')
    '/System/Library/Frameworks/SystemConfiguration.framework/Versions/A/SystemConfiguration' = @('_SCDynamicStoreCopyProxies')
}
# Every libSystem symbol the sources may import. A different set fails the
# build so new host requirements are reviewed against Alchemy's provider.
$expectedImports = @('___error', '___stderrp', '___stdinp', '___stdoutp', '_accept', '_arc4random', '_bind',
    '_bsearch', '_bzero', '_calloc', '_clock_gettime', '_close', '_connect', '_fclose', '_fcntl', '_fdopen',
    '_feof', '_ferror', '_fflush', '_fgets', '_fileno', '_fopen', '_fputc', '_fread', '_free', '_freeaddrinfo',
    '_fseek', '_fstat', '_fwrite', '_getaddrinfo', '_getenv', '_gethostname', '_getpeername', '_getsockname',
    '_getsockopt', '_gettimeofday', '_gmtime', '_if_nametoindex', '_lseek', '_malloc', '_memchr', '_memcmp',
    '_memcpy', '_memmove', '_memset', '_open', '_pipe', '_poll', '_pthread_cond_destroy', '_pthread_cond_init',
    '_pthread_cond_signal', '_pthread_cond_timedwait', '_pthread_cond_wait', '_pthread_create', '_pthread_detach',
    '_pthread_equal', '_pthread_join', '_pthread_mutex_destroy', '_pthread_mutex_init', '_pthread_mutex_lock',
    '_pthread_mutex_unlock', '_pthread_self', '_qsort', '_read', '_realloc', '_recv', '_rename', '_select',
    '_send', '_setsockopt', '_snprintf', '_socket', '_stat', '_strchr', '_strcmp', '_strcspn', '_strdup',
    '_strerror', '_strftime', '_strlen', '_strncmp', '_strpbrk', '_strrchr', '_strspn', '_strstr', '_time',
    '_unlink', '_write')

$repo = (Resolve-Path "$PSScriptRoot/..").Path
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
function Get-Sha256([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Get-RelativeName([string]$root, [string]$path) { $path.Substring($root.Length).TrimStart('\', '/').Replace('\', '/') }

# The vendored files must be byte-identical to the release (PROVENANCE.md).
$pinned = @(Get-Content -LiteralPath (Join-Path $source 'PROVENANCE.md') | Where-Object { $_ -match '^[0-9a-f]{64}  \S' })
if ($pinned.Count -lt 300) { throw "third_party/curl/PROVENANCE.md lists $($pinned.Count) hashes" }
foreach ($line in $pinned) {
    $hash, $name = $line -split '  ', 2
    $path = Join-Path $source $name
    if (!(Test-Path -LiteralPath $path)) { throw "vendored curl file missing: $name" }
    if ((Get-Sha256 $path) -ne $hash) { throw "vendored curl file differs from $version`: $name" }
}

# The library sources: CSOURCES of lib/Makefile.inc, with its variables expanded.
$makefile = [System.IO.File]::ReadAllText((Join-Path $source 'lib/Makefile.inc')) -replace '\\\r?\n', ' '
$variables = @{}
foreach ($match in [regex]::Matches($makefile, '(?m)^([A-Z_]+)\s*=\s*(.*)$')) {
    $variables[$match.Groups[1].Value] = @($match.Groups[2].Value -split '\s+' | Where-Object { $_ })
}
function Expand-Variable([string]$name) {
    foreach ($item in $variables[$name]) {
        if ($item -match '^\$\(([A-Z_]+)\)$') { Expand-Variable $Matches[1] } else { $item }
    }
}
$files = @(Expand-Variable 'CSOURCES' | Where-Object { $_ -like '*.c' })
if ($files.Count -lt 150) { throw "lib/Makefile.inc lists $($files.Count) sources" }

$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$out = (Resolve-Path -LiteralPath $OutputDirectory).Path
$objects = Join-Path $out 'obj'
$null = New-Item -ItemType Directory -Force -Path $objects
$sdk = Join-Path $repo 'runtime/include'
$config = Join-Path $repo 'runtime/libcurl'
$tools = Split-Path $Clang
$linker = Join-Path $tools 'ld64.lld.exe'
$nm = Join-Path $tools 'llvm-nm.exe'
$resourceInclude = ((& $Clang -print-resource-dir).Trim()) + '/include'
$flags = @('--target=arm64-apple-macos11', '-mcpu=generic', '-march=armv8-a', '-O2', '-fno-stack-protector', '-nostdinc',
    '-isystem', $sdk, '-isystem', $resourceInclude, '-I', $config, '-I', (Join-Path $source 'include'),
    '-I', (Join-Path $source 'lib'), '-DHAVE_CONFIG_H', '-DBUILDING_LIBCURL', '-DNDEBUG', '-fvisibility=hidden',
    '-std=gnu11', '-Wall', '-Werror')
$objectFiles = @()
foreach ($file in $files) {
    $object = Join-Path $objects (($file -replace '[/\\]', '_') -replace '\.c$', '.o')
    & $Clang @flags -c (Join-Path $source "lib/$file") -o $object
    if ($LASTEXITCODE) { throw "curl compile failed: $file" }
    $objectFiles += $object
}

$undefined = @($objectFiles | ForEach-Object { & $nm -u $_ } | ForEach-Object { $_.Trim() } | Where-Object { $_ } | Sort-Object -Unique)
$internal = @($objectFiles | ForEach-Object { & $nm -g --defined-only $_ } | ForEach-Object { ($_ -split '\s+')[-1] } | Sort-Object -Unique)
$frameworkNames = @($frameworkImports.Values | ForEach-Object { $_ })
$imports = @($undefined | Where-Object { $internal -notcontains $_ -and $_ -ne '___dso_handle' -and $frameworkNames -notcontains $_ })
$difference = Compare-Object (@($expectedImports) + '' | Sort-Object) (@($imports) + '' | Sort-Object)
if ($difference) { throw "curl libSystem imports changed: $(($difference | ForEach-Object { $_.SideIndicator + $_.InputObject }) -join ', ')" }

# Link-only libSystem interface for two-level binding; the symbols are
# implemented by Alchemy's libSystem provider at run time.
$stub = Join-Path $out 'link/libSystem.B.tbd'
$null = New-Item -ItemType Directory -Force -Path (Split-Path $stub)
$tbd = @('--- !tapi-tbd', 'tbd-version: 4', 'targets: [ arm64-macos ]', "install-name: '/usr/lib/libSystem.B.dylib'",
    'current-version: 1.0.0', 'exports:', '  - targets: [ arm64-macos ]', "    symbols: [ $($expectedImports -join ', ') ]", '...')
[System.IO.File]::WriteAllLines($stub, [string[]]$tbd)
$stubs = @($stub)
foreach ($identity in $frameworkImports.Keys) {
    $path = Join-Path $out ("link/" + ($identity -split '/')[-1] + '.tbd')
    $tbd = @('--- !tapi-tbd', 'tbd-version: 4', 'targets: [ arm64-macos ]', "install-name: '$identity'",
        'current-version: 1.0.0', 'exports:', '  - targets: [ arm64-macos ]', "    symbols: [ $($frameworkImports[$identity] -join ', ') ]", '...')
    [System.IO.File]::WriteAllLines($path, [string[]]$tbd)
    $stubs += $path
}

$dylib = Join-Path $out 'libcurl.4.dylib'
# The object list goes through a response file: it is longer than a Windows command line allows.
$responseFile = Join-Path $out 'link/objects.rsp'
[System.IO.File]::WriteAllLines($responseFile, [string[]]($objectFiles | ForEach-Object { '"' + ($_ -replace '\\', '/') + '"' }))
& $linker -arch arm64 -platform_version macos 11.0 11.0 -fixup_chains -dylib -install_name $installName `
    -compatibility_version $compatibilityVersion -current_version $currentVersion "@$responseFile" @stubs -o $dylib
if ($LASTEXITCODE) { throw 'curl link failed' }
$exports = @(& $nm -gU $dylib | ForEach-Object { ($_ -split '\s+')[-1] } | Sort-Object -Unique)
$missing = @($requiredExports | Where-Object { $exports -notcontains $_ })
if ($missing) { throw "libcurl.4.dylib does not export $($missing -join ', ')" }
$private = @($exports | Where-Object { $_ -notlike '_curl_*' })
if ($private) { throw "libcurl.4.dylib exports names outside the curl_* API: $($private -join ', ')" }

$include = Join-Path $out 'include/curl'
$null = New-Item -ItemType Directory -Force -Path $include
foreach ($header in Get-ChildItem -LiteralPath (Join-Path $source 'include/curl') -Filter '*.h') {
    Copy-Item -LiteralPath $header.FullName -Destination (Join-Path $include $header.Name)
}
Copy-Item -LiteralPath (Join-Path $source 'COPYING') -Destination (Join-Path $out 'LICENSE.curl.txt')
Copy-Item -LiteralPath (Join-Path $repo 'runtime/THIRD_PARTY_NOTICES.md') -Destination (Join-Path $out 'THIRD_PARTY_NOTICES.md')
Write-Host "Built $dylib from curl $version, $($exports.Count) exports"

$provenance = [ordered]@{
    source = $upstream
    source_version = $version
    source_archive = $archive
    source_archive_sha256 = $archiveSha256
    license = 'curl'
    install_name = $installName
    compatibility_version = $compatibilityVersion
    current_version = $currentVersion
    target = 'arm64-apple-macos11'
    architecture_flags = @('-mcpu=generic', '-march=armv8-a')
    optimization = '-O2'
    configuration = 'HTTP and FILE; IPv6; Unix sockets; threaded resolver; no TLS, zlib, IDN, HTTP/2 or HTTP/3'
    sources = @($files | ForEach-Object { [ordered]@{ name = "third_party/curl/lib/$_"; sha256 = (Get-Sha256 (Join-Path $source "lib/$_")) } })
    clang_version = ((& $Clang --version) | Out-String).Trim()
    linker_version = ((& $linker --version) | Out-String).Trim()
    guest_header_profile = 'Alchemy-authored Darwin LP64 C/POSIX declarations; no Apple SDK payloads'
    build_script_sha256 = (Get-Sha256 $PSCommandPath)
    configuration_header = [ordered]@{ name = 'runtime/libcurl/curl_config.h'; sha256 = (Get-Sha256 (Join-Path $config 'curl_config.h')) }
    guest_headers = @(Get-ChildItem -LiteralPath $sdk -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{ name = (Get-RelativeName $sdk $_.FullName); sha256 = (Get-Sha256 $_.FullName) }
    })
    exports = $exports
    libsystem_imports = $expectedImports
    framework_imports = $frameworkImports
    outputs = @([ordered]@{ name = 'libcurl.4.dylib'; sha256 = (Get-Sha256 $dylib) })
    licenses = @(@('LICENSE.curl.txt', 'THIRD_PARTY_NOTICES.md') | ForEach-Object {
        [ordered]@{ name = $_; sha256 = (Get-Sha256 (Join-Path $out $_)) }
    })
}
$provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'provenance.json') -Encoding utf8
