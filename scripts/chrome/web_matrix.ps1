# The real-web compatibility matrix (docs/ALCHEMY_CHROME_AUDIT.md, "Real web and performance"): each site in a fresh
# windowed Chrome under alchemy-fx (web_visit.ps1: cloaked, fresh anonymous profile, network open with the update and
# telemetry hosts refused), one site per browser, ANGLE and Graphite on SwiftShader with V8's JIT unless -Cpu/-Jitless.
# With -Native <chrome.exe> the same sites run in a native Windows Chrome (the baseline; -Exe is then ignored).
# Usage: web_matrix.ps1 -Exe <build>\Release\alchemy-fx.exe -Out <scratch dir> [-Batch 1|2] [-Only name,name] [-Cpu]
#          [-Jitless] [-Native <chrome.exe>] [-Root <alchemy-root-chrome>] [-RuntimeDir <alchemy-llvm-arm64>]
# -Batch 2 is the second set of sites: logged-out landing pages of large web apps, a read-only editor view and a WebGL
# demo (frame rate read by the fps action). Nothing is typed, submitted, bought or signed into.
# Output: <Out>\<site>\ (web_visit.ps1's files) and the CPU load before each site in <Out>\<site>\cpu.txt.
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Out,
    [int]$Batch = 1,
    [string[]]$Only = @(),
    [switch]$Cpu,
    [switch]$Jitless,
    [string]$Native = '',
    [string]$Root = '',
    [string]$RuntimeDir = ''
)
# The checkout's artifacts by default ($PSScriptRoot is empty in parameter defaults under powershell -File).
$checkout = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Root) { $Root = Join-Path $checkout 'artifacts\alchemy-root-chrome' }
if (-not $RuntimeDir) { $RuntimeDir = Join-Path $checkout 'artifacts\alchemy-llvm-arm64' }
$ErrorActionPreference = 'Stop'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
# name, URL, action (web_visit.ps1), settle seconds
$sites = @(
    @('example', 'https://example.com/', 'none', 5),
    @('wiki-article', 'https://en.wikipedia.org/wiki/Alan_Turing', 'scroll', 5),
    @('wiki-search', 'https://en.wikipedia.org/w/index.php?search=web+browser+engine&fulltext=1&ns0=1', 'none', 5),
    @('bbc-news', 'https://www.bbc.com/news', 'scroll', 8),
    @('github-repo', 'https://github.com/torvalds/linux', 'none', 8),
    @('github-file', 'https://github.com/python/cpython/blob/main/README.rst', 'none', 8),
    @('mdn', 'https://developer.mozilla.org/en-US/docs/Web/JavaScript', 'none', 5),
    @('python-docs', 'https://docs.python.org/3/library/asyncio.html', 'none', 5),
    @('youtube-front', 'https://www.youtube.com/', 'reject-consent', 10),
    @('youtube-video', 'https://www.youtube.com/watch?v=aqz-KE-bpKQ', 'reject-consent+video', 10),
    @('google-search', 'https://www.google.com/search?q=alchemy+compatibility+layer', 'none', 5),
    @('google-maps', 'https://www.google.com/maps/@48.8584,2.2945,15z', 'reject-consent', 25),
    @('google-fonts', 'https://fonts.google.com/specimen/Roboto', 'none', 10),
    @('cjk-ja-wiki', 'https://ja.wikipedia.org/wiki/%E6%97%A5%E6%9C%AC%E8%AA%9E', 'none', 5),
    @('emoji-wiki', 'https://en.wikipedia.org/wiki/Emoji', 'none', 5),
    @('hacker-news', 'https://news.ycombinator.com/', 'none', 3),
    @('guardian', 'https://www.theguardian.com/international', 'scroll', 8),
    @('react-dev', 'https://react.dev/', 'none', 5),
    @('stackoverflow', 'https://stackoverflow.com/questions/927358/how-do-i-undo-the-most-recent-local-commits-in-git', 'none', 8),
    @('cf-trace', 'https://www.cloudflare.com/cdn-cgi/trace', 'none', 2)
)
if ($Batch -eq 2) {
    $sites = @(
        @('reddit', 'https://www.reddit.com/', 'scroll', 10),
        @('x-profile', 'https://x.com/NASA', 'none', 10),
        @('amazon', 'https://www.amazon.com/', 'scroll', 10),
        @('wiki-edit', 'https://en.wikipedia.org/w/index.php?title=Alan_Turing&action=edit', 'none', 8),
        @('discord', 'https://discord.com/', 'scroll', 10),
        @('figma', 'https://www.figma.com/', 'scroll', 10),
        @('google-docs', 'https://www.google.com/docs/about/', 'scroll', 8),
        @('webgl-aquarium', 'https://webglsamples.org/aquarium/aquarium.html', 'fps', 15)
    )
}
foreach ($site in $sites) {
    if ($Only.Count -and -not ($Only -contains $site[0])) { continue }
    $dir = Join-Path $Out $site[0]
    $load = (Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average
    "=== $($site[0]) $(Get-Date -Format HH:mm:ss) cpu $load%"
    $arguments = @{ Exe = $Exe; OutDir = $dir; Url = @($site[1]); Name = @($site[0]); Action = @($site[2]); Settle = $site[3];
        Root = $Root; RuntimeDir = $RuntimeDir; Jit = (-not $Jitless); SwiftShader = (-not $Cpu -and -not $Native) }
    if ($Native) { $arguments.Native = $Native }
    & (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'web_visit.ps1') @arguments 2>&1 | Select-Object -Last 6
    "cpu-at-start $load" | Set-Content (Join-Path $dir 'cpu.txt')
}
