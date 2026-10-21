<#
.SYNOPSIS
Builds the fx end-to-end corpus (tests/fx/corpus) for ARM64 and x64, runs
each program natively (x64) and under transmute-fx (ARM64), and compares
stdout bytes and exit codes.

.DESCRIPTION
The x64 build is the reference: its output must match the committed expected
output (tests/fx/corpus/expected/<name>.txt, compared with LF line endings),
and the ARM64 build under transmute-fx must reproduce the native stdout
byte-for-byte and the same exit code. When transmute-fx.exe does not exist
yet, only the native side is validated.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File scripts\fx_corpus.ps1
powershell -ExecutionPolicy Bypass -File scripts\fx_corpus.ps1 -Filter seh,cpp_* -FxExe build-fx\Release\transmute-fx.exe
powershell -ExecutionPolicy Bypass -File scripts\fx_corpus.ps1 -UpdateExpected
#>
[CmdletBinding()]
param(
    [string]$BuildDir,
    [string[]]$Filter = @("*"),
    [string]$FxExe,
    [string[]]$FxArgs = @(),
    [switch]$NoBuild,
    [switch]$BuildOnly,
    [switch]$Rebuild,
    [switch]$UpdateExpected,
    [switch]$OldEngine,
    [string]$OldEngineExe,
    [switch]$NoGuestCrt,
    [int]$Timeout = 120,
    [switch]$RegenerateDefs
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "fx_toolchain.ps1")

$repo = Get-FxRepoRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repo "build-fx-corpus\fx" }
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$corpusDir = Join-Path $repo "tests\fx\corpus"
$srcDir = Join-Path $corpusDir "src"
$expectedDir = Join-Path $corpusDir "expected"
$manifest = Get-Content -LiteralPath (Join-Path $corpusDir "corpus.json") -Raw -Encoding UTF8 | ConvertFrom-Json

function Get-Field($Object, [string]$Name, $Default) {
    if ($Object.PSObject.Properties.Name -contains $Name -and $null -ne $Object.$Name) { return $Object.$Name }
    return $Default
}

function Test-Selected([string]$Name) {
    foreach ($pattern in $Filter) {
        foreach ($p in ($pattern -split ',')) { if ($Name -like $p.Trim()) { return $true } }
    }
    return $false
}

$programs = @($manifest.programs)
$selected = @($programs | Where-Object { Test-Selected $_.name })
# A selected program pulls in the corpus DLLs it links or loads.
$needDlls = @{}
foreach ($p in $selected) { foreach ($d in @(Get-Field $p "link" @())) { $needDlls[$d] = $true } }
if ($selected | Where-Object { $_.name -eq "dynload" }) { $needDlls["fxcorpus_dll"] = $true }
$toBuild = @($programs | Where-Object { (Test-Selected $_.name) -or $needDlls.ContainsKey($_.name) })

$toolchain = Get-FxToolchain
if ($RegenerateDefs) { Update-FxCrtDefs $toolchain }

function Get-ImagePath([string]$Arch, $Program) {
    $ext = if ((Get-Field $Program "kind" "exe") -eq "dll") { ".dll" } else { ".exe" }
    Join-Path $BuildDir "$Arch\$($Program.name)$ext"
}

# ---- build -------------------------------------------------------------------
$buildFailures = @{}
if (-not $NoBuild) {
    Write-Host "Building ARM64 CRT"
    $crt = Build-FxArm64Crt -Toolchain $toolchain -OutDir (Join-Path $BuildDir "crt") -Force:$Rebuild
    if (-not $crt.ok) { throw "ARM64 CRT build failed" }
    $headers = @(Get-ChildItem -LiteralPath $srcDir -Filter *.h | ForEach-Object FullName)
    $crtInputs = @($crt.exeLib, $crt.dllLib) + $crt.importLibs

    foreach ($program in $toBuild) {
        foreach ($arch in "arm64", "x64") {
            $name = $program.name
            $kind = Get-Field $program "kind" "exe"
            $extra = @(Get-Field $program "flags" @()) + @(Get-Field $program "$($arch)Flags" @())
            $objDir = Join-Path $BuildDir "obj\$arch\$name"
            New-Item -ItemType Directory -Force -Path $objDir | Out-Null
            $flagsKey = ($extra + @(Get-Field $program "linkFlags" @())) -join " "
            $stamp = Join-Path $objDir "flags.txt"
            $flagsChanged = -not (Test-Path -LiteralPath $stamp) -or ((Get-Content -LiteralPath $stamp -Raw) -ne $flagsKey)
            $objects = @()
            $ok = $true
            foreach ($src in @($program.sources)) {
                $source = Join-Path $srcDir $src
                $object = Join-Path $objDir ([System.IO.Path]::GetFileNameWithoutExtension($src) + ".obj")
                if ($Rebuild -or $flagsChanged -or (Test-FxStale $object (@($source) + $headers))) {
                    if (-not (Invoke-FxCompile $toolchain $arch $source $object $extra @($srcDir))) { $ok = $false }
                }
                $objects += $object
            }
            $image = Get-ImagePath $arch $program
            $libs = @(@(Get-Field $program "link" @()) | ForEach-Object { Join-Path $BuildDir "$arch\$_.lib" })
            $def = if (Get-Field $program "def" $null) { Join-Path $srcDir $program.def } else { $null }
            $linkInputs = $objects + $libs + @($def)
            if ($arch -eq "arm64") { $linkInputs += $crtInputs }
            if ($ok -and ($Rebuild -or $flagsChanged -or (Test-FxStale $image $linkInputs))) {
                $ok = Invoke-FxLink -Toolchain $toolchain -Arch $arch -Kind $kind -Output $image -Objects $objects `
                    -Libs $libs -Entry (Get-Field $program "entry" "main") -Def $def -Crt $crt `
                    -Extra @(Get-Field $program "linkFlags" @())
            }
            if ($ok) {
                Set-Content -LiteralPath $stamp -Value $flagsKey -NoNewline
            } else {
                $buildFailures["$name/$arch"] = $true
                if (Test-Path -LiteralPath $stamp) { Remove-Item -LiteralPath $stamp }
            }
        }
    }

    $arm64Dir = Join-Path $BuildDir "arm64"
    foreach ($dll in Get-FxCrtDllNames) {
        $target = Join-Path $arm64Dir "$dll.dll"
        if ($NoGuestCrt) {
            if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force }
        } else {
            $source = Join-Path $toolchain.Arm64Crt "$dll.dll"
            if ((Test-Path -LiteralPath $source) -and (Test-FxStale $target @($source))) {
                Copy-Item -LiteralPath $source -Destination $target -Force
            }
        }
    }
    $built = ($toBuild.Count * 2) - $buildFailures.Count
    Write-Host ("Built {0} images ({1} failed) in {2}" -f $built, $buildFailures.Count, $BuildDir)
}
if ($BuildOnly) { exit $buildFailures.Count }

# ---- run ---------------------------------------------------------------------
$fx = Find-FxEngine $FxExe
if ($fx) {
    Write-Host "transmute-fx: $fx"
} else {
    Write-Host "transmute-fx.exe not found (looked for build-fx*\Release\transmute-fx.exe); validating the native side only." -ForegroundColor Yellow
}
$old = $null
if ($OldEngine) {
    $old = Find-FxOldEngine $OldEngineExe
    if (-not $old) { Write-Host "old engine not found; skipping it" -ForegroundColor Yellow }
}

function ConvertTo-Lf([byte[]]$Bytes) {
    ([System.Text.Encoding]::UTF8.GetString($Bytes)) -replace "`r`n", "`n"
}

function Format-ExitCode($Code) {
    if ($null -eq $Code) { return "timeout" }
    $u = [uint32]([int64]$Code -band 0xffffffffL)
    if ($u -ge 0x80000000) { return ("0x{0:X8}" -f $u) }
    return "$u"
}

function Get-FirstDifference([string]$Expected, [string]$Actual) {
    $e = $Expected -split "`n"
    $a = $Actual -split "`n"
    $n = [Math]::Max($e.Count, $a.Count)
    for ($i = 0; $i -lt $n; $i++) {
        $el = if ($i -lt $e.Count) { $e[$i] } else { "<eof>" }
        $al = if ($i -lt $a.Count) { $a[$i] } else { "<eof>" }
        if ($el -ne $al) {
            $clip = { param($s) if ($s.Length -gt 100) { $s.Substring(0, 100) + "..." } else { $s } }
            return "line $($i + 1): expected [$(& $clip $el)] got [$(& $clip $al)]"
        }
    }
    return "line endings or trailing bytes differ"
}

function Invoke-CorpusRun([string]$Engine, [string]$Exe, [string[]]$Arguments, $Program) {
    $runDir = Join-Path $BuildDir "run\$Engine\$($Program.name)"
    if (Test-Path -LiteralPath $runDir) { Remove-Item -LiteralPath $runDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    $environment = @{}
    $envObject = Get-Field $Program "env" $null
    if ($envObject) { foreach ($p in $envObject.PSObject.Properties) { $environment[$p.Name] = [string]$p.Value } }
    $limit = [int](Get-Field $Program "timeout" $Timeout)
    $result = Invoke-FxProcess -Exe $Exe -Arguments $Arguments -Environment $environment -WorkingDirectory $runDir -TimeoutSeconds $limit
    [System.IO.File]::WriteAllBytes((Join-Path $runDir "stdout.txt"), $result.Stdout)
    [System.IO.File]::WriteAllText((Join-Path $runDir "stderr.txt"), $result.Stderr)
    $result
}

$rows = @()
$failures = 0
foreach ($program in $selected) {
    if ((Get-Field $program "kind" "exe") -eq "dll") { continue }
    $name = $program.name
    $progArgs = @(@(Get-Field $program "args" @()) | ForEach-Object { [string]$_ })
    $wantExit = [int](Get-Field $program "exit" 0)
    $expectedName = Get-Field $program "expected" $name
    $expectedFile = Join-Path $expectedDir "$expectedName.txt"
    $row = [ordered]@{ Program = $name; Native = "-"; Fx = "-"; Old = "-"; NativeSec = ""; FxSec = ""; Detail = "" }

    $nativeExe = Get-ImagePath "x64" $program
    $armExe = Get-ImagePath "arm64" $program
    $native = $null
    if ($buildFailures.ContainsKey("$name/x64") -or -not (Test-Path -LiteralPath $nativeExe)) {
        $row.Native = "BUILD"
        $row.Detail = "x64 build failed"
        $failures++
    } else {
        $native = Invoke-CorpusRun "native" $nativeExe $progArgs $program
        $row.NativeSec = "{0:N2}" -f $native.Seconds
        $text = ConvertTo-Lf $native.Stdout
        $problems = @()
        if ($native.TimedOut) { $problems += "timed out" }
        if ($native.ExitCode -ne $wantExit) { $problems += "exit $(Format-ExitCode $native.ExitCode) (want $wantExit)" }
        if ($UpdateExpected -and -not $native.TimedOut) {
            New-Item -ItemType Directory -Force -Path $expectedDir | Out-Null
            if ($expectedName -eq $name) {
                [System.IO.File]::WriteAllText($expectedFile, $text, (New-Object System.Text.UTF8Encoding $false))
            }
        }
        if (Test-Path -LiteralPath $expectedFile) {
            $want = [System.IO.File]::ReadAllText($expectedFile) -replace "`r`n", "`n"
            if ($want -ne $text) { $problems += "stdout differs from expected: " + (Get-FirstDifference $want $text) }
        } else {
            $problems += "no expected output (run with -UpdateExpected)"
        }
        if ($problems.Count) {
            $row.Native = "FAIL"
            $row.Detail = $problems -join "; "
            $failures++
        } else {
            $row.Native = "pass"
        }
    }

    if ($fx) {
        if ($buildFailures.ContainsKey("$name/arm64") -or -not (Test-Path -LiteralPath $armExe)) {
            $row.Fx = "BUILD"
            $failures++
        } elseif ($native) {
            $run = Invoke-CorpusRun "fx" $fx (@($FxArgs) + @($armExe) + $progArgs) $program
            $row.FxSec = "{0:N2}" -f $run.Seconds
            $problems = @()
            if ($run.TimedOut) { $problems += "timed out" }
            if ($run.ExitCode -ne $native.ExitCode) { $problems += "exit $(Format-ExitCode $run.ExitCode) (native $(Format-ExitCode $native.ExitCode))" }
            $same = [System.Linq.Enumerable]::SequenceEqual([byte[]]$run.Stdout, [byte[]]$native.Stdout)
            if (-not $same) { $problems += "stdout: " + (Get-FirstDifference (ConvertTo-Lf $native.Stdout) (ConvertTo-Lf $run.Stdout)) }
            if ($problems.Count) {
                $row.Fx = "FAIL"
                $detail = $problems -join "; "
                $stderrLine = ($run.Stderr -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 1)
                if ($stderrLine) { $detail += " | stderr: " + $stderrLine.Trim() }
                $row.Detail = (@($row.Detail, "fx: $detail") | Where-Object { $_ }) -join " ; "
                $failures++
            } else {
                $row.Fx = "pass"
            }
        }
    } else {
        $row.Fx = "skip"
    }

    if ($old -and $native -and (Test-Path -LiteralPath $armExe)) {
        $run = Invoke-CorpusRun "old" $old (@("--jit", "--instruction-limit", "100000000000", $armExe) + $progArgs) $program
        $same = [System.Linq.Enumerable]::SequenceEqual([byte[]]$run.Stdout, [byte[]]$native.Stdout)
        $row.Old = if ($same -and $run.ExitCode -eq $native.ExitCode) { "pass" } elseif ($run.TimedOut) { "timeout" } else { "fail" }
    }
    $rows += [pscustomobject]$row
}

$columns = @("Program", "Native", "Fx")
if ($OldEngine) { $columns += "Old" }
$columns += @("NativeSec", "FxSec", "Detail")
$rows | Format-Table -Property $columns -AutoSize -Wrap | Out-String -Width 220 | Write-Host
$nativePass = @($rows | Where-Object { $_.Native -eq "pass" }).Count
$fxPass = @($rows | Where-Object { $_.Fx -eq "pass" }).Count
Write-Host ("native: {0}/{1} pass" -f $nativePass, $rows.Count)
if ($fx) { Write-Host ("fx:     {0}/{1} pass" -f $fxPass, $rows.Count) } else { Write-Host "fx:     not run (transmute-fx.exe missing)" }
if ($OldEngine -and $old) { Write-Host ("old:    {0}/{1} pass (informational)" -f @($rows | Where-Object { $_.Old -eq "pass" }).Count, $rows.Count) }
exit [Math]::Min($failures, 255)
