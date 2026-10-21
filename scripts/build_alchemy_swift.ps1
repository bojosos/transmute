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
        Dependencies = @('libswiftFoundation.dylib'); IncludeDirectory = "$qlabSwift/include" }
)
$extraIncludeRoots = [System.Collections.Generic.List[string]]::new()
if ($Library -eq 'all') {
    Build-Overlay 'Foundation' 'swiftFoundation'
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
        ForEach-Object { Get-Item -LiteralPath "$out/$($_.File)" }) | Sort-Object Name)
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
    guest_headers = @($headerRecords)
    outputs = @($outputs | ForEach-Object { [ordered]@{ name = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } })
    licenses = @(Get-ChildItem -LiteralPath $out -File | Where-Object { $_.Name -like 'LICENSE.*.txt' -or $_.Name -eq 'THIRD_PARTY_NOTICES.md' } | Sort-Object Name | ForEach-Object {
        [ordered]@{ name = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
}
$provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$out/provenance.json" -Encoding utf8
