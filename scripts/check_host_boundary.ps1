# Host boundary check (ctest alchemy_host_boundary; docs/ALCHEMY_BRIEF_REFACTOR.md).
#
# Outside the host backends (src/alchemy/host/<backend>/), fx's own host seams
# (src/fx/host/) and tests, no source may include an OS header: Windows
# (windows.h, winsock2.h, WinHTTP, COM, WASAPI, ...) or POSIX (unistd.h,
# sys/*, pthread.h, ...). Files that still do are listed in
# scripts/host_boundary_allow.txt, the to-do list of the host-boundary work:
# the check fails on an OS include in a file not listed there, and on a
# listed file that no longer has one (remove it from the list).
#
#   scripts/check_host_boundary.ps1 [-Root <repo>] [-List]
#     -List  prints every offending file with its OS headers and exits 0.
param([string] $Root = (Resolve-Path "$PSScriptRoot/..").Path, [switch] $List)
$ErrorActionPreference = 'Stop'

$osHeaders = '^(windows|winsock2?|ws2tcpip|ws2def|mswsock|afunix|winhttp|wininet|bcrypt|ncrypt|wincrypt|mmdeviceapi|audioclient|audiopolicy|endpointvolume|functiondiscoverykeys(_devpkey)?|mmsystem|mmreg|ks|ksmedia|dwrite(_\d)?|d2d1(_\d)?|d2d1helper|d3d1\d|dxgi(\d_\d)?|dwmapi|wincodec|shlobj(_core)?|shellapi|shlwapi|knownfolders|propvarutil|propkey|objbase|combaseapi|comdef|comip|unknwn|oleauto|ole2|wrl(/.*)?|winrt/.*|iphlpapi|netioapi|windns|winternl|ntstatus|winioctl|dbghelp|psapi|tlhelp32|userenv|sddl|aclapi|accctrl|lmcons|wtsapi32|powrprof|setupapi|cfgmgr32|hidsdi|hidpi|hidclass|devpkey|initguid|winbase|wingdi|winuser|winnls|winnt|winerror|imm|commctrl|commdlg|uxtheme|versionhelpers|timeapi|avrt|process|io|direct|conio|share|crtdbg|vulkan/vulkan_win32|unistd|pthread|dlfcn|fcntl|poll|sched|termios|netdb|ifaddrs|syslog|pwd|grp|spawn|semaphore|sys/.+|arpa/.+|netinet/.+|net/.+|mach/.+|linux/.+)\.h$'
$includePattern = '^\s*#\s*include\s*<([^>]+)>'

function Relative([string] $path) { $path.Substring($Root.Length + 1).Replace('\', '/') }
function Exempt([string] $rel) {
    $rel -match '^src/alchemy/host/(?!common/)[^/]+/' -or $rel -match '^src/fx/host/' -or $rel -match '(^|/)tests?/' -or $rel -match '_tests?\.(cpp|hpp|h)$' -or $rel -match '^src/fx/testing/' -or
    $rel -match '(test_support|_test_util|_test_machine)\.(cpp|hpp|h)$' -or
    $rel -match '/swift/'  # guest-side Swift overlay headers (compiled for macOS)
}

$files = foreach ($dir in 'include/alchemy', 'src/alchemy', 'src/fx') {
    $full = Join-Path $Root $dir
    if (Test-Path $full) {
        Get-ChildItem $full -Recurse -File -Include *.cpp, *.hpp, *.h, *.inc, *.cc, *.ipp
    }
}
$offenders = [ordered]@{}
foreach ($file in $files) {
    $rel = Relative $file.FullName
    if (Exempt $rel) { continue }
    $found = Select-String -Path $file.FullName -Pattern $includePattern -AllMatches |
        ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() } |
        Where-Object { $_.ToLowerInvariant() -match $osHeaders } | Sort-Object -Unique
    if ($found) { $offenders[$rel] = $found }
}

if ($List) {
    foreach ($key in $offenders.Keys) { "$key`t$($offenders[$key] -join ' ')" }
    "$($offenders.Count) files include OS headers outside the backends"
    exit 0
}

$allowFile = Join-Path $PSScriptRoot 'host_boundary_allow.txt'
$allowed = @{}
if (Test-Path $allowFile) {
    Get-Content $allowFile | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') } |
        ForEach-Object { $allowed[$_] = $true }
}
$failures = 0
foreach ($key in $offenders.Keys) {
    if (-not $allowed.ContainsKey($key)) {
        "NEW OS include outside the host backends: $key ($($offenders[$key] -join ', '))"
        $failures++
    }
}
foreach ($key in $allowed.Keys) {
    if (-not $offenders.Contains($key)) {
        "no longer includes an OS header (remove it from scripts/host_boundary_allow.txt): $key"
        $failures++
    }
}
"host boundary: $($offenders.Count) files with OS includes outside the backends ($($allowed.Count) allowed); $failures problems"
if ($failures) { exit 1 }
exit 0
