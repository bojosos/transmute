# Classroom render, native x64 Blender vs ARM64 Blender under transmute-fx.
# Prints process wall time, time to PNG, Blender's render Time, CPU seconds and a PNG hash.
# Compare images with tools/fx_imgdiff.py (PNG hashes include metadata).
param(
    [string] $Fx = "",
    [int] $Percent = 25,
    [int] $Samples = 16,
    [int] $Threads = 8,
    [string[]] $Modes = @('native', 'fx'),
    [int] $Runs = 1,
    [int] $ExitGraceSeconds = 30,
    [string] $OutDir = 'C:\dev\transmute\artifacts\fx-classroom',
    [long] $Affinity = 0,   # processor mask (0 = unrestricted); e.g. 1 = first logical processor
    [switch] $ControlledScheduling
)
$ErrorActionPreference = 'Stop'
if ($ControlledScheduling) { . "$PSScriptRoot/fx_bench_control.ps1" }
$arm = "C:\dev\transmute\artifacts\arm64-stage\Blender Foundation\Blender 4.5\blender.exe"
$native = "C:\Program Files\Blender Foundation\Blender 4.5\blender.exe"
$scene = "C:\dev\transmute\artifacts\scene\classroom\classroom.blend"
$outdir = $OutDir
New-Item -ItemType Directory -Force $outdir | Out-Null
$expr = "import bpy; s=bpy.context.scene; s.cycles.samples=$Samples; s.render.resolution_percentage=$Percent"

foreach ($mode in $Modes) {
    for ($r = 0; $r -lt $Runs; $r++) {
        $tag = "{0}_p{1}_s{2}_t{3}_r{4}" -f $mode, $Percent, $Samples, $Threads, $r
        $prefix = Join-Path $outdir ($tag + '_#####')
        $png = Join-Path $outdir ($tag + '_00001.png')
        $log = Join-Path $outdir ($tag + '.log')
        Remove-Item -Force -ErrorAction SilentlyContinue $png, $log
        $bargs = @('--factory-startup', '-b', $scene, '-E', 'CYCLES', '-t', "$Threads", '-o', $prefix, '-F', 'PNG',
                   '--python-expr', $expr, '-f', '1', '--', '--cycles-device', 'CPU')
        if ($mode -eq 'fx') { $exe = $Fx; $argv = @($arm) + $bargs } else { $exe = $native; $argv = $bargs }
        $quoted = ($argv | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $exe
        $psi.Arguments = $quoted
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.WorkingDirectory = $outdir
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $p = [System.Diagnostics.Process]::Start($psi)
        if ($ControlledScheduling) { Set-FxBenchChildControl $p }
        if ($Affinity -ne 0) { $p.ProcessorAffinity = [IntPtr]$Affinity }
        $outTask = $p.StandardOutput.ReadToEndAsync()
        $errTask = $p.StandardError.ReadToEndAsync()
        $savedAt = $null
        while (-not $p.HasExited) {
            if (-not $savedAt -and (Test-Path $png)) { $savedAt = $sw.Elapsed.TotalSeconds }
            if ($savedAt -and ($sw.Elapsed.TotalSeconds - $savedAt) -gt $ExitGraceSeconds) { $p.Kill(); break }
            Start-Sleep -Milliseconds 100
        }
        $p.WaitForExit()
        $total = $sw.Elapsed.TotalSeconds
        if (-not $savedAt -and (Test-Path $png)) { $savedAt = $total }
        $text = $outTask.Result + "`n--- stderr ---`n" + $errTask.Result
        Set-Content -Path $log -Value $text -Encoding utf8
        $time = ($text -split "`n" | Select-String '^Time:\s+(\S+)' | Select-Object -Last 1)
        $t = if ($time) { $time.Matches[0].Groups[1].Value } else { '?' }
        $hash = if (Test-Path $png) { (Get-FileHash $png -Algorithm SHA256).Hash.Substring(0, 12) } else { 'none' }
        $cpu = try { $p.TotalProcessorTime.TotalSeconds } catch { -1 }
        $cycles = if ($ControlledScheduling) { Get-FxBenchProcessCycles $p } else { 0 }
        '{0,-28} process {1,7:N2} s | png@{2,7:N2} s | blender Time {3} | cpu {6,7:N1} s | exit 0x{4:X} | {5} | cycles {7}' -f $tag, $total, $savedAt, $t, $p.ExitCode, $hash, $cpu, $cycles
    }
}
