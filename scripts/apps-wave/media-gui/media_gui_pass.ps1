# Runtime pass for the apps-wave media/GUI apps: strict and probe=trap launches, then deterministic work runs.
# Every run goes through run_guest.ps1 (cloaked, loopback-only, own process tree killed on timeout).
#
# Usage: media_gui_pass.ps1 -OutDir <dir> [-Base <dir>] [-Only name,...] [-Exe <alchemy-fx.exe>]
#          [-ExtraRuntimeDir dir,...] [-Notes <notes dir>]
#   -Base (default $env:ALCHEMY_APPS_MEDIA_DIR): roots\<app>, media\ (make_test_media.ps1), work\godot\, work\firefox\
#   -ExtraRuntimeDir: runtime artifacts added after the main checkout's llvm/swift/expat ones (e.g. libcurl)
#   -Notes: passed to run_guest.ps1 (default $env:ALCHEMY_APPS_NOTES)
# Runs: <app>-version-{strict,trap}, audacity/obs-{strict,trap}, hb-transcode, vlc-wav, vlc-y4m, godot-probe,
# firefox-screenshot (headless, its sandboxes off through Mozilla's MOZ_DISABLE_*_SANDBOX variables: Alchemy refuses
# the macOS sandbox).
param(
    [Parameter(Mandatory)][string]$OutDir,
    [string[]]$Only = @(),
    [string]$Exe = (Join-Path $PSScriptRoot '..\..\..\build-ninja\Release\alchemy-fx.exe'),
    [string]$Base = $env:ALCHEMY_APPS_MEDIA_DIR,
    [string[]]$ExtraRuntimeDir = @(),
    [string]$Notes = $env:ALCHEMY_APPS_NOTES
)
$ErrorActionPreference = 'Stop'
if (-not $Base) { throw 'pass -Base or set ALCHEMY_APPS_MEDIA_DIR to the folder holding roots\, media\ and work\' }
$Base = [IO.Path]::GetFullPath($Base)
$runGuest = Join-Path $PSScriptRoot 'run_guest.ps1'
$R = "$Base\roots"
# Host drive X: is /Volumes/X in the guest.
function GuestPath([string]$path) { "/Volumes/$($path.Substring(0, 1).ToUpper())" + ($path.Substring(2) -replace '\\', '/') }
$guestBase = GuestPath $Base
$media = "$guestBase/media"
$trap = @{ ALCHEMY_PROBE_MISSING = 'trap' }

$runs = @()
foreach ($mode in 'strict', 'trap') {
    $envs = if ($mode -eq 'trap') { $trap } else { @{} }
    $runs += @(
        @{ n = "hb-version-$mode"; r = 'handbrake'; p = '/usr/local/bin/HandBrakeCLI'; a = @('--version'); e = $envs },
        @{ n = "godot-version-$mode"; r = 'godot'; p = '/Applications/Godot.app'; a = @('--headless', '--version'); e = $envs },
        @{ n = "vlc-version-$mode"; r = 'vlc'; p = '/Applications/VLC.app/Contents/MacOS/VLC'; a = @('--version'); e = $envs },
        @{ n = "mpv-version-$mode"; r = 'mpv'; p = '/Applications/mpv.app'; a = @('--version'); e = $envs },
        @{ n = "firefox-version-$mode"; r = 'firefox'; p = '/Applications/Firefox.app'; a = @('--version'); e = $envs },
        @{ n = "audacity-$mode"; r = 'audacity'; p = '/Applications/Audacity 4.app'; a = @(); e = $envs; gui = $true },
        @{ n = "obs-$mode"; r = 'obs'; p = '/Applications/OBS.app'; a = @(); e = $envs; gui = $true }
    )
}
$runs += @(
    @{ n = 'hb-transcode'; r = 'handbrake'; p = '/usr/local/bin/HandBrakeCLI'; e = $trap; t = 300; h = @(60, 180)
        a = @('-i', "$media/bars.y4m", '-o', '<out>/out.mp4', '-e', 'x264', '-q', '30', '--encoder-preset', 'ultrafast') },
    @{ n = 'vlc-wav'; r = 'vlc'; p = '/Applications/VLC.app/Contents/MacOS/VLC'; e = $trap; t = 180; h = @(120)
        a = @('-I', 'dummy', '--no-sout-display', "$media/tone-440.wav",
            '--sout', '#transcode{acodec=s16l,channels=2,samplerate=48000}:std{access=file,mux=wav,dst=<out>/out.wav}', 'vlc://quit') },
    @{ n = 'vlc-y4m'; r = 'vlc'; p = '/Applications/VLC.app/Contents/MacOS/VLC'; e = $trap; t = 180; h = @(120)
        a = @('-I', 'dummy', "$media/bars.y4m",
            '--sout', '#transcode{vcodec=mp4v,vb=1000}:std{access=file,mux=ts,dst=<out>/out.ts}', 'vlc://quit') },
    @{ n = 'godot-probe'; r = 'godot'; p = '/Applications/Godot.app'; e = $trap; t = 180; h = @(120)
        a = @('--headless', '--path', "$guestBase/work/godot", '--script', 'res://probe.gd') },
    @{ n = 'firefox-screenshot'; r = 'firefox'; p = '/Applications/Firefox.app'; t = 240; h = @(180)
        e = @{ ALCHEMY_PROBE_MISSING = 'trap'; MOZ_DISABLE_CONTENT_SANDBOX = '1'; MOZ_DISABLE_GMP_SANDBOX = '1'
            MOZ_DISABLE_RDD_SANDBOX = '1'; MOZ_DISABLE_SOCKET_PROCESS_SANDBOX = '1'; MOZ_DISABLE_UTILITY_SANDBOX = '1'
            MOZ_DISABLE_GPU_SANDBOX = '1' }
        a = @('--headless', '--no-remote', '--profile', "$guestBase/work/firefox/profile", '--window-size=400,300',
            '--screenshot', '<out>/shot.png', "file://$guestBase/work/firefox/page.html") }
)
$checkout = Split-Path -Parent (git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir).Trim()
$runtimeDirs = @(@('llvm', 'swift', 'expat') | ForEach-Object { Join-Path $checkout "artifacts\alchemy-$_-arm64" }) + $ExtraRuntimeDir

foreach ($x in $runs) {
    if ($Only.Count -and -not ($Only | Where-Object { $x.n -like $_ })) { continue }
    $out = Join-Path $OutDir $x.n
    $guestOut = GuestPath ([IO.Path]::GetFullPath($out))
    $programArgs = @($x.a | ForEach-Object { $_ -replace '<out>', $guestOut })
    $timeout = if ($x.t) { $x.t } elseif ($x.gui) { 120 } else { 90 }
    $p = @{ Exe = $Exe; Root = "$R\$($x.r)"; Program = $x.p; OutDir = $out; ProgramArgs = $programArgs; Timeout = $timeout
        Env = $x.e; Watch = $true; Capture = [bool]$x.gui; RuntimeDir = $runtimeDirs; Notes = $Notes }
    if ($x.h) { $p.HangDumpAt = $x.h } elseif ($x.gui -and $x.e.Count) { $p.HangDumpAt = @(60, 100) }
    Write-Host "$(Get-Date -Format s) $($x.n)"
    # run_guest.ps1 sets -Env in this process, so a previous run's probe mode or MOZ_* variables would leak into the next one.
    Get-ChildItem Env: | Where-Object { $_.Name -eq 'ALCHEMY_PROBE_MISSING' -or $_.Name -like 'MOZ_DISABLE_*' } |
        ForEach-Object { Remove-Item "Env:$($_.Name)" }
    & $runGuest @p
}
Write-Host "$(Get-Date -Format s) done"
