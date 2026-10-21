param(
    [Parameter(Mandatory = $true)] [string]$Stage,
    [string]$OutputDirectory = 'build\embree-probe',
    [long]$InstructionLimit = 50000000,
    [ValidateSet('load', 'device', 'newscene', 'scene', 'triangle')]
    [string]$Phase = 'scene',
    [ValidateRange(0, 5)] [int]$TriangleStep = 0,
    [switch]$RayQuery,
    [ValidateSet(1, 4, 8, 16)] [int]$PacketWidth = 16,
    [switch]$BlenderGeometry,
    [switch]$Instance,
    [ValidateSet(0, 16, 32, 64)] [int]$GridSize = 0,
    [switch]$Callbacks,
    [switch]$ExtendedApis,
    [switch]$DynamicUpdate,
    [switch]$ArgumentFilter,
    [switch]$SingleThread,
    [switch]$DiagnoseExceptions,
    [switch]$Jit,
    [switch]$BuildOnly,
    [switch]$Native
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($InstructionLimit -lt 1) { throw 'InstructionLimit must be positive' }
if ($RayQuery -and $Phase -ne 'triangle') { throw 'RayQuery requires -Phase triangle' }
if ($RayQuery -and $TriangleStep -ne 0) { throw 'RayQuery requires the full triangle phase' }
if ($Instance -and (-not $RayQuery -or $Phase -ne 'triangle')) {
    throw 'Instance requires -Phase triangle -RayQuery'
}
if ($GridSize -gt 0 -and $Phase -ne 'triangle') {
    throw 'GridSize requires -Phase triangle'
}
if ($Callbacks -and $GridSize -eq 0) {
    throw 'Callbacks requires a grid scene'
}
if ($ExtendedApis -and ($Phase -ne 'triangle' -or -not $RayQuery -or
        -not $Instance -or $BlenderGeometry)) {
    throw 'ExtendedApis requires -Phase triangle -RayQuery -Instance, without BlenderGeometry'
}
if ($DynamicUpdate -and ($Phase -ne 'triangle' -or -not $RayQuery)) {
    throw 'DynamicUpdate requires -Phase triangle -RayQuery'
}
if ($ArgumentFilter -and ($Phase -ne 'triangle' -or -not $RayQuery)) {
    throw 'ArgumentFilter requires -Phase triangle -RayQuery'
}
if ($Native -and ($Jit -or $DiagnoseExceptions)) {
    throw 'Jit and DiagnoseExceptions apply only to the ARM64 Transmute probe'
}

$stagePath = (Resolve-Path -LiteralPath $Stage).Path
$shared = Join-Path $stagePath 'blender.shared'
$crt = Join-Path $stagePath 'blender.crt'
if (-not (Test-Path -LiteralPath (Join-Path $shared 'embree4.dll') -PathType Leaf)) {
    throw 'The staged ARM64 embree4.dll was not found'
}
if (-not (Test-Path -LiteralPath $crt -PathType Container)) {
    throw 'The staged Blender CRT directory was not found'
}

$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$fixture = Join-Path $repository 'tests\fixtures\embree_smoke.c'
$definition = Join-Path $repository 'tests\fixtures\embree_smoke.def'
$transmute = Join-Path $repository 'build\Release\transmute.exe'
if (-not $Native -and -not $BuildOnly -and
    -not (Test-Path -LiteralPath $transmute -PathType Leaf)) {
    throw 'Build the Release transmute.exe before running this probe'
}

$llvmRoot = Get-ChildItem -LiteralPath (Join-Path $env:ProgramFiles 'Microsoft Visual Studio') `
    -Filter llvm-lib.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\VC\\Tools\\Llvm\\x64\\bin\\' } |
    Select-Object -First 1 -ExpandProperty DirectoryName
if (-not $llvmRoot) { throw 'Visual Studio x64 LLVM tools were not found' }

$output = [IO.Path]::GetFullPath((Join-Path $repository $OutputDirectory))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$machine = if ($Native) { 'x64' } else { 'arm64' }
$target = if ($Native) { 'x86_64-pc-windows-msvc' } else { 'arm64-pc-windows-msvc' }
$object = Join-Path $output "embree_smoke_$machine.obj"
$importLibrary = Join-Path $output "embree4_$machine.lib"
$executable = Join-Path $output "embree_smoke_$machine.exe"

& (Join-Path $llvmRoot 'llvm-lib.exe') "/machine:$machine" "/def:$definition" "/out:$importLibrary"
if ($LASTEXITCODE -ne 0) { throw 'Embree import-library generation failed' }
$phaseDefine = switch ($Phase) {
    'load' { '/DEMBREE_LOAD_ONLY' }
    'device' { '/DEMBREE_DEVICE_ONLY' }
    'newscene' { '/DEMBREE_NEW_SCENE_ONLY' }
    'triangle' { '/DEMBREE_TRIANGLE' }
    default { '/DEMBREE_SCENE' }
}
$configurationDefine = if ($SingleThread) { '/DEMBREE_SINGLE_THREAD' } else { '/DEMBREE_DEFAULT_THREADS' }
$triangleDefine = "/DEMBREE_TRIANGLE_STEP=$TriangleStep"
$rayDefine = if ($RayQuery) { '/DEMBREE_RAY_QUERY' } else { '/DEMBREE_NO_RAY_QUERY' }
$packetDefine = "/DEMBREE_PACKET_MAX=$PacketWidth"
$geometryDefine = if ($BlenderGeometry) { '/DEMBREE_BLENDER_GEOMETRY' } else { '/DEMBREE_STANDARD_GEOMETRY' }
$instanceDefine = if ($Instance) { '/DEMBREE_INSTANCE' } else { '/DEMBREE_NO_INSTANCE' }
$gridDefine = "/DEMBREE_GRID_SIZE=$GridSize"
$callbacksDefine = if ($Callbacks) { '/DEMBREE_CALLBACKS' } else { '/DEMBREE_NO_CALLBACKS' }
$extendedDefine = if ($ExtendedApis) { '/DEMBREE_EXTENDED_APIS' } else { '/DEMBREE_NO_EXTENDED_APIS' }
$dynamicDefine = if ($DynamicUpdate) { '/DEMBREE_DYNAMIC_UPDATE' } else { '/DEMBREE_NO_DYNAMIC_UPDATE' }
$filterDefine = if ($ArgumentFilter) { '/DEMBREE_ARGUMENT_FILTER' } else { '/DEMBREE_NO_ARGUMENT_FILTER' }
& (Join-Path $llvmRoot 'clang-cl.exe') "--target=$target" `
    /c /O2 /GS- $phaseDefine $configurationDefine $triangleDefine $rayDefine $packetDefine $geometryDefine $instanceDefine $gridDefine $callbacksDefine $extendedDefine $dynamicDefine $filterDefine "/Fo$object" $fixture
if ($LASTEXITCODE -ne 0) { throw 'Embree probe compilation failed' }
if ($Native) {
    & (Join-Path $llvmRoot 'lld-link.exe') "/machine:$machine" `
        /subsystem:console "/out:$executable" $object $importLibrary
}
else {
    & (Join-Path $llvmRoot 'lld-link.exe') "/machine:$machine" /entry:guest_entry `
        /subsystem:console /nodefaultlib "/out:$executable" $object $importLibrary
}
if ($LASTEXITCODE -ne 0) { throw 'Embree probe linking failed' }
if ($BuildOnly) {
    Write-Output "Built Embree fixture: $executable"
    exit 0
}

if ($Native) {
    $previousPath = $env:PATH
    try {
        $env:PATH = "$shared;$crt;$previousPath"
        & $executable
        $code = $LASTEXITCODE
    }
    finally {
        $env:PATH = $previousPath
    }
    Write-Output "Native Embree exit: $code"
    exit $code
}

$options = @('--instruction-limit', "$InstructionLimit")
if ($Jit) { $options += '--jit' }
if ($DiagnoseExceptions) { $options += '--diagnose-exceptions' }
& $transmute @options --dll-dir $shared --dll-dir $crt $executable
$code = $LASTEXITCODE
Write-Output "Transmute Embree exit: $code"
exit $code
