# A measurement window for the fx perf suite (docs/FX_PERF.md, "Method"): holds <LockDir>\perf.lock for -Minutes
# (default 40) and writes <LockDir>\window.md, so a build-lock script that honours the lock starts no new build and
# sessions start no guest app runs. Running builds and runs are left to finish. fx_ab.ps1 runs inside the window as
# usual: it leaves a window's lock alone and still waits for a quiet machine before each sample.
#   scripts/perf/window.ps1 [-Minutes 40] [-LockDir <dir>]   # run it in the background; -Release ends it early
#   scripts/perf/window.ps1 -Release
# -LockDir defaults to ALCHEMY_PERF_LOCK_DIR, else %TEMP%\alchemy-perf (the same default as fx_ab.ps1).
param([int]$Minutes = 40, [switch]$Release, [string]$LockDir = $env:ALCHEMY_PERF_LOCK_DIR)
$ErrorActionPreference = 'Stop'
if (-not $LockDir) { $LockDir = Join-Path $env:TEMP 'alchemy-perf' }
$dir = $LockDir
$lock = Join-Path $dir 'perf.lock'
$state = Join-Path $dir 'window.md'
New-Item -ItemType Directory -Force $dir | Out-Null
if ($Release) {
    if (Test-Path $lock) {
        $owner = ((Get-Content $lock -TotalCount 1) -split ' ')[0]
        Stop-Process -Id ([int]$owner) -Force -ErrorAction SilentlyContinue
        Remove-Item -Force $lock -ErrorAction SilentlyContinue
    }
    "# fx perf measurement window`n`nNone open (released $(Get-Date -Format s)). Building and guest runs are fine." | Set-Content $state
    'released'
    return
}
$until = (Get-Date).AddMinutes($Minutes)
"$PID $(Get-Date -Format o) window until $($until.ToString('s'))" | Set-Content $lock
@"
# fx perf measurement window

**Open until $($until.ToString('HH:mm')) ($Minutes min from $(Get-Date -Format 'yyyy-MM-dd HH:mm'))**, lock held by pid $PID.

While it is open: start no builds (build_locked.ps1 waits by itself) and no guest app runs (Chrome, Blender,
QLab, renderer plug-ins, fixtures, ctest of alchemy_* GUI tests). Let what is already running finish. Resume when this file
says none is open.
"@ | Set-Content $state
try {
    while ((Get-Date) -lt $until) {
        Start-Sleep -Seconds 30
        if (-not (Test-Path $lock) -or -not ((Get-Content $lock -TotalCount 1) -like "$PID *")) { break }   # released
    }
} finally {
    if ((Test-Path $lock) -and ((Get-Content $lock -TotalCount 1) -like "$PID *")) { Remove-Item -Force $lock }
    "# fx perf measurement window`n`nNone open (closed $(Get-Date -Format s)). Building and guest runs are fine." | Set-Content $state
}
'window closed'
