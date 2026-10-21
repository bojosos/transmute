param(
    [Parameter(Mandatory)] [string] $Exe,
    [string[]] $ExeArgs = @(),
    [string] $Only = '',
    [string[]] $Bases = @('default', '0x120000000'),
    [int] $TimeoutSeconds = 900
)
# Python acceptance workloads (same arguments, environment and expected
# stdout as tests/alchemy_python_tests.cpp), run as separate processes.
$R = 'C:/dev/transmute'
$PH = "$R/artifacts/alchemy-blender/extracted/Blender/Blender.app/Contents/Resources/4.5/python"
$RT = "$R/artifacts/alchemy-python/runtime"
$MAC = "$R/build/alchemy-expansion/macos"
$FIX = "$R/tests/fixtures/macos"
function G([string] $p) { '/Volumes/C' + $p.Substring(2) }

function Quote([string] $a) {
    if ($a -notmatch '[\s"]') { return $a }
    '"' + ($a -replace '(\\*)"', '$1$1\"') + '"'
}

function RunCase($name, $launcher, $ext, $expected, $xexit, [string[]] $guestArgs) {
    if ($Only -and $name -notmatch $Only) { return }
    foreach ($base in $Bases) {
        $all = @() + $ExeArgs
        if ($base -ne 'default') { $all += @('--load-base', $base) }
        $all += @('--dylib-dir', $ext, $launcher) + $guestArgs
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $Exe
        $psi.Arguments = ($all | ForEach-Object { Quote $_ }) -join ' '
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.EnvironmentVariables.Clear()
        $psi.EnvironmentVariables['SystemRoot'] = 'C:\Windows'
        $psi.EnvironmentVariables['PYTHONHOME'] = (G $PH)
        $psi.EnvironmentVariables['PYTHONNOUSERSITE'] = '1'
        $psi.EnvironmentVariables['PYTHONDONTWRITEBYTECODE'] = '1'
        $psi.EnvironmentVariables['PYTHONUTF8'] = '1'
        $psi.EnvironmentVariables['LC_ALL'] = 'C'
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $p = [System.Diagnostics.Process]::Start($psi)
        $outTask = $p.StandardOutput.ReadToEndAsync()
        $errTask = $p.StandardError.ReadToEndAsync()
        if (-not $p.WaitForExit($TimeoutSeconds * 1000)) { $p.Kill(); $p.WaitForExit() }
        $sw.Stop()
        $out = $outTask.Result; $err = $errTask.Result
        $code = $p.ExitCode
        $ok = ($code -eq $xexit) -and ($out -eq "$expected`n")
        $v = if ($ok) { 'PASS' } else { 'FAIL' }
        '{0} {1,-34} {2,-11} {3,8} ms exit={4}' -f $v, $name, $base, $sw.ElapsedMilliseconds, $code
        if (-not $ok) {
            $o = $out; if ($o.Length -gt 300) { $o = $o.Substring(0, 300) }
            $e = $err; if ($e.Length -gt 900) { $e = $e.Substring($e.Length - 900) }
            '  stdout: ' + ($o -replace "`r?`n", '|')
            '  stderr: ' + ($e -replace "`r?`n", '|')
        }
    }
}

$CMD = "import sys, _struct, math, _json, _random, binascii; assert sys.platform == 'darwin'; assert sys.version_info[:2] == (3, 11); assert math.sqrt(729) == 27; assert _struct.unpack('<i', _struct.pack('<i', 27)) == (27,); assert binascii.hexlify(bytes([27, 42])) == b'1b2a'; assert _json.scanstring('`"Alchemy`"', 1)[0] == 'Alchemy'; assert 0 <= _random.Random(27).random() < 1; print('Guest CPython command: 27')"
$SITE = "import sys, site; assert 'site' in sys.modules; assert sys.flags.no_site == 0; assert any('site-packages' in item for item in sys.path); print('Guest CPython site: 27')"
$EXITC = "import sys; print('Guest CPython exit: 27'); sys.exit(27)"

foreach ($L in 'embed', 'original') {
    $LA = if ($L -eq 'embed') { "$RT/python_embed" } else { "$PH/bin/python3.11" }
    $tmp = G (((Get-Item $env:TEMP).FullName -replace '\\', '/') + "/alchemy-fx-workload-$L.bin")
    RunCase "$L version" $LA $RT 'Python 3.11.15' 0 @('--version')
    RunCase "$L command" $LA $RT 'Guest CPython command: 27' 0 @('-S', '-B', '-c', $CMD)
    RunCase "$L site" $LA $RT 'Guest CPython site: 27' 0 @('-B', '-c', $SITE)
    RunCase "$L exit" $LA $RT 'Guest CPython exit: 27' 27 @('-S', '-B', '-c', $EXITC)
    RunCase "$L workload" $LA $RT 'Guest CPython workload: 27' 0 @('-S', '-B', (G "$FIX/python_workload.py"), $tmp)
    RunCase "$L extensions" $LA $RT 'Guest CPython native extensions: 69' 0 @('-S', '-B', (G "$FIX/python_extensions.py"), (G $RT))
    RunCase "$L compression" $LA $RT 'Guest CPython compression: 27' 0 @('-S', '-B', (G "$FIX/python_compression.py"), (G $RT))
    RunCase "$L threads" $LA $RT 'Guest CPython threads: 27' 0 @('-S', '-B', (G "$FIX/python_threads.py"), (G $RT))
    RunCase "$L ctypes_calls" $LA $MAC 'Guest CPython ctypes: 69' 0 @('-S', '-B', (G "$FIX/python_ctypes_calls.py"), (G $MAC))
    RunCase "$L ctypes_callbacks" $LA $MAC 'Guest CPython ctypes callbacks: 69' 0 @('-S', '-B', (G "$FIX/python_ctypes.py"), (G $MAC))
    RunCase "$L network" $LA $RT 'Alchemy Python networking passed' 0 @('-S', '-B', (G "$FIX/python_network.py"), (G $RT))
    RunCase "$L numpy" $LA $RT 'Alchemy NumPy: arrays BLAS LU Cholesky' 0 @('-S', '-B', (G "$FIX/python_numpy.py"), (G $RT))
}
