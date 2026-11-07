# Runs the command-line toolchain checks under alchemy-fx and prints one line per check.
#
#   scripts/apps-wave/run_toolchains.ps1 -Exe <build>\Release\alchemy-fx.exe [-Toolchains C:\dev\apps-wave\toolchains]
#       [-Work C:\dev\apps-wave\work] [-Out C:\dev\apps-wave\runs\<stamp>] [-Only clang,node,...]
#
# The toolchains are the official prebuilt macOS arm64 releases listed in the apps-wave manifest, extracted under
# -Toolchains. -Work is a scratch directory on the same drive: the fixtures are copied there, and guest paths name it as
# /<path without drive>. Every guest run is cloaked and loopback-only (toolchain_common.ps1).
#
# Link stubs: one guest run with ALCHEMY_REGISTRATION_AUDIT writes Alchemy's export inventory, and
# inventory_to_tbd.py turns it into .tbd files under <Work>\stubs. Linking needs ALCHEMY_PROBE_MISSING=trap until
# libxml2 has exports (lld binds xml* without calling them). cmake needs =zero until NXGetArchInfoFromCpuType exists.

param(
    [Parameter(Mandatory)] [string]$Exe,
    [string]$Toolchains = 'C:\dev\apps-wave\toolchains',
    [string]$Work = 'C:\dev\apps-wave\work',
    [string]$Out = (Join-Path 'C:\dev\apps-wave\runs' (Get-Date -Format 'yyyyMMdd-HHmmss')),
    [string]$RuntimeDir = '',
    [string[]]$Only = @()
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'toolchain_common.ps1')

$Exe = (Resolve-Path -LiteralPath $Exe).Path
if (-not $RuntimeDir) {
    # The runtimes live in the main checkout's artifacts, which worktrees share.
    $common = (git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir).Trim()
    $RuntimeDir = Join-Path (Split-Path -Parent $common) 'artifacts\alchemy-llvm-arm64'
}
$rt = @('--runtime-dir', $RuntimeDir)
$llvm = Join-Path $Toolchains 'llvm-23.1.5-xz\LLVM-23.1.5-macOS-ARM64\bin'
$node = Join-Path $Toolchains 'node-26.13.0\node-v26.13.0-darwin-arm64\bin\node'
$npmCli = ConvertTo-AwGuestPath (Join-Path $Toolchains 'node-26.13.0\node-v26.13.0-darwin-arm64\lib\node_modules\npm\bin\npm-cli.js')
$rustc = Join-Path $Toolchains 'rust-1.99.1\rust-1.99.1-aarch64-apple-darwin\rustc\bin\rustc'
$cmake = Join-Path $Toolchains 'cmake-4.4.5\cmake-4.4.5-macos-universal\CMake.app\Contents\bin\cmake'
$ninja = Join-Path $Toolchains 'ninja-1.13.2\ninja'
$go = Join-Path $Toolchains 'go-1.27.3\go\bin\go'
$guestClang = ConvertTo-AwGuestPath (Join-Path $llvm 'clang')
$guestStubs = ConvertTo-AwGuestPath (Join-Path $Work 'stubs')
$trap = @{ ALCHEMY_PROBE_MISSING = 'trap' }
$zero = @{ ALCHEMY_PROBE_MISSING = 'zero' }

New-Item -ItemType Directory -Force $Work, $Out | Out-Null
Copy-Item -Recurse -Force (Join-Path $PSScriptRoot 'fixtures\*') $Work
$results = [System.Collections.Generic.List[object]]::new()

function Test-Wanted([string]$Group) { $Only.Count -eq 0 -or $Only -contains $Group }

# Runs one guest command; $Expect is a regex its stdout must match (empty: exit 0 is enough).
function Step([string]$Group, [string]$Name, [string]$Program, [string[]]$Arguments, [hashtable]$Environment = @{},
              [string]$Expect = '', [int]$Timeout = 600, [string]$Directory = $Work, [switch]$NoRuntime) {
    if (-not (Test-Wanted $Group)) { return }
    $options = if ($NoRuntime) { @() } else { $rt }
    $run = Invoke-AwGuest -Exe $Exe -Program $Program -Arguments $Arguments -AlchemyOptions $options -OutDir $Out -Name $Name `
        -WorkingDirectory $Directory -Environment $Environment -TimeoutSeconds $Timeout
    $stdout = (Get-Content -LiteralPath $run.Stdout -Raw -ErrorAction SilentlyContinue) + ''
    $ok = -not $run.TimedOut -and $run.ExitCode -eq 0 -and ($Expect -eq '' -or $stdout -match $Expect)
    $why = ''
    if (-not $ok) {
        $why = if ($run.TimedOut) { 'timeout' } else {
            (Get-Content -LiteralPath $run.Stderr -ErrorAction SilentlyContinue |
                Where-Object { $_ -notmatch '^\[alchemy probe\] function' } | Select-Object -First 1) + ''
        }
    }
    $line = [pscustomobject]@{ Check = $Name; Ok = $ok; Exit = $run.ExitCode; Seconds = $run.Seconds; Note = $why }
    $results.Add($line)
    Write-Host ('{0,-24} {1,-5} {2,7}s  {3}' -f $Name, $(if ($ok) { 'ok' } else { 'FAIL' }), $run.Seconds, $why)
}

# Link stubs from the export inventory.
if ((Test-Wanted 'clang') -or (Test-Wanted 'rust') -or (Test-Wanted 'cmake')) {
    $inventory = Join-Path $Out 'inventory.tsv'
    Step stubs inventory $ninja @('--version') -Environment @{ ALCHEMY_REGISTRATION_AUDIT = $inventory }
    python -I (Join-Path $PSScriptRoot 'inventory_to_tbd.py') $inventory (Join-Path $Work 'stubs') | Out-Null
    Copy-Item -Force (Join-Path $Work 'libc++.tbd') (Join-Path $Work 'stubs\libc++.tbd')
}

$versions = @(
    @('ninja', $ninja, '--version'), @('cmake', $cmake, '--version'), @('node', $node, '--version'),
    @('rust', $rustc, '--version'), @('go', $go, 'version'), @('clang', (Join-Path $llvm 'clang'), '--version'),
    @('clang', (Join-Path $llvm 'ld64.lld'), '--version'), @('clang', (Join-Path $llvm 'llvm-ar'), '--version'))
foreach ($v in $versions) {
    $environment = if ($v[1] -match 'cmake|lld') { $trap } else { @{} }
    Step $v[0] ("version-" + (Split-Path -Leaf $v[1])) $v[1] @($v[2]) -Environment $environment -Timeout 300
}

# C and C++: compile, link, run.
Step clang clang-c (Join-Path $llvm 'clang') @('--target=arm64-apple-macos11', '-O2', '-c', 'hello.c', '-o', 'hello.o')
Step clang lld-link (Join-Path $llvm 'ld64.lld') @('-arch', 'arm64', '-platform_version', 'macos', '11.0', '11.0', 'hello.o',
    '-Lstubs', '-lSystem', '-o', 'hello') -Environment $trap
Step clang run-hello (Join-Path $Work 'hello') @('a', 'b') -Expect 'hello from clang, argc=3' -NoRuntime
Step clang clangxx-driver (Join-Path $llvm 'clang++') @('--target=arm64-apple-macos11', '-fuse-ld=lld', '-O2', 'hello.cpp',
    '-Lstubs', '-o', 'hello_cpp') -Environment $trap
Step clang run-hello-cpp (Join-Path $Work 'hello_cpp') @() -Expect 'sides=4 caught=-7'
Step clang clang-argv (Join-Path $llvm 'clang') @('--target=arm64-apple-macos11', '-fuse-ld=lld', 'argv_layout.c', '-Lstubs',
    '-o', 'argv_layout') -Environment $trap
Step clang run-argv-layout (Join-Path $Work 'argv_layout') @('one', 'two-two', 'three') -NoRuntime

# Rust: rustc spawns clang, which spawns ld64.lld.
Step rust rustc-hello $rustc @('hello.rs', '-O', '-C', "linker=$guestClang", '-C', 'link-arg=-fuse-ld=lld', '-C',
    "link-arg=-L$guestStubs", '-o', 'hello_rs') -Environment $trap -Timeout 1200
Step rust run-hello-rs (Join-Path $Work 'hello_rs') @('x') -Expect 'threads sum=60, args=2' -Timeout 120

# CMake + ninja: configure, build, run.
if (Test-Wanted 'cmake') { Remove-Item -Recurse -Force (Join-Path $Work 'cmake-build') -ErrorAction SilentlyContinue }
Step cmake cmake-configure $cmake @('-S', 'cmake-hello', '-B', 'cmake-build', '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$(ConvertTo-AwGuestPath $ninja)", "-DCMAKE_C_COMPILER=$guestClang",
    "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -L$guestStubs", "-DCMAKE_OSX_SYSROOT=$guestStubs",
    '-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0') -Environment $zero -Timeout 1800
Step cmake cmake-build $cmake @('--build', 'cmake-build') -Environment $zero -Timeout 1800
Step cmake run-cmake-hello (Join-Path $Work 'cmake-build\hello') @() -Expect 'hello from clang' -NoRuntime -Timeout 120

# Node and npm (offline, with HOME and the npm cache inside -Work).
$npmEnv = @{ HOME = (ConvertTo-AwGuestPath (Join-Path $Work 'home')); npm_config_cache = (ConvertTo-AwGuestPath (Join-Path $Work 'home\.npm'))
             npm_config_offline = 'true'; npm_config_update_notifier = 'false' }
New-Item -ItemType Directory -Force (Join-Path $Work 'home') | Out-Null
Step node node-hello $node @('hello.js') -Expect 'hello from node .* value=42'
Step node node-procs $node @('procs.js') -Expect '(?s)child: 2.*worker: 42.*pipe: through a pipe.*signal: SIGTERM'
Step node node-title $node @('-e', 'process.title=1;console.log(2)') -Expect '2'
Step node npm-version $node @($npmCli, '--version') -Environment $npmEnv -Directory (Join-Path $Work 'npm-hello')
Step node npm-pack $node @($npmCli, 'pack') -Environment $npmEnv -Directory (Join-Path $Work 'npm-hello')

# Go (blocked at load today).
Step go go-run $go @('run', 'hello.go') -Expect 'hello from go' -Environment @{ GOCACHE = (ConvertTo-AwGuestPath (Join-Path $Work 'home\gocache')); GOPATH = (ConvertTo-AwGuestPath (Join-Path $Work 'home\go')); GOTOOLCHAIN = 'local'; GOFLAGS = '-mod=mod' } -NoRuntime -Timeout 1200

$results | Export-Csv -NoTypeInformation -LiteralPath (Join-Path $Out 'summary.csv')
$passed = @($results | Where-Object Ok).Count
Write-Host "$passed of $($results.Count) checks passed; outputs in $Out"
