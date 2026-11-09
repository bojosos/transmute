param(
    [string]$SourceDirectory = "$PSScriptRoot/../artifacts/alchemy-swift-source",
    [string]$ToolchainDirectory = "$PSScriptRoot/../artifacts/swift-toolchain",
    [string]$LlvmSourceDirectory = "$PSScriptRoot/../artifacts/alchemy-llvm-source",
    [string]$OutputDirectory = "$PSScriptRoot/../artifacts/alchemy-swift-arm64",
    [string]$Python = 'python',
    # The guest LLVM C++ runtime libswiftCore links (scripts/build_alchemy_llvm.ps1).
    [string]$LlvmRuntimeDirectory = "$PSScriptRoot/../artifacts/alchemy-llvm-arm64",
    # Mach-O LLD (the swift.org toolchain's ld64.lld does not link for macOS).
    [string]$Linker = 'C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/bin/ld64.lld.exe',
    [ValidateSet('core', 'all')][string]$Library = 'all',
    [int]$Jobs = [Environment]::ProcessorCount,
    [switch]$Fetch
)
# Builds the open-source Swift 6.2 runtime and standard library as guest
# ARM64 Mach-O libraries for the /usr/lib/swift identities (see
# docs/ALCHEMY_SWIFT.md). Inputs: the swift.org Windows toolchain (its swiftc
# and clang only), pinned swift/libdispatch source checkouts, LLVM's libc++
# headers, and the project-authored Darwin declarations in runtime/swift/include
# and runtime/include. No Apple SDK is used.
$ErrorActionPreference = 'Stop'
$swiftTag = 'swift-6.2-RELEASE'
$swiftCommit = '1ff1cc1170617ab23ab74aa8b741c8daca1903f6'
$dispatchCommit = 'a554fa8222f7d30d6286b10480ef1e188e0b5420'
$repo = (Resolve-Path "$PSScriptRoot/..").Path

function Slash([string]$path) { return $path.Replace('\', '/') }

if (!(Test-Path -LiteralPath "$SourceDirectory/swift")) {
    if (!$Fetch) { throw 'Swift sources are missing. Pass -Fetch to retrieve the pinned sparse checkouts.' }
    $null = New-Item -ItemType Directory -Force -Path $SourceDirectory
    & git clone --depth 1 --branch $swiftTag --filter=blob:none --sparse https://github.com/swiftlang/swift "$SourceDirectory/swift"
    if ($LASTEXITCODE) { throw 'Swift source fetch failed' }
    & git -C "$SourceDirectory/swift" sparse-checkout set --no-cone '/stdlib/' '/include/swift/' '/lib/Demangling/' '/lib/Threading/' '/cmake/' '/utils/' '/LICENSE.txt'
    if ($LASTEXITCODE) { throw 'Swift sparse checkout failed' }
    & git clone --depth 1 --branch $swiftTag https://github.com/swiftlang/swift-corelibs-libdispatch "$SourceDirectory/libdispatch"
    if ($LASTEXITCODE) { throw 'libdispatch source fetch failed' }
}
$actual = & git -C "$SourceDirectory/swift" rev-parse HEAD
if ($actual -ne $swiftCommit) { throw "Expected swift $swiftTag commit $swiftCommit; found $actual" }
$actual = & git -C "$SourceDirectory/libdispatch" rev-parse HEAD
if ($actual -ne $dispatchCommit) { throw "Expected libdispatch $swiftTag commit $dispatchCommit; found $actual" }

$swift = Slash (Resolve-Path -LiteralPath "$SourceDirectory/swift").Path
$llvm = Slash (Resolve-Path -LiteralPath $LlvmSourceDirectory).Path
$toolchain = (Resolve-Path -LiteralPath $ToolchainDirectory).Path
$bin = Join-Path $toolchain 'LocalApp/Programs/Swift/Toolchains/6.2.0+Asserts/usr/bin'
$frontend = Join-Path $bin 'swift-frontend.exe'
$clang = Join-Path $bin 'clang.exe'
$linker = $Linker
foreach ($tool in @($frontend, $clang, $linker)) { if (!(Test-Path -LiteralPath $tool)) { throw "Swift toolchain tool missing: $tool" } }
# The toolchain's own runtime DLLs (rtl.msi) live at the extraction root.
$env:PATH = "$bin;$toolchain;$env:PATH"
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$out = Slash (Resolve-Path -LiteralPath $OutputDirectory).Path
$obj = "$out/obj"
$null = New-Item -ItemType Directory -Force -Path "$obj/gyb", "$obj/include/swift/Runtime", "$obj/core", "$obj/c"
$resourceInclude = Slash "$bin/../lib/clang/19/include"
$sysroot = @((Slash "$repo/runtime/swift/include"), (Slash "$repo/runtime/include"))
$scriptHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
$headersChanged = (@($sysroot | ForEach-Object { Get-ChildItem -LiteralPath $_ -File -Recurse }) |
    Measure-Object -Property LastWriteTimeUtc -Maximum).Maximum

# --- Runs processes $Jobs at a time; each job is @{ Name; Exe; Arguments }.
function Invoke-Jobs([object[]]$work) {
    $queue = [System.Collections.Generic.Queue[object]]::new()
    foreach ($item in $work) { $queue.Enqueue($item) }
    $running = [System.Collections.Generic.List[object]]::new()
    $failures = [System.Collections.Generic.List[string]]::new()
    while ($queue.Count -or $running.Count) {
        while ($queue.Count -and $running.Count -lt $Jobs) {
            $item = $queue.Dequeue()
            $info = New-Object System.Diagnostics.ProcessStartInfo
            $info.FileName = $item.Exe
            $info.Arguments = $item.Arguments
            $info.UseShellExecute = $false
            $info.RedirectStandardError = $true
            $info.RedirectStandardOutput = $true
            $process = [System.Diagnostics.Process]::Start($info)
            $running.Add([pscustomobject]@{ Item = $item; Process = $process
                Error = $process.StandardError.ReadToEndAsync(); Output = $process.StandardOutput.ReadToEndAsync() })
        }
        Start-Sleep -Milliseconds 40
        foreach ($entry in @($running)) {
            if (!$entry.Process.HasExited) { continue }
            $entry.Process.WaitForExit()
            $text = $entry.Error.Result + $entry.Output.Result
            if ($entry.Process.ExitCode) { $failures.Add("$($entry.Item.Name):`n$text") }
            elseif ($entry.Item.Done) { & $entry.Item.Done }
            $null = $running.Remove($entry)
        }
    }
    if ($failures.Count) { throw ("Swift runtime build failed:`n" + ($failures -join "`n")) }
}

function Needs-Build([string]$output, [string]$signature, [string[]]$inputs, [switch]$IgnoreHeaders) {
    if (!(Test-Path -LiteralPath $output)) { return $true }
    $signaturePath = "$output.flags"
    if (!(Test-Path -LiteralPath $signaturePath) -or (Get-Content -LiteralPath $signaturePath -Raw).TrimEnd() -ne $signature.TrimEnd()) { return $true }
    $time = (Get-Item -LiteralPath $output).LastWriteTimeUtc
    if (!$IgnoreHeaders -and $time -lt $headersChanged) { return $true }
    foreach ($input in $inputs) { if ((Get-Item -LiteralPath $input).LastWriteTimeUtc -gt $time) { return $true } }
    return $false
}

# --- gyb expansion.
$coreGyb = @('AtomicInt', 'FloatingPointParsing', 'FloatingPointTypes', 'IntegerTypes', 'LegacyInt128',
    'UnsafeBufferPointer', 'UnsafeRawBufferPointer', 'Tuple', 'SIMDConcreteOperations', 'SIMDVectorTypes')
$gybInputs = @($coreGyb | ForEach-Object { @{ Source = "$swift/stdlib/public/core/$_.swift.gyb"; Output = "$obj/gyb/$_.swift" } })
$gybInputs += @{ Source = "$swift/stdlib/public/stubs/SwiftNativeNSXXXBase.mm.gyb"; Output = "$obj/gyb/SwiftNativeNSXXXBase.mm" }
foreach ($gyb in $gybInputs) {
    if ((Test-Path -LiteralPath $gyb.Output) -and (Get-Item -LiteralPath $gyb.Output).LastWriteTimeUtc -gt (Get-Item -LiteralPath $gyb.Source).LastWriteTimeUtc) { continue }
    & $Python -I "$repo/scripts/alchemy_swift_gyb.py" "$swift/utils" -DCMAKE_SIZEOF_VOID_P=8 '--line-directive=' -o $gyb.Output $gyb.Source
    if ($LASTEXITCODE) { throw "gyb failed: $($gyb.Source)" }
}

# CMake's configure_file output for include/swift/Runtime/CMakeConfig.h.in.
$config = "#ifndef SWIFT_RUNTIME_CMAKECONFIG_H`n#define SWIFT_RUNTIME_CMAKECONFIG_H`n#define SWIFT_BNI_OS_BUILD 0`n#define SWIFT_BNI_XCODE_BUILD 0`n#define SWIFT_VERSION_MAJOR `"6`"`n#define SWIFT_VERSION_MINOR `"2`"`n#endif`n"
$configPath = "$obj/include/swift/Runtime/CMakeConfig.h"
if (!(Test-Path -LiteralPath $configPath) -or (Get-Content -LiteralPath $configPath -Raw) -ne $config) {
    [System.IO.File]::WriteAllText($configPath, $config)
}

# --- The core standard library (module Swift), mirroring the stdlib CMake
# flags for a Darwin arm64 stable-ABI build (SwiftSource.cmake,
# AddSwiftStdlib.cmake, stdlib/public/core/CMakeLists.txt).
$coreList = Get-Content -LiteralPath "$swift/stdlib/public/core/CMakeLists.txt"
$coreSources = [System.Collections.Generic.List[string]]::new()
$inList = $false
foreach ($line in $coreList) {
    if ($line -match '^split_embedded_sources\(') { $inList = $true; continue }
    if ($inList -and $line -match 'OUT_LIST_EMBEDDED SWIFTLIB_EMBEDDED_GYB_SOURCES') { break }
    if ($inList -and $line -match '^\s+(EMBEDDED|NORMAL)\s+([A-Za-z0-9+_/]+\.swift)\b') { $coreSources.Add($Matches[2]) }
}
$coreSources.Add('SIMDVector.swift')
$coreSources.Add('ObjectIdentifier+DebugDescription.swift')
$coreFiles = @($coreSources | ForEach-Object { "$swift/stdlib/public/core/$_" }) + @($coreGyb | ForEach-Object { "$obj/gyb/$_.swift" })
$availability = @(Get-Content -LiteralPath "$swift/utils/availability-macros.def" | Where-Object { $_ -notmatch '^\s*(#.*)?$' })
$deployment = [version]'11.0'
$coreFlags = [System.Collections.Generic.List[string]]::new()
$coreFlags.AddRange([string[]]@('-frontend', '-c', '-target', 'arm64-apple-macosx11.0',
    '-parse-stdlib', '-nostdimport', '-module-name', 'Swift', '-module-link-name', 'swiftCore',
    '-parse-as-library', '-swift-version', '5', '-O', '-num-threads', "$Jobs",
    '-enable-library-evolution', '-library-level', 'api', '-require-explicit-availability=ignore',
    '-enforce-exclusivity=unchecked', '-group-info-path', "$swift/stdlib/public/core/GroupInfo.json",
    '-empty-abi-descriptor', '-runtime-compatibility-version', 'none',
    '-disable-autolinking-runtime-compatibility-dynamic-replacements',
    '-disable-autolinking-runtime-compatibility-concurrency',
    '-enable-upcoming-feature', 'MemberImportVisibility', '-strict-memory-safety',
    '-disable-objc-attr-requires-foundation-module', '-enable-experimental-concise-pound-file',
    '-prespecialize-generic-metadata', '-target-min-inlining-version', 'min',
    '-Xllvm', '-sil-inline-generics', '-Xllvm', '-sil-partial-specialization',
    '-Xcc', '-DswiftCore_EXPORTS', '-Xcc', '-DSWIFT_STDLIB_HAS_ENVIRON',
    '-I', "$swift/stdlib/public/SwiftShims/swift/shims", '-plugin-path', (Slash $bin),
    '-module-cache-path', "$obj/module-cache"))
foreach ($feature in @('NoncopyableGenerics2', 'SuppressedAssociatedTypes', 'SE427NoInferenceOnExtension',
        'NonescapableTypes', 'LifetimeDependence', 'InoutLifetimeDependence', 'LifetimeDependenceMutableAccessors',
        'Macros', 'FreestandingMacros', 'Extern', 'BitwiseCopyable', 'ValueGenerics', 'AddressableParameters',
        'AddressableTypes', 'AllowUnsafeAttribute')) { $coreFlags.Add('-enable-experimental-feature'); $coreFlags.Add($feature) }
foreach ($define in @('SWIFT_ENABLE_REFLECTION', 'SWIFT_RUNTIME_OS_VERSIONING', 'SWIFT_STDLIB_ENABLE_UNICODE_DATA',
        'SWIFT_STDLIB_ENABLE_VECTOR_TYPES', 'SWIFT_STDLIB_HAS_COMMANDLINE', 'SWIFT_STDLIB_HAS_STDIN',
        'SWIFT_STDLIB_HAS_ENVIRON', 'SWIFT_STDLIB_OVERRIDABLE_RETAIN_RELEASE', 'SWIFT_THREADING_DARWIN',
        'SWIFT_ENABLE_SYNCHRONIZATION', 'SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY', 'SWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED',
        'SWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING', 'SWIFT_ENABLE_EXPERIMENTAL_STRING_PROCESSING',
        'SWIFT_ENABLE_EXPERIMENTAL_OBSERVATION')) { $coreFlags.Add('-D'); $coreFlags.Add($define) }
foreach ($definition in $availability) {
    $coreFlags.Add('-define-availability'); $coreFlags.Add($definition)
    if ($definition -match '^SwiftStdlib ') {
        # StdlibDeploymentTarget x.y maps newer platforms to the deployment target.
        $current = $definition -replace '^SwiftStdlib', 'StdlibDeploymentTarget'
        if ($definition -match 'macOS ([0-9.]+)' -and $Matches[1] -ne '9999' -and [version]($Matches[1] + $(if ($Matches[1] -notmatch '\.') { '.0' })) -gt $deployment) {
            $current = $current -replace ':.*', ":macOS $deployment"
        }
        $coreFlags.Add('-define-availability'); $coreFlags.Add($current)
    }
}
$coreFlags.Add('-emit-module-path'); $coreFlags.Add("$obj/core/Swift.swiftmodule")
$coreFlags.Add('-emit-module-interface-path'); $coreFlags.Add("$obj/core/Swift.swiftinterface")
$coreObjects = @($coreFiles | ForEach-Object { "$obj/core/" + [System.IO.Path]::GetFileNameWithoutExtension($_) + '.o' })
$coreSignature = ($coreFlags + $coreFiles) -join "`n"
# The core library imports only SwiftShims, never the sysroot headers.
if (Needs-Build $coreObjects[-1] $coreSignature $coreFiles -IgnoreHeaders) {
    $rsp = [System.Collections.Generic.List[string]]::new()
    foreach ($flag in $coreFlags) { $rsp.Add($(if ($flag -match '\s') { '"' + $flag + '"' } else { $flag })) }
    foreach ($file in $coreFiles) { $rsp.Add($file) }
    foreach ($object in $coreObjects) { $rsp.Add('-o'); $rsp.Add($object) }
    [System.IO.File]::WriteAllLines("$obj/core.rsp", $rsp)
    Write-Host "Compiling the Swift core library ($($coreFiles.Count) files)"
    # Warnings go to stderr; keep them in a log instead of letting Windows
    # PowerShell turn native stderr into terminating errors.
    $process = Start-Process -FilePath $frontend -ArgumentList "@`"$obj/core.rsp`"" -NoNewWindow -Wait -PassThru `
        -RedirectStandardError "$obj/core.log" -RedirectStandardOutput "$obj/core.out"
    if ($process.ExitCode) {
        Get-Content -LiteralPath "$obj/core.log" | Where-Object { $_ -match 'error:' } | Select-Object -First 40 | ForEach-Object { Write-Host $_ }
        throw 'Swift core library compile failed'
    }
    Set-Content -LiteralPath "$($coreObjects[-1]).flags" -Value $coreSignature -Encoding utf8
}

# --- Runtime C/C++/Objective-C++ objects incorporated into libswiftCore
# (swiftRuntimeCore, swiftLLVMSupport, swiftDemangling, swiftStdlibStubs,
# swiftThreading, swiftCommandLineSupport).
$commonC = @('--target=arm64-apple-macos11', '-mcpu=generic', '-march=armv8-a', '-O2', '-g0', '-fno-stack-protector',
    '-nostdinc', '-isystem', "$llvm/libcxx/include", '-isystem', "$llvm/libcxxabi/include") +
    @($sysroot | ForEach-Object { @('-isystem', $_) }) + @('-isystem', $resourceInclude,
    '-D_LIBCPP_DISABLE_AVAILABILITY', '-DNDEBUG', '-DSWIFT_RUNTIME', '-DSWIFT_LIB_SUBDIR="macosx"', '-DSWIFT_ARCH="arm64"',
    '-DSWIFT_LIBRARY_EVOLUTION=1', '-DSWIFT_STDLIB_SUPPORT_BACK_DEPLOYMENT', '-DSWIFT_ENABLE_REFLECTION',
    '-DSWIFT_STDLIB_HAS_DLADDR', '-DSWIFT_STDLIB_HAS_DLSYM=1', '-DSWIFT_STDLIB_HAS_FILESYSTEM',
    '-DSWIFT_STDLIB_HAS_DARWIN_LIBMALLOC=1', '-DSWIFT_STDLIB_HAS_STDIN',
    '-DSWIFT_STDLIB_HAS_ENVIRON', '-DSWIFT_STDLIB_HAS_LOCALE', '-DSWIFT_THREADING_DARWIN', '-DSWIFT_RUNTIME_OS_VERSIONING',
    '-DSWIFT_STDLIB_SHORT_MANGLING_LOOKUPS', '-DSWIFT_STDLIB_ENABLE_VECTOR_TYPES', '-DSWIFT_STDLIB_HAS_TYPE_PRINTING',
    '-DSWIFT_STDLIB_SUPPORTS_BACKTRACE_REPORTING', '-DSWIFT_STDLIB_ENABLE_UNICODE_DATA',
    '-DSWIFT_STDLIB_OVERRIDABLE_RETAIN_RELEASE', '-DswiftCore_EXPORTS',
    '-fno-exceptions', '-fno-rtti', '-funwind-tables', '-fvisibility=hidden', '-fvisibility-inlines-hidden',
    '-I', "$obj/include", '-I', "$swift/include", '-I', "$swift/stdlib/include", '-I', "$swift/stdlib/include/llvm/Support",
    '-I', "$swift/stdlib/public", '-I', "$swift/stdlib/public/SwiftShims", '-Wno-everything')
$runtimeList = Get-Content -LiteralPath "$swift/stdlib/public/runtime/CMakeLists.txt" -Raw
if ($runtimeList -notmatch '(?s)set\(swift_runtime_sources(.*?)\)') { throw 'Cannot read the runtime source list' }
$runtimeSources = @([regex]::Matches($Matches[1], '[A-Za-z0-9_./]+\.cpp') | ForEach-Object { "stdlib/public/runtime/$($_.Value)" })
$runtimeSources += @('ErrorObject.mm', 'SwiftObject.mm', 'SwiftValue.mm', 'ReflectionMirrorObjC.mm',
    'ObjCRuntimeGetImageNameFromClass.mm' | ForEach-Object { "stdlib/public/runtime/$_" })
$runtimeSources += 'lib/Threading/ThreadSanitizer.cpp'
$units = [System.Collections.Generic.List[object]]::new()
function Add-Units([string[]]$files, [string]$libraryName, [string[]]$extra) {
    foreach ($file in $files) { $units.Add(@{ File = $file; Library = $libraryName; Extra = $extra }) }
}
Add-Units $runtimeSources 'swiftRuntimeCore' @()
Add-Units @('ErrorHandling', 'Hashing', 'MemAlloc', 'SmallPtrSet', 'SmallVector', 'StringRef' | ForEach-Object { "stdlib/public/LLVMSupport/$_.cpp" }) 'swiftLLVMSupport' @()
Add-Units @('Context', 'Demangler', 'ManglingUtils', 'NodePrinter', 'Punycode', 'Remangler', 'NodeDumper', 'Errors',
    'OldDemangler', 'OldRemangler' | ForEach-Object { "lib/Demangling/$_.cpp" }) 'swiftDemangling' @('-DSWIFT_SUPPORT_OLD_MANGLING=1')
Add-Units @('Assert.cpp', 'GlobalObjects.cpp', 'LibcShims.cpp', 'Random.cpp', 'Stubs.cpp', 'ThreadLocalStorage.cpp',
    'MathStubs.cpp', 'Unicode/UnicodeData.cpp', 'Unicode/UnicodeGrapheme.cpp', 'Unicode/UnicodeNormalization.cpp',
    'Unicode/UnicodeScalarProps.cpp', 'Unicode/UnicodeWord.cpp', 'Availability.mm', 'FoundationHelpers.mm',
    'OptionalBridgingHelper.mm', 'Reflection.mm', 'SwiftNativeNSObject.mm' | ForEach-Object { "stdlib/public/stubs/$_" }) 'swiftStdlibStubs' @()
Add-Units @('stdlib/public/stubs/SwiftNativeNSXXXBaseARC.m') 'swiftStdlibStubs' @('-fobjc-arc')
Add-Units @("$obj/gyb/SwiftNativeNSXXXBase.mm") 'swiftStdlibStubs' @()
Add-Units @('C11', 'Linux', 'Pthreads', 'Win32' | ForEach-Object { "lib/Threading/$_.cpp" }) 'swiftThreading' @()
Add-Units @('stdlib/public/CommandLineSupport/CommandLine.cpp') 'swiftCommandLineSupport' @()

$cObjects = [System.Collections.Generic.List[string]]::new()
$work = [System.Collections.Generic.List[object]]::new()
foreach ($unit in $units) {
    $source = if ([System.IO.Path]::IsPathRooted($unit.File)) { $unit.File } else { "$swift/$($unit.File)" }
    $language = switch -Regex ($source) {
        '\.mm$' { @('-x', 'objective-c++', '-std=c++17') }
        '\.m$' { @('-x', 'objective-c', '-std=c11') }
        '\.c$' { @('-x', 'c', '-std=c11') }
        default { @('-x', 'c++', '-std=c++17') }
    }
    $name = ($unit.Library + '_' + [System.IO.Path]::GetFileName($source)) -replace '[^A-Za-z0-9_.+-]', '_'
    $object = "$obj/c/$name.o"
    $flags = @($commonC + $language + @("-DSWIFT_TARGET_LIBRARY_NAME=$($unit.Library)") + $unit.Extra + @('-c', $source, '-o', $object))
    $signature = $flags -join "`n"
    $cObjects.Add($object)
    if (!(Needs-Build $object $signature @($source))) { continue }
    $rspPath = "$object.rsp"
    [System.IO.File]::WriteAllLines($rspPath, [string[]]@($flags | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ } }))
    $signatureCopy = $signature
    $objectCopy = $object
    $work.Add(@{ Name = $source; Exe = $clang; Arguments = "@`"$rspPath`""
        Done = { Set-Content -LiteralPath "$objectCopy.flags" -Value $signatureCopy -Encoding utf8 }.GetNewClosure() })
}
if ($work.Count) { Write-Host "Compiling $($work.Count) runtime units"; Invoke-Jobs $work }

# --- Link /usr/lib/swift/libswiftCore.dylib against the guest libc++
# (/usr/lib/libc++.1.dylib, which re-exports libc++abi and libunwind) and
# link-only libobjc/libSystem interfaces generated from the objects' imports,
# so every import binds two-level to the identity whose Alchemy provider
# implements it. Swift's os_log/ASL reporting is not configured: fatal errors
# go to stderr.
$cxx = Slash (Resolve-Path -LiteralPath "$LlvmRuntimeDirectory/libc++.1.dylib").Path
$cxxabi = Slash (Resolve-Path -LiteralPath "$LlvmRuntimeDirectory/libc++abi.dylib").Path
$unwind = Slash (Resolve-Path -LiteralPath "$LlvmRuntimeDirectory/libunwind.dylib").Path
$nm = Join-Path (Split-Path $Linker) 'llvm-nm.exe'
$stubs = "$obj/link"
$null = New-Item -ItemType Directory -Force -Path $stubs
function Invoke-Tool([string]$exe, [string[]]$arguments, [string]$log) {
    [System.IO.File]::WriteAllLines("$log.rsp", [string[]]@($arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ } }))
    $result = Start-Process -FilePath $exe -ArgumentList "@`"$log.rsp`"" -NoNewWindow -Wait -PassThru -RedirectStandardError "$log.log" -RedirectStandardOutput "$log.out"
    if ($result.ExitCode) {
        Get-Content -LiteralPath "$log.log" | Select-Object -First 60 | ForEach-Object { Write-Host $_ }
        throw "$([System.IO.Path]::GetFileName($exe)) failed ($log)"
    }
}
# Links one guest library: its link-only libobjc/Foundation/libSystem
# interfaces are generated from what the objects import beyond `$runtimes`.
function Link-Library([string]$name, [string]$installName, [string[]]$objects, [string[]]$runtimes, [string]$fileName = "lib$name.dylib") {
    $interfaces = "$stubs/$name"
    $null = New-Item -ItemType Directory -Force -Path $interfaces
    # Swift's autolink force-load references (weak __swift_FORCE_LOAD_$_<library>, one for each module an
    # object imports, transitively) bind to that library when this build has made it, rather than to the
    # libSystem stub: it joins the libraries the link resolves against.
    $forceLoaded = @()
    for ($at = 0; $at -lt $objects.Count; $at += 64) {
        $batch = $objects[$at..([Math]::Min($at + 63, $objects.Count - 1))]
        $forceLoaded += @(& $nm -u -j @batch 2>$null | ForEach-Object { if ($_ -match '^__swift_FORCE_LOAD_\$_(.+)$') { $Matches[1] } })
    }
    foreach ($library in @($forceLoaded | Sort-Object -Unique)) {
        $file = @("$out/lib$library.dylib", "$out/$library") | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
        if ($file -and $file -ne "$out/$fileName" -and $runtimes -notcontains $file) { $runtimes += $file }
    }
    $stubScript = "$repo/scripts/alchemy_swift_link_stubs.py"
    $stubArguments = @($nm, $interfaces) + @($runtimes + @($cxxabi, $unwind)) + @('--') + @($objects)
    if ((($stubArguments | Measure-Object -Property Length -Sum).Sum + $stubArguments.Count * 3) -lt 30000) {
        & $Python -I $stubScript @stubArguments
    } else {
        # Beyond the Windows command-line limit (a long -OutputDirectory): the
        # same arguments reach the script's sys.argv from a file, one per line.
        $argumentFile = "$interfaces/link-stubs.args"
        [System.IO.File]::WriteAllLines($argumentFile, [string[]]$stubArguments)
        & $Python -I -c "import runpy, sys; sys.argv = [sys.argv[1]] + open(sys.argv[2], encoding='utf-8').read().splitlines(); runpy.run_path(sys.argv[0], run_name='__main__')" $stubScript $argumentFile
    }
    if ($LASTEXITCODE) { throw "link interface generation failed for $name" }
    $libraries = @()
    foreach ($stub in @(@{ Name = 'objc'; Install = '/usr/lib/libobjc.A.dylib'; File = 'libobjc.A.dylib' },
                        @{ Name = 'foundation'; Install = '/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation'; File = 'Foundation' },
                        @{ Name = 'coremedia'; Install = '/System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia'; File = 'CoreMedia' },
                        @{ Name = 'corefoundation'; Install = '/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation'; File = 'CoreFoundation' },
                        @{ Name = 'system'; Install = '/usr/lib/libSystem.B.dylib'; File = 'libSystem.B.dylib' })) {
        if (!(Select-String -LiteralPath "$interfaces/$($stub.Name).s" -Pattern '^\.globl' -Quiet)) { continue }
        Invoke-Tool $clang @('--target=arm64-apple-macos11', '-c', "$interfaces/$($stub.Name).s", '-o', "$interfaces/$($stub.Name).o") "$interfaces/$($stub.Name)-as"
        Invoke-Tool $linker @('-arch', 'arm64', '-platform_version', 'macos', '11.0', '11.0', '-dylib', '-install_name', $stub.Install,
            "$interfaces/$($stub.Name).o", '-o', "$interfaces/$($stub.File)") "$interfaces/$($stub.Name)-ld"
        $libraries += "$interfaces/$($stub.File)"
    }
    $dylib = "$out/$fileName"
    Invoke-Tool $linker (@('-arch', 'arm64', '-platform_version', 'macos', '11.0', '11.0', '-fixup_chains', '-dylib',
        '-install_name', $installName, '-compatibility_version', '1.0.0', '-current_version', '6.2.0',
        '-L', (Slash (Split-Path $cxx)), '-o', $dylib) + $objects + $runtimes + $libraries) "$obj/link-$name"
    Write-Host "Built $dylib"
}
Link-Library 'swiftCore' '/usr/lib/swift/libswiftCore.dylib' @($coreObjects + $cObjects) @($cxx)

# --- Open-source stdlib modules beyond the core, from the same pinned sources
# and with the core's Swift flags (so their ABI matches Apple's builds of the
# same release): the Darwin overlay (stdlib/public/Platform, over the Darwin
# Clang module of runtime/swift/include) and _Concurrency with its dispatch
# global executor.
$stdlibObj = "$obj/stdlib"
$null = New-Item -ItemType Directory -Force -Path "$stdlibObj/gyb", "$stdlibObj/c"
# The core's Swift flags without its identity, its outputs and its core-only
# optimizer flags (stdlib/public/core/CMakeLists.txt).
function Stdlib-Flags([string]$module, [string]$library, [switch]$ParseStdlib) {
    $flags = [System.Collections.Generic.List[string]]::new()
    for ($i = 0; $i -lt $coreFlags.Count; $i++) {
        $flag = $coreFlags[$i]
        if ($flag -in @('-module-name', '-module-link-name', '-group-info-path', '-emit-module-path', '-emit-module-interface-path')) { $i++; continue }
        if ($flag -in @('-nostdimport', '-empty-abi-descriptor', '-prespecialize-generic-metadata')) { continue }
        if ($flag -eq '-parse-stdlib' -and !$ParseStdlib) { continue }
        if ($flag -in @('-Xcc', '-Xllvm') -and $coreFlags[$i + 1] -match '^(-DswiftCore_EXPORTS|-sil-inline-generics|-sil-partial-specialization)$') { $i++; continue }
        $flags.Add($flag)
    }
    $flags.AddRange([string[]](@('-module-name', $module, '-module-link-name', $library, '-I', "$obj/core", '-I', $stdlibObj,
        '-disable-implicit-concurrency-module-import', '-disable-implicit-string-processing-module-import',
        '-emit-module-path', "$stdlibObj/$module.swiftmodule") + @($sysroot | ForEach-Object { @('-Xcc', "-I$_") })))
    return , $flags
}
function Build-StdlibSwift([string]$module, [string[]]$flags, [string[]]$sources) {
    $objects = @($sources | ForEach-Object { "$stdlibObj/$module-" + [System.IO.Path]::GetFileNameWithoutExtension($_) + '.o' })
    $signature = ($flags + $sources) -join "`n"
    if (Needs-Build $objects[-1] $signature $sources) {
        $arguments = @($flags) + $sources
        foreach ($object in $objects) { $arguments += @('-o', $object) }
        Invoke-Tool $frontend $arguments "$stdlibObj/$module"
        Set-Content -LiteralPath "$($objects[-1]).flags" -Value $signature -Encoding utf8
    }
    return $objects
}
function Expand-Gyb([string]$source, [string]$output) {
    if ((Test-Path -LiteralPath $output) -and (Get-Item -LiteralPath $output).LastWriteTimeUtc -gt (Get-Item -LiteralPath $source).LastWriteTimeUtc) { return }
    & $Python -I "$repo/scripts/alchemy_swift_gyb.py" "$swift/utils" -DCMAKE_SIZEOF_VOID_P=8 '--line-directive=' -o $output $source
    if ($LASTEXITCODE) { throw "gyb failed: $source" }
}
if ($Library -eq 'all') {
    # /usr/lib/swift/libswiftDarwin.dylib (Platform/CMakeLists.txt: swift_platform_sources, POSIXError, MachError and
    # Darwin.swift.gyb, an SDK overlay).
    $platform = "$swift/stdlib/public/Platform"
    foreach ($name in @('Darwin', 'tgmath')) { Expand-Gyb "$platform/$name.swift.gyb" "$stdlibObj/gyb/$name.swift" }
    $darwinFlags = Stdlib-Flags 'Darwin' 'swiftDarwin'
    $darwinFlags.AddRange([string[]]@('-autolink-force-load'))
    $darwinObjects = Build-StdlibSwift 'Darwin' $darwinFlags (@('Platform.swift', 'TiocConstants.swift', 'POSIXError.swift', 'MachError.swift' |
        ForEach-Object { "$platform/$_" }) + @("$stdlibObj/gyb/Darwin.swift", "$stdlibObj/gyb/tgmath.swift"))
    Link-Library 'swiftDarwin' '/usr/lib/swift/libswiftDarwin.dylib' $darwinObjects @($cxx, "$out/libswiftCore.dylib")

    # /usr/lib/swift/libswift_Concurrency.dylib (Concurrency/CMakeLists.txt with SWIFT_CONCURRENCY_GLOBAL_EXECUTOR
    # "dispatch" on Darwin). Its linker-support file only emits $ld$previous install-name redirects for back
    # deployment, which the /usr/lib/swift identity does not need.
    $concurrency = "$swift/stdlib/public/Concurrency"
    $concurrencyList = Get-Content -LiteralPath "$concurrency/CMakeLists.txt" -Raw
    function Concurrency-Sources([string]$variable) {
        if ($concurrencyList -notmatch "(?s)set\($variable\s(.*?)\)") { throw "Cannot read $variable from Concurrency/CMakeLists.txt" }
        return @([regex]::Matches($Matches[1], '[A-Za-z0-9_+./-]+\.(swift|cpp|c|gyb)') | ForEach-Object { $_.Value })
    }
    $concurrencyGyb = @(Concurrency-Sources 'SWIFT_CONCURRENCY_GYB_SOURCES')
    foreach ($name in $concurrencyGyb) { Expand-Gyb "$concurrency/$name" ("$stdlibObj/gyb/" + ($name -replace '\.gyb$', '')) }
    $concurrencySwift = @(@(Concurrency-Sources 'SWIFT_RUNTIME_CONCURRENCY_SWIFT_SOURCES') + @('DispatchExecutor.swift', 'CFExecutor.swift', 'ExecutorImpl.swift') |
        ForEach-Object { "$concurrency/$_" }) + @($concurrencyGyb | ForEach-Object { "$stdlibObj/gyb/" + ($_ -replace '\.gyb$', '') })
    $concurrencyFlags = Stdlib-Flags '_Concurrency' 'swift_Concurrency' -ParseStdlib
    $concurrencyFlags.AddRange([string[]]@('-I', "$concurrency/InternalShims", '-enable-experimental-feature', 'IsolatedAny',
        '-swift-async-frame-pointer=always', '-D', 'SWIFT_CONCURRENCY_USES_DISPATCH'))
    $concurrencyObjects = Build-StdlibSwift '_Concurrency' $concurrencyFlags $concurrencySwift
    $concurrencyC = @(@(Concurrency-Sources 'SWIFT_RUNTIME_CONCURRENCY_C_SOURCES') + @('DispatchGlobalExecutor.cpp', 'ExecutorImpl.cpp') |
        Where-Object { $_ -notlike 'linker-support/*' })
    $concurrencyCommon = @($commonC | Where-Object { $_ -ne '-DswiftCore_EXPORTS' }) + @('-Dswift_Concurrency_EXPORTS',
        '-DSWIFT_CONCURRENCY_USES_DISPATCH', '-D__STDC_WANT_LIB_EXT1__=1', '-fswift-async-fp=always', '-fblocks',
        '-DSWIFT_TARGET_LIBRARY_NAME=swift_Concurrency', '-I', $concurrency)
    # Local patch (approved; recorded in provenance.json): DispatchGlobalExecutor.cpp converts deadlines to Apple
    # arm64's fixed 125/3 mach timebase, but Alchemy's mach_timebase_info is 1/1 (nanoseconds). The generated copy
    # takes the file's own nanoseconds configuration (upstream's x86_64 and non-Apple one) and is marked as modified.
    $patchedDirectory = "$stdlibObj/patched"
    $null = New-Item -ItemType Directory -Force -Path $patchedDirectory
    $executorSource = Get-Content -LiteralPath "$concurrency/DispatchGlobalExecutor.cpp" -Raw
    $timeUnitLine = '#if defined(__i386__) || defined(__x86_64__) || !defined(__APPLE__)'
    if ($executorSource.IndexOf($timeUnitLine) -lt 0) { throw 'DispatchGlobalExecutor.cpp: the time-unit selection line changed upstream' }
    $executorPatched = "// Modified by Alchemy (scripts/build_alchemy_swift.ps1) from swift $swiftTag" +
        " stdlib/public/Concurrency/DispatchGlobalExecutor.cpp:`n// TIME_UNIT_USES_NANOSECONDS is forced on, because Alchemy's mach timebase is 1/1.`n" +
        $executorSource.Replace($timeUnitLine, '#if 1 // Alchemy: was ' + $timeUnitLine.Substring(4))
    $executorCopy = "$patchedDirectory/DispatchGlobalExecutor.cpp"
    if (!(Test-Path -LiteralPath $executorCopy) -or (Get-Content -LiteralPath $executorCopy -Raw) -ne $executorPatched) {
        [System.IO.File]::WriteAllText($executorCopy, $executorPatched)
    }
    $work = [System.Collections.Generic.List[object]]::new()
    foreach ($file in $concurrencyC) {
        $source = if ($file -eq 'DispatchGlobalExecutor.cpp') { $executorCopy } else { "$concurrency/$file" }
        $object = "$stdlibObj/c/" + [System.IO.Path]::GetFileNameWithoutExtension($file) + '.o'
        $flags = @($concurrencyCommon + @('-x', 'c++', '-std=c++17', '-c', $source, '-o', $object))
        $signature = $flags -join "`n"
        $concurrencyObjects += $object
        if (!(Needs-Build $object $signature @($source))) { continue }
        $rspPath = "$object.rsp"
        [System.IO.File]::WriteAllLines($rspPath, [string[]]@($flags | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ } }))
        $signatureCopy = $signature
        $objectCopy = $object
        $work.Add(@{ Name = $source; Exe = $clang; Arguments = "@`"$rspPath`""
            Done = { Set-Content -LiteralPath "$objectCopy.flags" -Value $signatureCopy -Encoding utf8 }.GetNewClosure() })
    }
    if ($work.Count) { Write-Host "Compiling $($work.Count) _Concurrency units"; Invoke-Jobs $work }
    # _Concurrency incorporates its own copy of the threading package (INCORPORATE_OBJECT_LIBRARIES swiftThreading).
    $concurrencyObjects += @($cObjects | Where-Object { $_ -like '*/swiftThreading_*' })
    Link-Library 'swift_Concurrency' '/usr/lib/swift/libswift_Concurrency.dylib' $concurrencyObjects @($cxx, "$out/libswiftCore.dylib", "$out/libswiftDarwin.dylib")

    # /usr/lib/swift/libswiftObservation.dylib (Observation/Sources/Observation/CMakeLists.txt).
    $observation = "$swift/stdlib/public/Observation/Sources/Observation"
    $observationFlags = Stdlib-Flags 'Observation' 'swiftObservation'
    $observationFlags.AddRange([string[]]@('-enable-experimental-feature', 'ExtensionMacros'))
    $observationObjects = Build-StdlibSwift 'Observation' $observationFlags @('Locking.swift', 'Observable.swift',
        'ObservationRegistrar.swift', 'ObservationTracking.swift', 'Observations.swift', 'ThreadLocal.swift' | ForEach-Object { "$observation/$_" })
    $observationCommon = @($commonC | Where-Object { $_ -ne '-DswiftCore_EXPORTS' }) + @('-DswiftObservation_EXPORTS',
        '-DSWIFT_TARGET_LIBRARY_NAME=swiftObservation')
    $work = [System.Collections.Generic.List[object]]::new()
    foreach ($file in @('Locking.cpp', 'ThreadLocal.cpp')) {
        $source = "$observation/$file"
        $object = "$stdlibObj/c/Observation-" + [System.IO.Path]::GetFileNameWithoutExtension($file) + '.o'
        $flags = @($observationCommon + @('-x', 'c++', '-std=c++17', '-c', $source, '-o', $object))
        $signature = $flags -join "`n"
        $observationObjects += $object
        if (!(Needs-Build $object $signature @($source))) { continue }
        $rspPath = "$object.rsp"
        [System.IO.File]::WriteAllLines($rspPath, [string[]]@($flags | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ } }))
        $signatureCopy = $signature
        $objectCopy = $object
        $work.Add(@{ Name = $source; Exe = $clang; Arguments = "@`"$rspPath`""
            Done = { Set-Content -LiteralPath "$objectCopy.flags" -Value $signatureCopy -Encoding utf8 }.GetNewClosure() })
    }
    if ($work.Count) { Invoke-Jobs $work }
    Link-Library 'swiftObservation' '/usr/lib/swift/libswiftObservation.dylib' $observationObjects @($cxx, "$out/libswiftCore.dylib",
        "$out/libswift_Concurrency.dylib", "$out/libswiftDarwin.dylib")

    # /usr/lib/swift/libswiftSynchronization.dylib (Synchronization/CMakeLists.txt, the Darwin Mutex).
    $synchronization = "$swift/stdlib/public/Synchronization"
    foreach ($name in @('AtomicIntegers', 'AtomicStorage')) { Expand-Gyb "$synchronization/Atomics/$name.swift.gyb" "$stdlibObj/gyb/$name.swift" }
    $synchronizationFlags = Stdlib-Flags 'Synchronization' 'swiftSynchronization'
    $synchronizationFlags.AddRange([string[]]@('-enable-builtin-module', '-enable-experimental-feature', 'RawLayout',
        '-enable-experimental-feature', 'StaticExclusiveOnly'))
    $synchronizationObjects = Build-StdlibSwift 'Synchronization' $synchronizationFlags (@('Atomic', 'AtomicBool', 'AtomicFloats',
        'AtomicLazyReference', 'AtomicMemoryOrderings', 'AtomicOptional', 'AtomicPointers', 'AtomicRepresentable', 'WordPair' |
        ForEach-Object { "$synchronization/Atomics/$_.swift" }) + @("$synchronization/Cell.swift", "$synchronization/Mutex/DarwinImpl.swift",
        "$synchronization/Mutex/Mutex.swift", "$stdlibObj/gyb/AtomicIntegers.swift", "$stdlibObj/gyb/AtomicStorage.swift"))
    Link-Library 'swiftSynchronization' '/usr/lib/swift/libswiftSynchronization.dylib' $synchronizationObjects @($cxx,
        "$out/libswiftCore.dylib", "$out/libswiftDarwin.dylib")
}

# --- Project-authored overlays (runtime/swift/overlays/<Module>): Swift
# modules named like the SDK overlays they stand in for, compiled with
# library evolution against the built standard library and the
# project-authored Clang modules (runtime/swift/include/module.modulemap).
$overlayFlags = @('-frontend', '-c', '-target', 'arm64-apple-macosx11.0', '-parse-as-library', '-O', '-num-threads', "$Jobs",
    '-enable-library-evolution', '-swift-version', '5', '-autolink-force-load',
    '-disable-implicit-concurrency-module-import', '-disable-implicit-string-processing-module-import',
    '-runtime-compatibility-version', 'none', '-I', "$obj/core", '-I', "$obj/overlays",
    '-I', "$swift/stdlib/public/SwiftShims/swift/shims", '-module-cache-path', "$obj/module-cache") +
    @($sysroot | ForEach-Object { @('-Xcc', "-I$_") })
$null = New-Item -ItemType Directory -Force -Path "$obj/overlays"
# An overlay module: its *.swift sources (by default runtime/swift/overlays/<Module>)
# compiled into `$fileName` with install name `$installName`, linked against
# libswiftCore and the given already-built `$dependencies` (output file names).
# `$extraFlags` (for example -Xcc -I<dir> of a module-specific Clang module)
# come after the shared overlay flags.
function Build-Overlay([string]$module, [string]$library, [string]$sourceDirectory = "$repo/runtime/swift/overlays/$module",
        [string]$installName = "/usr/lib/swift/lib$library.dylib", [string]$fileName = "lib$library.dylib",
        [string[]]$dependencies = @(), [string[]]$extraFlags = @(), [string[]]$extraInputs = @()) {
    $sources = @(Get-ChildItem -LiteralPath $sourceDirectory -Filter '*.swift' | Sort-Object Name | ForEach-Object { Slash $_.FullName })
    if (!$sources.Count) { throw "No Swift sources for overlay $module in $sourceDirectory" }
    $objects = @($sources | ForEach-Object { "$obj/overlays/$module-" + [System.IO.Path]::GetFileNameWithoutExtension($_) + '.o' })
    $flags = $overlayFlags + $extraFlags + @('-module-name', $module, '-module-link-name', $library,
        '-emit-module-path', "$obj/overlays/$module.swiftmodule")
    $signature = ($flags + $sources) -join "`n"
    if (Needs-Build $objects[-1] $signature @($sources + $extraInputs)) {
        $arguments = $flags + $sources
        foreach ($object in $objects) { $arguments += @('-o', $object) }
        Invoke-Tool $frontend $arguments "$obj/overlays/$module"
        Set-Content -LiteralPath "$($objects[-1]).flags" -Value $signature -Encoding utf8
    }
    Link-Library $library $installName $objects (@($cxx, "$out/libswiftCore.dylib") + @($dependencies | ForEach-Object { "$out/$_" })) $fileName
}
# Overlays authored outside runtime/swift/overlays (QLab gap group G6,
# src/alchemy/darwin/swift): Apple SDK Swift modules that
# QLab 5.6.3 imports from, built after the overlays they depend on. Each
# entry may name a module-specific Clang include directory (IncludeDirectory)
# passed only to that module's compile.
$qlabSwift = "$repo/src/alchemy/darwin/swift"
$extraOverlays = @(
    [ordered]@{ Module = 'CryptoKit'; Library = 'CryptoKit'; Source = "$qlabSwift/CryptoKit"
        Install = '/System/Library/Frameworks/CryptoKit.framework/Versions/A/CryptoKit'; File = 'CryptoKit'
        Dependencies = @('libswiftFoundation.dylib') },
    [ordered]@{ Module = 'Dispatch'; Library = 'swiftDispatch'; Source = "$qlabSwift/Dispatch"
        Install = '/usr/lib/swift/libswiftDispatch.dylib'; File = 'libswiftDispatch.dylib'
        Dependencies = @('libswiftos.dylib'); IncludeDirectory = "$qlabSwift/include" },
    [ordered]@{ Module = 'OSLog'; Library = 'swiftOSLog'; Source = "$qlabSwift/OSLog"
        Install = '/usr/lib/swift/libswiftOSLog.dylib'; File = 'libswiftOSLog.dylib'
        Dependencies = @('libswiftFoundation.dylib'); IncludeDirectory = "$qlabSwift/include" },
    [ordered]@{ Module = 'CoreAudio'; Library = 'swiftCoreAudio'; Source = "$qlabSwift/CoreAudio"
        Install = '/usr/lib/swift/libswiftCoreAudio.dylib'; File = 'libswiftCoreAudio.dylib'
        Dependencies = @(); IncludeDirectory = "$qlabSwift/include" },
    [ordered]@{ Module = 'CoreMIDI'; Library = 'swiftCoreMIDI'; Source = "$qlabSwift/CoreMIDI"
        Install = '/usr/lib/swift/libswiftCoreMIDI.dylib'; File = 'libswiftCoreMIDI.dylib'
        Dependencies = @(); IncludeDirectory = "$qlabSwift/include" },
    [ordered]@{ Module = 'CoreMedia'; Library = 'swiftCoreMedia'; Source = "$qlabSwift/CoreMedia"
        Install = '/usr/lib/swift/libswiftCoreMedia.dylib'; File = 'libswiftCoreMedia.dylib'
        Dependencies = @(); IncludeDirectory = "$qlabSwift/include" },
    [ordered]@{ Module = 'AVFoundation'; Library = 'swiftAVFoundation'; Source = "$qlabSwift/AVFoundation"
        Install = '/usr/lib/swift/libswiftAVFoundation.dylib'; File = 'libswiftAVFoundation.dylib'
        Dependencies = @('libswiftFoundation.dylib'); IncludeDirectory = "$qlabSwift/include" },
    # libswiftMetal (metal-parity M8): Apple's Swift extensions of the Metal protocols.
    [ordered]@{ Module = 'Metal'; Library = 'swiftMetal'; Source = "$qlabSwift/Metal"
        Install = '/usr/lib/swift/libswiftMetal.dylib'; File = 'libswiftMetal.dylib'
        Dependencies = @('libswiftFoundation.dylib'); IncludeDirectory = "$qlabSwift/include" }
)
$extraIncludeRoots = [System.Collections.Generic.List[string]]::new()
if ($Library -eq 'all') {
    # /usr/lib/swift/libswiftObjectiveC.dylib first: once its module exists, SEL imports as ObjectiveC.Selector and
    # NSObject is Hashable in every overlay built after it.
    Build-Overlay 'ObjectiveC' 'swiftObjectiveC'
    # /usr/lib/swift/libswiftCoreFoundation.dylib next: CGFloat and _CFObject, which every overlay that imports
    # CoreFoundation after it sees.
    Build-Overlay 'CoreFoundation' 'swiftCoreFoundation'

    # /System/Library/Frameworks/Combine.framework/Versions/A/Combine: OpenCombine's code (third_party/opencombine,
    # vendored by scripts/vendor_opencombine.py) and the project's runtime/swift/combine, as the resilient module
    # Combine. Its async publishers use _Concurrency, so that module is imported implicitly here.
    $opencombine = "$repo/third_party/opencombine/Sources"
    $combineObj = "$obj/combine"
    $null = New-Item -ItemType Directory -Force -Path "$combineObj/helpers"
    $helperHeader = Slash "$opencombine/COpenCombineHelpers/include/COpenCombineHelpers.h"
    $helperMap = "module COpenCombineHelpers [system] {`n  header `"$helperHeader`"`n  export *`n}`n"
    if (!(Test-Path -LiteralPath "$combineObj/helpers/module.modulemap") -or (Get-Content -LiteralPath "$combineObj/helpers/module.modulemap" -Raw) -ne $helperMap) {
        [System.IO.File]::WriteAllText("$combineObj/helpers/module.modulemap", $helperMap)
    }
    # The C++ lock and identifier helpers (they use exceptions internally and catch them at the C boundary).
    $helperSource = Slash "$opencombine/COpenCombineHelpers/COpenCombineHelpers.cpp"
    $helperObject = "$combineObj/COpenCombineHelpers.o"
    $helperFlags = @('--target=arm64-apple-macos11', '-mcpu=generic', '-march=armv8-a', '-O2', '-g0', '-std=c++17', '-nostdinc',
        '-isystem', "$llvm/libcxx/include", '-isystem', "$llvm/libcxxabi/include") + @($sysroot | ForEach-Object { @('-isystem', $_) }) +
        @('-isystem', $resourceInclude, '-D_LIBCPP_DISABLE_AVAILABILITY', '-DNDEBUG', '-fexceptions', '-fno-rtti', '-fvisibility=hidden',
        '-I', (Slash "$opencombine/COpenCombineHelpers/include"), '-c', $helperSource, '-o', $helperObject)
    if (Needs-Build $helperObject ($helperFlags -join "`n") @($helperSource, $helperHeader)) {
        Invoke-Tool $clang $helperFlags "$combineObj/helpers-cc"
        Set-Content -LiteralPath "$helperObject.flags" -Value ($helperFlags -join "`n") -Encoding utf8
    }
    $combineSources = @(@(Get-ChildItem -LiteralPath "$opencombine/OpenCombine" -Recurse -Filter '*.swift') +
        @(Get-ChildItem -LiteralPath "$repo/runtime/swift/combine" -Filter '*.swift') | Sort-Object FullName | ForEach-Object { Slash $_.FullName })
    $combineObjects = @($combineSources | ForEach-Object { "$combineObj/Combine-" + [System.IO.Path]::GetFileNameWithoutExtension($_) + '.o' })
    $combineFlags = @($overlayFlags | Where-Object { $_ -ne '-disable-implicit-concurrency-module-import' }) + @('-I', $stdlibObj,
        '-Xcc', (Slash "-I$combineObj/helpers"), '-module-name', 'Combine', '-module-link-name', 'Combine',
        '-emit-module-path', "$combineObj/Combine.swiftmodule")
    $combineSignature = ($combineFlags + $combineSources) -join "`n"
    if (Needs-Build $combineObjects[-1] $combineSignature $combineSources) {
        $arguments = $combineFlags + $combineSources
        foreach ($object in $combineObjects) { $arguments += @('-o', $object) }
        Invoke-Tool $frontend $arguments "$combineObj/Combine"
        Set-Content -LiteralPath "$($combineObjects[-1]).flags" -Value $combineSignature -Encoding utf8
    }
    Link-Library 'Combine' '/System/Library/Frameworks/Combine.framework/Versions/A/Combine' @($combineObjects + $helperObject) `
        @($cxx, "$out/libswiftCore.dylib", "$out/libswift_Concurrency.dylib", "$out/libswiftDarwin.dylib") 'Combine'

    # Foundation's Combine publishers (NotificationCenter.Publisher, Timer.TimerPublisher) and their locks (Darwin's
    # os_unfair_lock) need Combine, _Concurrency and Darwin. Foundation's interface names Combine, so every overlay
    # compiled after it (any that imports Foundation) finds those modules too.
    $overlayFlags += @('-I', $stdlibObj, '-I', $combineObj)
    Build-Overlay 'Foundation' 'swiftFoundation' -dependencies @('libswiftObjectiveC.dylib', 'Combine', 'libswift_Concurrency.dylib',
        'libswiftDarwin.dylib')
    Build-Overlay 'os' 'swiftos'
    foreach ($overlay in $extraOverlays) {
        if (!(Test-Path -LiteralPath $overlay.Source)) { continue }
        $extraFlags = @()
        $extraInputs = @()
        if ($overlay.Contains('IncludeDirectory')) {
            $include = Slash (Resolve-Path -LiteralPath $overlay.IncludeDirectory).Path
            $extraFlags = @('-Xcc', "-I$include")
            $extraInputs = @(Get-ChildItem -LiteralPath $include -File -Recurse | ForEach-Object { $_.FullName })
            if (!$extraIncludeRoots.Contains($include)) { $extraIncludeRoots.Add($include) }
        }
        Build-Overlay $overlay.Module $overlay.Library $overlay.Source $overlay.Install $overlay.File $overlay.Dependencies $extraFlags $extraInputs
    }
}

# --- Licenses and provenance.
Copy-Item -LiteralPath "$swift/LICENSE.txt" -Destination "$out/LICENSE.swift.txt"
Copy-Item -LiteralPath "$SourceDirectory/libdispatch/LICENSE" -Destination "$out/LICENSE.libdispatch.txt"
Copy-Item -LiteralPath "$repo/runtime/THIRD_PARTY_NOTICES.md" -Destination "$out/THIRD_PARTY_NOTICES.md"
$outputs = @(@(Get-ChildItem -LiteralPath $out -File -Filter '*.dylib') +
    @($extraOverlays | Where-Object { $_.File -notlike '*.dylib' -and (Test-Path -LiteralPath "$out/$($_.File)") } |
        ForEach-Object { Get-Item -LiteralPath "$out/$($_.File)" }) +
    @(Get-Item -LiteralPath "$out/Combine" -ErrorAction SilentlyContinue) | Sort-Object Name)
$headerRecords = @(foreach ($root in $sysroot) {
    $rootPath = (Resolve-Path -LiteralPath $root).Path.TrimEnd('\', '/')
    Get-ChildItem -LiteralPath $root -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{ name = (Split-Path -Leaf (Split-Path -Parent $rootPath)) + '/' + (Split-Path -Leaf $rootPath) + '/' + $_.FullName.Substring($rootPath.Length).TrimStart('\', '/').Replace('\', '/')
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
})
# Module-specific Clang modules of the extra overlays, named by repository path.
$repoPath = $repo.TrimEnd('\', '/')
$headerRecords += @(foreach ($root in $extraIncludeRoots) {
    Get-ChildItem -LiteralPath $root -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{ name = $_.FullName.Substring($repoPath.Length).TrimStart('\', '/').Replace('\', '/')
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
})
$provenance = [ordered]@{
    source = 'https://github.com/swiftlang/swift'
    source_tag = $swiftTag
    source_commit = $swiftCommit
    additional_sources = @([ordered]@{ source = 'https://github.com/swiftlang/swift-corelibs-libdispatch'; source_tag = $swiftTag; source_commit = $dispatchCommit })
    license = 'Apache-2.0 WITH Swift-exception'
    toolchain = 'swift.org swift-6.2-RELEASE-windows10.exe (sha256 80fbbc17d4f9edec74a83abbefeb9ff418ffc2158cd347111583c45e47f9789b), administrative extraction'
    swiftc_version = ((& $frontend -version) | Out-String).Trim()
    clang_version = ((& $clang --version) | Out-String).Trim()
    linker_version = ((& $linker --version) | Out-String).Trim()
    target = 'arm64-apple-macos11'
    architecture_flags = @('-mcpu=generic', '-march=armv8-a')
    guest_header_profile = 'Alchemy-authored Darwin, Objective-C and Foundation declarations; no Apple SDK payloads'
    build_script_sha256 = $scriptHash
    local_patches = @([ordered]@{ file = 'stdlib/public/Concurrency/DispatchGlobalExecutor.cpp'
        change = 'TIME_UNIT_USES_NANOSECONDS forced on (Alchemy mach timebase 1/1); generated copy marked as modified' })
    guest_headers = @($headerRecords)
    outputs = @($outputs | ForEach-Object { [ordered]@{ name = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } })
    licenses = @(Get-ChildItem -LiteralPath $out -File | Where-Object { $_.Name -like 'LICENSE.*.txt' -or $_.Name -eq 'THIRD_PARTY_NOTICES.md' } | Sort-Object Name | ForEach-Object {
        [ordered]@{ name = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
}
$provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$out/provenance.json" -Encoding utf8
