param(
    [string] $Exe = "$PSScriptRoot/../build-alchemy-fx/Release/alchemy-fx.exe",
    [ValidateSet('off', 'zero', 'trap')] [string] $Probe = 'zero',
    [string[]] $Stages = @('version', 'factory', 'python', 'render'),
    [string] $OutDir = "$PSScriptRoot/../artifacts/alchemy-blender/runs",
    [int] $Threads = 1,
    [int] $TimeoutSeconds = 900
)
# Runs the official macOS ARM64 Blender 4.5.14 through Alchemy in headless
# stages and reports the exit status, wall time, probe hits and last error of
# each. The render stage draws the factory Cube with Cycles CPU at 16 x 16,
# one sample, and checks the PNG.
$ErrorActionPreference = 'Stop'
$R = (Resolve-Path "$PSScriptRoot/..").Path -replace '\\', '/'
# Artifacts live in the main checkout (absent from git worktrees).
$A = if (Test-Path "$R/artifacts/alchemy-blender") { "$R/artifacts" } else { 'C:/dev/transmute/artifacts' }
$Blender = "$A/alchemy-blender/extracted/Blender/Blender.app/Contents/MacOS/Blender"
$Runtime = "$A/alchemy-llvm-arm64"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path -replace '\\', '/'
function G([string] $p) { '/Volumes/' + $p.Substring(0, 1).ToUpper() + $p.Substring(2) }
function Quote([string] $a) { if ($a -notmatch '[\s"]') { return $a }; '"' + ($a -replace '(\\*)"', '$1$1\"') + '"' }

$png = "$OutDir/render_0001.png"
$renderPy = "import bpy; s = bpy.context.scene; s.render.engine = 'CYCLES'; s.cycles.device = 'CPU'; " +
    "s.cycles.samples = 1; s.cycles.use_denoising = False; s.render.resolution_x = 16; s.render.resolution_y = 16; " +
    "s.render.resolution_percentage = 100; s.render.threads_mode = 'FIXED'; s.render.threads = $Threads; " +
    "s.render.image_settings.file_format = 'PNG'; s.render.filepath = '" + (G "$OutDir/render_0001.png") + "'; " +
    "bpy.ops.render.render(write_still=True); print('ALCHEMY RENDER DONE')"
$stageArgs = @{
    version = @('--version')
    factory = @('-b', '--factory-startup', '-noaudio')
    python  = @('-b', '--factory-startup', '-noaudio', '--python-expr',
                "import bpy, sys; print('ALCHEMY BPY', bpy.app.version_string, len(bpy.data.objects), sys.platform)")
    render  = @('-b', '--factory-startup', '-noaudio', '--python-expr', $renderPy)
}

foreach ($stage in $Stages) {
    if ($stage -eq 'render' -and (Test-Path $png)) { Remove-Item $png }
    $all = @('--runtime-dir', $Runtime, $Blender) + $stageArgs[$stage]
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path $Exe).Path
    $psi.Arguments = ($all | ForEach-Object { Quote $_ }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    if ($Probe -ne 'off') { $psi.EnvironmentVariables['ALCHEMY_PROBE_MISSING'] = $Probe }
    else { $psi.EnvironmentVariables.Remove('ALCHEMY_PROBE_MISSING') }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync(); $e = $p.StandardError.ReadToEndAsync()
    $timedOut = -not $p.WaitForExit($TimeoutSeconds * 1000)
    if ($timedOut) { $p.Kill(); $p.WaitForExit() }
    $sw.Stop()
    $out = $o.Result; $err = $e.Result
    Set-Content -Encoding utf8 "$OutDir/$stage.out" $out
    Set-Content -Encoding utf8 "$OutDir/$stage.err" $err
    $probeLines = @($err -split "`n" | Where-Object { $_.Contains('[alchemy probe]') })
    $calls = @($probeLines | Where-Object { $_.Contains('zero-return') }).Count
    $errLines = @($err -split "`n" | Where-Object { $_.Trim() -and -not $_.Contains('[alchemy probe]') })
    $last = if ($errLines.Count) { ($errLines | Select-Object -Last 1).Trim() } else { '' }
    if ($last.Length -gt 400) { $last = $last.Substring(0, 400) }
    $status = if ($timedOut) { 'TIMEOUT' } else { "exit=$($p.ExitCode)" }
    $extra = ''
    if ($stage -eq 'render') {
        if (Test-Path $png) {
            $bytes = [System.IO.File]::ReadAllBytes($png)
            $w = [System.BitConverter]::ToUInt32(@($bytes[19], $bytes[18], $bytes[17], $bytes[16]), 0)
            $h = [System.BitConverter]::ToUInt32(@($bytes[23], $bytes[22], $bytes[21], $bytes[20]), 0)
            $extra = " png=${w}x${h} ($($bytes.Length) bytes)"
        } else { $extra = ' png=missing' }
    }
    '{0,-8} {1,-12} {2,7} ms probe_calls={3}{4}' -f $stage, $status, $sw.ElapsedMilliseconds, $calls, $extra
    if ($last) { "         last: $last" }
}
"logs: $OutDir"
