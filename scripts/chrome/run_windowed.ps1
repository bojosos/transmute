# Windowed Chrome under alchemy-fx, ended over the DevTools protocol (CDP) on a pipe.
#
# Chrome's --remote-debugging-pipe reads NUL-terminated JSON messages from descriptor 3 and writes
# its replies to descriptor 4. This script creates two named pipes, hands them to the guest with
# alchemy-fx --fd 3=r:<pipe> --fd 4=w:<pipe> (a shell's 3< and 4> redirections), waits until a page
# target has the expected title (Target.setDiscoverTargets events, Target.getTargets polls) and,
# with -Frames, until ALCHEMY_CONTENTS_CAPTURE's window-*.txt reports that many frames, then sends
# Browser.close and checks that the process exits 0. No input is injected; DevTools over the pipe
# is the only control.
# -Frames counts the compositor's window composites (one per CATransaction commit that changed a
# composited layer tree), not Chrome's frames: a static page may produce only 2, so ask for 2 there.
#
# Runs are cloaked (ALCHEMY_GUI_CLOAK, ALCHEMY_GUI_NO_ACTIVATE, ALCHEMY_GUI_NO_CRASH_DIALOG) with
# ALCHEMY_NETWORK=loopback and the wave's network-blocking flags. HOME and the profile go under
# <Root>\Users\<User>\ (the root itself is read-only).
#
# Usage: run_windowed.ps1 -Exe <build>\Release\alchemy-fx.exe -OutDir <scratch dir>
#          [-Frames 1] [-Title 'Alchemy probe JS'] [-TitleNot '*running*'] [-Pass 'Alchemy WebGL: PASS']
#          [-Url file:///...] [-Timeout 300] [-CloseTimeout 120]
#          [-User harness] [-Headless] [-SingleProcess | -NoSingleProcess] [-Gpu | -SwiftShader] [-Jit] [-Extra @('--flag')] [-Env @{ NAME='value' }]
# -Title is a -like pattern ('Alchemy WebGL: *'); page titles matching -TitleNot are not taken. With -Pass, the
# matched title must also match that pattern, or the run fails (Chrome is still closed and its exit checked).
# The matched title is in status.txt.
# -Settle N: with -Frames, also accept a capture that has at least one frame and no new frame for N seconds
# (Chrome may draw a static page in a single composite when its compositor starts late, e.g. after GPU-process
# relaunches). The step log says which condition ended the wait.
# -SwiftShader (implies -Gpu): ANGLE on Chrome's bundled SwiftShader (--use-gl=angle --use-angle=swiftshader
# --enable-unsafe-swiftshader). WebGL and Graphite then render in software; see ALCHEMY_CHROME_AUDIT.md, "Stage 4 result".
# -Gpu drops --disable-gpu (the GPU process runs, on Alchemy's Metal); -Jit drops --js-flags=--jitless.
# Process model: windowed runs are multi-process by default (with --single-process, a windowed
# browser has no DiscardableMemoryAllocator while its renderers are child processes, and Chrome
# crashes by design); -Headless runs default to --single-process. -SingleProcess or
# -NoSingleProcess overrides either default.
# Output in -OutDir: stdout.txt, stderr.txt, cdp.txt (the protocol traffic), cmdline.txt,
# network-denied.txt, capture\ (BMPs: never commit them) and status.txt. The script's exit code is
# 0 only when every step passed and Chrome exited 0.
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$OutDir,
    [string]$Root = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'artifacts\alchemy-root-chrome'),
    [string]$RuntimeDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'artifacts\alchemy-llvm-arm64'),
    [string]$Program = '/Applications/Google Chrome.app',
    [string]$Url = '',
    [string]$Title = 'Alchemy probe JS',
    [string]$TitleNot = '',
    [string]$Pass = '',
    [int]$Frames = 0,
    [int]$Settle = 0,
    [int]$Timeout = 300,
    [int]$CloseTimeout = 120,
    [string]$User = 'harness',
    [switch]$Headless,
    [switch]$SingleProcess,
    [switch]$NoSingleProcess,
    [switch]$Gpu,
    [switch]$SwiftShader,
    [switch]$Jit,
    [string[]]$Extra = @(),
    [hashtable]$Env = @{}
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

# A host path as the guest sees it (Alchemy maps drive X: to /Volumes/X).
function Get-GuestPath([string]$HostPath) {
    $full = [IO.Path]::GetFullPath($HostPath)
    '/Volumes/' + $full.Substring(0, 1).ToUpperInvariant() + ($full.Substring(2) -replace '\\', '/')
}

$Root = [IO.Path]::GetFullPath($Root)
if (-not (Test-Path (Join-Path $Root '.alchemy-root'))) { throw "not an Alchemy root: $Root (pass -Root; a worktree has no artifacts)" }
if (-not $Url) { $Url = 'file://' + (Get-GuestPath (Join-Path $Root 'probe\test.html')) }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = [IO.Path]::GetFullPath($OutDir)
$capture = Join-Path $OutDir 'capture'
Remove-Item -Recurse -Force $capture -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $capture | Out-Null
$userDir = Join-Path $Root "Users\$User"
$runName = Split-Path -Leaf $OutDir
$profileDir = Join-Path $userDir "profiles\$runName"
Remove-Item -Recurse -Force $profileDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $userDir 'home') | Out-Null
$cdpLog = Join-Path $OutDir 'cdp.txt'
Set-Content $cdpLog ''

# ---- the pipes: Chrome reads fd 3 (we write) and writes fd 4 (we read) ----
$pipeBase = "alchemy-cdp-$PID-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
$toChrome = New-Object IO.Pipes.NamedPipeServerStream("$pipeBase-in", [IO.Pipes.PipeDirection]::Out, 1,
    [IO.Pipes.PipeTransmissionMode]::Byte, [IO.Pipes.PipeOptions]::Asynchronous)
$fromChrome = New-Object IO.Pipes.NamedPipeServerStream("$pipeBase-out", [IO.Pipes.PipeDirection]::In, 1,
    [IO.Pipes.PipeTransmissionMode]::Byte, [IO.Pipes.PipeOptions]::Asynchronous)

$chromeArgs = @()
if ($Headless) { $chromeArgs += '--headless' }
if ($SingleProcess -and $NoSingleProcess) { throw 'pass -SingleProcess or -NoSingleProcess, not both' }
$useSingleProcess = if ($SingleProcess) { $true } elseif ($NoSingleProcess) { $false } else { [bool]$Headless }
if ($useSingleProcess) { $chromeArgs += '--single-process' }
$chromeArgs += @('--remote-debugging-pipe', '--no-sandbox')
if ($SwiftShader) { $chromeArgs += @('--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader') }
elseif (-not $Gpu) { $chromeArgs += '--disable-gpu' }
$chromeArgs += '--use-mock-keychain'
if (-not $Jit) { $chromeArgs += '--js-flags=--jitless' }
$chromeArgs += @("--user-data-dir=$(Get-GuestPath $profileDir)", '--no-first-run', '--disable-background-networking', '--disable-component-update',
    '--disable-sync', '--disable-default-apps', '--metrics-recording-only', '--disable-breakpad',
    '--host-resolver-rules=MAP * ~NOTFOUND , EXCLUDE localhost', '--no-pings', '--disable-domain-reliability',
    '--disable-client-side-phishing-detection',
    '--disable-features=OptimizationHints,MediaRouter,DialMediaRouteProvider,Translate,AutofillServerCommunication,InterestFeedContentSuggestions') + $Extra + @($Url)
$all = @('--runtime-dir', $RuntimeDir, '--root', $Root,
    '--fd', "3=r:\\.\pipe\$pipeBase-in", '--fd', "4=w:\\.\pipe\$pipeBase-out", $Program) + $chromeArgs

$env:ALCHEMY_NETWORK = 'loopback'; $env:ALCHEMY_GUI_CLOAK = '1'; $env:ALCHEMY_GUI_NO_ACTIVATE = '1'; $env:ALCHEMY_GUI_NO_CRASH_DIALOG = '1'
$env:ALCHEMY_CONTENTS_CAPTURE = $capture
# A timeout touches this file first: every Alchemy process of the tree prints its guest threads to stderr
# (ALCHEMY_HANG_DUMP, src/alchemy/process.cpp), so a hang shows where each waits.
$hangTrigger = Join-Path $OutDir 'hang-dump.trigger'
$env:ALCHEMY_HANG_DUMP = $hangTrigger
$env:HOME = Join-Path $userDir 'home'
foreach ($k in $Env.Keys) { Set-Item "Env:$k" $Env[$k] }
$quoted = ($all | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
Set-Content (Join-Path $OutDir 'cmdline.txt') "$Exe $quoted"

$steps = New-Object Collections.Generic.List[string]
function Step([string]$Text) {
    $line = '{0,7:0.0} s  {1}' -f $script:clock.Elapsed.TotalSeconds, $Text
    $steps.Add($line); Write-Host $line
}

$clock = [Diagnostics.Stopwatch]::StartNew()
$process = Start-Process -FilePath $Exe -ArgumentList $quoted -NoNewWindow -PassThru `
    -RedirectStandardOutput (Join-Path $OutDir 'stdout.txt') -RedirectStandardError (Join-Path $OutDir 'stderr.txt')
$null = $process.Handle   # keeps the exit code readable after exit
$failure = $null
$found = $null

# ---- CDP over the pipes ----
$encoding = New-Object Text.UTF8Encoding($false)
$pending = New-Object Collections.Generic.List[byte]
$buffer = New-Object byte[] 65536
$readTask = $null
$nextId = 0
function Send-Cdp([string]$Method, [hashtable]$Params = @{}) {
    $script:nextId++
    $json = (@{ id = $script:nextId; method = $Method; params = $Params } | ConvertTo-Json -Compress -Depth 8)
    Add-Content $cdpLog "-> $json"
    $bytes = $encoding.GetBytes($json + [char]0)
    $toChrome.Write($bytes, 0, $bytes.Length)
    $toChrome.Flush()
    $script:nextId
}
# Waits up to $Milliseconds for messages; returns the decoded ones (an empty list at the deadline
# or once the pipe has ended).
function Receive-Cdp([int]$Milliseconds) {
    $messages = @()
    if ($null -eq $script:readTask) { $script:readTask = $fromChrome.ReadAsync($buffer, 0, $buffer.Length) }
    if (-not $script:readTask.Wait($Milliseconds)) { return , $messages }
    $count = $script:readTask.Result
    $script:readTask = $null
    if ($count -eq 0) { $script:pipeEnded = $true; return , $messages }
    for ($i = 0; $i -lt $count; $i++) {
        if ($buffer[$i] -eq 0) {
            $text = $encoding.GetString($pending.ToArray())
            $pending.Clear()
            $short = if ($text.Length -gt 400) { $text.Substring(0, 400) + '...' } else { $text }
            Add-Content $cdpLog "<- $short"
            try { $messages += , ($text | ConvertFrom-Json) } catch { Add-Content $cdpLog '   (not JSON)' }
        } else { $pending.Add($buffer[$i]) }
    }
    return , $messages
}
$pipeEnded = $false

function Get-CaptureFrames {
    $most = 0
    foreach ($file in @(Get-ChildItem -Path $capture -Filter 'window-*.txt' -ErrorAction SilentlyContinue)) {
        $text = Get-Content $file.FullName -Raw -ErrorAction SilentlyContinue
        if ($text -and $text -match 'frames (\d+)') { $most = [math]::Max($most, [int]$Matches[1]) }
    }
    $most
}

try {
    $connectIn = $toChrome.WaitForConnectionAsync()
    $connectOut = $fromChrome.WaitForConnectionAsync()
    while (-not ($connectIn.IsCompleted -and $connectOut.IsCompleted)) {
        if ($process.HasExited) { throw "alchemy-fx exited ($($process.ExitCode)) before it opened the DevTools pipes" }
        if ($clock.Elapsed.TotalSeconds -gt 60) { throw 'alchemy-fx did not open the DevTools pipes within 60 s' }
        Start-Sleep -Milliseconds 100
    }
    Step 'DevTools pipes connected (fd 3 and 4)'
    $null = Send-Cdp 'Target.setDiscoverTargets' @{ discover = $true }
    $poll = [Diagnostics.Stopwatch]::StartNew()
    $found = $null
    while (-not $found) {
        if ($clock.Elapsed.TotalSeconds -gt $Timeout) {
            Set-Content $hangTrigger (Get-Date -Format o)   # ALCHEMY_HANG_DUMP: where each process waits
            Start-Sleep -Seconds 3
            throw "no page target titled '$Title' within $Timeout s"
        }
        if ($process.HasExited) { throw "Chrome exited ($($process.ExitCode)) before a page target was titled '$Title'" }
        if ($pipeEnded) { throw 'the DevTools pipe ended before the page target appeared' }
        if ($poll.Elapsed.TotalSeconds -gt 3) { $null = Send-Cdp 'Target.getTargets'; $poll.Restart() }
        foreach ($message in (Receive-Cdp 500)) {
            $infos = @()
            if ($message.PSObject.Properties['params'] -and $message.params -and $message.params.PSObject.Properties['targetInfo']) { $infos += $message.params.targetInfo }
            if ($message.PSObject.Properties['result'] -and $message.result -and $message.result.PSObject.Properties['targetInfos']) { $infos += $message.result.targetInfos }
            foreach ($info in $infos) {
                if ($info.type -eq 'page' -and $info.title -like $Title -and -not ($TitleNot -and $info.title -like $TitleNot)) { $found = $info }
            }
        }
    }
    Step "page target '$($found.title)' ($($found.url))"
    if ($Frames -gt 0) {
        $seen = 0; $since = [Diagnostics.Stopwatch]::StartNew(); $settled = $false
        while ((Get-CaptureFrames) -lt $Frames) {
            $now = Get-CaptureFrames
            if ($now -ne $seen) { $seen = $now; $since.Restart() }
            if ($Settle -gt 0 -and $seen -ge 1 -and $since.Elapsed.TotalSeconds -ge $Settle) { $settled = $true; break }
            if ($clock.Elapsed.TotalSeconds -gt $Timeout) { throw "the capture has $(Get-CaptureFrames) frames, not $Frames, after $Timeout s" }
            if ($process.HasExited) { throw "Chrome exited ($($process.ExitCode)) before the capture had $Frames frames" }
            $null = Receive-Cdp 500
        }
        if ($settled) { Step "capture settled at $(Get-CaptureFrames) frames (no new frame for $Settle s)" }
        else { Step "capture has $(Get-CaptureFrames) frames" }
    }
    $closeId = Send-Cdp 'Browser.close'
    Step 'Browser.close sent'
    $closing = [Diagnostics.Stopwatch]::StartNew()
    while (-not $process.HasExited -and $closing.Elapsed.TotalSeconds -lt $CloseTimeout) {
        if ($pipeEnded) { Start-Sleep -Milliseconds 200; continue }
        foreach ($message in (Receive-Cdp 500)) {
            if ($message.PSObject.Properties['id'] -and $message.id -eq $closeId) { Step 'Browser.close answered' }
        }
    }
    # HasExited, not WaitForExit: WaitForExit also waits for the redirected streams, which helper
    # processes may hold open a little longer.
    if (-not $process.HasExited) { throw "Chrome did not exit within $CloseTimeout s of Browser.close" }
    Step "exit $($process.ExitCode)"
    if ($process.ExitCode -ne 0) { $failure = "exit code $($process.ExitCode)" }
    elseif ($Pass -and -not ($found.title -like $Pass)) { $failure = "page title '$($found.title)' does not match -Pass '$Pass'" }
} catch {
    $failure = "$_"
    Start-Sleep -Milliseconds 500
    if ($process.HasExited) { $failure += " (alchemy-fx exited $($process.ExitCode); see stderr.txt)" }
    Step "FAILED: $failure"
} finally {
    if (-not $process.HasExited) {
        # Only this run's own process tree.
        & taskkill.exe /PID $process.Id /T /F 2>&1 | Out-Null
        $process.WaitForExit(10000) | Out-Null
        Step "killed the tree of pid $($process.Id)"
    }
    $toChrome.Dispose(); $fromChrome.Dispose()
}

$stderr = Join-Path $OutDir 'stderr.txt'
$denied = @(Select-String -Path $stderr -Pattern 'network denied' -ErrorAction SilentlyContinue | ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique)
$denied | Set-Content (Join-Path $OutDir 'network-denied.txt')
$result = if ($failure) { "FAIL: $failure" } elseif ($Pass) { "PASS: '$($found.title)', Browser.close, exit 0" } else { 'PASS: Browser.close, exit 0' }
$matched = if ($found) { "title '$($found.title)'" } else { 'title (none matched)' }
$summary = @($result, $matched, "frames $(Get-CaptureFrames)", "network denied: $($denied.Count) unique") + $steps
$summary | Set-Content (Join-Path $OutDir 'status.txt')
Write-Host $result
if ($failure) { exit 1 }
exit 0
