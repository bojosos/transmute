# One cloaked guest run for the apps wave: starts a Mach-O program under alchemy-fx, waits for it to exit (or for
# -Timeout), and records what happened. No input is injected.
#
# * Always cloaked and offline: ALCHEMY_GUI_CLOAK, ALCHEMY_GUI_NO_ACTIVATE, ALCHEMY_GUI_NO_CRASH_DIALOG = 1 and
#   ALCHEMY_NETWORK=loopback (cloaked runs also send audio to the null sink, so they stay silent).
# * -Capture sets ALCHEMY_CONTENTS_CAPTURE=<OutDir>\capture (window-N.bmp/.txt; never commit the BMPs).
# * -Watch runs <notes>\fx-perf\cloak_watch.ps1 beside the run: a visible, uncloaked window of this checkout's
#   alchemy-fx is logged and its tree killed at once.
# * HOME is <Root>\Users\<User>\home (TMPDIR follows the host temp directory).
# * On -Timeout the run's own process tree is stopped: the started process, its descendants, and any process of this
#   -Exe (full path compared) created after the run began.
#
# Usage: run_guest.ps1 -Exe <build>\Release\alchemy-fx.exe -Root <alchemy root> -Program '/Applications/X.app'
#          -OutDir <dir> [-ProgramArgs @(...)] [-Timeout 120] [-Capture] [-Watch] [-User media] [-Env @{ NAME='value' }]
#          [-Cwd <host dir>] [-HangDumpAt @(60, 90)] [-RuntimeDir dir,...] [-Notes <notes dir>]
# -RuntimeDir defaults to the main checkout's artifacts\alchemy-{llvm,swift,expat}-arm64 (found through git, so it
# works from a worktree). -Notes (default $env:ALCHEMY_APPS_NOTES) is the shared notes folder holding
# fx-perf\window.md, fx-perf\perf.lock and fx-perf\cloak_watch.ps1; it is required, since no run may start while an fx
# perf measurement window is open.
# Output in -OutDir: cmdline.txt, stdout.txt, stderr.txt, status.txt (exit code, seconds, timed out), capture\,
# cloak-watch.txt.
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Program,
    [Parameter(Mandatory)][string]$OutDir,
    [string[]]$ProgramArgs = @(),
    [int]$Timeout = 120,
    [switch]$Capture,
    [switch]$Watch,
    [string]$User = 'media',
    [hashtable]$Env = @{},
    [string]$Cwd = '',
    [int[]]$HangDumpAt = @(),
    [string[]]$RuntimeDir = @(),
    [string]$Notes = $env:ALCHEMY_APPS_NOTES
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

if (-not $Notes) { throw 'pass -Notes or set ALCHEMY_APPS_NOTES to the notes folder holding fx-perf\window.md' }
if (-not $RuntimeDir.Count) {
    $checkout = Split-Path -Parent (git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir).Trim()
    $RuntimeDir = @('llvm', 'swift', 'expat') | ForEach-Object { Join-Path $checkout "artifacts\alchemy-$_-arm64" }
}

$window = Get-Content (Join-Path $Notes 'fx-perf\window.md') -Raw
if ($window -match '(?m)^\s*Open\b' -or $window -match '(?i)\bwindow is open\b') { throw "an fx perf measurement window is open (fx-perf\window.md): no guest runs" }
$perfLock = Join-Path $Notes 'fx-perf\perf.lock'
if (Test-Path $perfLock) {
    $owner = ((Get-Content $perfLock -TotalCount 1) -split ' ')[0]
    if ($owner -and (Get-Process -Id $owner -ErrorAction SilentlyContinue)) { throw "fx perf is measuring (perf.lock names live pid $owner): no guest runs" }
}

$Exe = (Resolve-Path $Exe).Path
$Root = [IO.Path]::GetFullPath($Root)
if (-not (Test-Path (Join-Path $Root '.alchemy-root'))) { throw "not an Alchemy root: $Root" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = [IO.Path]::GetFullPath($OutDir)
$homeDir = Join-Path $Root "Users\$User\home"
New-Item -ItemType Directory -Force $homeDir | Out-Null

$all = @($RuntimeDir | ForEach-Object { '--runtime-dir', $_ }) + @('--root', $Root, $Program) + $ProgramArgs
$quoted = ($all | ForEach-Object { if ($_ -eq '' -or $_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
Set-Content (Join-Path $OutDir 'cmdline.txt') "$Exe $quoted"

$env:ALCHEMY_NETWORK = 'loopback'; $env:ALCHEMY_GUI_CLOAK = '1'; $env:ALCHEMY_GUI_NO_ACTIVATE = '1'; $env:ALCHEMY_GUI_NO_CRASH_DIALOG = '1'
$env:HOME = $homeDir
Remove-Item Env:ALCHEMY_CONTENTS_CAPTURE -ErrorAction SilentlyContinue
if ($Capture) {
    $captureDir = Join-Path $OutDir 'capture'
    Remove-Item -Recurse -Force $captureDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $captureDir | Out-Null
    $env:ALCHEMY_CONTENTS_CAPTURE = $captureDir
}
Remove-Item Env:ALCHEMY_HANG_DUMP -ErrorAction SilentlyContinue
$hangTrigger = Join-Path $OutDir 'hang-dump.trigger'
if ($HangDumpAt.Count) { Remove-Item $hangTrigger -ErrorAction SilentlyContinue; $env:ALCHEMY_HANG_DUMP = $hangTrigger }
foreach ($k in $Env.Keys) { Set-Item "Env:$k" $Env[$k] }

$watcher = $null
if ($Watch) {
    $watchLog = Join-Path $OutDir 'cloak-watch.txt'
    Set-Content $watchLog ''
    $watcher = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        (Join-Path $Notes 'fx-perf\cloak_watch.ps1'), '-PathFilter', (Split-Path -Parent $Exe), '-Log', $watchLog, '-Seconds', ($Timeout + 60))
}

$started = Get-Date
$clock = [Diagnostics.Stopwatch]::StartNew()
$startArgs = @{ FilePath = $Exe; ArgumentList = $quoted; NoNewWindow = $true; PassThru = $true
    RedirectStandardOutput = (Join-Path $OutDir 'stdout.txt'); RedirectStandardError = (Join-Path $OutDir 'stderr.txt') }
if ($Cwd) { $startArgs.WorkingDirectory = $Cwd }
$process = Start-Process @startArgs
$null = $process.Handle
# -HangDumpAt: touch the ALCHEMY_HANG_DUMP trigger at these seconds (every guest thread is printed to stderr).
foreach ($at in @($HangDumpAt | Sort-Object)) {
    $left = [int]($at * 1000 - $clock.ElapsedMilliseconds)
    if ($left -gt 0 -and $process.WaitForExit($left)) { break }
    if ($process.HasExited) { break }
    Set-Content $hangTrigger "$at"
}
$timedOut = -not $process.WaitForExit([math]::Max(0, [int]($Timeout * 1000 - $clock.ElapsedMilliseconds)))
$killed = 0
if ($timedOut) {
    $procs = @(Get-CimInstance Win32_Process | Where-Object { $_.CreationDate -ge $started.AddSeconds(-1) })
    $tree = @($process.Id); $grew = $true
    while ($grew) { $grew = $false; foreach ($q in $procs) { if ($tree -contains $q.ParentProcessId -and $tree -notcontains $q.ProcessId) { $tree += $q.ProcessId; $grew = $true } } }
    foreach ($q in $procs) { if ($q.ExecutablePath -and [IO.Path]::GetFullPath($q.ExecutablePath) -eq $Exe -and $tree -notcontains $q.ProcessId) { $tree += $q.ProcessId } }
    [array]::Reverse($tree)
    # A process that is already exiting refuses taskkill; with -ErrorAction Stop that stderr line would end this script.
    foreach ($id in $tree) { try { & taskkill.exe /PID $id /F 2>&1 | Out-Null } catch {} }
    $killed = $tree.Count
    $process.WaitForExit(10000) | Out-Null
}
$elapsed = $clock.Elapsed.TotalSeconds
if ($watcher -and -not $watcher.HasExited) { Stop-Process -Id $watcher.Id -Force -ErrorAction SilentlyContinue }
$code = if ($timedOut) { 'timeout' } else { $process.ExitCode }
$status = "exit $code`nseconds {0:0.0}`ntimed_out $timedOut`nkilled $killed" -f $elapsed
Set-Content (Join-Path $OutDir 'status.txt') $status
Write-Host ($status -replace "`n", '; ')
