param(
    [Parameter(Mandatory = $true)] [string]$Stage,
    [string]$RuntimeExecutable,
    [string]$PythonScript,
    [long]$InstructionLimit = [long]::MaxValue,
    [switch]$FactoryStartup,
    [switch]$Audio,
    [switch]$Diagnose,
    [switch]$DiagnoseSync,
    [switch]$SnapshotRuntime,
    [switch]$DebugGpu,
    [ValidateCount(4, 4)] [int[]]$WindowGeometry,
    # fx: the production engine (transmute-fx.exe, native threads); legacy: transmute.exe --jit.
    [ValidateSet('fx', 'legacy')] [string]$Engine = 'fx',
    # Vulkan is the default under fx: same frame rate as native, no driver-thread crash seen.
    [ValidateSet('opengl', 'vulkan')] [string]$GpuBackend = 'vulkan'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($InstructionLimit -lt 1) { throw 'InstructionLimit must be positive' }
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$stagePath = (Resolve-Path -LiteralPath $Stage).Path
$runtime = if ($RuntimeExecutable) { (Resolve-Path -LiteralPath $RuntimeExecutable).Path }
    elseif ($Engine -eq 'fx') { Join-Path $repository 'build-fx-integ\Release\transmute-fx.exe' }
    else { Join-Path $repository 'build\Release\transmute.exe' }
$blender = Join-Path $stagePath 'blender.exe'
$shared = Join-Path $stagePath 'blender.shared'
$crt = Join-Path $stagePath 'blender.crt'
foreach ($path in @($runtime, $blender, $shared, $crt)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required path was not found: $path" }
}
$artifactDirectory = Join-Path $repository 'artifacts'
New-Item -ItemType Directory -Path $artifactDirectory -Force | Out-Null
$prefix = Join-Path $artifactDirectory ('blender-gui-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
if ($SnapshotRuntime) {
    $runtimeSnapshot = $prefix + '.runtime.exe'
    Copy-Item -LiteralPath $runtime -Destination $runtimeSnapshot
    $runtime = $runtimeSnapshot
}
if ($Engine -eq 'fx') {
    # fx resolves blender.shared/blender.crt through the guest manifest search path
    # and has no instruction budget or legacy diagnostics switches.
    $arguments = @(('"' + $blender + '"'), '--gpu-backend', $GpuBackend)
} else {
    $runtimeLimit = $InstructionLimit.ToString()
    $arguments = @('--jit', '--instruction-limit', $runtimeLimit,
        '--dll-dir', ('"' + $shared + '"'), '--dll-dir', ('"' + $crt + '"'),
        ('"' + $blender + '"'), '--gpu-backend', $GpuBackend)
    if ($Diagnose) { $arguments = @('--diagnose-exceptions') + $arguments }
    if ($DiagnoseSync) { $arguments = @('--diagnose-sync') + $arguments }
}
if ($WindowGeometry) {
    if ($WindowGeometry[2] -le 0 -or $WindowGeometry[3] -le 0) {
        throw 'Window width and height must be positive'
    }
    $arguments += @('--window-geometry') + @($WindowGeometry | ForEach-Object { "$_" })
}
if ($FactoryStartup) { $arguments += '--factory-startup' }
if (-not $Audio) { $arguments += '-noaudio' }
if ($DebugGpu) { $arguments += '--debug-gpu' }
if ($PythonScript) {
    $scriptPath = (Resolve-Path -LiteralPath $PythonScript).Path
    $arguments += @('--python', ('"' + $scriptPath + '"'))
}
# This is an interactive GUI launch. SW_HIDE can suppress Blender's first
# ShowWindow and prevent its default framebuffer from being presented.
$process = Start-Process -FilePath $runtime -ArgumentList $arguments -WindowStyle Normal `
    -WorkingDirectory $stagePath -RedirectStandardOutput ($prefix + '.out.log') `
    -RedirectStandardError ($prefix + '.err.log') -PassThru
[pscustomobject]@{
    ProcessId = $process.Id
    RuntimeExecutable = $runtime
    StandardOutput = $prefix + '.out.log'
    StandardError = $prefix + '.err.log'
    Audio = [bool]$Audio
    Engine = $Engine
    GpuBackend = $GpuBackend
    Note = 'Launch requested. A process ID alone does not confirm that Blender opened its GUI.'
}
