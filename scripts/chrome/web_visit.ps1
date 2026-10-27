# Windowed Chrome under alchemy-fx (or, with -Native, a native Windows Chrome or Edge) visiting public web pages and
# running browser benchmarks, driven only over DevTools (CDP). No OS input is injected.
#
# The Alchemy launch is run_windowed.ps1's (named pipes handed to the guest as fds 3 and 4, cloaked) with the network
# open: ALCHEMY_NETWORK=full, and the update, crash, metrics and GCM hosts refused twice, by Chrome's
# --host-resolver-rules and by Alchemy's ALCHEMY_NETWORK_DENY (which also covers GoogleUpdater, a separate process
# that ignores Chrome's flags). ALCHEMY_NETWORK_LOG=1 lists every name and address the tree reached, and
# ALCHEMY_TRACE_PROCESS=1 names each child (for the memory breakdown). A fresh anonymous profile goes under
# <Root>\Users\<User>\profiles\<run> (-Native: <OutDir>\profile, DevTools on a loopback WebSocket).
#
# Each -Url is loaded with Page.navigate in the one page target, in order. Per URL: the time from Page.navigate to
# Page.loadEventFired, the page's own navigation timing, title, text size, protocols and failed requests (Network
# events), renderer crashes (Inspector.targetCrashed), an optional -Action, and a PNG from Page.captureScreenshot.
# Over the whole run: the peak private and working-set memory of the process tree, sampled every -SampleSeconds,
# with the peak's processes by role, the window capture (ALCHEMY_CONTENTS_CAPTURE; the first three composites are
# kept as frame-1..3) and the Alchemy lines of stderr.
#
# Actions, after the load event and -Settle seconds:
#   scroll                     to the bottom in four steps, then back to the top;
#   video                      waits up to 20 s for a <video>, mutes it and plays it for -Play seconds (a play()
#                              still pending after 10 s is reported), then reads its playback quality; the media
#                              players' errors and warnings (CDP Media domain) go into the entry's "media";
#   fps                        counts requestAnimationFrame callbacks for -Play seconds (a page's frame rate);
#   html                       saves the page's markup (document.documentElement.outerHTML) as page-<n>.html;
#   reject-consent[+action]    a cookie consent wall: clicks its "Reject all" button (never anything else; the only
#                              click this script makes), waits for the page, then runs the action;
#   speedometer, jetstream, motionmark
#                              browserbench.org's Speedometer 3, JetStream 2.2 and MotionMark 1.3, started as their
#                              Start buttons do and read when they finish (no probe or screenshot; the Network
#                              domain stays off for the whole run).
# Nothing is typed or submitted.
#
# Usage: web_visit.ps1 -Exe <build>\Release\alchemy-fx.exe -OutDir <scratch dir> -Url https://example.com/[,...]
#          [-Name label,...] [-Action <action>,...] [-SwiftShader] [-Jit] [-Settle 5] [-LoadTimeout 120] [-Play 5]
#          [-BenchmarkTimeout 7200] [-NoScreenshot] [-NetLog] [-Native <chrome.exe>] [-Extra @('--flag')] [-CdpTimeout 30]
#          [-Env @{ NAME='value' }]
# Output in -OutDir: results.json (one entry per URL plus the run's totals), stdout.txt, stderr.txt, cdp.txt,
# cmdline.txt, memory.csv, network-allowed.txt, network-denied.txt, alchemy-lines.txt, shot-<n>.png, capture\
# (PNG files: never commit them), netlog.json with -NetLog. Exit code 0 when the browser exited 0.
param(
    [string]$Exe = '',   # alchemy-fx.exe; not needed with -Native
    [Parameter(Mandatory)][string]$OutDir,
    [Parameter(Mandatory)][string[]]$Url,
    [string[]]$Name = @(),
    [string[]]$Action = @(),
    [string]$Root = '',
    [string]$RuntimeDir = '',
    [string]$Program = '/Applications/Google Chrome.app',
    [string]$User = 'realweb',
    [int]$Settle = 5,
    [int]$LoadTimeout = 120,
    [int]$StartTimeout = 180,
    [int]$CloseTimeout = 120,
    [int]$Play = 5,
    [int]$BenchmarkTimeout = 7200,
    [int]$SampleSeconds = 2,
    [int]$CdpTimeout = 30,
    [int]$Width = 1280,
    [int]$Height = 900,
    [switch]$SwiftShader,
    [switch]$Jit,
    [switch]$NoScreenshot,
    [switch]$NetLog,
    [string]$Native = '',
    [string[]]$Extra = @(),
    [hashtable]$Env = @{}
)
# The checkout's artifacts by default ($PSScriptRoot is empty in parameter defaults under powershell -File).
$checkout = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Root) { $Root = Join-Path $checkout 'artifacts\alchemy-root-chrome' }
if (-not $RuntimeDir) { $RuntimeDir = Join-Path $checkout 'artifacts\alchemy-llvm-arm64' }
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

# The hosts that must never be reached: Chrome's and GoogleUpdater's update servers, crash upload, variations and
# metrics. Chrome gets them as --host-resolver-rules; every process of the tree as ALCHEMY_NETWORK_DENY.
$denyHosts = @('update.googleapis.com', '*.update.googleapis.com', 'gvt1.com', '*.gvt1.com', 'clients2.google.com',
    'clients4.google.com', 'clientservices.googleapis.com', 'dl.google.com', '*.dl.google.com', 'tools.google.com',
    'crashpad.chromium.org', '*.crashpad.chromium.org', 'chromium-crash-uploads.appspot.com', 'clients2.googleusercontent.com',
    'android.clients.google.com', 'mtalk.google.com')   # the last two: GCM device check-in and push

function Get-GuestPath([string]$HostPath) {
    $full = [IO.Path]::GetFullPath($HostPath)
    '/Volumes/' + $full.Substring(0, 1).ToUpperInvariant() + ($full.Substring(2) -replace '\\', '/')
}

if (-not $Native -and -not $Exe) { throw 'pass -Exe (alchemy-fx.exe) or -Native (a Windows chrome.exe)' }
$Root = [IO.Path]::GetFullPath($Root)
if (-not $Native -and -not (Test-Path (Join-Path $Root '.alchemy-root'))) { throw "not an Alchemy root: $Root (pass -Root; a worktree has no artifacts)" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = [IO.Path]::GetFullPath($OutDir)
$capture = Join-Path $OutDir 'capture'
Remove-Item -Recurse -Force $capture -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $capture | Out-Null
# A fresh anonymous profile per run, under the root's Users\<User>\ (the root itself is read-only).
$userDir = Join-Path $Root "Users\$User"
$runName = Split-Path -Leaf $OutDir
$profileDir = if ($Native) { Join-Path $OutDir 'profile' } else { Join-Path $userDir "profiles\$runName" }
Remove-Item -Recurse -Force $profileDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $userDir 'home') | Out-Null
$cdpLog = Join-Path $OutDir 'cdp.txt'
# One open writer: a busy page sends thousands of messages, and Add-Content opens the file for each.
$cdpWriter = New-Object IO.StreamWriter($cdpLog, $false, (New-Object Text.UTF8Encoding($false)))
$cdpWriter.AutoFlush = $true
function Log-Cdp([string]$Text) { $script:cdpWriter.WriteLine($Text) }

$pipeBase = "alchemy-cdp-$PID-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
$toChrome = New-Object IO.Pipes.NamedPipeServerStream("$pipeBase-in", [IO.Pipes.PipeDirection]::Out, 1,
    [IO.Pipes.PipeTransmissionMode]::Byte, [IO.Pipes.PipeOptions]::Asynchronous)
$fromChrome = New-Object IO.Pipes.NamedPipeServerStream("$pipeBase-out", [IO.Pipes.PipeDirection]::In, 1,
    [IO.Pipes.PipeTransmissionMode]::Byte, [IO.Pipes.PipeOptions]::Asynchronous)

$rules = ($denyHosts | ForEach-Object { "MAP $_ ~NOTFOUND" }) -join ' , '
# -Native: the same flags for a native Windows Chrome or Edge, which keeps its hardware GPU unless -SwiftShader, has
# DevTools on a loopback WebSocket (--remote-debugging-port=0) and a visible window: occlusion and background
# throttling are off so a covered window still renders.
# --no-proxy-server: without it a native Windows Chrome spends seconds per load on proxy auto-detection (WPAD) here,
# which the macOS Chrome under Alchemy does not do.
$chromeArgs = if ($Native) { @('--remote-debugging-port=0', '--no-proxy-server',
    '--disable-backgrounding-occluded-windows', '--disable-renderer-backgrounding', '--disable-background-timer-throttling') }
    else { @('--remote-debugging-pipe', '--no-sandbox') }
if ($SwiftShader) { $chromeArgs += @('--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader') }
elseif (-not $Native) { $chromeArgs += '--disable-gpu' }
if (-not $Native) { $chromeArgs += '--use-mock-keychain' }
if (-not $Jit) { $chromeArgs += '--js-flags=--jitless' }
$profileArg = if ($Native) { $profileDir } else { Get-GuestPath $profileDir }
$chromeArgs += @("--user-data-dir=$profileArg", '--no-first-run', '--no-default-browser-check',
    '--disable-background-networking', '--disable-component-update', '--disable-sync', '--disable-default-apps',
    '--metrics-recording-only', '--disable-breakpad', "--host-resolver-rules=$rules", '--no-pings',
    '--disable-domain-reliability', '--disable-client-side-phishing-detection', '--password-store=basic',
    '--autoplay-policy=no-user-gesture-required', "--window-size=$Width,$Height",
    # One --disable-features list (Chrome takes only the last); CalculateNativeWinOcclusion is for -Native's covered windows.
    '--disable-features=OptimizationHints,MediaRouter,DialMediaRouteProvider,Translate,AutofillServerCommunication,InterestFeedContentSuggestions,CalculateNativeWinOcclusion')
$netLogFile = Join-Path $OutDir 'netlog.json'
if ($NetLog) { $chromeArgs += @("--log-net-log=$(if ($Native) { $netLogFile } else { Get-GuestPath $netLogFile })", '--net-log-capture-mode=Default') }
$chromeArgs += $Extra + @('about:blank')
if ($Native) { $all = $chromeArgs; $Exe = $Native } else { $all = @('--runtime-dir', $RuntimeDir, '--root', $Root,
    '--fd', "3=r:\\.\pipe\$pipeBase-in", '--fd', "4=w:\\.\pipe\$pipeBase-out", $Program) + $chromeArgs }

$env:ALCHEMY_NETWORK = 'full'
$env:ALCHEMY_NETWORK_DENY = $denyHosts -join ','
$env:ALCHEMY_NETWORK_LOG = '1'
$env:ALCHEMY_TRACE_PROCESS = '1'   # one line per child: the memory breakdown names each process by its role
$env:ALCHEMY_GUI_CLOAK = '1'; $env:ALCHEMY_GUI_NO_ACTIVATE = '1'; $env:ALCHEMY_GUI_NO_CRASH_DIALOG = '1'
$env:ALCHEMY_CONTENTS_CAPTURE = $capture
# A CDP call that gets no answer touches this file: every Alchemy process of the tree then prints its guest
# threads (where each waits) to stderr (ALCHEMY_HANG_DUMP, src/alchemy/process.cpp).
$hangTrigger = Join-Path $OutDir 'hang-dump.trigger'
if (-not $Native) { $env:ALCHEMY_HANG_DUMP = $hangTrigger }
$env:HOME = Join-Path $userDir 'home'
foreach ($k in $Env.Keys) { Set-Item "Env:$k" $Env[$k] }
$quoted = ($all | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
Set-Content (Join-Path $OutDir 'cmdline.txt') "$Exe $quoted"

$clock = [Diagnostics.Stopwatch]::StartNew()
function Step([string]$Text) { Write-Host ('{0,7:0.0} s  {1}' -f $script:clock.Elapsed.TotalSeconds, $Text) }

$process = Start-Process -FilePath $Exe -ArgumentList $quoted -PassThru `
    -RedirectStandardOutput (Join-Path $OutDir 'stdout.txt') -RedirectStandardError (Join-Path $OutDir 'stderr.txt')
$null = $process.Handle

# ---- memory of the process tree ----
$memoryCsv = Join-Path $OutDir 'memory.csv'
Set-Content $memoryCsv 'seconds,processes,private_mb,working_set_mb,site'
$peak = @{ private = 0.0; working = 0.0; processes = 0 }
$sitePeak = @{ private = 0.0; working = 0.0 }
$peakEach = @{}   # pid -> private MB at the tree's peak
$sampleClock = [Diagnostics.Stopwatch]::StartNew()
$currentSite = ''
function Sample-Memory {
    if ($script:sampleClock.Elapsed.TotalSeconds -lt $SampleSeconds) { return }
    $script:sampleClock.Restart()
    $all = @(Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId, PrivatePageCount, WorkingSetSize -ErrorAction SilentlyContinue)
    $children = @{}
    foreach ($p in $all) { $children[[int]$p.ParentProcessId] += @($p) }
    $queue = New-Object Collections.Generic.Queue[int]; $queue.Enqueue($process.Id)
    $seen = New-Object Collections.Generic.HashSet[int]
    $private = 0.0; $working = 0.0; $count = 0
    $each = @{}   # pid -> private MB
    $self = $all | Where-Object { $_.ProcessId -eq $process.Id }
    if ($self) { $private += $self.PrivatePageCount; $working += $self.WorkingSetSize; $count++; $each[[int]$self.ProcessId] = [math]::Round($self.PrivatePageCount / 1MB) }
    while ($queue.Count) {
        $id = $queue.Dequeue()
        if (-not $seen.Add($id)) { continue }
        foreach ($c in @($children[$id])) {
            if (-not $c -or $c.ProcessId -eq $id) { continue }
            $private += $c.PrivatePageCount; $working += $c.WorkingSetSize; $count++
            $each[[int]$c.ProcessId] = [math]::Round($c.PrivatePageCount / 1MB)
            $queue.Enqueue([int]$c.ProcessId)
        }
    }
    $pm = $private / 1MB; $wm = $working / 1MB
    Add-Content $memoryCsv ('{0:0.0},{1},{2:0},{3:0},{4}' -f $script:clock.Elapsed.TotalSeconds, $count, $pm, $wm, $script:currentSite)
    if ($pm -gt $script:peak.private) { $script:peak.private = $pm; $script:peakEach = $each }
    if ($wm -gt $script:peak.working) { $script:peak.working = $wm }
    if ($count -gt $script:peak.processes) { $script:peak.processes = $count }
    if ($pm -gt $script:sitePeak.private) { $script:sitePeak.private = $pm }
    if ($wm -gt $script:sitePeak.working) { $script:sitePeak.working = $wm }
}

# ---- window capture: keep the first three composites ----
$keptFrames = 0
function Keep-Frames {
    if ($script:keptFrames -ge 3) { return }
    foreach ($file in @(Get-ChildItem -Path $capture -Filter 'window-*.txt' -ErrorAction SilentlyContinue)) {
        $text = Get-Content $file.FullName -Raw -ErrorAction SilentlyContinue
        if (-not ($text -and $text -match 'frames (\d+)')) { continue }
        $frames = [int]$Matches[1]
        while ($script:keptFrames -lt [math]::Min($frames, 3)) {
            $script:keptFrames++
            $bmp = [IO.Path]::ChangeExtension($file.FullName, '.bmp')
            Copy-Item $bmp (Join-Path $capture "frame-$($script:keptFrames).bmp") -ErrorAction SilentlyContinue
        }
    }
}

# ---- CDP ----
$encoding = New-Object Text.UTF8Encoding($false)
$pending = New-Object IO.MemoryStream
$buffer = New-Object byte[] 1048576
$readTask = $null
$socket = $null   # -Native: the DevTools WebSocket (one message per frame) instead of the NUL-separated pipe
$nextId = 0
$pipeEnded = $false
# Events nothing here reads (counted, not parsed or logged): per-chunk network data and frame bookkeeping.
$ignored = '"method":"(Network\.(dataReceived|requestWillBeSent|requestWillBeSentExtraInfo|responseReceivedExtraInfo|loadingFinished|requestServedFromCache|resourceChangedPriority)|Page\.(frame\w+|navigatedWithinDocument|lifecycleEvent|domContentEventFired|documentOpened))"'
$ignoredCount = 0
$responses = @{}
$events = New-Object Collections.Generic.List[object]
function Send-Cdp([string]$Method, [hashtable]$Params = @{}, [string]$Session = '') {
    $script:nextId++
    $message = @{ id = $script:nextId; method = $Method; params = $Params }
    if ($Session) { $message.sessionId = $Session }
    $json = $message | ConvertTo-Json -Compress -Depth 8
    Log-Cdp "-> $json"
    if ($script:socket) {
        $bytes = $encoding.GetBytes($json)
        $script:socket.SendAsync((New-Object ArraySegment[byte] -ArgumentList @(, $bytes)), [Net.WebSockets.WebSocketMessageType]::Text, $true,
            [Threading.CancellationToken]::None).Wait()
    } else {
        $bytes = $encoding.GetBytes($json + [char]0)
        $toChrome.Write($bytes, 0, $bytes.Length)
        $toChrome.Flush()
    }
    $script:nextId
}
# Reads what has arrived (waiting up to $Milliseconds for the first data), then drains whatever is already there for
# up to 500 ms: a busy page sends thousands of events, and a reply queued behind them must not wait for one read
# per call. Pipe data is NUL-separated messages; a WebSocket read is one message or part of one.
function Pump-Cdp([int]$Milliseconds) {
    Sample-Memory; Keep-Frames
    if ($script:pipeEnded) { Start-Sleep -Milliseconds $Milliseconds; return }
    $wait = $Milliseconds
    $budget = [Diagnostics.Stopwatch]::StartNew()
    while ($budget.ElapsedMilliseconds -lt 500) {
        if ($null -eq $script:readTask) {
            $script:readTask = if ($script:socket) { $script:socket.ReceiveAsync((New-Object ArraySegment[byte] -ArgumentList @(, $buffer)), [Threading.CancellationToken]::None) }
                else { $fromChrome.ReadAsync($buffer, 0, $buffer.Length) }
        }
        try { if (-not $script:readTask.Wait($wait)) { return } }
        catch {
            $reason = if ($_.Exception.InnerException) { $_.Exception.InnerException.Message } else { $_.Exception.Message }
            Log-Cdp "   (read failed: $reason)"
            $script:readTask = $null; $script:pipeEnded = $true; return
        }
        $wait = 0
        $frameEnd = $false
        if ($script:socket) {
            $received = $script:readTask.Result
            if ($received.MessageType -eq [Net.WebSockets.WebSocketMessageType]::Close) { $script:readTask = $null; $script:pipeEnded = $true; return }
            $count = $received.Count; $frameEnd = $received.EndOfMessage
        } else { $count = $script:readTask.Result }
        $script:readTask = $null
        if ($count -eq 0 -and -not $frameEnd) { $script:pipeEnded = $true; return }
        $offset = 0
        while ($offset -lt $count -or $frameEnd) {
            $nul = if ($script:socket -or $offset -ge $count) { -1 } else { [Array]::IndexOf($buffer, [byte]0, $offset, $count - $offset) }
            $stop = if ($nul -ge 0) { $nul } else { $count }
            if ($stop -gt $offset) { $pending.Write($buffer, $offset, $stop - $offset) }
            $offset = if ($nul -ge 0) { $nul + 1 } else { $count }
            if ($nul -lt 0 -and -not $frameEnd) { break }   # the rest of this message comes with the next read
            $frameEnd = $false
            $text = $encoding.GetString($pending.ToArray())
            $pending.SetLength(0)
            $head = if ($text.Length -lt 160) { $text } else { $text.Substring(0, 160) }   # an event starts with its method
            if ($head -match $script:ignored) { $script:ignoredCount++; continue }
            $short = if ($text.Length -gt 400) { $text.Substring(0, 400) + "... ($($text.Length) chars)" } else { $text }
            Log-Cdp "<- $short"
            try { $message = $text | ConvertFrom-Json } catch { Log-Cdp '   (not JSON)'; continue }
            if ($message.PSObject.Properties['id']) { $script:responses[[int]$message.id] = $message }
            elseif ($message.PSObject.Properties['method']) { $script:events.Add($message) }
        }
    }
}
# Sends a command and waits for its answer; throws on a protocol error or the deadline.
function Invoke-Cdp([string]$Method, [hashtable]$Params = @{}, [string]$Session = '', [int]$TimeoutSeconds = $CdpTimeout) {
    $id = Send-Cdp $Method $Params $Session
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while (-not $script:responses.ContainsKey($id)) {
        if ($deadline.Elapsed.TotalSeconds -gt $TimeoutSeconds) {
            if (-not $Native) {   # ALCHEMY_HANG_DUMP: the processes print where they wait, then the run goes on
                Set-Content $hangTrigger (Get-Date -Format o)
                Start-Sleep -Seconds 3
            }
            throw "$Method had no answer within $TimeoutSeconds s"
        }
        if ($process.HasExited) { throw "Chrome exited ($($process.ExitCode)) during $Method" }
        if ($script:pipeEnded) { throw "the DevTools pipe ended during $Method" }
        Pump-Cdp 200
    }
    $answer = $script:responses[$id]; $script:responses.Remove($id)
    if ($answer.PSObject.Properties['error']) { throw "$Method failed: $($answer.error.message)" }
    $answer.result
}
function Evaluate([string]$Expression, [string]$Session, [int]$TimeoutSeconds = $CdpTimeout) {
    $result = Invoke-Cdp 'Runtime.evaluate' @{ expression = $Expression; returnByValue = $true; awaitPromise = $true } $Session $TimeoutSeconds
    if ($result.PSObject.Properties['exceptionDetails']) { return "exception: $($result.exceptionDetails.text)" }
    if ($result.result.PSObject.Properties['value']) { return $result.result.value }
    $null
}

# What the page reports about itself, as one JSON string.
$probe = @'
JSON.stringify((() => {
  const n = performance.getEntriesByType('navigation')[0] || {};
  const paints = {}; for (const p of performance.getEntriesByType('paint')) paints[p.name] = Math.round(p.startTime);
  const resources = performance.getEntriesByType('resource');
  const protocols = {}; for (const r of resources) { const k = r.nextHopProtocol || '?'; protocols[k] = (protocols[k] || 0) + 1; }
  const images = [...document.images]; const broken = images.filter(i => i.complete && i.naturalWidth === 0 && i.src).length;
  let fonts = []; try { for (const f of document.fonts) if (f.status === 'loaded') fonts.push(f.family); } catch (e) {}
  return { title: document.title, url: location.href, ready: document.readyState,
    protocol: n.nextHopProtocol || '', ttfb: Math.round(n.responseStart || 0), dcl: Math.round(n.domContentLoadedEventEnd || 0),
    load: Math.round(n.loadEventEnd || 0), fcp: paints['first-contentful-paint'] || null,
    transfer_kb: Math.round((n.transferSize || 0) / 1024 + resources.reduce((a, r) => a + (r.transferSize || 0), 0) / 1024),
    resources: resources.length, protocols, text: (document.body ? document.body.innerText.length : 0),
    images: images.length, broken_images: broken, fonts_loaded: [...new Set(fonts)].slice(0, 12), fonts_status: document.fonts ? document.fonts.status : '',
    visibility: document.visibilityState, focus: document.hasFocus(), dpr: window.devicePixelRatio, inner: innerWidth + 'x' + innerHeight,
    scroll_height: document.documentElement.scrollHeight, webgl: !!document.createElement('canvas').getContext('webgl') };
})())
'@
# Rejects a cookie consent wall: clicks the (first) button labelled "Reject all" (or its German or French form). Never
# accepts anything; answers what it did.
$rejectScript = @'
(() => { const labels = /^\s*(reject all|alle ablehnen|tout refuser)\s*$/i;
  const buttons = [...document.querySelectorAll('button, input[type=submit], [role=button]')].filter(b => labels.test(b.textContent || b.value || ''));
  if (!buttons.length) return 'no reject button on ' + location.host;   // the page may repeat it (top and bottom)
  setTimeout(() => buttons[0].click(), 50); return 'rejected on ' + location.host; })()   // the click navigates: answer first
'@
$scrollScript = @'
(async () => { const h = document.documentElement.scrollHeight; for (let i = 1; i <= 4; i++) { window.scrollTo(0, h * i / 4); await new Promise(r => setTimeout(r, 1000)); }
  const y = window.scrollY; window.scrollTo(0, 0); await new Promise(r => setTimeout(r, 500)); return 'scrolled to ' + Math.round(y) + ' of ' + h; })()
'@
$fpsScript = @'
(async () => { let n = 0; let run = true; const t0 = performance.now();
  const tick = () => { n++; if (run) requestAnimationFrame(tick); }; requestAnimationFrame(tick);
  await new Promise(r => setTimeout(r, PLAY_MS)); run = false; const s = (performance.now() - t0) / 1000;
  return JSON.stringify({ frames: n, seconds: +s.toFixed(2), fps: +(n / s).toFixed(1), canvases: document.querySelectorAll('canvas').length }); })()
'@
$videoScript = @'
(async () => { let v = document.querySelector('video');
  for (let i = 0; !v && i < 40; i++) { await new Promise(r => setTimeout(r, 500)); v = document.querySelector('video'); }
  if (!v) return JSON.stringify({ video: 'none' });
  v.muted = true; let error = '';
  try { await Promise.race([v.play(), new Promise((_, no) => setTimeout(() => no(new Error('play() pending after 10 s')), 10000))]); }
  catch (e) { error = String(e); }
  const t0 = v.currentTime; await new Promise(r => setTimeout(r, PLAY_MS));
  const q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : {};
  return JSON.stringify({ video: 'found', error, played_s: +(v.currentTime - t0).toFixed(2), paused: v.paused, ready: v.readyState,
    width: v.videoWidth, height: v.videoHeight, frames: q.totalVideoFrames, dropped: q.droppedVideoFrames, src: (v.currentSrc || '').slice(0, 40) }); })()
'@

# Benchmarks (browserbench.org), each as three page scripts: ready (the page can start it), start (as its Start
# button does) and result (JSON). A running benchmark leaves the page's main thread little time for DevTools, so
# completion is not polled: `start` also installs a watcher (a MutationObserver, or the benchmark's own completion
# callback) that sets the page title to the marker below, and the driver waits for the browser's
# Target.targetInfoChanged event with that title.
$doneTitle = 'ALCHEMY-BENCHMARK-DONE'
# The page may set its own title after the marker (the browser reports only the last of quick changes), so the
# marker is set again whenever the title changes back.
$watch = "const alchemyMark = () => { if (document.title !== '$doneTitle') document.title = '$doneTitle'; new MutationObserver(() => { if (document.title !== '$doneTitle') document.title = '$doneTitle'; }).observe(document.head || document.documentElement, { subtree: true, childList: true, characterData: true }); };"
# While a benchmark runs, the page writes its progress into the title every 2 s ("ALCHEMY-PROGRESS <text>", the
# benchmark's `progress` expression), so a run whose renderer dies still says how far it got.
$progressPrefix = 'ALCHEMY-PROGRESS '
$benchmarks = @{
    # Speedometer 3: the score and its 95 % interval, and each iteration's score.
    speedometer = @{
        progress = "benchmarkClient._finishedTestCount + ' of ' + benchmarkClient.stepCount + ' tests, ' + (benchmarkClient._measuredValuesList || []).length + ' iterations done'"
        title = '*Results*'   # its own title when it shows the results
        ready = "!!window.benchmarkClient && document.readyState === 'complete'"
        start = "(() => { $watch const done = () => !!(document.querySelector('#result-number') || {}).textContent && !benchmarkClient._isRunning; new MutationObserver(() => { if (done()) alchemyMark(); }).observe(document.documentElement, { subtree: true, childList: true, characterData: true, attributes: true }); benchmarkClient.start(); return 'started'; })()"
        result = "JSON.stringify({ score: document.querySelector('#result-number').textContent.trim(), delta: ((document.querySelector('#confidence-number') || {}).textContent || '').trim(), iterations: (benchmarkClient._measuredValuesList || []).map(v => +(+v.score).toFixed(2)) })"
    }
    # JetStream 2.2: started once its resources are loaded (the status offers "Start Test"); its start() resolves at the end.
    jetstream = @{
        progress = "document.querySelectorAll('.benchmark-done').length + ' of ' + JetStream.benchmarks.length + ' done, running ' + Array.from(document.querySelectorAll('.benchmark-running')).map(e => e.id).join(' ')"
        ready = "!!window.JetStream && /start/i.test((document.getElementById('status') || {}).textContent || '')"
        start = "(() => { $watch JetStream.start().then(alchemyMark, e => { window.__alchemyError = String(e); alchemyMark(); }); return 'started'; })()"
        result = "(() => { const scores = {}; for (const b of JetStream.benchmarks) { try { scores[b.name] = b.score === undefined ? null : +(+b.score).toFixed(3); } catch (e) { scores[b.name] = 'error'; } } return JSON.stringify({ score: ((document.querySelector('#result-summary .score') || {}).textContent || '').trim(), error: window.__alchemyError || '', scores }); })()"
    }
    # MotionMark 1.3: started once frame-rate detection has enabled its button; done when it shows the results.
    motionmark = @{
        ready = "!!window.benchmarkController && !!document.getElementById('start-button') && !document.getElementById('start-button').disabled"
        start = "(() => { $watch const original = benchmarkController.showResults; benchmarkController.showResults = function () { original.call(this); alchemyMark(); }; benchmarkController.startBenchmark(); return 'started'; })()"
        result = "(() => { const r = benchmarkRunnerClient.results; const tests = {}; try { for (const suite of r.data[0].testsResults || []) for (const t of suite.testsResults || []) tests[t.name] = +(+t.score).toFixed(2); } catch (e) {} return JSON.stringify({ score: +r.score.toFixed(2), lower: +r.scoreLowerBound.toFixed(2), upper: +r.scoreUpperBound.toFixed(2), text: (document.querySelector('#results .score') || {}).textContent || '', size: document.body.className, tests }); })()"
    }
}
function Run-Benchmark([hashtable]$Bench, [string]$Session, [string]$TargetId, [string]$Label) {
    $wait = [Diagnostics.Stopwatch]::StartNew()
    while ((Evaluate $Bench.ready $Session ([math]::Max(60, $CdpTimeout))) -ne $true) {   # an exception answers a string: not ready
        if ($wait.Elapsed.TotalSeconds -gt 600) { throw 'the benchmark page did not become ready within 600 s' }
        Start-Sleep -Seconds 2
    }
    $events.Clear()
    $started = Evaluate $Bench.start $Session ([math]::Max(60, $CdpTimeout))
    if ($started -ne 'started') { throw "the benchmark did not start: $started" }
    if ($Bench.ContainsKey('progress')) {
        $null = Evaluate "(() => { setInterval(() => { try { if (document.title !== '$doneTitle') document.title = '$progressPrefix' + ($($Bench.progress)); } catch (e) { } }, 2000); return true; })()" $Session ([math]::Max(60, $CdpTimeout))
    }
    $progress = ''
    $clock = [Diagnostics.Stopwatch]::StartNew()
    Step "[$Label] started"
    # Page events (every iframe navigation of Speedometer) only cost the page and the driver time from here on.
    try { $null = Invoke-Cdp 'Page.disable' @{} $Session 30 } catch { Log-Cdp "   (Page.disable: $_)" }
    $finished = $false
    $titleClock = [Diagnostics.Stopwatch]::StartNew()
    $isDone = { param($info) $info.targetId -eq $TargetId -and ($info.title -eq $doneTitle -or ($Bench.ContainsKey('title') -and $info.title -like $Bench.title)) }
    while (-not $finished) {
        # The browser answers Target.getTargets itself (the renderer may be too busy for DevTools), every 10 s.
        # A native browser's window title says it too, without DevTools (its replies were sometimes lost here).
        if ($titleClock.Elapsed.TotalSeconds -ge 10) {
            $titleClock.Restart()
            try {
                foreach ($info in @((Invoke-Cdp 'Target.getTargets' @{} '' 20).targetInfos)) {
                    if (& $isDone $info) { $finished = $true }
                    if ($info.targetId -eq $TargetId -and $info.title -like "$progressPrefix*") { $progress = $info.title.Substring($progressPrefix.Length) }
                }
            }
            catch { Log-Cdp "   (Target.getTargets: $_)" }
            if ($Native) {
                $process.Refresh()
                $windowTitle = $process.MainWindowTitle -replace ' - (Google Chrome|Microsoft.*Edge)$', ''
                if (& $isDone @{ targetId = $TargetId; title = $windowTitle }) { $finished = $true }
            }
            if ($finished) { break }
        }
        if ($clock.Elapsed.TotalSeconds -gt $BenchmarkTimeout) { throw "the benchmark did not finish within $BenchmarkTimeout s" }
        if ($process.HasExited) { throw "the browser exited ($($process.ExitCode)) during the benchmark" }
        if ($pipeEnded) { throw 'the DevTools connection ended during the benchmark (see cdp.txt)' }
        Pump-Cdp 500
        foreach ($e in $events) {
            if ($e.method -eq 'Target.targetInfoChanged' -and (& $isDone $e.params.targetInfo)) { $finished = $true }
            if ($e.method -eq 'Target.targetInfoChanged' -and $e.params.targetInfo.targetId -eq $TargetId -and $e.params.targetInfo.title -like "$progressPrefix*") {
                $progress = $e.params.targetInfo.title.Substring($progressPrefix.Length)
            }
            if ($e.method -eq 'Inspector.targetCrashed') {
                throw "the renderer crashed during the benchmark after $([math]::Round($clock.Elapsed.TotalSeconds)) s (last progress: $(if ($progress) { $progress } else { 'none seen' }))"
            }
        }
        $events.Clear()
    }
    $seconds = [math]::Round($clock.Elapsed.TotalSeconds)
    # The result through a fresh session: the benchmark's own session sometimes never answered here (a native
    # JetStream page answered a new client at once, but not the session that started it).
    $reader = $Session
    try { $reader = (Invoke-Cdp 'Target.attachToTarget' @{ targetId = $TargetId; flatten = $true } '' 60).sessionId } catch { Log-Cdp "   (attach for the result: $_)" }
    $raw = Evaluate $Bench.result $reader 300
    try { $value = $raw | ConvertFrom-Json } catch { $value = $raw }
    @{ result = $value; seconds = $seconds }
}

$results = New-Object Collections.Generic.List[object]
$failure = $null
try {
    if ($Native) {
        $portFile = Join-Path $profileDir 'DevToolsActivePort'
        while (-not (Test-Path $portFile) -or @(Get-Content $portFile -ErrorAction SilentlyContinue).Count -lt 2) {
            if ($process.HasExited) { throw "$Native exited ($($process.ExitCode)) before DevTools was listening" }
            if ($clock.Elapsed.TotalSeconds -gt 60) { throw 'no DevToolsActivePort within 60 s' }
            Start-Sleep -Milliseconds 100
        }
        $port, $path = @(Get-Content $portFile)[0, 1]
        $socket = New-Object Net.WebSockets.ClientWebSocket
        $socket.Options.KeepAliveInterval = [TimeSpan]::Zero
        $socket.ConnectAsync([Uri]"ws://127.0.0.1:$port$path", [Threading.CancellationToken]::None).Wait()
        Step "DevTools WebSocket connected (port $port)"
    } else {
        $connectIn = $toChrome.WaitForConnectionAsync()
        $connectOut = $fromChrome.WaitForConnectionAsync()
        while (-not ($connectIn.IsCompleted -and $connectOut.IsCompleted)) {
            if ($process.HasExited) { throw "alchemy-fx exited ($($process.ExitCode)) before it opened the DevTools pipes" }
            if ($clock.Elapsed.TotalSeconds -gt 60) { throw 'alchemy-fx did not open the DevTools pipes within 60 s' }
            Start-Sleep -Milliseconds 100
        }
        Step 'DevTools pipes connected'
    }
    $null = Send-Cdp 'Target.setDiscoverTargets' @{ discover = $true }
    $page = $null
    while (-not $page) {
        if ($clock.Elapsed.TotalSeconds -gt $StartTimeout) { throw "no page target within $StartTimeout s" }
        if ($process.HasExited) { throw "Chrome exited ($($process.ExitCode)) before its page target appeared" }
        # A browser still starting answers late (a native Chrome on a fresh profile: well over 10 s under
        # load), so this first request may take the rest of -StartTimeout, not just -CdpTimeout.
        $targets = Invoke-Cdp 'Target.getTargets' @{} '' ([math]::Max($CdpTimeout, [int]($StartTimeout - $clock.Elapsed.TotalSeconds)))
        $page = @($targets.targetInfos | Where-Object { $_.type -eq 'page' }) | Select-Object -First 1
        if (-not $page) { Pump-Cdp 1000 }
    }
    $startup = $clock.Elapsed.TotalSeconds
    Step "page target $($page.targetId) ($($page.url))"
    $session = (Invoke-Cdp 'Target.attachToTarget' @{ targetId = $page.targetId; flatten = $true }).sessionId
    # Benchmark runs leave the Network domain off (its events cost the page time and fill the protocol log).
    $benchmarkRun = @($Action | Where-Object { $benchmarks.ContainsKey($_) }).Count -gt 0
    foreach ($domain in @('Page.enable', 'Inspector.enable') + $(if ($benchmarkRun) { @() } else { @('Network.enable') })) { $null = Invoke-Cdp $domain @{} $session }
    # Video actions also record the media players' errors and warnings (Media domain).
    if (@($Action | Where-Object { $_ -match 'video' }).Count) { $null = Invoke-Cdp 'Media.enable' @{} $session }

    for ($index = 0; $index -lt $Url.Count; $index++) {
        $target = $Url[$index]
        $label = if ($index -lt $Name.Count -and $Name[$index]) { $Name[$index] } else { ([Uri]$target).Host }
        $act = if ($index -lt $Action.Count -and $Action[$index]) { $Action[$index] } else { 'none' }
        $currentSite = $label
        $sitePeak.private = 0.0; $sitePeak.working = 0.0
        $entry = [ordered]@{ name = $label; url = $target; action = $act; loaded = $false; load_event_s = $null; crashes = 0;
            failed_requests = 0; failed_samples = @(); responses = 0; response_protocols = @{}; page = $null; action_result = $null; error = $null;
            media = @() }
        $events.Clear()
        Step "[$label] Page.navigate $target"
        $navClock = [Diagnostics.Stopwatch]::StartNew()
        try {
            $nav = Invoke-Cdp 'Page.navigate' @{ url = $target } $session $LoadTimeout
            if ($nav.PSObject.Properties['errorText'] -and $nav.errorText) { $entry.error = "navigate: $($nav.errorText)" }
            $loaded = $false
            while (-not $loaded -and -not $entry.error) {
                if ($navClock.Elapsed.TotalSeconds -gt $LoadTimeout) { $entry.error = "no load event within $LoadTimeout s"; break }
                if ($process.HasExited) { throw "Chrome exited ($($process.ExitCode))" }
                Pump-Cdp 250
                foreach ($e in $events) {
                    if ($e.method -eq 'Page.loadEventFired') { $loaded = $true }
                    if ($e.method -eq 'Inspector.targetCrashed') { $entry.error = 'renderer crashed before the load event' }
                }
            }
            if ($loaded) { $entry.loaded = $true; $entry.load_event_s = [math]::Round($navClock.Elapsed.TotalSeconds, 2) }
            Step "[$label] load event: $($entry.load_event_s) s $(if ($entry.error) { $entry.error })"
            $settleClock = [Diagnostics.Stopwatch]::StartNew()
            while ($settleClock.Elapsed.TotalSeconds -lt $Settle) { Pump-Cdp 250 }
            if ($act -like 'reject-consent*' -and -not $entry.error) {
                # A consent wall in front of the page: reject all optional cookies (never accept), then wait for the page.
                $act = ($act -replace '^reject-consent\+?', ''); if (-not $act) { $act = 'none' }
                $events.Clear()
                $entry.consent = Evaluate $rejectScript $session ([math]::Max(30, $CdpTimeout))
                Step "[$label] consent: $($entry.consent)"
                if ($entry.consent -like 'rejected*') {
                    $consentClock = [Diagnostics.Stopwatch]::StartNew(); $reloaded = $false
                    while (-not $reloaded -and $consentClock.Elapsed.TotalSeconds -lt $LoadTimeout) {
                        Pump-Cdp 250
                        foreach ($e in $events) { if ($e.method -eq 'Page.loadEventFired') { $reloaded = $true } }
                    }
                    $entry.consent_load_s = if ($reloaded) { [math]::Round($consentClock.Elapsed.TotalSeconds, 2) } else { $null }
                    $settleClock = [Diagnostics.Stopwatch]::StartNew()
                    while ($settleClock.Elapsed.TotalSeconds -lt $Settle) { Pump-Cdp 250 }
                }
            }
            if ($benchmarks.ContainsKey($act) -and -not $entry.error) {
                # A benchmark page: no probe or screenshot (they would compete with it), only its result.
                $run = Run-Benchmark $benchmarks[$act] $session $page.targetId $label
                $entry.action_result = $run.result
                $entry.benchmark_wall_s = $run.seconds
                Step "[$label] $act`: $($run.result | ConvertTo-Json -Compress -Depth 4) in $($run.seconds) s"
            } elseif (-not ($entry.error -like 'renderer crashed*')) {
                $raw = Evaluate $probe $session ([math]::Max(30, $CdpTimeout))
                if ($raw -and $raw -notlike 'exception:*') { $entry.page = $raw | ConvertFrom-Json } else { $entry.page = $raw }
                if ($act -eq 'scroll') { $entry.action_result = Evaluate $scrollScript $session ([math]::Max(60, $CdpTimeout)) }
                elseif ($act -eq 'html') {
                    $markup = Evaluate 'document.documentElement.outerHTML' $session ([math]::Max(60, $CdpTimeout))
                    [IO.File]::WriteAllText((Join-Path $OutDir "page-$($index + 1).html"), [string]$markup)
                    $entry.action_result = "saved $(([string]$markup).Length) characters"
                }
                elseif ($act -eq 'fps') { $entry.action_result = Evaluate ($fpsScript -replace 'PLAY_MS', "$($Play * 1000)") $session ($Play + 60) }
                elseif ($act -eq 'video') { $entry.action_result = Evaluate ($videoScript -replace 'PLAY_MS', "$($Play * 1000)") $session ($Play + 90) }
                if (-not $NoScreenshot) {
                    $shot = Invoke-Cdp 'Page.captureScreenshot' @{ format = 'png' } $session 60
                    [IO.File]::WriteAllBytes((Join-Path $OutDir "shot-$($index + 1)-$($label -replace '[^\w.-]', '_').png"), [Convert]::FromBase64String($shot.data))
                }
            }
        } catch {
            $entry.error = "$_"
            Step "[$label] error: $_"
            if ($process.HasExited) { throw }
        }
        $failedSamples = New-Object Collections.Generic.List[string]
        $media = New-Object Collections.Generic.List[string]
        $protocols = @{}
        foreach ($e in $events) {
            switch ($e.method) {
                'Inspector.targetCrashed' { $entry.crashes++ }
                'Media.playerErrorsRaised' {
                    # PlayerError: errorType, code, stack (file, line), data.
                    foreach ($m in $e.params.errors) {
                        if ($media.Count -ge 40) { break }
                        $where = @($m.stack | Where-Object { $_ } | ForEach-Object { "$($_.file):$($_.line)" }) -join ' < '
                        $data = if ($m.PSObject.Properties['data']) { $m.data | ConvertTo-Json -Compress -Depth 4 } else { '' }
                        $media.Add("error $($m.errorType) $($m.code) $data $where".Trim())
                    }
                }
                'Media.playerMessagesLogged' {
                    foreach ($m in $e.params.messages) { if ($m.level -in @('error', 'warning') -and $media.Count -lt 40) { $media.Add("$($m.level) $($m.message)") } }
                }
                'Network.loadingFailed' {
                    if ($e.params.PSObject.Properties['canceled'] -and $e.params.canceled) { break }
                    $entry.failed_requests++
                    if ($failedSamples.Count -lt 8) { $failedSamples.Add("$($e.params.errorText) $(if ($e.params.PSObject.Properties['blockedReason']) { $e.params.blockedReason })".Trim()) }
                }
                'Network.responseReceived' {
                    $entry.responses++
                    $p = if ($e.params.response.PSObject.Properties['protocol']) { $e.params.response.protocol } else { '?' }
                    $protocols[$p] = 1 + $(if ($protocols.ContainsKey($p)) { $protocols[$p] } else { 0 })
                }
            }
        }
        $entry.failed_samples = @($failedSamples | Sort-Object -Unique)
        $entry.media = @($media)
        $entry.response_protocols = $protocols
        $entry.peak_private_mb = [math]::Round($sitePeak.private)
        $entry.peak_working_set_mb = [math]::Round($sitePeak.working)
        Step "[$label] done: crashes $($entry.crashes), failed requests $($entry.failed_requests), peak private $($entry.peak_private_mb) MB"
        $results.Add($entry)
        if ($entry.crashes -and $index + 1 -lt $Url.Count) {
            # A crashed renderer leaves the target in the sad-tab state; reload it so the next URL gets a renderer.
            try { $null = Invoke-Cdp 'Page.navigate' @{ url = 'about:blank' } $session 60 } catch { }
        }
    }
    $currentSite = 'close'
    $null = Send-Cdp 'Browser.close'
    Step 'Browser.close sent'
    $closing = [Diagnostics.Stopwatch]::StartNew()
    while (-not $process.HasExited -and $closing.Elapsed.TotalSeconds -lt $CloseTimeout) { Pump-Cdp 500 }
    if (-not $process.HasExited) { throw "Chrome did not exit within $CloseTimeout s of Browser.close" }
    Step "exit $($process.ExitCode)"
    if ($process.ExitCode -ne 0) { $failure = "exit code $($process.ExitCode)" }
} catch {
    $failure = "$_"
    Start-Sleep -Milliseconds 500
    if ($process.HasExited) { $failure += " (alchemy-fx exited $($process.ExitCode); see stderr.txt)" }
    Step "FAILED: $failure"
} finally {
    if (-not $process.HasExited) {
        & taskkill.exe /PID $process.Id /T /F 2>&1 | Out-Null
        $process.WaitForExit(10000) | Out-Null
        Step "killed the tree of pid $($process.Id)"
    }
    $toChrome.Dispose(); $fromChrome.Dispose()
    if ($socket) { $socket.Dispose() }
    $cdpWriter.Dispose()
}

# PNG copies of the kept frames and the last capture (BMPs are large).
try {
    Add-Type -AssemblyName System.Drawing
    foreach ($bmp in @(Get-ChildItem $capture -Filter '*.bmp')) {
        $image = [Drawing.Image]::FromFile($bmp.FullName)
        try { $image.Save([IO.Path]::ChangeExtension($bmp.FullName, '.png'), [Drawing.Imaging.ImageFormat]::Png) } finally { $image.Dispose() }
        Remove-Item $bmp.FullName
    }
} catch { Step "capture conversion: $_" }

$stderr = Join-Path $OutDir 'stderr.txt'
$lines = @(Get-Content $stderr -ErrorAction SilentlyContinue)
$allowedLines = @($lines | Where-Object { $_ -like 'alchemy: network allowed:*' } | Sort-Object -Unique)
$deniedLines = @($lines | Where-Object { $_ -like '*network denied*' } | ForEach-Object { $_.Trim() } | Sort-Object -Unique)
$allowedLines | Set-Content (Join-Path $OutDir 'network-allowed.txt')
$deniedLines | Set-Content (Join-Path $OutDir 'network-denied.txt')
# Alchemy's own lines other than the network log, deduplicated with their counts.
$alchemyLines = @($lines | Where-Object { $_ -match 'alchemy' -and $_ -notlike 'alchemy: network allowed:*' -and $_ -notlike '[[]alchemy process]*' } |
    ForEach-Object { ($_ -replace '0x[0-9a-fA-F]+', '0x?' -replace '\b\d{3,}\b', 'N').Trim() } | Group-Object | Sort-Object Count -Descending |
    ForEach-Object { '{0,6}  {1}' -f $_.Count, $_.Name })
$alchemyLines | Set-Content -Encoding utf8 (Join-Path $OutDir 'alchemy-lines.txt')
# The peak's processes by role: Alchemy children from ALCHEMY_TRACE_PROCESS ("child <pid> of <pid>: <path> ... --type=X"),
# the root as the browser.
$roles = @{ ([int]$process.Id) = 'browser (root)' }
foreach ($line in $lines) {
    if ($line -match '^\[alchemy process\] child (\d+) of \d+: (.*)$') {
        $argv = $Matches[2]; $pidValue = [int]$Matches[1]
        $role = if ($argv -match '--type=([a-z-]+)') { $Matches[1] } else { Split-Path -Leaf (($argv -split " '")[0]) }
        if ($argv -match '--utility-sub-type=([A-Za-z_.]+)') { $role += ':' + ($Matches[1] -replace '\.mojom\.\w+$', '') }
        $roles[$pidValue] = $role
    }
}
$peakByRole = [ordered]@{}
foreach ($entry in ($peakEach.GetEnumerator() | Sort-Object Value -Descending)) {
    $role = if ($roles.ContainsKey([int]$entry.Key)) { $roles[[int]$entry.Key] } else { 'other' }
    $peakByRole["$role ($($entry.Key))"] = $entry.Value
}
$hit = @($allowedLines | Where-Object {
    $line = $_; @($denyHosts | Where-Object { $pattern = $_; $line -like "* $pattern" -or $line -like "*://$pattern" }).Count })
$summary = [ordered]@{
    result = if ($failure) { "FAIL: $failure" } else { 'PASS: Browser.close, exit 0' }
    exit_code = if ($process.HasExited) { $process.ExitCode } else { $null }
    startup_s = if (Get-Variable startup -ErrorAction SilentlyContinue) { [math]::Round($startup, 1) } else { $null }
    wall_s = [math]::Round($clock.Elapsed.TotalSeconds, 1)
    peak_private_mb = [math]::Round($peak.private); peak_working_set_mb = [math]::Round($peak.working); peak_processes = $peak.processes
    peak_private_by_process_mb = $peakByRole
    frames_kept = $keptFrames
    network_allowed = $allowedLines.Count; network_denied = $deniedLines.Count; deny_list_hosts_reached = $hit
    alchemy_lines = $alchemyLines.Count
    sites = $results
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path $OutDir 'results.json')
Write-Host $summary.result
if ($hit.Count) { Write-Host "DENY-LIST HOST REACHED: $($hit -join '; ')" }
if ($failure) { exit 1 }
exit 0
