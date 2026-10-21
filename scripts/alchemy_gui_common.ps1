# Shared Win32 probing and log analysis for the Alchemy GUI scripts
# (alchemy_gui_harness.ps1, alchemy_blender_gui.ps1). Dot-source it.
#
# [AlchemyGuiWin32]:
#   MainWindow(pid)       the guest's main window: the largest visible
#                         top-level "AlchemyWindow" (the window server's class)
#                         of the process; else the largest visible top-level
#                         window that is not a dialog (#32770) or tooltip.
#   Dialogs(pid)          visible modal dialogs (#32770: MessageBoxW, NSAlert,
#                         TaskDialog) of the process.
#   DialogText(hwnd)      the dialog's title and its controls' text.
#   PressDefault(hwnd)    presses the default button (DM_GETDEFID, else OK)
#                         with WM_COMMAND; returns the button's text.
#   Capture(hwnd, path, client)
#                         PrintWindow(PW_RENDERFULLCONTENT); when that fails or
#                         returns only black, a BitBlt of the window's screen
#                         region. Returns "printwindow" or "screen".
#   PixelStats(path)      { non-black ratio, mean luma, distinct colours (max 4096) }
#                         of a PNG (channels <= 16 count as black).
#   Client(hwnd) / Bounds(hwnd) / Title(hwnd) / ClassName(hwnd)
#
# Get-AlchemyStopPoint <stdout> <stderr>: the run's stop point, preferring a
# guest signal, then an unsupported Objective-C message, then an "alchemy:"
# fatal line, then an explicit "unsupported" line, over informational stderr
# notes. Returns [ordered]@{ kind; line; detail }.
# Get-AlchemyPresentedFrames <stderr> [<stats file>]: the exact presented-frame
# count (at-exit "[alchemy present] totals:" line, else the
# ALCHEMY_PRESENT_STATS file), else the last "[alchemy present] frame <n>"
# (ALCHEMY_TRACE_PRESENT=1, sampled), else $null.
# Read-AlchemyPresentStats / Get-AlchemyPresentSummary / New-AlchemyFrameSampler:
# the stats file, and status.json's present_stats with periodic samples.

if (-not ('AlchemyGuiWin32' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
// Reads a redirected stream on its own thread, in order.
public sealed class AlchemyLinePump {
    readonly StringBuilder all = new StringBuilder();
    readonly System.Collections.Concurrent.ConcurrentQueue<string> fresh = new System.Collections.Concurrent.ConcurrentQueue<string>();
    readonly System.Threading.Thread thread;
    public AlchemyLinePump(System.IO.StreamReader reader) {
        thread = new System.Threading.Thread(() => {
            string line;
            while ((line = reader.ReadLine()) != null) {
                lock (all) all.AppendLine(line);
                fresh.Enqueue(line);
            }
        });
        thread.IsBackground = true;
        thread.Start();
    }
    public string Text { get { lock (all) return all.ToString(); } }
    public bool TryDequeue(out string line) { return fresh.TryDequeue(out line); }
    // True when the stream ended within `milliseconds`.
    public bool Wait(int milliseconds) { return thread.Join(milliseconds); }
}
public static class AlchemyGuiWin32 {
    delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc callback, IntPtr parameter);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr window, ref POINT point);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SendMessageTimeout(IntPtr window, uint message,
        IntPtr w, StringBuilder l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr w, IntPtr l,
        uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr dialog, int id);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    static AlchemyGuiWin32() {
        // Physical pixels (per-monitor v2), like alchemy-fx's window server.
        try { if (!SetProcessDpiAwarenessContext(new IntPtr(-4))) SetProcessDPIAware(); } catch { SetProcessDPIAware(); }
    }

    // WM_GETTEXT with a timeout: a hung guest UI thread must not hang the probe.
    public static string Title(IntPtr window) {
        var text = new StringBuilder(4096);
        IntPtr ignored;
        if (SendMessageTimeout(window, 0x000D, new IntPtr(text.Capacity), text, 0x0002, 500, out ignored) == IntPtr.Zero) return "";
        return text.ToString();
    }
    public static string ClassName(IntPtr window) {
        var text = new StringBuilder(256);
        GetClassName(window, text, text.Capacity);
        return text.ToString();
    }
    public static bool Alive(IntPtr window) { return IsWindow(window); }
    public static int[] Bounds(IntPtr window) {
        RECT rect;
        GetWindowRect(window, out rect);
        return new int[] { rect.Left, rect.Top, rect.Right - rect.Left, rect.Bottom - rect.Top };
    }
    // Client area in screen pixels: x, y, width, height.
    public static int[] Client(IntPtr window) {
        RECT rect;
        GetClientRect(window, out rect);
        var origin = new POINT();
        ClientToScreen(window, ref origin);
        return new int[] { origin.X, origin.Y, rect.Right - rect.Left, rect.Bottom - rect.Top };
    }
    static List<IntPtr> TopLevel(uint process) {
        var found = new List<IntPtr>();
        EnumWindows((window, parameter) => {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            RECT rect;
            if (owner == process && IsWindowVisible(window) && GetWindowRect(window, out rect) &&
                rect.Right > rect.Left && rect.Bottom > rect.Top) found.Add(window);
            return true;
        }, IntPtr.Zero);
        return found;
    }
    static long Area(IntPtr window) { var b = Bounds(window); return (long)b[2] * b[3]; }
    public static IntPtr MainWindow(uint process) {
        IntPtr best = IntPtr.Zero, fallback = IntPtr.Zero;
        foreach (var window in TopLevel(process)) {
            var name = ClassName(window);
            if (name == "AlchemyWindow") { if (best == IntPtr.Zero || Area(window) > Area(best)) best = window; }
            else if (name != "#32770" && name != "tooltips_class32" && name != "IME" && name != "MSCTFIME UI" &&
                     (fallback == IntPtr.Zero || Area(window) > Area(fallback))) fallback = window;
        }
        return best != IntPtr.Zero ? best : fallback;
    }
    public static IntPtr[] Windows(uint process) { return TopLevel(process).ToArray(); }
    public static IntPtr[] Dialogs(uint process) {
        var found = new List<IntPtr>();
        foreach (var window in TopLevel(process)) if (ClassName(window) == "#32770") found.Add(window);
        return found.ToArray();
    }
    static List<IntPtr> Children(IntPtr parent) {
        var found = new List<IntPtr>();
        EnumChildWindows(parent, (child, parameter) => { found.Add(child); return true; }, IntPtr.Zero);
        return found;
    }
    // "<title> | <static text> | [<button>] ..." (controls in Z order).
    public static string DialogText(IntPtr dialog) {
        var parts = new List<string>();
        var title = Title(dialog);
        if (title.Length > 0) parts.Add(title);
        foreach (var child in Children(dialog)) {
            if (!IsWindowVisible(child)) continue;
            var name = ClassName(child);
            var text = Title(child);
            if (text.Length == 0) continue;
            parts.Add(name == "Button" ? "[" + text.Replace("&", "") + "]" : text);
        }
        if (parts.Count == 0) return "(no readable text)";
        return string.Join(" | ", parts.ToArray()).Replace("\r", "").Replace("\n", " ");
    }
    // Presses the default button: DM_GETDEFID, else a button titled OK, else
    // IDOK. WM_COMMAND(BN_CLICKED) to the dialog works without focus.
    public static string PressDefault(IntPtr dialog) {
        IntPtr result;
        int id = 0;
        if (SendMessageTimeout(dialog, 0x0400, IntPtr.Zero, IntPtr.Zero, 0x0002, 500, out result) != IntPtr.Zero &&
            ((result.ToInt64() >> 16) & 0xFFFF) == 0x534B) id = (int)(result.ToInt64() & 0xFFFF);
        IntPtr button = id != 0 ? GetDlgItem(dialog, id) : IntPtr.Zero;
        if (button == IntPtr.Zero) {
            foreach (var child in Children(dialog))
                if (ClassName(child) == "Button" && Title(child).Replace("&", "") == "OK") { button = child; id = GetDlgCtrlID(child); break; }
        }
        if (id == 0) id = 1;   // IDOK
        var label = button != IntPtr.Zero ? Title(button).Replace("&", "") : "IDOK";
        PostMessage(dialog, 0x0111, new IntPtr(id), button);   // WM_COMMAND, BN_CLICKED (high word 0)
        return label;
    }

    // ---- capture -----------------------------------------------------------
    // Blank: black, or uniformly the window server's unpresented background
    // (RGB 0x32,0x32,0x32, win32_server.hpp unpresented_background), which is
    // what PrintWindow returns for a directly flipped swapchain.
    static bool Blank(Bitmap bitmap) {
        var data = bitmap.LockBits(new Rectangle(0, 0, bitmap.Width, bitmap.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        try {
            var row = new byte[bitmap.Width * 4];
            bool black = true, background = true;
            for (int y = 0; y < bitmap.Height && (black || background); y += 4) {
                Marshal.Copy(IntPtr.Add(data.Scan0, y * data.Stride), row, 0, row.Length);
                for (int x = 0; x < row.Length; x += 16) {
                    if (row[x] > 16 || row[x + 1] > 16 || row[x + 2] > 16) black = false;
                    if (Math.Abs(row[x] - 0x32) > 2 || Math.Abs(row[x + 1] - 0x32) > 2 || Math.Abs(row[x + 2] - 0x32) > 2) background = false;
                }
            }
            return black || background;
        } finally { bitmap.UnlockBits(data); }
    }
    // ---- desktop state -----------------------------------------------------
    [DllImport("user32.dll")] static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool GetUserObjectInformation(IntPtr handle, int index,
        StringBuilder info, int length, out int needed);
    [DllImport("wtsapi32.dll")] static extern bool WTSQuerySessionInformationW(IntPtr server, int session, int info,
        out IntPtr buffer, out int bytes);
    [DllImport("wtsapi32.dll")] static extern void WTSFreeMemory(IntPtr memory);
    // WTSINFOEXW level 1 SessionFlags: WTS_SESSIONSTATE_LOCK (0) while the
    // lock screen (LockApp, still on the Default desktop) covers the session.
    public static bool SessionLocked() {
        IntPtr buffer; int bytes;
        if (!WTSQuerySessionInformationW(IntPtr.Zero, -1, 25, out buffer, out bytes)) return false;   // WTSSessionInfoEx
        try {
            if (bytes < 20 || Marshal.ReadInt32(buffer) != 1) return false;   // Level
            // WTSINFOEX_LEVEL1_W holds LARGE_INTEGERs, so the union after
            // Level is 8-byte aligned: SessionId 8, SessionState 12, SessionFlags 16.
            return Marshal.ReadInt32(buffer, 16) == 0;
        } finally { WTSFreeMemory(buffer); }
    }
    // False while the workstation is locked or another desktop (UAC, logon)
    // has the input: the screen then does not show the window.
    public static bool DesktopVisible() {
        if (SessionLocked()) return false;
        var desktop = OpenInputDesktop(0, false, 0x0001);   // DESKTOP_READOBJECTS
        if (desktop == IntPtr.Zero) return false;
        try {
            var name = new StringBuilder(256);
            int needed;
            if (!GetUserObjectInformation(desktop, 2, name, name.Capacity * 2, out needed)) return false;   // UOI_NAME
            return string.Equals(name.ToString(), "Default", StringComparison.OrdinalIgnoreCase);
        } finally { CloseDesktop(desktop); }
    }

    // QuartzCore's on-demand capture (ALCHEMY_PRESENT_CAPTURE=<directory>):
    // the next present of `window` writes its swapchain image. False when no
    // frame was presented within `timeout` ms (the guest is idle).
    public static bool PresentCapture(IntPtr window, string directory, string path, int timeout) {
        var bmp = path + ".present.bmp";
        var error = bmp + ".error";
        foreach (var stale in new[] { bmp, error }) if (System.IO.File.Exists(stale)) System.IO.File.Delete(stale);
        var request = System.IO.Path.Combine(directory, "request");
        System.IO.File.WriteAllText(request + ".tmp", window.ToInt64() + " " + bmp);
        if (System.IO.File.Exists(request)) System.IO.File.Delete(request);
        System.IO.File.Move(request + ".tmp", request);
        var clock = System.Diagnostics.Stopwatch.StartNew();
        while (clock.ElapsedMilliseconds < timeout) {
            if (System.IO.File.Exists(error)) throw new Exception(System.IO.File.ReadAllText(error).Trim());
            if (System.IO.File.Exists(bmp)) {
                using (var image = new Bitmap(bmp))
                using (var copy = image.Clone(new Rectangle(0, 0, image.Width, image.Height), PixelFormat.Format32bppRgb))
                    copy.Save(path, ImageFormat.Png);
                System.IO.File.Delete(bmp);
                return true;
            }
            System.Threading.Thread.Sleep(20);
        }
        if (System.IO.File.Exists(request)) System.IO.File.Delete(request);   // not consumed
        return false;
    }

    // Captures the window (client: only its client area) to `path`. Order:
    // PrintWindow(PW_RENDERFULLCONTENT) when it has content; QuartzCore's
    // present capture (presentDirectory set; client area only); a screen
    // BitBlt of the region (window raised, desktop visible); else the blank
    // PrintWindow image. Returns "printwindow", "present", "screen" or
    // "printwindow-blank".
    public static string Capture(IntPtr window, string path, bool client, string presentDirectory) {
        var bounds = Bounds(window);
        var area = Client(window);
        if (bounds[2] <= 0 || bounds[3] <= 0) throw new Exception("the window has no area");
        var crop = new Rectangle(0, 0, bounds[2], bounds[3]);
        if (client) {
            crop = new Rectangle(area[0] - bounds[0], area[1] - bounds[1], area[2], area[3]);
            crop.Intersect(new Rectangle(0, 0, bounds[2], bounds[3]));
            if (crop.Width <= 0 || crop.Height <= 0) throw new Exception("the client area is empty");
        }
        using (var full = new Bitmap(bounds[2], bounds[3], PixelFormat.Format32bppArgb)) {
            bool ok;
            using (var graphics = Graphics.FromImage(full)) {
                var dc = graphics.GetHdc();
                ok = PrintWindow(window, dc, 2);   // PW_RENDERFULLCONTENT: includes DWM-redirected swapchain content
                graphics.ReleaseHdc(dc);
            }
            using (var part = full.Clone(crop, PixelFormat.Format32bppArgb)) {
                // A directly flipped Vulkan swapchain is not in the DWM
                // redirection surface PrintWindow reads (and nothing is
                // composed while the workstation is locked): black client.
                if (ok && !Blank(part)) { part.Save(path, ImageFormat.Png); return "printwindow"; }
                if (client && !string.IsNullOrEmpty(presentDirectory) && PresentCapture(window, presentDirectory, path, 3000))
                    return "present";
                if (DesktopVisible()) {
                    SetWindowPos(window, new IntPtr(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010);   // HWND_TOPMOST, no activation
                    System.Threading.Thread.Sleep(200);
                    try {
                        using (var graphics = Graphics.FromImage(part))
                            graphics.CopyFromScreen(bounds[0] + crop.X, bounds[1] + crop.Y, 0, 0, new Size(crop.Width, crop.Height));
                    } finally {
                        SetWindowPos(window, new IntPtr(-2), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010);   // HWND_NOTOPMOST
                    }
                    part.Save(path, ImageFormat.Png);
                    return "screen";
                }
                part.Save(path, ImageFormat.Png);
                return "printwindow-blank";
            }
        }
    }
    // [non-black ratio, mean luma 0..255, distinct colours (capped at 4096)].
    public static double[] PixelStats(string path) {
        using (var bitmap = new Bitmap(path)) {
            var data = bitmap.LockBits(new Rectangle(0, 0, bitmap.Width, bitmap.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            try {
                long lit = 0, total = 0; double luma = 0;
                var colours = new HashSet<int>();
                var row = new byte[bitmap.Width * 4];
                for (int y = 0; y < bitmap.Height; ++y) {
                    Marshal.Copy(IntPtr.Add(data.Scan0, y * data.Stride), row, 0, row.Length);
                    for (int x = 0; x < row.Length; x += 4) {
                        int b = row[x], g = row[x + 1], r = row[x + 2];
                        if (r > 16 || g > 16 || b > 16) ++lit;
                        luma += 0.2126 * r + 0.7152 * g + 0.0722 * b;
                        if (colours.Count < 4096) colours.Add((r << 16) | (g << 8) | b);
                        ++total;
                    }
                }
                if (total == 0) return new double[] { 0, 0, 0 };
                return new double[] { (double)lit / total, luma / total, colours.Count };
            } finally { bitmap.UnlockBits(data); }
        }
    }
}
'@
}

# Informational stderr lines that never are a stop point.
$script:AlchemyInformational = @(
    '^\[alchemy (probe|present)\]', '^\[objc\]', '^Alchemy AppKit: NSAlert (accessory view|auto-dismissed)',
    '^\s*(Traceback|File "|\^+\s*$)', '^\s{2,}', '^(pc|lr) 0x', '^\s*#\d+ 0x'
)

function Get-AlchemyStopPoint([string] $Stdout, [string] $Stderr, $ExitCode = $null) {
    $lines = @($Stderr -split "`r?`n")
    $result = [ordered]@{ kind = 'none'; line = ''; detail = @() }
    $pick = {
        param([string] $kind, [string] $pattern, [bool] $last)
        $hits = @(for ($i = 0; $i -lt $lines.Count; ++$i) { if ($lines[$i] -match $pattern) { $i } })
        if (-not $hits.Count) { return $null }
        $index = if ($last) { $hits[-1] } else { $hits[0] }
        $detail = @()
        # A guest crash report continues with indented pc/lr/frame lines.
        for ($j = $index + 1; $j -lt $lines.Count -and $detail.Count -lt 6; ++$j) {
            if ($lines[$j] -match '^\s+(pc|lr|#\d+) ') { $detail += $lines[$j].Trim() } else { break }
        }
        [ordered]@{ kind = $kind; line = $lines[$index].Trim(); detail = $detail }
    }
    foreach ($rule in @(
            @('guest_signal', 'guest terminated by|EXC_BAD_ACCESS|\bSIG(SEGV|BUS|ILL|ABRT|FPE)\b', $false),
            @('unsupported_objc_message', 'unsupported Objective-C message', $false),
            @('alchemy_fatal', '^alchemy: ', $true),
            @('unsupported', 'unsupported', $true))) {
        $found = & $pick $rule[0] $rule[1] $rule[2]
        if ($found) { $result = $found; break }
    }
    # A host exception (NTSTATUS exit code such as 0xC0000005) ends the
    # process without a guest report; it outranks leftover stderr lines.
    if ($null -ne $ExitCode -and $result.kind -eq 'none' -and [int64]$ExitCode -lt 0) {
        $status = '0x{0:X8}' -f [int32]$ExitCode
        $result = [ordered]@{ kind = 'host_exception'; line = "host process ended with exception $status"; detail = @() }
        $last = @($lines | Where-Object { $_.Trim() } | Select-Object -Last 3)
        if ($last.Count) { $result.detail = @($last | ForEach-Object { $_.Trim() }) }
    }
    if ($result.kind -eq 'none') {
        $other = @($lines | Where-Object { $line = $_; $_.Trim() -and -not ($script:AlchemyInformational | Where-Object { $line -match $_ }) })
        if ($other.Count) { $result = [ordered]@{ kind = 'stderr'; line = $other[-1].Trim(); detail = @() } }
    }
    if ($result.line.Length -gt 600) { $result.line = $result.line.Substring(0, 600) }
    $result
}

function Get-AlchemyPresentedFrames([string] $Stderr, [string] $StatsPath = '') {
    # Exact: the at-exit totals, then QuartzCore's stats file.
    $totals = Get-AlchemyExitTotals $Stderr
    if ($totals) { return $totals.presented }
    if ($StatsPath) {
        $stats = Read-AlchemyPresentStats $StatsPath
        if ($stats) { return [long]$stats.presented }
    }
    # Sampled: the last traced frame number (frames 1-3, every 60th, changes).
    $frames = [regex]::Matches($Stderr, '\[alchemy present\] frame (\d+):')
    if ($frames.Count) { return [long]$frames[$frames.Count - 1].Groups[1].Value }
    $null
}

# "[alchemy present] totals: presented <n> skipped <m>" (ALCHEMY_TRACE_PRESENT,
# logged when the process exits normally) or $null.
function Get-AlchemyExitTotals([string] $Stderr) {
    $m = [regex]::Matches($Stderr, '\[alchemy present\] totals: presented (\d+) skipped (\d+)')
    if (-not $m.Count) { return $null }
    $last = $m[$m.Count - 1]
    [ordered]@{ presented = [long]$last.Groups[1].Value; skipped = [long]$last.Groups[2].Value }
}

# QuartzCore's ALCHEMY_PRESENT_STATS file:
#   { pid, presented, skipped, uptime_ms, final, windows: [{ window, presented, width, height }] }
# rewritten atomically about every 100 ms while frames are presented and with
# final = true at a normal exit. $null when absent or unreadable.
function Read-AlchemyPresentStats([string] $Path) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return $null }
    for ($attempt = 0; $attempt -lt 5; ++$attempt) {
        try { return [System.IO.File]::ReadAllText($Path) | ConvertFrom-Json }
        catch { Start-Sleep -Milliseconds 20 }   # being replaced
    }
    $null
}

# Periodic samples of the stats file for status.json: call Add-... from the
# polling loop; samples are taken at most every -EverySeconds.
function New-AlchemyFrameSampler([string] $StatsPath, [double] $EverySeconds = 1.0) {
    [ordered]@{ path = $StatsPath; every = $EverySeconds; last = -1e9; samples = (New-Object System.Collections.Generic.List[object]) }
}
function Add-AlchemyFrameSample($Sampler, [double] $Seconds, [switch] $Force) {
    if (-not $Sampler -or (-not $Force -and $Seconds - $Sampler.last -lt $Sampler.every)) { return }
    $stats = Read-AlchemyPresentStats $Sampler.path
    if (-not $stats) { return }
    $Sampler.last = $Seconds
    $Sampler.samples.Add([ordered]@{ t_s = [math]::Round($Seconds, 1); presented = [long]$stats.presented; skipped = [long]$stats.skipped })
}
# status.json's present_stats: the last stats file (final when the process
# exited normally), the stderr at-exit totals and the periodic samples.
function Get-AlchemyPresentSummary([string] $StatsPath, [string] $Stderr, $Sampler = $null) {
    $stats = Read-AlchemyPresentStats $StatsPath
    $totals = Get-AlchemyExitTotals $Stderr
    $source = if ($totals) { 'exit-totals' } elseif ($stats) { if ($stats.final) { 'stats-file-final' } else { 'stats-file-periodic' } } else { 'trace' }
    [ordered]@{
        source = $source
        presented = $(if ($totals) { $totals.presented } elseif ($stats) { [long]$stats.presented } else { Get-AlchemyPresentedFrames $Stderr })
        skipped = $(if ($totals) { $totals.skipped } elseif ($stats) { [long]$stats.skipped } else { $null })
        final = [bool]($totals -or ($stats -and $stats.final))
        exit_totals = $totals
        stats_file = $(if ($stats) { [ordered]@{ path = $StatsPath; presented = [long]$stats.presented; skipped = [long]$stats.skipped
            uptime_ms = [long]$stats.uptime_ms; final = [bool]$stats.final; windows = @($stats.windows) } } else { $null })
        samples = $(if ($Sampler) { [object[]]$Sampler.samples } else { @() })
    }
}

# Guest error lines on stdout (Blender's "ERROR (...)" reports, shader
# compile errors): the first few, for status.json.
function Get-AlchemyGuestErrors([string] $Stdout, [int] $Limit = 8) {
    @($Stdout -split "`r?`n" | Where-Object { $_ -match '^(ERROR|Error)\b|\bError: ' } | ForEach-Object { $_.Trim() } |
        Select-Object -Unique | Select-Object -First $Limit)
}
