param(
    [string]$SourceDirectory = "$PSScriptRoot/../artifacts/alchemy-llvm-source",
    [string]$OutputDirectory = "$PSScriptRoot/../artifacts/alchemy-llvm-arm64",
    [string]$Clang = 'C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/bin/clang.exe',
    [ValidateSet('unwind', 'abi', 'cxx', 'builtins', 'all')][string]$Runtime = 'all',
    [switch]$Fetch,
    [switch]$UnwindTrace
)
$ErrorActionPreference = 'Stop'
$llvmCommit = 'ab4b5a2db582958af1ee308a790cfdb42bd24720'
$repo = (Resolve-Path "$PSScriptRoot/..").Path
if (!(Test-Path -LiteralPath $SourceDirectory)) {
    if (!$Fetch) { throw 'LLVM source is missing. Pass -Fetch to retrieve the pinned permissive dependency.' }
    & git -c core.longpaths=true clone --filter=blob:none --sparse --depth 1 --branch llvmorg-19.1.5 https://github.com/llvm/llvm-project.git $SourceDirectory
    if ($LASTEXITCODE) { throw 'LLVM source fetch failed' }
    & git -C $SourceDirectory sparse-checkout set libcxx libcxxabi libunwind compiler-rt runtimes cmake llvm/cmake
    if ($LASTEXITCODE) { throw 'LLVM runtime checkout failed' }
}
$actual = & git -C $SourceDirectory rev-parse HEAD
if ($actual -ne $llvmCommit) { throw "Expected LLVM 19.1.5 commit $llvmCommit; found $actual" }
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$out = (Resolve-Path -LiteralPath $OutputDirectory).Path
$sdk = "$repo/runtime/include"
$sdkRoot = (Resolve-Path -LiteralPath $sdk).Path.TrimEnd('\', '/')
$linker = Join-Path (Split-Path $Clang) 'ld64.lld.exe'
$resourceInclude = ((& $Clang -print-resource-dir).Trim()) + '/include'
$flags = @('--target=arm64-apple-macos11', '-mcpu=generic', '-march=armv8-a', '-O1', '-fno-stack-protector', '-nostdinc',
    '-isystem', "$source/libcxx/include", '-isystem', "$source/libcxxabi/include", '-isystem', "$source/libunwind/include", '-isystem', $sdk,
    '-isystem', $resourceInclude, '-DNDEBUG', '-D_LIBUNWIND_IS_NATIVE_ONLY',
    '-D_LIBCPP_BUILDING_LIBRARY', '-D_LIBCPP_DISABLE_AVAILABILITY',
    '-D_LIBCXXABI_BUILDING_LIBRARY', '-DHAVE___CXA_THREAD_ATEXIT_IMPL', '-fvisibility=hidden')
$headersChanged = (Get-ChildItem -LiteralPath $sdk -File -Recurse | Measure-Object -Property LastWriteTimeUtc -Maximum).Maximum
$builtOutputs = [System.Collections.Generic.List[string]]::new()
$previousOutputs = @()
if (Test-Path -LiteralPath "$out/provenance.json") {
    $previousOutputs = @((Get-Content -LiteralPath "$out/provenance.json" -Raw | ConvertFrom-Json).outputs | ForEach-Object { $_.name })
}
function Build-Library([string]$name, [string]$identity, [string[]]$files, [string[]]$extraFlags) {
    $objects = @()
    foreach ($file in $files) {
        $object = Join-Path $out (($file -replace '[/\\.]', '_') + '.o')
        $args = $flags + $extraFlags
        if ($file.EndsWith('.cpp')) { $args += '-std=c++23' }
        $signature = $args -join "`n"
        $signaturePath = "$object.flags"
        $inputTime = (Get-Item -LiteralPath "$source/$file").LastWriteTimeUtc
        $scriptTime = (Get-Item -LiteralPath $PSCommandPath).LastWriteTimeUtc
        $rebuild = !(Test-Path -LiteralPath $object)
        if (!$rebuild) {
            $objectTime = (Get-Item -LiteralPath $object).LastWriteTimeUtc
            $rebuild = $objectTime -lt $inputTime -or $objectTime -lt $scriptTime -or $objectTime -lt $headersChanged -or
                !(Test-Path -LiteralPath $signaturePath) -or (Get-Content -LiteralPath $signaturePath -Raw).TrimEnd() -ne $signature
        }
        if ($rebuild) {
            & $Clang @args -c "$source/$file" -o $object
            if ($LASTEXITCODE) { throw "LLVM compile failed: $file" }
            Set-Content -LiteralPath $signaturePath -Value $signature -Encoding utf8
        }
        $objects += $object
    }
    $dependencies = @()
    if ($name -eq 'libc++abi.dylib') { $dependencies += @('-reexport_library', "$out/libunwind.dylib") }
    if ($name -eq 'libc++.1.dylib') { $dependencies += @('-reexport_library', "$out/libc++abi.dylib", '-needed_library', "$out/libclang_rt.alchemy.dylib") }
    & $linker -arch arm64 -platform_version macos 11.0 11.0 -fixup_chains -dylib -install_name $identity -rpath '@loader_path' -L $out -undefined dynamic_lookup @dependencies @objects -o "$out/$name"
    if ($LASTEXITCODE) { throw "LLVM link failed: $name" }
    $builtOutputs.Add($name)
    Copy-Item -LiteralPath "$source/LICENSE.TXT" -Destination "$out/LICENSE.LLVM.txt"
    foreach ($component in @('libcxx', 'libcxxabi', 'libunwind', 'compiler-rt')) {
        $license = "$source/$component/LICENSE.TXT"
        if (Test-Path -LiteralPath $license) { Copy-Item -LiteralPath $license -Destination "$out/LICENSE.$component.txt" }
    }
    Copy-Item -LiteralPath "$repo/runtime/THIRD_PARTY_NOTICES.md" -Destination "$out/THIRD_PARTY_NOTICES.md"
    Write-Host "Built $out/$name from LLVM $llvmCommit"
}
if ($Runtime -in @('builtins', 'all')) {
    $names = @('absvdi2', 'absvsi2', 'absvti2', 'addvdi3', 'addvsi3', 'addvti3', 'ashlti3', 'ashrti3', 'clzti2',
        'cmpti2', 'ctzti2', 'divti3', 'modti3', 'muloti4', 'multi3', 'mulvdi3', 'mulvsi3', 'mulvti3', 'negti2',
        'subvdi3', 'subvsi3', 'subvti3', 'ucmpti2', 'udivmodti4', 'udivti3', 'umodti3', 'lshrti3', 'int_util')
    Build-Library 'libclang_rt.alchemy.dylib' '@rpath/libclang_rt.alchemy.dylib' ($names | ForEach-Object { "compiler-rt/lib/builtins/$_.c" }) @('-fno-builtin', '-fvisibility=default')
}
if ($Runtime -in @('unwind', 'all')) {
    # Other architecture/platform sources preprocess to empty text sections.
    # LLD 19 emits duplicate zero compact-unwind entries at those boundaries.
    $files = @('libunwind/src/libunwind.cpp', 'libunwind/src/UnwindLevel1.c',
        'libunwind/src/UnwindLevel1-gcc-ext.c', 'libunwind/src/UnwindRegistersRestore.S', 'libunwind/src/UnwindRegistersSave.S')
    $unwindFlags = @('-fno-exceptions', '-fno-rtti', '-funwind-tables')
    if ($UnwindTrace) { $unwindFlags += '-UNDEBUG' }
    Build-Library 'libunwind.dylib' '/usr/lib/libunwind.dylib' $files $unwindFlags
}
if ($Runtime -in @('abi', 'all')) {
    $names = @('cxa_aux_runtime', 'cxa_default_handlers', 'cxa_demangle', 'cxa_exception_storage', 'cxa_guard',
        'cxa_handlers', 'cxa_vector', 'cxa_virtual', 'stdlib_exception', 'stdlib_stdexcept', 'stdlib_typeinfo',
        'abort_message', 'fallback_malloc', 'private_typeinfo', 'stdlib_new_delete', 'cxa_exception', 'cxa_personality')
    Build-Library 'libc++abi.dylib' '/usr/lib/libc++abi.dylib' ($names | ForEach-Object { "libcxxabi/src/$_.cpp" }) @('-I', "$source/libcxx/src", '-fexceptions', '-frtti')
}
if ($Runtime -in @('cxx', 'all')) {
    $names = @('algorithm', 'any', 'bind', 'call_once', 'charconv', 'chrono', 'error_category', 'exception', 'expected',
        'functional', 'hash', 'legacy_pointer_safety', 'memory', 'memory_resource', 'new_handler', 'new_helpers', 'optional',
        'print', 'random_shuffle', 'stdexcept', 'string', 'system_error', 'typeinfo', 'valarray', 'variant', 'vector',
        'verbose_abort', 'atomic', 'barrier', 'condition_variable_destructor', 'condition_variable', 'future',
        'mutex_destructor', 'mutex', 'shared_mutex', 'thread', 'random', 'fstream', 'ios', 'ios.instantiations',
        'iostream', 'locale', 'ostream', 'regex', 'strstream', 'new', 'filesystem/filesystem_clock',
        'filesystem/filesystem_error', 'filesystem/path', 'filesystem/directory_entry', 'filesystem/directory_iterator',
        'filesystem/operations', 'filesystem/int128_builtins', 'ryu/d2fixed', 'ryu/d2s', 'ryu/f2s')
    Build-Library 'libc++.1.dylib' '/usr/lib/libc++.1.dylib' ($names | ForEach-Object { "libcxx/src/$_.cpp" }) @('-I', "$source/libcxx/src", '-DLIBCXX_BUILDING_LIBCXXABI', '-fexceptions', '-frtti')
}
$provenance = [ordered]@{
    source = 'https://github.com/llvm/llvm-project'
    source_tag = 'llvmorg-19.1.5'
    source_commit = $llvmCommit
    target = 'arm64-apple-macos11'
    architecture_flags = @('-mcpu=generic', '-march=armv8-a')
    optimization = '-O1'
    runtime_selection = $Runtime
    unwind_trace_available = [bool]$UnwindTrace
    clang_version = ((& $Clang --version) | Out-String).Trim()
    linker_version = ((& $linker --version) | Out-String).Trim()
    guest_header_profile = 'Alchemy-authored Darwin LP64 C/POSIX declarations; no Apple SDK payloads'
    build_script_sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
    guest_headers = @(Get-ChildItem -LiteralPath $sdk -File -Recurse | Sort-Object FullName | ForEach-Object {
        # Windows PowerShell 5.1 has no [System.IO.Path]::GetRelativePath.
        [ordered]@{ name = $_.FullName.Substring($sdkRoot.Length).TrimStart('\', '/').Replace('\', '/'); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    # The loader accepts /usr/lib runtime identities only when this list holds
    # their current hashes, so a partial rebuild keeps earlier outputs listed.
    outputs = @(@($builtOutputs) + @($previousOutputs | Where-Object { $builtOutputs -notcontains $_ -and (Test-Path -LiteralPath "$out/$_") }) | ForEach-Object {
        [ordered]@{ name = $_; sha256 = (Get-FileHash -LiteralPath "$out/$_" -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    licenses = @(Get-ChildItem -LiteralPath $out -File | Where-Object { $_.Name -like 'LICENSE.*.txt' -or $_.Name -eq 'THIRD_PARTY_NOTICES.md' } | Sort-Object Name | ForEach-Object {
        [ordered]@{ name = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
}
$provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$out/provenance.json" -Encoding utf8
