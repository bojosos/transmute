# Runs one guest program of an installed app under alchemy-fx, cloaked, and records the outcome.
#
# Every run is cloaked (ALCHEMY_GUI_CLOAK, ALCHEMY_GUI_NO_ACTIVATE, ALCHEMY_GUI_NO_CRASH_DIALOG) with
# ALCHEMY_NETWORK=loopback, and ALCHEMY_CONTENTS_CAPTURE=<OutDir>\capture, so the first window's
# contents land there as BMPs without any input or focus change. HOME is <Root>\Users\<User>\home.
# On timeout only the process this script started (and its children, matched by parent id and by
# a command line naming -Root) is stopped.
#
# Usage: run_guest.ps1 -Exe <build>\Release\alchemy-fx.exe -Root <alchemy root> -OutDir <dir>
#          -Program '/Applications/GIMP.app/Contents/MacOS/gimp' [-GuestArgs @(...)] [-Timeout 300]
#          [-Env @{ ALCHEMY_PROBE_MISSING = 'zero' }] [-User creative]
# Output in -OutDir: cmdline.txt, stdout.txt, stderr.txt, capture\, status.json.
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$OutDir,
    [Parameter(Mandatory)][string]$Program,
    [string[]]$GuestArgs = @(),
    # Guest runtimes: LLVM's libc++ and friends, and Expat's /usr/lib/libexpat.1.dylib.
    [string[]]$RuntimeDir = @('C:\dev\transmute\artifacts\alchemy-llvm-arm64', 'C:\dev\transmute\artifacts\alchemy-expat-arm64'),
    [int]$Timeout = 300,
    [string]$User = 'creative',
    [hashtable]$Env = @{},
    # The same as 'NAME=value;NAME=value' text, for callers going through powershell -File.
    [string]$EnvText = '',
    # -GuestArgs as base64 of a UTF-8 JSON array, for callers going through powershell -File.
    [string]$GuestArgsBase64 = '',
    # -RuntimeDir as ';'-separated text, for callers going through powershell -File.
    [string]$RuntimeDirText = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
. "$PSScriptRoot/../alchemy_gui_common.ps1"

if ($GuestArgsBase64) {
    # Windows PowerShell's ConvertFrom-Json emits a JSON array as one object: enumerate it.
    $parsed = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($GuestArgsBase64)) | ConvertFrom-Json
    $GuestArgs = @($parsed | ForEach-Object { [string]$_ })
}
if ($RuntimeDirText) { $RuntimeDir = @($RuntimeDirText -split ';' | Where-Object { $_ }) }
$Root = [IO.Path]::GetFullPath($Root)
if (-not (Test-Path (Join-Path $Root '.alchemy-root'))) { throw "not an Alchemy root: $Root" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = [IO.Path]::GetFullPath($OutDir)
$capture = Join-Path $OutDir 'capture'
Remove-Item -Recurse -Force $capture -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $capture | Out-Null
$home_ = Join-Path $Root "Users\$User\home"
New-Item -ItemType Directory -Force $home_ | Out-Null

$all = @($RuntimeDir | ForEach-Object { '--runtime-dir'; $_ }) + @('--root', $Root, $Program) + $GuestArgs
$quoted = ($all | ForEach-Object { if ($_ -eq '' -or $_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
Set-Content (Join-Path $OutDir 'cmdline.txt') "$Exe $quoted"

$env:ALCHEMY_NETWORK = 'loopback'; $env:ALCHEMY_GUI_CLOAK = '1'; $env:ALCHEMY_GUI_NO_ACTIVATE = '1'; $env:ALCHEMY_GUI_NO_CRASH_DIALOG = '1'
$env:ALCHEMY_CONTENTS_CAPTURE = $capture
$env:HOME = $home_
foreach ($pair in ($EnvText -split ';' | Where-Object { $_ })) { $k, $v = $pair -split '=', 2; $Env[$k] = $v }
foreach ($k in $Env.Keys) { Set-Item "Env:$k" $Env[$k] }

$clock = [Diagnostics.Stopwatch]::StartNew()
$process = Start-Process -FilePath $Exe -ArgumentList $quoted -NoNewWindow -PassThru `
    -RedirectStandardOutput (Join-Path $OutDir 'stdout.txt') -RedirectStandardError (Join-Path $OutDir 'stderr.txt')
$null = $process.Handle
$timedOut = -not $process.WaitForExit($Timeout * 1000)
if ($timedOut) {
    # Descendants first (guest children such as plug-ins, and theirs): processes under ours whose command
    # line names this root, with either path separator (children get it as --root C:/...).
    $rootPattern = [regex]::Escape($Root) -replace '\\\\', '[\\/]'
    $all = @(Get-CimInstance Win32_Process)
    $parents = @($process.Id); $descendants = @()
    while ($parents.Count) {
        $next = @($all | Where-Object { $parents -contains $_.ParentProcessId -and $_.CommandLine -match $rootPattern })
        $descendants += $next; $parents = @($next | ForEach-Object ProcessId)
    }
    foreach ($child in ($descendants | Sort-Object CreationDate -Descending)) { Stop-Process -Id $child.ProcessId -Force -ErrorAction SilentlyContinue }
    $mine = Get-CimInstance Win32_Process -Filter "ProcessId = $($process.Id)"
    if ($mine -and $mine.CommandLine -match $rootPattern) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
    $null = $process.WaitForExit(10000)
}
$elapsed = $clock.Elapsed.TotalSeconds
$stdout = Get-Content -Raw (Join-Path $OutDir 'stdout.txt') -ErrorAction SilentlyContinue; if ($null -eq $stdout) { $stdout = '' }
$stderr = Get-Content -Raw (Join-Path $OutDir 'stderr.txt') -ErrorAction SilentlyContinue; if ($null -eq $stderr) { $stderr = '' }
$exitCode = if ($timedOut) { $null } else { $process.ExitCode }
$status = [ordered]@{
    program = $Program
    args = $GuestArgs
    exit = $exitCode
    timed_out = $timedOut
    seconds = [math]::Round($elapsed, 1)
    stop = (Get-AlchemyStopPoint $stdout $stderr $exitCode)
    captures = @(Get-ChildItem $capture -File -ErrorAction SilentlyContinue | ForEach-Object Name)
}
$status | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $OutDir 'status.json')
$status | ConvertTo-Json -Depth 5
