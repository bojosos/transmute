param(
    [string] $AlchemyFx = "$PSScriptRoot/../build-wf-verify/Release/alchemy-fx.exe",
    [string] $TransmuteFx = '',
    [string] $Native = "$PSScriptRoot/../artifacts/x64/blender-4.5.14-windows-x64/blender.exe",
    [string[]] $Modes = @('native', 'windows-arm64-fx', 'macos-arm64-alchemy'),
    [int] $Percent = 10,
    [int] $Samples = 16,
    [int] $Threads = 4,
    [string] $OutDir = "$PSScriptRoot/../artifacts/alchemy-blender/classroom-compare"
)
# Renders Classroom with the same Cycles CPU settings through native x64
# Blender, Windows ARM64 Blender on transmute-fx, and the official macOS ARM64
# Blender on alchemy-fx. Reports wall time and Blender's own render time.
$ErrorActionPreference = 'Stop'
$R = (Resolve-Path "$PSScriptRoot/..").Path
$scene = "$R\artifacts\scene\classroom\classroom.blend"
$arm = "$R\artifacts\arm64-stage\Blender Foundation\Blender 4.5\blender.exe"
$mac = "$R\artifacts\alchemy-blender\extracted\Blender\Blender.app\Contents\MacOS\Blender"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
function G([string] $p) { '/Volumes/' + $p.Substring(0, 1).ToUpper() + ($p.Substring(2) -replace '\\', '/') }
function Quote([string] $a) { if ($a -notmatch '[\s"]') { return $a }; '"' + ($a -replace '(\\*)"', '$1$1\"') + '"' }
$expr = "import bpy; s=bpy.context.scene; s.cycles.samples=$Samples; s.render.resolution_percentage=$Percent"

foreach ($mode in $Modes) {
    $tag = "{0}_p{1}_s{2}_t{3}" -f $mode, $Percent, $Samples, $Threads
    $png = Join-Path $OutDir "$tag`_00001.png"
    Remove-Item -Force -ErrorAction SilentlyContinue $png
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = $OutDir
    if ($mode -eq 'macos-arm64-alchemy') {
        $psi.FileName = (Resolve-Path $AlchemyFx).Path
        $argv = @('--runtime-dir', "$R\artifacts\alchemy-llvm-arm64", $mac, '--factory-startup', '-b', (G $scene),
                  '-E', 'CYCLES', '-t', "$Threads", '-o', (G "$OutDir\$tag`_#####"), '-F', 'PNG',
                  '--python-expr', $expr, '-f', '1', '--', '--cycles-device', 'CPU')
    } else {
        $argv = @('--factory-startup', '-b', $scene, '-E', 'CYCLES', '-t', "$Threads", '-o', "$OutDir\$tag`_#####",
                  '-F', 'PNG', '--python-expr', $expr, '-f', '1', '--', '--cycles-device', 'CPU')
        if ($mode -eq 'windows-arm64-fx') {
            $psi.FileName = (Resolve-Path $TransmuteFx).Path
            $argv = @($arm) + $argv
            $cache = "$OutDir\fx-cache-$tag"
            Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $cache
            $psi.EnvironmentVariables['TRANSMUTE_FX_CACHE_DIR'] = $cache
            $psi.EnvironmentVariables['TRANSMUTE_FX_BACKGROUND_AOT'] = '0'
        } else {
            $psi.FileName = (Resolve-Path $Native).Path
        }
    }
    $psi.Arguments = ($argv | ForEach-Object { Quote $_ }) -join ' '
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)
    $o = $p.StandardOutput.ReadToEndAsync(); $e = $p.StandardError.ReadToEndAsync()
    $p.WaitForExit()
    $sw.Stop()
    Set-Content -Encoding utf8 "$OutDir\$tag.out" $o.Result
    Set-Content -Encoding utf8 "$OutDir\$tag.err" $e.Result
    $cpu = $p.TotalProcessorTime.TotalSeconds
    $timeLine = ($o.Result -split "`n" | Where-Object { $_ -match '^\s*Time: ' } | Select-Object -Last 1)
    $renderTime = if ($timeLine) { $timeLine.Trim() } else { '(no Time line)' }
    '{0,-22} exit={1} wall={2,7:N1} s cpu={3,7:N1} s  blender: {4}  png={5}' -f $mode, $p.ExitCode,
        $sw.Elapsed.TotalSeconds, $cpu, $renderTime, (Test-Path $png)
}
