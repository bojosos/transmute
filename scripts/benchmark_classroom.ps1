param(
    [Parameter(Mandatory = $true)] [string] $Blender,
    [Parameter(Mandatory = $true)] [string] $Scene,
    [Parameter(Mandatory = $true)] [string] $OutputDirectory,
    [string] $Transmute,
    [string[]] $DllDirectory = @(),
    [switch] $Jit,
    [UInt64] $InstructionLimit = 1000000000000,
    [ValidateRange(1, 100000)] [int] $Samples = 16,
    [ValidateRange(1, 100)] [int] $ResolutionPercentage = 100,
    [ValidateRange(1, 256)] [int] $Threads = 8,
    [ValidateRange(0, 10)] [int] $Warmups = 1,
    [ValidateRange(1, 20)] [int] $Runs = 3
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'benchmark_power.ps1')
$blenderPath = (Resolve-Path -LiteralPath $Blender).Path
$scenePath = (Resolve-Path -LiteralPath $Scene).Path
$transmutePath = if ($Transmute) { (Resolve-Path -LiteralPath $Transmute).Path } else { $null }
if ($Jit -and -not $transmutePath) {
    throw '-Jit requires -Transmute'
}
$dllArguments = @()
foreach ($directory in $DllDirectory) {
    $dllArguments += @('--dll-dir', (Resolve-Path -LiteralPath $directory).Path)
}
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($outputPath) | Out-Null

$records = @()
for ($index = 0; $index -lt $Warmups + $Runs; $index++) {
    $isWarmup = $index -lt $Warmups
    $name = if ($isWarmup) { 'warmup_{0:D2}' -f $index } else { 'run_{0:D2}' -f ($index - $Warmups) }
    $prefix = Join-Path $outputPath ($name + '_#####')
    $image = Join-Path $outputPath ($name + '_00001.png')
    $log = Join-Path $outputPath ($name + '.log')
    $configure = "import bpy; bpy.context.scene.cycles.samples = $Samples; bpy.context.scene.render.resolution_percentage = $ResolutionPercentage"
    $renderArguments = @('--factory-startup', '-b', $scenePath, '-E', 'CYCLES',
                         '-t', "$Threads", '-o', $prefix, '-F', 'PNG',
                         '--python-expr', $configure, '-f', '1', '--', '--cycles-device', 'CPU')

    $started = [DateTime]::UtcNow
    $powerBefore = Get-ProbePower
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    if ($transmutePath) {
        $translatorArguments = if ($Jit) { @('--jit') } else { @() }
        & $transmutePath '--instruction-limit' "$InstructionLimit" @translatorArguments `
            @dllArguments $blenderPath @renderArguments *> $log
    } else {
        & $blenderPath @renderArguments *> $log
    }
    $exitCode = $LASTEXITCODE
    $timer.Stop()
    if ($exitCode -ne 0) {
        throw "$name exited with code $exitCode. See $log"
    }
    if (-not (Test-Path -LiteralPath $image)) {
        throw "$name did not produce $image. See $log"
    }
    if ((Get-Item -LiteralPath $image).LastWriteTimeUtc -lt $started) {
        throw "$name left a stale image at $image. See $log"
    }

    $renderTime = $null
    $timeLine = Get-Content -LiteralPath $log | Select-String '^Time:\s+(\d{2}:\d{2}\.\d+)' | Select-Object -Last 1
    if ($timeLine) {
        $renderTime = $timeLine.Matches[0].Groups[1].Value
    }

    $bytes = [System.IO.File]::ReadAllBytes($image)
    if ($bytes.Length -lt 24 -or $bytes[0] -ne 137 -or $bytes[1] -ne 80 -or
        $bytes[2] -ne 78 -or $bytes[3] -ne 71) {
        throw "$name produced an invalid PNG"
    }
    $width = [int]($bytes[16] * 16777216 + $bytes[17] * 65536 + $bytes[18] * 256 + $bytes[19])
    $height = [int]($bytes[20] * 16777216 + $bytes[21] * 65536 + $bytes[22] * 256 + $bytes[23])
    $expectedWidth = [int][math]::Floor(1920 * $ResolutionPercentage / 100)
    $expectedHeight = [int][math]::Floor(1080 * $ResolutionPercentage / 100)
    if ($width -ne $expectedWidth -or $height -ne $expectedHeight) {
        throw "$name produced ${width}x${height}, expected ${expectedWidth}x${expectedHeight} Classroom frame"
    }
    $record = [pscustomobject]@{
        name = $name
        warmup = $isWarmup
        wall_seconds = [math]::Round($timer.Elapsed.TotalSeconds, 3)
        power_before = $powerBefore
        power_after = Get-ProbePower
        blender_render_time = $renderTime
        width = $width
        height = $height
        output_bytes = $bytes.Length
        image = $image
        log = $log
    }
    $records += $record
    Write-Output $record
}

$measured = @($records | Where-Object { -not $_.warmup } | Sort-Object wall_seconds)
$middle = [int][math]::Floor($measured.Count / 2)
$median = if ($measured.Count % 2 -eq 1) {
    $measured[$middle].wall_seconds
} else {
    ($measured[$middle - 1].wall_seconds + $measured[$middle].wall_seconds) / 2
}
$report = [pscustomobject]@{
    blender = $blenderPath
    blender_sha256 = (Get-FileHash -LiteralPath $blenderPath -Algorithm SHA256).Hash
    scene = $scenePath
    scene_sha256 = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
    transmute = $transmutePath
    jit = [bool]$Jit
    cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name)
    samples = $Samples
    resolution_percentage = $ResolutionPercentage
    threads = $Threads
    median_wall_seconds = $median
    runs = $records
}
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outputPath 'result.json')
Write-Output "Median measured wall time: $median seconds"
