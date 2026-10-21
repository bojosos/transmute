# Run from an Administrator PowerShell window for hardware counters.
param(
    [string]$Fx = "$PSScriptRoot/../build-fx-investigate/Release/transmute-fx.exe",
    [string]$OutDir = "$PSScriptRoot/../artifacts/fx-investigate/vtune-admin",
    [int]$Percent = 25,
    [int]$Samples = 64
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/..").Path
$vtune = 'C:/Program Files (x86)/Intel/oneAPI/vtune/latest/bin64/vtune.exe'
$native = 'C:/Program Files/Blender Foundation/Blender 4.5/blender.exe'
$arm = "$repo/artifacts/arm64-stage/Blender Foundation/Blender 4.5/blender.exe"
$scene = "$repo/artifacts/scene/classroom/classroom.blend"
$Fx = (Resolve-Path $Fx).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$runDir = Join-Path $OutDir (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory $runDir | Out-Null
$savedEnv = @{}
$names = @('TRANSMUTE_FX_SAMPLE', 'TRANSMUTE_FX_STATS', 'TRANSMUTE_FX_CORE', 'TRANSMUTE_FX_FP', 'TRANSMUTE_FX_NEON', 'TRANSMUTE_FX_GLOBALS', 'TRANSMUTE_FX_INLINE', 'TRANSMUTE_FX_CACHE_DIR')
$self = [Diagnostics.Process]::GetCurrentProcess()
$affinity = $self.ProcessorAffinity
try {
    foreach ($name in $names) {
        $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    $env:TRANSMUTE_FX_CACHE_DIR = "$repo/build-fx-investigate/cache"
    $self.ProcessorAffinity = [IntPtr]1
    foreach ($mode in 'native', 'fx') {
        [string[]]$target = if ($mode -eq 'native') { @($native) } else { @($Fx, $arm) }
        $target += @('--factory-startup', '-b', $scene, '-E', 'CYCLES', '-t', '1',
            '-o', "$runDir/$mode-render", '-F', 'PNG', '--python-expr',
            "import bpy; s=bpy.context.scene; s.cycles.samples=$Samples; s.render.resolution_percentage=$Percent",
            '-f', '1', '--', '--cycles-device', 'CPU')
        Write-Host "Collecting $mode into $runDir/$mode"
        # ProcessStartInfo preserves the Python expression as one argument on PS5 and PS7.
        $argv = @('-collect', 'uarch-exploration', '-result-dir', "$runDir/$mode", '--') + $target
        $psi = New-Object Diagnostics.ProcessStartInfo
        $psi.FileName = $vtune
        $psi.Arguments = ($argv | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $p = [Diagnostics.Process]::Start($psi)
        $stdout = $p.StandardOutput.ReadToEndAsync()
        $stderr = $p.StandardError.ReadToEndAsync()
        $p.WaitForExit()
        ($stdout.Result + "`n" + $stderr.Result) | Set-Content "$runDir/$mode-console.txt"
        if ($p.ExitCode -ne 0) {
            Get-Content "$runDir/$mode-console.txt" -Tail 25
            throw "VTune $mode collection failed, exit $($p.ExitCode)"
        }
        & $vtune -report summary -r "$runDir/$mode" -report-output "$runDir/$mode-summary.txt"
        if ($LASTEXITCODE -ne 0) { throw "Summary report failed for $mode" }
        & $vtune -report hotspots -r "$runDir/$mode" -group-by module -column 'CPU Time,Clockticks,Instructions Retired,CPI Rate' -report-output "$runDir/$mode-modules.txt"
        if ($LASTEXITCODE -ne 0) { throw "Module report failed for $mode" }
        # Exclude most load/setup time when comparing steady rendering.
        # The summary report ignores time-filter; hotspots supports it.
        & $vtune -report hotspots -r "$runDir/$mode" -group-by process -column 'CPU Time,Clockticks,Instructions Retired,CPI Rate' -time-filter '5:9999' -format csv -report-output "$runDir/$mode-render-counters.csv"
        if ($LASTEXITCODE -ne 0) { Write-Warning "Time-filtered report failed; full report is saved." }
    }
} finally {
    $self.ProcessorAffinity = $affinity
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $savedEnv[$name], 'Process') }
}
Write-Host "Results saved in $runDir"
