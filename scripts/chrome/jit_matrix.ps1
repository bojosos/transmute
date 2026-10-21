# Chrome JIT matrix: pages x {jitless, jit} x runs, interleaved, with fx counters (ALCHEMY_FX_STATS=1).
# Wraps the stage 3 run script (run_chrome_s3.ps1: strict headless --dump-dom, cloaked, ALCHEMY_NETWORK=loopback).
# It is not in the repository: pass -RunScript, or set ALCHEMY_CHROME_RUN_SCRIPT.
#
# Usage:
#   jit_matrix.ps1 -OutRoot <scratch dir> -Exe <build>\Release\alchemy-fx.exe [-Runs 3] [-Pages probe,js_bench,wasm]
#     [-Modes jitless,jit] [-Label m1] [-MaxLoad 30] [-Force] [-Timeout 600] [-BenchScale 1] [-RunScript <run_chrome_s3.ps1>]
#     [-PagesDir <dir with js_bench.html, wasm.html>] [-Group j3] [-Env @{ ALCHEMY_PERF='1' }] [-StatsPeriod 5]
#
# fx counters: processes that Chrome terminates (renderers at browser exit) never print their exit statistics,
# so -StatsPeriod (default 5) sets ALCHEMY_FX_STATS_PERIOD: every process prints a cumulative
# "alchemy-fx invalidation (pid N, T s): ..." line each period. SMC faults, icache flushes, translations and
# re-translations are the sum over processes of each pid's last periodic value (up to one period short);
# without periodic lines they come from the exit lines. "invalidated" (regions) and interrupts exist only in
# the exit lines, so they miss terminated processes. -StatsPeriod 0 turns the periodic lines off.
#
# Refuses to start while the CPU load (average of a few samples) is above -MaxLoad percent, unless -Force.
# Each run records the load and free commit before it starts. Order: for each round, every page, the modes
# rotated per round, so no mode always runs first. One Chrome at a time.
# Output: <OutRoot>\matrix-<Label>.csv (one row per run), <OutRoot>\matrix-<Label>.md (per-run and per-cell
# tables, also printed), runs under <OutRoot>\runs\<Label>-<page>-<mode>-<n>\ (the run script's layout).
param([Parameter(Mandatory)][string]$OutRoot, [Parameter(Mandatory)][string]$Exe, [int]$Runs = 3,
  [string[]]$Pages = @('probe', 'js_bench', 'wasm'), [string[]]$Modes = @('jitless', 'jit'),
  [string]$Label = ('m' + (Get-Date -Format 'MMdd-HHmm')), [int]$MaxLoad = 30, [switch]$Force, [int]$Timeout = 600,
  [double]$BenchScale = 1, [string]$Group = 'j3', [hashtable]$Env = @{}, [int]$StatsPeriod = 5,
  [string]$RunScript = $env:ALCHEMY_CHROME_RUN_SCRIPT,
  [string]$PagesDir = '',
  [string]$ProbeUrl = 'file:///Volumes/C/dev/transmute/artifacts/alchemy-root-chrome/probe/test.html')
$ErrorActionPreference = 'Stop'
if (-not $PagesDir) { $PagesDir = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'pages' }   # this script's pages directory
$Pages = @($Pages | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Modes = @($Modes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($m in $Modes) { if ($m -notin @('jitless', 'jit')) { throw "unknown mode $m" } }
if (-not (Test-Path $Exe)) { throw "no $Exe" }
if (-not (Test-Path $RunScript)) { throw "no run script $RunScript" }
New-Item -ItemType Directory -Force $OutRoot | Out-Null
$Env = @{} + $Env
if ($StatsPeriod -gt 0 -and -not $Env.ContainsKey('ALCHEMY_FX_STATS_PERIOD')) { $Env.ALCHEMY_FX_STATS_PERIOD = "$StatsPeriod" }

function Get-Load {
  $samples = foreach ($i in 1..3) { (Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average; Start-Sleep -Milliseconds 700 }
  [math]::Round(($samples | Measure-Object -Average).Average)
}
function Get-CommitFreeGB { [math]::Round((Get-CimInstance Win32_OperatingSystem).FreeVirtualMemory / 1MB, 1) }
# C:\dev\x -> file:///Volumes/C/dev/x (Alchemy's guest view of host drives).
function Get-GuestUrl([string]$path) {
  $full = [IO.Path]::GetFullPath($path)
  'file:///Volumes/' + $full.Substring(0, 1).ToUpper() + ($full.Substring(2) -replace '\\', '/')
}
function Get-PageUrl([string]$page) {
  if ($page -eq 'probe') { return $ProbeUrl }
  $file = Join-Path $PagesDir "$page.html"
  if (-not (Test-Path $file)) { throw "no page $file" }
  $url = Get-GuestUrl $file
  if ($page -eq 'js_bench' -and $BenchScale -ne 1) { $url += "?scale=$BenchScale" }
  $url
}
function Sum-Matches([string[]]$lines, [string]$pattern) {
  $total = [double]0; $max = [double]0; $count = 0
  foreach ($line in $lines) { foreach ($m in [regex]::Matches($line, $pattern)) { $v = [double]$m.Groups[1].Value; $total += $v; if ($v -gt $max) { $max = $v }; $count++ } }
  @{ total = $total; max = $max; count = $count }
}
# Periodic "alchemy-fx invalidation (pid N, T s): ..." lines are cumulative: the last value per pid, summed.
function Sum-Periodic([string[]]$lines, [string]$pattern) {
  $last = @{}
  foreach ($line in $lines) {
    if ($line -match 'alchemy-fx invalidation \(pid (\d+), ' ) {
      $pid_ = $Matches[1]
      if ($line -match $pattern) { $v = [double]$Matches[1]; if (-not $last.ContainsKey($pid_) -or $v -ge $last[$pid_]) { $last[$pid_] = $v } }
    }
  }
  $total = [double]0; $max = [double]0
  foreach ($v in $last.Values) { $total += $v; if ($v -gt $max) { $max = $v } }
  @{ total = $total; max = $max; count = $last.Count }
}

$load = Get-Load
if ($load -gt $MaxLoad -and -not $Force) { throw "CPU load $load % is above $MaxLoad %; wait for a quiet machine or pass -Force" }
$csv = Join-Path $OutRoot "matrix-$Label.csv"
$md = Join-Path $OutRoot "matrix-$Label.md"
$header = "# JIT matrix $Label, $(Get-Date -Format s), exe $Exe (sha256 $((Get-FileHash $Exe).Hash.Substring(0, 12))), runs $Runs, load at start $load %$(if ($Force) { ' (forced)' })"
Write-Host $header
$urls = @{}; foreach ($p in $Pages) { $urls[$p] = Get-PageUrl $p; Write-Host "  $p -> $($urls[$p])" }

$rows = @()
for ($r = 1; $r -le $Runs; $r++) {
  $order = if ($r % 2) { $Modes } else { @($Modes[($Modes.Count - 1)..0]) }
  foreach ($page in $Pages) {
    foreach ($mode in $order) {
      $name = "$Label-$page-$mode-$r"
      $before = Get-Load; $commit = Get-CommitFreeGB
      $params = @{ Name = $name; OutRoot = $OutRoot; Group = $Group; Exe = $Exe; Stats = $true; Timeout = $Timeout; Url = $urls[$page]; Env = $Env }
      if ($mode -eq 'jitless') { $params.Jitless = $true }
      $statusLine = (& $RunScript @params | Select-Object -Last 1)
      $dir = Join-Path $OutRoot "runs\$name"
      $stderr = @(Get-Content (Join-Path $dir 'stderr.txt') -ErrorAction SilentlyContinue)
      $dom = (Get-Content (Join-Path $dir 'stdout.txt') -Raw -ErrorAction SilentlyContinue); if (-not $dom) { $dom = '' }
      $exit = if ($statusLine -match '^exit (-?\d+)') { $Matches[1] } elseif ($statusLine -match '^timeout') { 'timeout' } else { '?' }
      $wall = if ($statusLine -match 'wall ([\d.]+) s') { [double]$Matches[1] } else { $null }
      $cpu = if ($statusLine -match 'root cpu ([\d.]+) s') { [double]$Matches[1] } else { $null }
      $statsLines = @($stderr | Where-Object { $_ -match 'alchemy-fx stats:' })
      $exitLines = @(for ($i = 0; $i -lt $stderr.Count; $i++) { if ($stderr[$i] -match 'alchemy-fx stats:' -and $i + 1 -lt $stderr.Count) { $stderr[$i + 1] } })
      $periodic = @($stderr | Where-Object { $_ -match 'alchemy-fx invalidation \(pid ' })
      # Exit lines (no pid) cover processes that ended normally, periodic lines (per pid) those that ran at
      # least one period; neither covers all, and they cannot be matched. Per column the larger sum is
      # taken: the page renderer dominates the JIT counters, so this recovers it whichever way it ended.
      $exitInval = @($stderr | Where-Object { $_ -match 'alchemy-fx invalidation:' })
      $pick = {
        param($periodicPattern, $exitLines, $exitPattern)
        $a = Sum-Periodic $periodic $periodicPattern; $b = Sum-Matches $exitLines $exitPattern
        if ($a.total -ge $b.total) { $a.from = 'periodic'; $a } else { $b.from = 'exit'; $b }
      }
      $smc = & $pick 'smc faults (\d+)' $statsLines 'smc faults (\d+)'
      $trans = & $pick '; translations (\d+)' $statsLines 'translations (\d+)'
      $flushes = & $pick 'icache flushes (\d+)' $exitInval 'icache flushes (\d+)'
      $retrans = & $pick 're-translations (\d+)' $exitInval 're-translations (\d+)'
      $reused = & $pick 'reused unchanged (\d+)' $exitInval 'reused unchanged (\d+)'
      $periodicPids = @($periodic | ForEach-Object { if ($_ -match '\(pid (\d+),') { $Matches[1] } } | Sort-Object -Unique).Count
      $counted = "SMC from $($smc.from) ($periodicPids periodic pids, $($statsLines.Count) exit stats)"
      $inval = Sum-Matches $statsLines 'invalidated (\d+)'
      $tsec = Sum-Matches $statsLines 'translate ([\d.]+) s'
      $intr = Sum-Matches $exitLines '\binterrupt (\d+)'
      # The page's results.
      $result = switch ($page) {
        'probe' { if ($dom -match 'data-js="ran"') { 'ran' } else { 'no dump' } }
        'js_bench' {
          if ($dom -match 'data-done="1"') {
            $parts = foreach ($m in [regex]::Matches($dom, 'id="r-([a-z]+)">([\d.]+)<')) { "$($m.Groups[1].Value) $($m.Groups[2].Value)" }
            $sum = if ($dom -match 'id="c-total">(\d+)<') { $Matches[1] } else { '?' }
            ($parts -join ', ') + "; checksum $sum"
          } else { 'no dump' } }
        'wasm' {
          if ($dom -match 'data-wasm="([a-z]+)"') {
            $state = $Matches[1]
            $hot = if ($dom -match 'id="r-hot-sum-median">([\d.]+)<') { ", hot sum median $($Matches[1]) ms" } else { '' }
            $first = if ($dom -match 'id="r-first-sum">([\d.]+)<') { ", first sum $($Matches[1]) ms" } else { '' }
            $comp = if ($dom -match 'id="r-compile">([\d.]+)<') { ", compile $($Matches[1]) ms" } else { '' }
            $async = if ($dom -match 'data-wasm-async="([a-z]+)"') { ", async $($Matches[1])" } else { ', async not before the dump' }
            "wasm $state$comp$first$hot$async"
          } else { 'no dump' } }
        default { if ($dom.Length) { "dumped $($dom.Length) chars" } else { 'no dump' } }
      }
      # A stop: a FATAL check, an allocation failure or a guest killed by a signal. Chrome's ERROR-level
      # "Check failed" lines (e.g. observer_list.h at teardown) do not stop it; they are counted apart.
      $stop = ($stderr | Where-Object { $_ -match 'FATAL|bad allocation|guest thread ended by an error|terminated by SIG' } | Select-Object -First 1)
      if ($stop -and $stop -match '(\[\d+:\d+:[^\]]*FATAL.*)$') { $stop = $Matches[1] }
      $checks = @($stderr | Where-Object { $_ -match ':ERROR:.*Check failed' }).Count
      $watchdog = [bool]($stderr | Where-Object { $_ -match 'Teardown watchdog expired' } | Select-Object -First 1)
      $denied = @(Get-Content (Join-Path $dir 'network-denied.txt') -ErrorAction SilentlyContinue | Where-Object { $_ })
      $unexpected = @($denied | Where-Object { $_ -notmatch '\[2001:4860:4860::8888\]:443|https://update\.googleapis\.com' })
      $row = [pscustomobject]@{
        run = $name; page = $page; mode = $mode; round = $r; load = $before; commit_free_gb = $commit; exit = $exit; wall_s = $wall; root_cpu_s = $cpu
        processes = $statsLines.Count; smc_faults = $smc.total; smc_max = $smc.max; smc_per_s = $(if ($wall) { [math]::Round($smc.total / $wall, 1) } else { $null })
        invalidated = $inval.total; translations = $trans.total; translate_s = [math]::Round($tsec.total, 1); interrupts = $intr.total
        icache_flushes = $flushes.total; retranslations = $retrans.total; reused_unchanged = $reused.total; counters_from = $counted
        result = $result; watchdog = $watchdog; error_checks = $checks; stop = $(if ($stop) { $stop.Trim() } else { '' })
        denied = ($denied -join ' | '); unexpected_denied = ($unexpected -join ' | ')
      }
      $rows += $row
      $row | Export-Csv -NoTypeInformation -Append -Path $csv
      Write-Host ("{0}: {1}; load {2} %; smc {3}; invalidated {4}; {5}{6}" -f $name, $statusLine, $before, $smc.total, $inval.total, $result,
        $(if ($unexpected.Count) { "; NEW network denied: $($unexpected -join ', ')" } else { '' }))
    }
  }
}

function Median([object[]]$values) {
  $v = @($values | Where-Object { $_ -ne $null } | Sort-Object { [double]$_ })
  if (-not $v.Count) { return '' }
  if ($v.Count % 2) { return $v[[int](($v.Count - 1) / 2)] }
  [math]::Round(([double]$v[$v.Count / 2 - 1] + [double]$v[$v.Count / 2]) / 2, 1)
}
$note = if ($Env.ALCHEMY_FX_STATS_PERIOD) {
  "Counters (SMC faults, icache flushes, translations, re-translations): per column the larger of (the last ALCHEMY_FX_STATS_PERIOD line per pid, summed; each process's last <= $($Env.ALCHEMY_FX_STATS_PERIOD) s missing) and (the exit lines, summed; terminated processes missing). Columns marked (exit) come from exit statistics only."
} else { 'Counters: exit statistics only; processes Chrome terminates are missing.' }
$out = @($header, '', $note, '', '## Runs', '',
  '| run | load % | commit free GB | exit | wall s | root CPU s | procs (exit stats) | counters from | SMC faults (max/proc) | SMC/s | icache flushes | re-translations (reused) | invalidated (exit) | translations | translate s (exit) | interrupts (exit) | result | watchdog | ERROR checks | stop |',
  '| --- | ---: | ---: | --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- | ---: | --- |')
foreach ($x in $rows) {
  $out += "| $($x.run) | $($x.load) | $($x.commit_free_gb) | $($x.exit) | $($x.wall_s) | $($x.root_cpu_s) | $($x.processes) | $($x.counters_from) | $($x.smc_faults) ($($x.smc_max)) | $($x.smc_per_s) | $($x.icache_flushes) | $($x.retranslations) ($($x.reused_unchanged)) | $($x.invalidated) | $($x.translations) | $($x.translate_s) | $($x.interrupts) | $($x.result -replace '\|', '/') | $(if ($x.watchdog) { 'yes' } else { '' }) | $($x.error_checks) | $($x.stop -replace '\|', '/') |"
}
$out += @('', '## Per cell (medians)', '',
  '| page | mode | exits | wall s | root CPU s | SMC faults | SMC/s | icache flushes | re-translations | invalidated (exit) | translations | interrupts (exit) |',
  '| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |')
foreach ($page in $Pages) {
  foreach ($mode in $Modes) {
    $cell = @($rows | Where-Object { $_.page -eq $page -and $_.mode -eq $mode })
    $out += "| $page | $mode | $(($cell | ForEach-Object { $_.exit }) -join ', ') | $(Median ($cell.wall_s)) | $(Median ($cell.root_cpu_s)) | $(Median ($cell.smc_faults)) | $(Median ($cell.smc_per_s)) | $(Median ($cell.icache_flushes)) | $(Median ($cell.retranslations)) | $(Median ($cell.invalidated)) | $(Median ($cell.translations)) | $(Median ($cell.interrupts)) |"
  }
}
# js_bench kernels: median ms per kernel and mode.
$bench = @($rows | Where-Object { $_.page -eq 'js_bench' -and $_.result -ne 'no dump' })
if ($bench.Count) {
  $kernels = @([regex]::Matches($bench[0].result, '([a-z]+) [\d.]+') | ForEach-Object { $_.Groups[1].Value } | Where-Object { $_ -ne 'checksum' })
  $out += @('', '## js_bench (median ms)', '', "| mode | $($kernels -join ' | ') | checksums |", "| --- |$(' ---: |' * $kernels.Count) --- |")
  foreach ($mode in $Modes) {
    $cell = @($bench | Where-Object { $_.mode -eq $mode })
    if (-not $cell.Count) { continue }
    $cols = foreach ($k in $kernels) { Median (@($cell | ForEach-Object { if ($_.result -match "\b$k ([\d.]+)") { [double]$Matches[1] } })) }
    $sums = @($cell | ForEach-Object { if ($_.result -match 'checksum (\d+)') { $Matches[1] } } | Sort-Object -Unique)
    $out += "| $mode | $($cols -join ' | ') | $($sums -join ', ') |"
  }
}
$out | Set-Content $md
$out | Write-Host
