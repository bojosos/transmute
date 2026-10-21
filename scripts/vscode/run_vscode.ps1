# Visual Studio Code (macOS arm64, Electron) under alchemy-fx.
#
# Two modes:
# * -Cli @('--version'): runs Code with these arguments and waits for it to exit (-RunAsNode sets
#   ELECTRON_RUN_AS_NODE=1, so Code is a plain Node.js; pass the script path in -Cli).
# * windowed (default): starts Code with --remote-debugging-port=0, reads the port from
#   <user-data-dir>\DevToolsActivePort, talks the DevTools protocol (CDP) over a WebSocket to
#   127.0.0.1, waits until a page target's title matches -Title and, with -Frames, until
#   ALCHEMY_CONTENTS_CAPTURE's window-*.txt reports that many frames. -Eval runs expressions in the
#   workbench page (Runtime.evaluate) and logs their values. Then it asks Code to quit (-Quit:
#   'browser' sends Browser.close, 'none' skips it) and kills the run's own process tree only if
#   Code is still running after -CloseTimeout. No input is injected.
#
# Runs are cloaked (ALCHEMY_GUI_CLOAK, ALCHEMY_GUI_NO_ACTIVATE, ALCHEMY_GUI_NO_CRASH_DIALOG) with
# ALCHEMY_NETWORK=loopback. The user-data and extensions directories and HOME go under
# <Root>\Users\<User>\ (the root itself stays read-only); each run starts from a fresh user-data
# directory with a settings.json that turns off updates, telemetry, experiments and the online
# extension and settings-search lookups.
#
# Usage: run_vscode.ps1 -Exe <build>\Release\alchemy-fx.exe -OutDir <scratch dir>
#          [-Cli @('--version')] [-RunAsNode] [-Open @('/Volumes/C/...')] [-Title '*Visual Studio Code'] [-TitleNot 'vscode-file:*']
#          [-Frames 2] [-Settle 20] [-Eval @('document.title')] [-Screenshot] [-Probe [-NoTerminal]] [-Quit browser|none]
#          [-Timeout 600] [-CloseTimeout 120] [-User vscode] [-Gpu] [-NoJit] [-Extra @('--flag')] [-Env @{ NAME='value' }]
# Output in -OutDir: stdout.txt, stderr.txt, cdp.txt, cmdline.txt, network-denied.txt, capture\ (BMPs:
# never commit them; window-N-settled.bmp is the capture when the frames settled, before quitting), eval.txt, page.png (-Screenshot: CDP Page.captureScreenshot) and status.txt. The exit code is 0 only when every step passed.
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$OutDir,
    [string]$Root = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'artifacts\alchemy-root-vscode'),
    [string]$RuntimeDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'artifacts\alchemy-llvm-arm64'),
    [string]$Program = '/Applications/Visual Studio Code.app',
    [string[]]$Cli = @(),
    [switch]$RunAsNode,
    [string[]]$Open = @(),
    [string]$Title = '*Visual Studio Code',
    [string]$TitleNot = 'vscode-file:*',
    [int]$Frames = 0,
    [int]$Settle = 0,
    [string[]]$Eval = @(),
    [switch]$Screenshot,
    [switch]$Probe,
    [switch]$NoTerminal,
    [ValidateSet('browser', 'none')][string]$Quit = 'browser',
    [int]$Timeout = 600,
    [int]$CloseTimeout = 120,
    [string]$User = 'vscode',
    [switch]$Gpu,
    [switch]$NoJit,
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
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = [IO.Path]::GetFullPath($OutDir)
$capture = Join-Path $OutDir 'capture'
Remove-Item -Recurse -Force $capture -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $capture | Out-Null
$userDir = Join-Path $Root "Users\$User"
$runName = Split-Path -Leaf $OutDir
$dataDir = Join-Path $userDir "profiles\$runName"
$extDir = Join-Path $userDir 'extensions'
Remove-Item -Recurse -Force $dataDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $userDir 'home'), (Join-Path $dataDir 'User'), $extDir | Out-Null
$settings = [ordered]@{
    'update.mode' = 'none'
    'update.showReleaseNotes' = $false
    'telemetry.telemetryLevel' = 'off'
    'extensions.autoUpdate' = $false
    'extensions.autoCheckUpdates' = $false
    'extensions.ignoreRecommendations' = $true
    'workbench.enableExperiments' = $false
    'workbench.settings.enableNaturalLanguageSearch' = $false
    'workbench.startupEditor' = 'welcomePage'
    'window.restoreWindows' = 'none'
    'npm.fetchOnlinePackageInfo' = $false
    'git.autofetch' = $false
    'chat.disableAIFeatures' = $true
    'security.workspace.trust.enabled' = $false
    # Resolving the login environment starts a second Code (as Node) through the shell, which takes
    # longer than the default 10 s under Alchemy.
    'application.shellEnvironmentResolutionTimeout' = 60
}
[IO.File]::WriteAllText((Join-Path $dataDir 'User\settings.json'), ($settings | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))

# -Probe: a small workspace and the probe extension (probe-extension/), which opens hello.ts,
# searches for "greet" (ripgrep), shows the Search view and starts an integrated terminal,
# writes <workspace>\.alchemy-probe.json and sets the window title to "probe done - ...".
$workspace = $null
if ($Probe) {
    $workspace = Join-Path $userDir "workspaces\$runName"
    Remove-Item -Recurse -Force $workspace -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force (Join-Path $workspace 'src'), (Join-Path $workspace '.vscode') | Out-Null
    $utf8 = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText((Join-Path $workspace 'hello.ts'), @'
// A TypeScript file for the syntax highlighting check.
interface Greeting {
    name: string;
    times: number;
}

export function greet(greeting: Greeting): string {
    const parts: string[] = [];
    for (let index = 0; index < greeting.times; index++) {
        parts.push(`Hello, ${greeting.name}!`);
    }
    return parts.join(" ");
}

console.log(greet({ name: "Alchemy", times: 2 }));
'@, $utf8)
    [IO.File]::WriteAllText((Join-Path $workspace 'src\data.json'), '{ "greet": true, "count": 3, "items": ["a", "b"] }', $utf8)
    [IO.File]::WriteAllText((Join-Path $workspace 'README.md'), "# Probe workspace`n`nCall ``greet`` to say hello.`n", $utf8)
    $probeSettings = [ordered]@{ 'alchemyProbe.terminal' = (-not $NoTerminal) }
    [IO.File]::WriteAllText((Join-Path $workspace '.vscode\settings.json'), ($probeSettings | ConvertTo-Json), $utf8)
    $extension = Join-Path $userDir 'probe-extension'
    Remove-Item -Recurse -Force $extension -ErrorAction SilentlyContinue
    Copy-Item -Recurse (Join-Path $PSScriptRoot 'probe-extension') $extension
    # The proposal list in package.json is not honoured for a development extension; the switch is.
    $Open = @("--extensionDevelopmentPath=/Users/$User/probe-extension", '--enable-proposed-api', 'alchemy.alchemy-probe', "/Users/$User/workspaces/$runName", "/Users/$User/workspaces/$runName/hello.ts") + $Open
    if (-not $PSBoundParameters.ContainsKey('Title')) { $Title = '*probe done*' }
}

$codeArgs = @()
if ($Cli.Count) { $codeArgs = $Cli }
else {
    # Guest paths under the root's /Users (VS Code treats /Volumes/... as network shares and does not watch them).
    $codeArgs += @("--user-data-dir=/Users/$User/profiles/$runName", "--extensions-dir=/Users/$User/extensions",
        '--remote-debugging-port=0', '--no-sandbox', '--disable-telemetry', '--skip-release-notes')
    if (-not $Gpu) { $codeArgs += '--disable-gpu' }
    if ($NoJit) { $codeArgs += '--js-flags=--jitless' }
    $codeArgs += $Extra + $Open
}
$all = @('--runtime-dir', $RuntimeDir, '--root', $Root, $Program) + $codeArgs

$env:ALCHEMY_NETWORK = 'loopback'; $env:ALCHEMY_GUI_CLOAK = '1'; $env:ALCHEMY_GUI_NO_ACTIVATE = '1'; $env:ALCHEMY_GUI_NO_CRASH_DIALOG = '1'
$env:ALCHEMY_CONTENTS_CAPTURE = $capture
$env:HOME = Join-Path $userDir 'home'
if ($RunAsNode) { $env:ELECTRON_RUN_AS_NODE = '1' } else { Remove-Item Env:ELECTRON_RUN_AS_NODE -ErrorAction SilentlyContinue }
foreach ($k in $Env.Keys) { Set-Item "Env:$k" $Env[$k] }
$quoted = ($all | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
Set-Content (Join-Path $OutDir 'cmdline.txt') "$Exe $quoted"
$cdpLog = Join-Path $OutDir 'cdp.txt'
$evalLog = Join-Path $OutDir 'eval.txt'
Set-Content $cdpLog ''; Set-Content $evalLog ''

$steps = New-Object Collections.Generic.List[string]
function Step([string]$Text) {
    $line = '{0,7:0.0} s  {1}' -f $script:clock.Elapsed.TotalSeconds, $Text
    $steps.Add($line); Write-Host $line
}

$clock = [Diagnostics.Stopwatch]::StartNew()
$started = Get-Date
$process = Start-Process -FilePath $Exe -ArgumentList $quoted -NoNewWindow -PassThru `
    -RedirectStandardOutput (Join-Path $OutDir 'stdout.txt') -RedirectStandardError (Join-Path $OutDir 'stderr.txt')
$null = $process.Handle   # keeps the exit code readable after exit
$failure = $null
$found = $null

# ---- CDP over a WebSocket ----
$socket = $null
$encoding = New-Object Text.UTF8Encoding($false)
$buffer = New-Object byte[] 1048576
$pending = New-Object IO.MemoryStream
$receiveTask = $null
$nextId = 0
$socketEnded = $false
function Send-Cdp([string]$Method, [hashtable]$Params = @{}, [string]$SessionId = '') {
    $script:nextId++
    $message = @{ id = $script:nextId; method = $Method; params = $Params }
    if ($SessionId) { $message.sessionId = $SessionId }
    $json = $message | ConvertTo-Json -Compress -Depth 8
    Add-Content $cdpLog "-> $json"
    $bytes = $encoding.GetBytes($json)
    $segment = New-Object ArraySegment[byte] -ArgumentList @(, $bytes)
    $script:socket.SendAsync($segment, [Net.WebSockets.WebSocketMessageType]::Text, $true, [Threading.CancellationToken]::None).Wait()
    $script:nextId
}
# Waits up to $Milliseconds for messages; returns the decoded ones.
function Receive-Cdp([int]$Milliseconds) {
    $messages = @()
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while ($deadline.ElapsedMilliseconds -lt $Milliseconds -and -not $script:socketEnded) {
        if ($null -eq $script:receiveTask) {
            $segment = New-Object ArraySegment[byte] -ArgumentList @(, $buffer)
            $script:receiveTask = $script:socket.ReceiveAsync($segment, [Threading.CancellationToken]::None)
        }
        $left = [math]::Max(1, $Milliseconds - [int]$deadline.ElapsedMilliseconds)
        try { if (-not $script:receiveTask.Wait($left)) { break } } catch { $script:socketEnded = $true; break }
        $result = $script:receiveTask.Result
        $script:receiveTask = $null
        if ($result.MessageType -eq [Net.WebSockets.WebSocketMessageType]::Close) { $script:socketEnded = $true; break }
        $pending.Write($buffer, 0, $result.Count)
        if ($result.EndOfMessage) {
            $text = $encoding.GetString($pending.ToArray())
            $pending.SetLength(0)
            $short = if ($text.Length -gt 600) { $text.Substring(0, 600) + '...' } else { $text }
            Add-Content $cdpLog "<- $short"
            try { $messages += , ($text | ConvertFrom-Json) } catch { Add-Content $cdpLog '   (not JSON)' }
            if ($messages.Count) { break }
        }
    }
    return , $messages
}
# Sends a command and waits for its reply (other messages are passed to $OnMessage).
function Invoke-Cdp([string]$Method, [hashtable]$Params = @{}, [string]$SessionId = '', [int]$Seconds = 60) {
    $id = Send-Cdp $Method $Params $SessionId
    $wait = [Diagnostics.Stopwatch]::StartNew()
    while ($wait.Elapsed.TotalSeconds -lt $Seconds) {
        if ($script:socketEnded) { throw "the DevTools socket closed while waiting for $Method" }
        foreach ($message in (Receive-Cdp 500)) {
            if ($message.PSObject.Properties['id'] -and $message.id -eq $id) {
                if ($message.PSObject.Properties['error']) { throw "$Method failed: $($message.error | ConvertTo-Json -Compress)" }
                return $message.result
            }
        }
    }
    throw "no reply to $Method within $Seconds s"
}

function Get-CaptureFrames {
    $most = 0
    foreach ($file in @(Get-ChildItem -Path $capture -Filter 'window-*.txt' -ErrorAction SilentlyContinue)) {
        $text = Get-Content $file.FullName -Raw -ErrorAction SilentlyContinue
        if ($text -and $text -match 'frames (\d+)') { $most = [math]::Max($most, [int]$Matches[1]) }
    }
    $most
}

# The run's own process tree (alchemy-fx and the helpers it started), children first.
function Stop-RunTree {
    $procs = @(Get-CimInstance Win32_Process | Where-Object { $_.CreationDate -ge $started })
    $tree = @($process.Id); $grew = $true
    while ($grew) { $grew = $false; foreach ($q in $procs) { if ($tree -contains $q.ParentProcessId -and $tree -notcontains $q.ProcessId) { $tree += $q.ProcessId; $grew = $true } } }
    # Children whose parent already ended are no longer linked to the tree: also every process of
    # this run's own alchemy-fx.exe started since the run began (the executable is this checkout's).
    $exePath = [IO.Path]::GetFullPath($Exe)
    foreach ($q in $procs) { if ($q.ExecutablePath -and [IO.Path]::GetFullPath($q.ExecutablePath) -eq $exePath -and $tree -notcontains $q.ProcessId) { $tree += $q.ProcessId } }
    [array]::Reverse($tree)
    $ErrorActionPreference = 'Continue'
    foreach ($id in $tree) { & taskkill.exe /PID $id /F 2>&1 | Out-Null }
    $ErrorActionPreference = 'Stop'
    $process.WaitForExit(10000) | Out-Null
    $tree.Count
}

try {
    if ($Cli.Count) {
        if (-not $process.WaitForExit($Timeout * 1000)) { throw "Code did not exit within $Timeout s" }
        Step "exit $($process.ExitCode)"
        if ($process.ExitCode -ne 0) { $failure = "exit code $($process.ExitCode)" }
    } else {
        $portFile = Join-Path $dataDir 'DevToolsActivePort'
        while (-not (Test-Path $portFile) -or -not (Get-Content $portFile -ErrorAction SilentlyContinue)) {
            if ($process.HasExited) { throw "alchemy-fx exited ($($process.ExitCode)) before Code wrote DevToolsActivePort" }
            if ($clock.Elapsed.TotalSeconds -gt $Timeout) { throw "no DevToolsActivePort within $Timeout s" }
            Start-Sleep -Milliseconds 250
        }
        Start-Sleep -Milliseconds 250
        $lines = @(Get-Content $portFile)
        $wsUrl = "ws://127.0.0.1:$($lines[0].Trim())$($lines[1].Trim())"
        $socket = New-Object Net.WebSockets.ClientWebSocket
        $socket.Options.KeepAliveInterval = [TimeSpan]::Zero
        $socket.ConnectAsync([Uri]$wsUrl, [Threading.CancellationToken]::None).Wait(30000) | Out-Null
        if ($socket.State -ne [Net.WebSockets.WebSocketState]::Open) { throw "cannot connect to $wsUrl" }
        Step "DevTools connected ($wsUrl)"
        $null = Send-Cdp 'Target.setDiscoverTargets' @{ discover = $true }
        $poll = [Diagnostics.Stopwatch]::StartNew()
        $titles = @{}
        while (-not $found) {
            if ($clock.Elapsed.TotalSeconds -gt $Timeout) { throw "no page target titled '$Title' within $Timeout s (titles seen: $(($titles.Keys | Sort-Object) -join ' | '))" }
            if ($process.HasExited) { throw "Code exited ($($process.ExitCode)) before a page target was titled '$Title'" }
            if ($socketEnded) { throw 'the DevTools socket closed before the page target appeared' }
            if ($poll.Elapsed.TotalSeconds -gt 3) { $null = Send-Cdp 'Target.getTargets'; $poll.Restart() }
            foreach ($message in (Receive-Cdp 500)) {
                $infos = @()
                if ($message.PSObject.Properties['params'] -and $message.params -and $message.params.PSObject.Properties['targetInfo']) { $infos += $message.params.targetInfo }
                if ($message.PSObject.Properties['result'] -and $message.result -and $message.result.PSObject.Properties['targetInfos']) { $infos += $message.result.targetInfos }
                foreach ($info in $infos) {
                    if ($info.title -and -not $titles.ContainsKey("$($info.type): $($info.title)")) { $titles["$($info.type): $($info.title)"] = 1; Step "target $($info.type) '$($info.title)' $($info.url)" }
                    if ($info.type -eq 'page' -and $info.title -like $Title -and -not ($TitleNot -and $info.title -like $TitleNot)) { $found = $info }
                }
            }
        }
        Step "page target '$($found.title)'"
        if ($Frames -gt 0) {
            $seen = 0; $since = [Diagnostics.Stopwatch]::StartNew(); $settled = $false
            while ((Get-CaptureFrames) -lt $Frames) {
                $now = Get-CaptureFrames
                if ($now -ne $seen) { $seen = $now; $since.Restart() }
                if ($Settle -gt 0 -and $seen -ge 1 -and $since.Elapsed.TotalSeconds -ge $Settle) { $settled = $true; break }
                if ($clock.Elapsed.TotalSeconds -gt $Timeout) { throw "the capture has $(Get-CaptureFrames) frames, not $Frames, after $Timeout s" }
                if ($process.HasExited) { throw "Code exited ($($process.ExitCode)) before the capture had $Frames frames" }
                $null = Receive-Cdp 500
            }
            if ($settled) { Step "capture settled at $(Get-CaptureFrames) frames (no new frame for $Settle s)" }
            else { Step "capture has $(Get-CaptureFrames) frames" }
            # The capture as it is now: the files are rewritten until exit, when the window is gone.
            foreach ($file in @(Get-ChildItem -Path $capture -Filter 'window-*.bmp' -ErrorAction SilentlyContinue)) {
                Copy-Item $file.FullName (Join-Path $capture ($file.BaseName + '-settled.bmp')) -Force
            }
        }
        $attached = $null
        if ($Eval.Count -or $Screenshot) { $attached = Invoke-Cdp 'Target.attachToTarget' @{ targetId = $found.targetId; flatten = $true } }
        if ($Eval.Count) {
            foreach ($expression in $Eval) {
                $value = Invoke-Cdp 'Runtime.evaluate' @{ expression = $expression; returnByValue = $true; awaitPromise = $true } $attached.sessionId 120
                $text = if ($value.PSObject.Properties['exceptionDetails']) { "EXCEPTION $($value.exceptionDetails | ConvertTo-Json -Compress -Depth 6)" } else { $value.result | ConvertTo-Json -Compress -Depth 6 }
                Add-Content $evalLog "$expression`n  => $text"
                Step "eval: $expression => $(if ($text.Length -gt 200) { $text.Substring(0, 200) + '...' } else { $text })"
            }
        }
        if ($Screenshot) {
            # The renderer's own composite of the page (CDP), next to the window capture.
            $shot = Invoke-Cdp 'Page.captureScreenshot' @{ format = 'png' } $attached.sessionId 120
            [IO.File]::WriteAllBytes((Join-Path $OutDir 'page.png'), [Convert]::FromBase64String($shot.data))
            Step 'page screenshot saved (page.png)'
        }
        if ($Quit -eq 'browser') {
            $closeId = Send-Cdp 'Browser.close'
            Step 'Browser.close sent'
            $closing = [Diagnostics.Stopwatch]::StartNew()
            while (-not $process.HasExited -and $closing.Elapsed.TotalSeconds -lt $CloseTimeout) {
                if ($socketEnded) { Start-Sleep -Milliseconds 200; continue }
                foreach ($message in (Receive-Cdp 500)) {
                    if ($message.PSObject.Properties['id'] -and $message.id -eq $closeId) {
                        $answer = if ($message.PSObject.Properties['error']) { "error $($message.error.message)" } else { 'ok' }
                        Step "Browser.close answered: $answer"
                    }
                }
            }
            if (-not $process.HasExited) { throw "Code did not exit within $CloseTimeout s of Browser.close" }
            Step "exit $($process.ExitCode)"
            if ($process.ExitCode -ne 0) { $failure = "exit code $($process.ExitCode)" }
        }
    }
} catch {
    $failure = "$_"
    Start-Sleep -Milliseconds 500
    if ($process.HasExited) { $failure += " (alchemy-fx exited $($process.ExitCode); see stderr.txt)" }
    Step "FAILED: $failure"
} finally {
    if (-not $process.HasExited) { Step "killed $(Stop-RunTree) processes of the tree of pid $($process.Id)" }
    else {
        # Code exited, but helpers may linger (an orphaned GPU or utility process).
        $left = @(Get-CimInstance Win32_Process | Where-Object { $_.CreationDate -ge $started -and $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath) -eq [IO.Path]::GetFullPath($Exe) })
        if ($left.Count) { foreach ($q in $left) { & taskkill.exe /PID $q.ProcessId /F 2>&1 | Out-Null }; Step "killed $($left.Count) lingering processes" }
    }
    if ($socket) { $socket.Dispose() }
}

if ($workspace -and (Test-Path (Join-Path $workspace '.alchemy-probe.json'))) {
    Copy-Item (Join-Path $workspace '.alchemy-probe.json') (Join-Path $OutDir 'probe.json') -Force
}
$stderr = Join-Path $OutDir 'stderr.txt'
$denied = @(Select-String -Path $stderr -Pattern 'network denied' -ErrorAction SilentlyContinue | ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique)
$denied | Set-Content (Join-Path $OutDir 'network-denied.txt')
$result = if ($failure) { "FAIL: $failure" } else { 'PASS' }
$matched = if ($found) { "title '$($found.title)'" } else { 'title (none matched)' }
$summary = @($result, $matched, "frames $(Get-CaptureFrames)", "network denied: $($denied.Count) unique") + $steps
$summary | Set-Content (Join-Path $OutDir 'status.txt')
Write-Host $result
if ($failure) { exit 1 }
exit 0
