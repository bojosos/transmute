param(
    [Parameter(Mandatory = $true)] [string]$Stage,
    [string]$Scene,
    [long]$InstructionLimit = 500000000,
    [int]$Threads = 8,
    [switch]$TinyScene,
    [switch]$Jit,
    [switch]$JitStats,
    [switch]$DiagnoseExceptions,
    [switch]$DiagnoseSync,
    [string]$RuntimeExecutable,
    [ValidateSet('Release', 'RelWithDebInfo')] [string]$Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'benchmark_power.ps1')
if ($InstructionLimit -lt 1) { throw 'InstructionLimit must be positive' }
if ($Threads -lt 1) { throw 'Threads must be positive' }
if (-not $TinyScene -and -not $Scene) {
    throw 'Provide -Scene, or use -TinyScene for the factory Cube render'
}
if ($TinyScene -and $Scene) { throw '-TinyScene and -Scene cannot be combined' }
if ($TinyScene -and -not $PSBoundParameters.ContainsKey('Threads')) { $Threads = 1 }

$stagePath = (Resolve-Path -LiteralPath $Stage).Path
$scenePath = if ($TinyScene) { $null } else { (Resolve-Path -LiteralPath $Scene).Path }
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$transmute = Join-Path $repository "build\$Configuration\transmute.exe"
if ($RuntimeExecutable) { $transmute = (Resolve-Path -LiteralPath $RuntimeExecutable).Path }
$blender = Join-Path $stagePath 'blender.exe'
$shared = Join-Path $stagePath 'blender.shared'
$crt = Join-Path $stagePath 'blender.crt'
foreach ($required in @($transmute, $blender)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required file was not found: $required"
    }
}
foreach ($required in @($shared, $crt)) {
    if (-not (Test-Path -LiteralPath $required -PathType Container)) {
        throw "Required directory was not found: $required"
    }
}

$options = @('--instruction-limit', "$InstructionLimit")
if ($JitStats) { $options += '--jit-stats' }
elseif ($Jit) { $options += '--jit' }
if ($DiagnoseExceptions) { $options += '--diagnose-exceptions' }
if ($DiagnoseSync) { $options += '--diagnose-sync' }
$label = if ($TinyScene) { 'arm64-tiny-cycles' } else { 'arm64-smoke' }
$output = Join-Path $repository "artifacts\$label-#####"
$expression = if ($TinyScene) {
    'import bpy; s=bpy.context.scene; s.cycles.samples=1; s.render.resolution_x=16; s.render.resolution_y=16; s.render.resolution_percentage=100'
} else {
    'import bpy; bpy.context.scene.cycles.samples = 16; bpy.context.scene.render.resolution_percentage = 100'
}
$guestArguments = @('--factory-startup', '-b')
if ($scenePath) { $guestArguments += $scenePath }
$guestArguments += @('-E', 'CYCLES', '-t', "$Threads", '-o', $output, '-F', 'PNG',
    '--python-expr', $expression, '-f', '1', '--', '--cycles-device', 'CPU')
$log = Join-Path $repository ("artifacts\{0}-{1}.log" -f $label, (Get-Date -Format 'yyyyMMdd-HHmmss'))
$started = [DateTime]::UtcNow
$powerBefore = Get-ProbePower
$watch = [Diagnostics.Stopwatch]::StartNew()
$code = 1
Write-Output "Probe log: $log"
try {
    & $transmute @options --dll-dir $shared --dll-dir $crt $blender @guestArguments 2>&1 |
        Tee-Object -FilePath $log
    $code = $LASTEXITCODE
} finally {
    $watch.Stop()
    Write-Output "Transmute exit: $code; wall seconds: $($watch.Elapsed.TotalSeconds)" |
        Tee-Object -FilePath $log -Append
}
if ($code -eq 0) {
    $image = $output.Replace('#####', '00001') + '.png'
    if (-not (Test-Path -LiteralPath $image -PathType Leaf) -or
        (Get-Item -LiteralPath $image).LastWriteTimeUtc -lt $started) {
        throw "Blender exited successfully without a fresh frame: $image"
    }
    $bytes = [IO.File]::ReadAllBytes($image)
    $signature = @(137, 80, 78, 71, 13, 10, 26, 10)
    if ($bytes.Length -lt 24) { throw "Blender produced an incomplete PNG: $image" }
    for ($index = 0; $index -lt $signature.Count; ++$index) {
        if ($bytes[$index] -ne $signature[$index]) {
            throw "Blender produced an invalid PNG: $image"
        }
    }
    $width = [int]($bytes[16] * 16777216 + $bytes[17] * 65536 + $bytes[18] * 256 + $bytes[19])
    $height = [int]($bytes[20] * 16777216 + $bytes[21] * 65536 + $bytes[22] * 256 + $bytes[23])
    if ($TinyScene -and ($width -ne 16 -or $height -ne 16)) {
        throw "Tiny Cycles frame is ${width}x${height}, expected 16x16"
    }
    [pscustomobject]@{
        blender = $blender
        blender_sha256 = (Get-FileHash -LiteralPath $blender -Algorithm SHA256).Hash
        transmute = $transmute
        transmute_sha256 = (Get-FileHash -LiteralPath $transmute -Algorithm SHA256).Hash
        scene = $scenePath
        tiny_scene = [bool]$TinyScene
        threads = $Threads
        jit = [bool]($Jit -or $JitStats)
        jit_stats = [bool]$JitStats
        instruction_limit = $InstructionLimit
        configuration = $Configuration
        wall_seconds = $watch.Elapsed.TotalSeconds
        power_before = $powerBefore
        power_after = Get-ProbePower
        width = $width
        height = $height
        image = $image
        image_sha256 = (Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash
        log = $log
    } | ConvertTo-Json | Set-Content -LiteralPath ([IO.Path]::ChangeExtension($log, '.json'))
}
exit $code
