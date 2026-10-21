# Shared toolchain helpers for scripts/fx_corpus.ps1 and scripts/fx_bench.ps1
# (owner: corpus/bench). Dot-source this file.
#
# ARM64 builds use clang-cl + lld-link with a small in-tree CRT
# (tests/fx/corpus/crt) because no ARM64 MSVC runtime libraries are
# installed. Import libraries for vcruntime140/vcruntime140_1/msvcp140 are
# generated from .def files written from the export tables of the real ARM64
# DLLs. x64 builds use clang-cl with the normal MSVC x64 libraries.

Set-StrictMode -Version 3

$script:FxRepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$script:FxToolchainScript = $PSCommandPath

function Get-FxRepoRoot { $script:FxRepoRoot }

function Get-FxLatestVersionDir([string]$Parent, [string]$Probe) {
    if (-not (Test-Path -LiteralPath $Parent)) { return $null }
    Get-ChildItem -LiteralPath $Parent -Directory |
        Where-Object { $_.Name -match '^\d+(\.\d+)+$' -and (Test-Path -LiteralPath (Join-Path $_.FullName $Probe)) } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1
}

function Get-FxToolchain {
    param([string]$Arm64CrtDir)

    $vsRoot = $null
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath 2>$null |
            Select-Object -First 1
        if (-not $vsRoot) { $vsRoot = & $vswhere -latest -products * -property installationPath 2>$null | Select-Object -First 1 }
    }
    if (-not $vsRoot) { $vsRoot = "C:\Program Files\Microsoft Visual Studio\2022\Professional" }
    $llvm = Join-Path $vsRoot "VC\Tools\Llvm\bin"
    if (-not (Test-Path -LiteralPath (Join-Path $llvm "clang-cl.exe"))) { throw "clang-cl not found under $llvm" }

    $msvc = Get-FxLatestVersionDir (Join-Path $vsRoot "VC\Tools\MSVC") "lib\x64\msvcrt.lib"
    if (-not $msvc) { throw "MSVC x64 libraries not found under $vsRoot" }

    $kitsLib = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Lib"
    $sdk = Get-FxLatestVersionDir $kitsLib "um\arm64\kernel32.lib"
    if (-not $sdk) { throw "Windows SDK with ARM64 libraries not found under $kitsLib" }

    if (-not $Arm64CrtDir) {
        $Arm64CrtDir = Join-Path $script:FxRepoRoot "artifacts\arm64-stage\Blender Foundation\Blender 4.5\blender.crt"
    }

    [pscustomobject]@{
        ClangCl   = Join-Path $llvm "clang-cl.exe"
        Clang     = Join-Path $llvm "clang.exe"
        LldLink   = Join-Path $llvm "lld-link.exe"
        Objdump   = Join-Path $llvm "llvm-objdump.exe"
        DllTool   = Join-Path $llvm "llvm-dlltool.exe"
        MsvcLib   = @{ x64 = Join-Path $msvc.FullName "lib\x64" }
        StlSrc    = Join-Path $msvc.FullName "crt\src\stl"
        SdkUm   = @{ arm64 = Join-Path $sdk.FullName "um\arm64"; x64 = Join-Path $sdk.FullName "um\x64" }
        SdkUcrt   = @{ arm64 = Join-Path $sdk.FullName "ucrt\arm64"; x64 = Join-Path $sdk.FullName "ucrt\x64" }
        Arm64Crt  = $Arm64CrtDir
        Targets   = @{ arm64 = "arm64-pc-windows-msvc"; x64 = "x86_64-pc-windows-msvc" }
    }
}

# Runs a native tool, captures its output, returns $true on exit code 0.
# On failure prints the command line and the first lines of output.
function Invoke-FxTool {
    param([string]$Exe, [string[]]$Arguments, [string]$What, [int]$ShowLines = 30)
    $ErrorActionPreference = "Continue"
    $output = & $Exe @Arguments 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAILED: $What" -ForegroundColor Red
        Write-Host ("    " + (Split-Path -Leaf $Exe) + " " + ($Arguments -join " ")) -ForegroundColor DarkGray
        $output | Select-Object -First $ShowLines | ForEach-Object { Write-Host "    $_" }
        return $false
    }
    return $true
}

# True when $Output is missing or older than any of $Inputs.
function Test-FxStale {
    param([string]$Output, [string[]]$Inputs)
    if (-not (Test-Path -LiteralPath $Output)) { return $true }
    $outTime = (Get-Item -LiteralPath $Output).LastWriteTimeUtc
    foreach ($file in $Inputs) {
        if (-not $file) { continue }
        if ((Get-Item -LiteralPath $file).LastWriteTimeUtc -gt $outTime) { return $true }
    }
    return $false
}

# Writes a .def file from a DLL's export table (llvm-objdump -p). All exports
# are emitted as code so both `name` (thunk) and `__imp_name` resolve; the
# dllimport'ed data exports of msvcp140 (std::cout...) go through __imp_.
function New-FxDefFile {
    param($Toolchain, [string]$Dll, [string]$Def)
    $lines = & $Toolchain.Objdump -p $Dll
    if ($LASTEXITCODE -ne 0) { throw "llvm-objdump failed on $Dll" }
    $start = [array]::IndexOf($lines, "Export Table:")
    if ($start -lt 0) { throw "no export table in $Dll" }
    $dllName = ($lines[$start + 1] -replace '^\s*DLL name:\s*', '').Trim()
    $names = New-Object System.Collections.Generic.List[string]
    for ($i = $start + 2; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s+\d+\s+0x[0-9a-fA-F]+\s+(\S+)\s*$') { $names.Add($Matches[1]) }
    }
    $text = New-Object System.Text.StringBuilder
    [void]$text.AppendLine("; Generated by scripts/fx_toolchain.ps1 from the ARM64 $dllName export table.")
    [void]$text.AppendLine("LIBRARY $dllName")
    [void]$text.AppendLine("EXPORTS")
    foreach ($name in $names) { [void]$text.AppendLine("    $name") }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Def) | Out-Null
    [System.IO.File]::WriteAllText($Def, $text.ToString(), [System.Text.Encoding]::ASCII)
    Write-Host "  wrote $Def ($($names.Count) exports)"
}

function Get-FxCrtImportNames {
    @("vcruntime140", "vcruntime140_1", "msvcp140", "msvcp140_1", "msvcp140_2", "msvcp140_atomic_wait",
      "msvcp140_codecvt_ids")
}

# ARM64 runtime DLLs copied next to ARM64 corpus binaries so the guest loads
# them (C++ EH and SEH need the ARM64 vcruntime140.dll).
function Get-FxCrtDllNames { Get-FxCrtImportNames }

# Regenerates tests/fx/corpus/crt/defs/*.def from the ARM64 DLLs.
function Update-FxCrtDefs {
    param($Toolchain)
    $defDir = Join-Path $script:FxRepoRoot "tests\fx\corpus\crt\defs"
    foreach ($name in Get-FxCrtImportNames) {
        $dll = Join-Path $Toolchain.Arm64Crt "$name.dll"
        if (-not (Test-Path -LiteralPath $dll)) { throw "missing ARM64 runtime DLL $dll" }
        New-FxDefFile -Toolchain $Toolchain -Dll $dll -Def (Join-Path $defDir "$name.def")
    }
}

# Common compile flags. Floating-point contraction is off on both targets so
# a*b+c rounds identically (ARM64 would otherwise fuse it into FMADD).
function Get-FxCompileFlags {
    param([string]$Arch, [string]$Language, [string[]]$Extra = @())
    $flags = @("/nologo", "/c", "/O2", "/MD", "/GS-", "/Gy", "/Z7", "/W3", "/utf-8",
               "/D_CRT_SECURE_NO_WARNINGS", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX",
               "/clang:-ffp-contract=off", "-Wno-unused-command-line-argument")
    if ($Arch -eq "arm64") { $flags += "/Zl" } else { $flags += "/clang:-mcx16" }
    if ($Language -eq "c++") { $flags += @("/EHsc", "/std:c++20", "/Zc:__cplusplus") }
    $flags + $Extra
}

function Invoke-FxCompile {
    param($Toolchain, [string]$Arch, [string]$Source, [string]$Object, [string[]]$Extra = @(), [string[]]$Includes = @())
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Object) | Out-Null
    $ext = [System.IO.Path]::GetExtension($Source).ToLowerInvariant()
    if ($ext -eq ".s") {
        $target = if ($Arch -eq "arm64") { "aarch64-pc-windows-msvc" } else { "x86_64-pc-windows-msvc" }
        return Invoke-FxTool $Toolchain.Clang @("--target=$target", "-c", $Source, "-o", $Object) "assemble $Source ($Arch)"
    }
    $language = if ($ext -in ".cpp", ".cc", ".cxx") { "c++" } else { "c" }
    $flags = @("--target=$($Toolchain.Targets[$Arch])") + (Get-FxCompileFlags $Arch $language $Extra)
    foreach ($inc in $Includes) { $flags += "/I$inc" }
    $langFlag = if ($language -eq "c++") { "/Tp$Source" } else { "/Tc$Source" }
    return Invoke-FxTool $Toolchain.ClangCl ($flags + @("/Fo$Object", $langFlag)) "compile $Source ($Arch)"
}

# Builds the ARM64 CRT into $OutDir: fxcrt_exe.lib, fxcrt_dll.lib and import
# libraries for the vcruntime/msvcp DLLs. Returns a hashtable of paths.
function Build-FxArm64Crt {
    param($Toolchain, [string]$OutDir, [switch]$Force)
    $crtSrc = Join-Path $script:FxRepoRoot "tests\fx\corpus\crt"
    $objDir = Join-Path $OutDir "obj"
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    $result = @{ ok = $true; exeLib = (Join-Path $OutDir "fxcrt_exe.lib"); dllLib = (Join-Path $OutDir "fxcrt_dll.lib"); importLibs = @() }

    foreach ($name in Get-FxCrtImportNames) {
        $def = Join-Path $crtSrc "defs\$name.def"
        if (-not (Test-Path -LiteralPath $def)) {
            $dll = Join-Path $Toolchain.Arm64Crt "$name.dll"
            if (-not (Test-Path -LiteralPath $dll)) { throw "missing $def and $dll" }
            New-FxDefFile -Toolchain $Toolchain -Dll $dll -Def $def
        }
        $lib = Join-Path $OutDir "$name.lib"
        if ($Force -or (Test-FxStale $lib @($def))) {
            if (-not (Invoke-FxTool $Toolchain.LldLink @("/lib", "/nologo", "/machine:arm64", "/def:$def", "/out:$lib") "import library $name")) {
                $result.ok = $false
            }
        }
        $result.importLibs += $lib
    }

    $headers = @(Get-ChildItem -LiteralPath $crtSrc -Filter *.h | ForEach-Object FullName)
    $common = @("crt_support.c", "cxx_support.cpp", "arm64_helpers.S")
    $exeOnly = @("exe_main.c", "exe_wmain.c", "exe_atexit.c")
    $dllOnly = @("dll_main.c")
    $objects = @{}
    foreach ($src in ($common + $exeOnly + $dllOnly)) {
        $source = Join-Path $crtSrc $src
        $object = Join-Path $objDir (([System.IO.Path]::GetFileNameWithoutExtension($src)) + ".obj")
        if ($Force -or (Test-FxStale $object (@($source) + $headers + @($script:FxToolchainScript)))) {
            if (-not (Invoke-FxCompile $Toolchain "arm64" $source $object)) { $result.ok = $false }
        }
        $objects[$src] = $object
    }
    if (-not $result.ok) { return $result }

    # The static ("import library") part of msvcprt.lib, compiled from the STL
    # sources that ship with MSVC: shared_mutex, call_once forwarders, facet
    # registration, charconv/format tables, std::nothrow, ...
    $stlObjects = @()
    foreach ($src in Get-FxStlImplibSources) {
        $source = Join-Path $Toolchain.StlSrc $src
        if (-not (Test-Path -LiteralPath $source)) { Write-Host "  missing STL source $source" -ForegroundColor Yellow; continue }
        $object = Join-Path $objDir ("stl_" + [System.IO.Path]::GetFileNameWithoutExtension($src) + ".obj")
        if ($Force -or (Test-FxStale $object @($source, $script:FxToolchainScript))) {
            $flags = @("--target=arm64-pc-windows-msvc", "/nologo", "/c", "/O2", "/MD", "/Zl", "/GS-", "/Gy", "/Z7",
                       "/EHsc", "/std:c++latest", "/utf-8", "/D_CRTBLD", "/D_CRT_SECURE_NO_WARNINGS",
                       "/D_ARM64_", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/I$($Toolchain.StlSrc)",
                       "/I$(Join-Path (Split-Path -Parent $Toolchain.StlSrc) "vcruntime")",
                       "/FI$(Join-Path $crtSrc "stl_compat.h")", "-w", "/Fo$object", "/Tp$source")
            if (-not (Invoke-FxTool $Toolchain.ClangCl $flags "compile STL $src (arm64)")) { $result.ok = $false }
        }
        $stlObjects += $object
    }
    if (-not $result.ok) { return $result }

    foreach ($pair in @(@($result.exeLib, ($common + $exeOnly)), @($result.dllLib, ($common + $dllOnly)))) {
        $lib = $pair[0]
        $objs = @($pair[1] | ForEach-Object { $objects[$_] }) + $stlObjects
        if ($Force -or (Test-FxStale $lib $objs)) {
            if (Test-Path -LiteralPath $lib) { Remove-Item -LiteralPath $lib -Force }
            if (-not (Invoke-FxTool $Toolchain.LldLink (@("/lib", "/nologo", "/machine:arm64", "/out:$lib") + $objs) "CRT library $lib")) {
                $result.ok = $false
            }
        }
    }
    return $result
}

# STL sources that msvcprt.lib links statically into every /MD module
# (see the static members of the x64 msvcprt.lib).
function Get-FxStlImplibSources {
    @("asan_noop.cpp", "charconv.cpp", "filesystem.cpp", "format.cpp", "locale0_implib.cpp", "nothrow.cpp",
      "print.cpp", "sharedmutex.cpp", "syserror_import_lib.cpp", "xcharconv_ryu_tables.cpp",
      "xcharconv_tables_double.cpp", "xcharconv_tables_float.cpp", "xonce2.cpp")
}

# Links an ARM64 or x64 image. $Kind is "exe" or "dll"; $Entry is "main" or
# "wmain" for executables. ARM64 images link against the in-tree CRT.
function Invoke-FxLink {
    param($Toolchain, [string]$Arch, [string]$Kind, [string]$Output, [string[]]$Objects,
          [string[]]$Libs = @(), [string]$Entry = "main", [string]$Def, $Crt, [string[]]$Extra = @())
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) | Out-Null
    $pdb = [System.IO.Path]::ChangeExtension($Output, ".pdb")
    $linkArgs = @("/nologo", "/machine:$Arch", "/subsystem:console", "/debug", "/pdb:$pdb", "/incremental:no",
              "/opt:ref", "/opt:icf", "/out:$Output")
    if ($Kind -eq "dll") { $linkArgs += "/dll" }
    if ($Def) { $linkArgs += "/def:$Def" }
    $system = @("kernel32.lib", "user32.lib", "advapi32.lib", "synchronization.lib")
    if ($Arch -eq "arm64") {
        $linkArgs += @("/nodefaultlib", "/libpath:$($Toolchain.SdkUm.arm64)", "/libpath:$($Toolchain.SdkUcrt.arm64)")
        if ($Kind -eq "dll") {
            $linkArgs += "/entry:_DllMainCRTStartup"
            $crtLib = $Crt.dllLib
        } else {
            $linkArgs += "/entry:$($Entry)CRTStartup"
            $crtLib = $Crt.exeLib
        }
        $linkArgs += $Objects + $Libs + @($crtLib) + $Crt.importLibs + @("ucrt.lib") + $system
    } else {
        $linkArgs += @("/libpath:$($Toolchain.MsvcLib.x64)", "/libpath:$($Toolchain.SdkUm.x64)", "/libpath:$($Toolchain.SdkUcrt.x64)")
        if ($Kind -ne "dll") { $linkArgs += "/entry:$($Entry)CRTStartup" }
        $linkArgs += $Objects + $Libs + @("msvcrt.lib", "vcruntime.lib", "ucrt.lib", "msvcprt.lib") + $system
    }
    $linkArgs += $Extra
    return Invoke-FxTool $Toolchain.LldLink $linkArgs "link $Output"
}

# Quotes one argument for CommandLineToArgvW / the CRT argv parser.
function ConvertTo-FxArgument([string]$Value) {
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append('"')
    $backslashes = 0
    $bs = [char]92
    foreach ($ch in $Value.ToCharArray()) {
        if ($ch -eq $bs) { $backslashes++; continue }
        if ($ch -eq [char]34) {
            [void]$sb.Append($bs, $backslashes * 2 + 1)
            [void]$sb.Append([char]34)
        } else {
            if ($backslashes) { [void]$sb.Append($bs, $backslashes) }
            [void]$sb.Append($ch)
        }
        $backslashes = 0
    }
    if ($backslashes) { [void]$sb.Append($bs, $backslashes * 2) }
    [void]$sb.Append('"')
    $sb.ToString()
}

# Runs a process with exact stdout/stderr bytes, optional extra environment
# variables, a working directory and a timeout. Returns stdout bytes, stderr
# text, exit code, wall time and whether it timed out.
function Invoke-FxProcess {
    param([string]$Exe, [string[]]$Arguments = @(), [hashtable]$Environment = @{}, [string]$WorkingDirectory,
          [int]$TimeoutSeconds = 120, [string]$Priority)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = (@($Arguments) | ForEach-Object { ConvertTo-FxArgument $_ }) -join " "
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardInput = $true
    $psi.CreateNoWindow = $true
    if ($WorkingDirectory) { $psi.WorkingDirectory = $WorkingDirectory }
    foreach ($key in $Environment.Keys) { $psi.EnvironmentVariables[$key] = [string]$Environment[$key] }
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $psi
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    [void]$process.Start()
    if ($Priority) { try { $process.PriorityClass = $Priority } catch { } }
    $process.StandardInput.Close()
    $stdout = New-Object System.IO.MemoryStream
    $stderr = New-Object System.IO.MemoryStream
    $outTask = $process.StandardOutput.BaseStream.CopyToAsync($stdout)
    $errTask = $process.StandardError.BaseStream.CopyToAsync($stderr)
    $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
    if ($timedOut) {
        try { $process.Kill() } catch { }
        [void]$process.WaitForExit(5000)
    }
    [void]$outTask.Wait(5000)
    [void]$errTask.Wait(5000)
    $watch.Stop()
    $exitCode = if ($timedOut) { $null } else { $process.ExitCode }
    $process.Dispose()
    [pscustomobject]@{
        Stdout   = $stdout.ToArray()
        Stderr   = [System.Text.Encoding]::UTF8.GetString($stderr.ToArray())
        ExitCode = $exitCode
        Seconds  = $watch.Elapsed.TotalSeconds
        TimedOut = $timedOut
    }
}

# Locates transmute-fx.exe: an explicit path, else the newest
# build-fx*/Release/transmute-fx.exe in the repository.
function Find-FxEngine {
    param([string]$Path)
    if ($Path) {
        if (Test-Path -LiteralPath $Path) { return (Resolve-Path -LiteralPath $Path).Path }
        return $null
    }
    $candidates = Get-ChildItem -LiteralPath $script:FxRepoRoot -Directory -Filter "build-fx*" -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "Release\transmute-fx.exe" } |
        Where-Object { Test-Path -LiteralPath $_ } |
        ForEach-Object { Get-Item -LiteralPath $_ } |
        Sort-Object LastWriteTimeUtc -Descending
    if ($candidates) { return @($candidates)[0].FullName }
    return $null
}

function Find-FxOldEngine {
    param([string]$Path)
    if (-not $Path) { $Path = Join-Path $script:FxRepoRoot "build\Release\transmute.exe" }
    if (Test-Path -LiteralPath $Path) { return (Resolve-Path -LiteralPath $Path).Path }
    return $null
}
