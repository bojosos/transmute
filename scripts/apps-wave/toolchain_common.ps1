# Shared helpers for the command-line toolchain checks under alchemy-fx.
# Dot-source this file. Every guest run is cloaked and loopback-only.
#
#   . scripts/apps-wave/toolchain_common.ps1
#   Invoke-AwGuest -Exe <alchemy-fx.exe> -Program C:\...\bin\clang -Arguments '--version' -OutDir <dir> -Name clang-version
#
# Each run writes <Name>.stdout.txt, <Name>.stderr.txt and returns an object with the exit code,
# the wall time and whether it timed out. The guest is started with the working directory given,
# so guest absolute paths such as /dev/apps-wave/... resolve on that drive.

function Set-AwCloak {
    $env:ALCHEMY_GUI_CLOAK = '1'
    $env:ALCHEMY_GUI_NO_ACTIVATE = '1'
    $env:ALCHEMY_GUI_NO_CRASH_DIALOG = '1'
    $env:ALCHEMY_NETWORK = 'loopback'
}

# Guest path for a host path on the working directory's drive: C:\dev\x -> /dev/x.
function ConvertTo-AwGuestPath([string]$HostPath) {
    $full = [System.IO.Path]::GetFullPath($HostPath)
    return ($full.Substring(2) -replace '\\', '/')
}

function Invoke-AwGuest {
    param(
        [Parameter(Mandatory)] [string]$Exe,
        [Parameter(Mandatory)] [string]$Program,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory)] [string]$OutDir,
        [Parameter(Mandatory)] [string]$Name,
        [string]$WorkingDirectory = (Get-Location).Path,
        [string[]]$AlchemyOptions = @(),
        [hashtable]$Environment = @{},
        [int]$TimeoutSeconds = 300
    )
    Set-AwCloak
    New-Item -ItemType Directory -Force $OutDir | Out-Null
    $stdout = Join-Path $OutDir "$Name.stdout.txt"
    $stderr = Join-Path $OutDir "$Name.stderr.txt"

    $saved = @{}
    foreach ($key in $Environment.Keys) {
        $saved[$key] = [Environment]::GetEnvironmentVariable($key)
        [Environment]::SetEnvironmentVariable($key, [string]$Environment[$key])
    }
    try {
        $all = @($AlchemyOptions) + @($Program) + @($Arguments)
        $quoted = $all | ForEach-Object {
            if ($_ -eq '' -or $_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
        }
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $process = Start-Process -FilePath $Exe -ArgumentList ($quoted -join ' ') -WorkingDirectory $WorkingDirectory `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr -NoNewWindow -PassThru
        $null = $process.Handle   # keeps the exit code readable after exit
        $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
        if ($timedOut) {
            # Only the process this function started, and its guest children (same exe, our command line).
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $process.WaitForExit(10000) | Out-Null
        }
        $clock.Stop()
        [pscustomobject]@{
            Name     = $Name
            ExitCode = if ($timedOut) { $null } else { $process.ExitCode }
            TimedOut = $timedOut
            Seconds  = [math]::Round($clock.Elapsed.TotalSeconds, 1)
            Stdout   = $stdout
            Stderr   = $stderr
        }
    }
    finally {
        foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key]) }
    }
}

# Prints a one-line summary and the first lines of output of a run.
function Show-AwRun($Run, [int]$Lines = 15) {
    $state = if ($Run.TimedOut) { 'TIMEOUT' } else { "exit $($Run.ExitCode)" }
    Write-Host "== $($Run.Name): $state, $($Run.Seconds) s"
    Get-Content -LiteralPath $Run.Stdout -TotalCount $Lines -ErrorAction SilentlyContinue | ForEach-Object { "  out| $_" }
    Get-Content -LiteralPath $Run.Stderr -TotalCount $Lines -ErrorAction SilentlyContinue | ForEach-Object { "  err| $_" }
}
