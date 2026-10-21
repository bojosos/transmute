param(
    # The Mach-O program (GUI fixture or Blender.app) run under alchemy-fx.
    [string] $Program = '',
    [string[]] $ProgramArgs = @(),
    [string] $Exe = "$PSScriptRoot/../build-wf-gui/Release/alchemy-fx.exe",
    # alchemy-fx --runtime-dir (default: artifacts/alchemy-llvm-arm64 when present).
    [string] $RuntimeDir = '',
    [string] $OutDir = "$PSScriptRoot/../artifacts/alchemy-gui-harness",
    # Actions, run in order once the window exists (see below). -ActionFile
    # reads one action per line ('#' starts a comment).
    [string[]] $Actions = @(),
    [string] $ActionFile = '',
    # Regex the window title must match (default: the program's largest
    # AlchemyWindow, never a modal #32770 dialog).
    [string] $WindowTitle = '',
    [int] $TimeoutSeconds = 120,
    # Regexes the program's stdout must match when the run ends.
    [string[]] $Expect = @(),
    # Keep the program running after the actions (default: stop it unless a
    # wait-exit action already saw it end).
    [switch] $KeepRunning,
    # Drives a native Windows Forms echo window instead of alchemy-fx, to
    # check SendInput, capture and compare on this desktop.
    [switch] $SelfTest,
    [string] $Python = 'python',
    # Answer every modal dialog (#32770: NSAlert, MessageBoxW) with its
    # default button between actions; their text is recorded in status.json.
    [switch] $DismissDialogs,
    # Do not set ALCHEMY_TRACE_PRESENT=1 (QuartzCore's presented-frame counter).
    [switch] $NoTracePresent,
    # A compare without a reference does not create one: the run is skipped
    # (exit 77) unless something else failed. For ctest on displays whose
    # scale has no blessed reference.
    [switch] $NoBless,
    # Let the program's windows take the foreground on their own (default:
    # ALCHEMY_GUI_NO_ACTIVATE=1; 'focus' and input actions activate explicitly).
    [switch] $AllowActivate
)
# Alchemy GUI test harness: launches a program under alchemy-fx, finds its
# top-level window, drives it with real input (SendInput: keys, text, mouse
# moves/clicks/drags, wheel), captures the window (PrintWindow with
# PW_RENDERFULLCONTENT, which includes Vulkan swapchain content, or a screen
# BitBlt) to PNG and compares captures with reference images through
# tools/fx_imgdiff.py. Writes <OutDir>/<timestamp>/{log.txt,status.json,*.png}
# and exits nonzero when an action, an expectation or a comparison fails.
#
# Actions (coordinates are client-area physical pixels, top-left origin):
#   wait-window                   wait for the window (implicit before the first input action)
#   wait-line <regex>             wait until a stdout line matches
#   sleep <ms>
#   focus                         bring the window to the foreground
#   key <name>[+<name>...]        press and release (a..z 0..9 enter esc tab space backspace delete
#                                 left right up down home end pageup pagedown f1..f12, with
#                                 ctrl alt shift win as modifiers: key ctrl+s)
#   keydown <name> / keyup <name>
#   type <text>                   Unicode text (KEYEVENTF_UNICODE), rest of the line
#   move <x> <y>
#   click <x> <y> [left|right|middle] [count]
#   down <x> <y> [button] / up <x> <y> [button]
#   drag <x1> <y1> <x2> <y2> [button] [steps]
#   wheel <notches> [<x> <y>]     vertical wheel, positive = away from the user
#   hwheel <notches> [<x> <y>]
#   resize <width> <height>       client size in pixels (SetWindowPos)
#   place <x> <y>                 move the window to screen pixel x,y (onto another monitor: a real DPI change)
#   screenshot <name.png> [client|window] [auto|printwindow|screen]
#                                 auto (default): PrintWindow, else QuartzCore's present capture, else a
#                                 screen BitBlt (see alchemy_gui_common.ps1)
#   compare <name.png> <reference.png> [<max channel diff> [<allowed % channels over it>]]
#                                 (a missing reference is created from the capture and reported, or
#                                 with -NoBless skips the run; "{size}" in the reference path is the
#                                 capture's <width>x<height>)
#   post-key <char>               WM_KEYDOWN/WM_CHAR/WM_KEYUP posted to the window (a-z, 0-9; no focus needed)
#   wait-frames <n>               wait until QuartzCore presented n frames in total (exact:
#                                 ALCHEMY_PRESENT_STATS=<run dir>/present-stats.json)
#   wait-stderr <regex>           wait until a stderr line matches
#   dialog [press]                wait for a modal dialog, record its text, optionally press its default button
#   close                         WM_CLOSE to the window
#   wait-exit [<seconds>]
#
# status.json also records the exit code, the stop point (guest signal >
# unsupported Objective-C message > "alchemy:" fatal line > "unsupported"
# line > other stderr), the presented-frame count and every modal dialog.
#
# Example (fixture a with real input):
#   scripts/alchemy_gui_harness.ps1 -Exe build-wf-gui/Release/alchemy-fx.exe `
#     -Program build-wf-gui/macos/gui_window_events -ProgramArgs --interactive `
#     -Actions 'wait-line ready;click 50 40;wheel 2 50 40;type ab;key q;wait-exit 10' `
#     -Expect 'mouse down button=0;scroll dx=;key down keyCode=12'
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/alchemy_gui_common.ps1"
$R = (Resolve-Path "$PSScriptRoot/..").Path
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$runDir = Join-Path (Resolve-Path $OutDir).Path $stamp
New-Item -ItemType Directory -Force $runDir | Out-Null
function Quote([string] $a) { if ($a -notmatch '[\s"]') { return $a }; '"' + ($a -replace '(\\*)"', '$1$1\"') + '"' }

if (-not ('AlchemyGuiHarness' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
public static class AlchemyGuiHarness {
    delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] struct INPUTUNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public INPUTUNION u; }
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr window, ref POINT point);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr window);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint attach, uint to, bool attach_flag);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] static extern uint SendInput(uint count, INPUT[] inputs, int size);
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    static AlchemyGuiHarness() {
        // Physical pixels everywhere (per-monitor v2), like alchemy-fx's window server.
        try { if (!SetProcessDpiAwarenessContext(new IntPtr(-4))) SetProcessDPIAware(); } catch { SetProcessDPIAware(); }
    }

    // ---- windows -----------------------------------------------------------
    public static IntPtr[] Windows(uint process) {
        var found = new List<IntPtr>();
        EnumWindows((window, parameter) => {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            RECT rect;
            if (owner == process && IsWindowVisible(window) && GetWindowRect(window, out rect) &&
                rect.Right > rect.Left && rect.Bottom > rect.Top) found.Add(window);
            return true;
        }, IntPtr.Zero);
        return found.ToArray();
    }
    public static string Title(IntPtr window) {
        var text = new StringBuilder(512);
        GetWindowText(window, text, text.Capacity);
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
    public static bool Focus(IntPtr window) {
        if (IsIconic(window)) ShowWindow(window, 9);   // SW_RESTORE
        if (GetForegroundWindow() == window) return true;
        SetForegroundWindow(window);
        if (GetForegroundWindow() == window) return true;
        // Foreground lock: borrow the foreground thread's input state.
        uint ignored;
        var foreground = GetWindowThreadProcessId(GetForegroundWindow(), out ignored);
        var current = GetCurrentThreadId();
        AttachThreadInput(current, foreground, true);
        BringWindowToTop(window);
        SetForegroundWindow(window);
        AttachThreadInput(current, foreground, false);
        Thread.Sleep(50);
        return GetForegroundWindow() == window;
    }
    public static void ResizeClient(IntPtr window, int width, int height) {
        var bounds = Bounds(window);
        var client = Client(window);
        SetWindowPos(window, IntPtr.Zero, 0, 0, width + bounds[2] - client[2], height + bounds[3] - client[3],
                     0x0002 | 0x0004 | 0x0010);   // SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE
    }
    public static void Close(IntPtr window) { PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero); }
    [DllImport("user32.dll")] static extern uint MapVirtualKey(uint code, uint type);
    // A key press posted to the window's queue (a-z, 0-9): no focus or
    // foreground rights needed, unlike SendInput.
    public static void PostKey(IntPtr window, char character) {
        uint vk = Char.ToUpperInvariant(character);
        uint scan = MapVirtualKey(vk, 0);
        PostMessage(window, 0x0100, new IntPtr(vk), new IntPtr(1 | (scan << 16)));                       // WM_KEYDOWN
        PostMessage(window, 0x0102, new IntPtr(character), new IntPtr(1 | (scan << 16)));                // WM_CHAR
        PostMessage(window, 0x0101, new IntPtr(vk), new IntPtr(unchecked((int)(1u | (scan << 16) | 0xC0000000u))));   // WM_KEYUP
    }
    public static void Place(IntPtr window, int x, int y) {
        SetWindowPos(window, IntPtr.Zero, x, y, 0, 0, 0x0001 | 0x0004 | 0x0010);   // SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
    }

    // ---- input ---------------------------------------------------------------
    static void Send(INPUT[] inputs) {
        var sent = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUT)));
        if (sent != inputs.Length) throw new Exception("SendInput injected " + sent + " of " + inputs.Length +
            " events (error " + Marshal.GetLastWin32Error() + "; is the desktop locked or the target elevated?)");
    }
    static INPUT Key(ushort vk, ushort scan, uint flags) {
        var input = new INPUT { type = 1 };
        input.u.ki = new KEYBDINPUT { wVk = vk, wScan = scan, dwFlags = flags };
        return input;
    }
    static bool Extended(ushort vk) {
        switch (vk) {
        case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28:
        case 0x2D: case 0x2E: case 0x5B: case 0x5C: return true;
        default: return false;
        }
    }
    public static void KeyDown(ushort vk) { Send(new[] { Key(vk, 0, Extended(vk) ? 1u : 0u) }); }
    public static void KeyUp(ushort vk) { Send(new[] { Key(vk, 0, (Extended(vk) ? 1u : 0u) | 2u) }); }
    public static void Chord(ushort[] keys) {
        var inputs = new List<INPUT>();
        foreach (var vk in keys) inputs.Add(Key(vk, 0, Extended(vk) ? 1u : 0u));
        for (int index = keys.Length - 1; index >= 0; --index) inputs.Add(Key(keys[index], 0, (Extended(keys[index]) ? 1u : 0u) | 2u));
        Send(inputs.ToArray());
    }
    public static void Text(string text) {
        foreach (var unit in text) {   // UTF-16 units; surrogate pairs arrive as two WM_CHARs
            Send(new[] { Key(0, unit, 4u), Key(0, unit, 4u | 2u) });   // KEYEVENTF_UNICODE (| KEYUP)
            Thread.Sleep(5);
        }
    }
    static INPUT Mouse(int x, int y, uint flags, uint data) {
        // Absolute coordinates over the virtual desktop, 0..65535.
        int left = GetSystemMetrics(76), top = GetSystemMetrics(77);
        int width = Math.Max(1, GetSystemMetrics(78) - 1), height = Math.Max(1, GetSystemMetrics(79) - 1);
        var input = new INPUT { type = 0 };
        input.u.mi = new MOUSEINPUT {
            dx = (int)Math.Round((x - left) * 65535.0 / width), dy = (int)Math.Round((y - top) * 65535.0 / height),
            mouseData = data, dwFlags = flags | 0x8000u | 0x4000u   // ABSOLUTE | VIRTUALDESK
        };
        return input;
    }
    public static void MoveClient(IntPtr window, int x, int y) {
        var client = Client(window);
        Send(new[] { Mouse(client[0] + x, client[1] + y, 0x0001u, 0) });
    }
    static uint ButtonFlag(string button, bool down) {
        switch (button) {
        case "right": return down ? 0x0008u : 0x0010u;
        case "middle": return down ? 0x0020u : 0x0040u;
        default: return down ? 0x0002u : 0x0004u;
        }
    }
    public static void Button(IntPtr window, int x, int y, string button, bool down) {
        var client = Client(window);
        Send(new[] { Mouse(client[0] + x, client[1] + y, 0x0001u | ButtonFlag(button, down), 0) });
    }
    public static void Wheel(IntPtr window, int x, int y, int notches, bool horizontal) {
        var client = Client(window);
        Send(new[] { Mouse(client[0] + x, client[1] + y, 0x0001u, 0),
                     Mouse(client[0] + x, client[1] + y, horizontal ? 0x1000u : 0x0800u, unchecked((uint)(notches * 120))) });
    }

    // ---- capture ----------------------------------------------------------
    // printwindow: PrintWindow(PW_RENDERFULLCONTENT), independent of overlap;
    // screen: BitBlt of the visible pixels. client: crop to the client area.
    public static void Capture(IntPtr window, string path, bool client, bool screen) {
        var bounds = Bounds(window);
        var area = Client(window);
        using (var full = new Bitmap(bounds[2], bounds[3], PixelFormat.Format32bppArgb)) {
            using (var graphics = Graphics.FromImage(full)) {
                if (screen) {
                    graphics.CopyFromScreen(bounds[0], bounds[1], 0, 0, new Size(bounds[2], bounds[3]));
                } else {
                    var dc = graphics.GetHdc();
                    bool ok = PrintWindow(window, dc, 2);
                    graphics.ReleaseHdc(dc);
                    if (!ok) throw new Exception("PrintWindow failed");
                }
            }
            if (!client) { full.Save(path, ImageFormat.Png); return; }
            var crop = new Rectangle(area[0] - bounds[0], area[1] - bounds[1], area[2], area[3]);
            crop.Intersect(new Rectangle(0, 0, bounds[2], bounds[3]));
            if (crop.Width <= 0 || crop.Height <= 0) throw new Exception("the client area is empty");
            using (var part = full.Clone(crop, PixelFormat.Format32bppArgb)) part.Save(path, ImageFormat.Png);
        }
    }
    // Fallback comparison when Python/numpy is unavailable: max channel
    // difference and the number of channels above `tolerance`.
    public static long[] Compare(string a, string b, int tolerance) {
        using (var left = new Bitmap(a)) using (var right = new Bitmap(b)) {
            if (left.Width != right.Width || left.Height != right.Height) return new long[] { -1, 0, 0 };
            long max = 0, over = 0, total = 0;
            for (int y = 0; y < left.Height; ++y)
                for (int x = 0; x < left.Width; ++x) {
                    var p = left.GetPixel(x, y); var q = right.GetPixel(x, y);
                    foreach (var d in new[] { Math.Abs(p.R - q.R), Math.Abs(p.G - q.G), Math.Abs(p.B - q.B), Math.Abs(p.A - q.A) }) {
                        if (d > max) max = d;
                        if (d > tolerance) ++over;
                        ++total;
                    }
                }
            return new long[] { max, over, total };
        }
    }
}
'@
}

# `powershell -File` cannot pass arrays: ';' also separates actions,
# expectations and program arguments.
$Actions = @($Actions | ForEach-Object { $_ -split ';' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$Expect = @($Expect | ForEach-Object { $_ -split ';' } | Where-Object { $_ })
$ProgramArgs = @($ProgramArgs | ForEach-Object { $_ -split ';' } | Where-Object { $_ })

# ---- target ----------------------------------------------------------------------
if ($SelfTest) {
    # A native echo window: prints key, char, mouse and wheel events and
    # paints a fixed pattern (left half red, right half blue).
    $echo = @'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
$form = New-Object System.Windows.Forms.Form
$form.Text = 'alchemy-harness-selftest'
$form.ClientSize = New-Object System.Drawing.Size(320, 200)
$form.StartPosition = 'Manual'; $form.Location = New-Object System.Drawing.Point(200, 200)
$form.KeyPreview = $true
$say = { param($t) [Console]::Out.WriteLine("selftest: $t"); [Console]::Out.Flush() }
$form.Add_Paint({ param($s, $e)
    $e.Graphics.FillRectangle([System.Drawing.Brushes]::Red, 0, 0, 160, 200)
    $e.Graphics.FillRectangle([System.Drawing.Brushes]::Blue, 160, 0, 160, 200) })
$form.Add_KeyDown({ param($s, $e) & $say "key down $($e.KeyCode) mods=$($e.Modifiers)" })
$form.Add_KeyPress({ param($s, $e) & $say "char $([int]$e.KeyChar)" ; if ($e.KeyChar -eq 'q') { $form.Close() } })
$form.Add_MouseDown({ param($s, $e) & $say "mouse down $($e.Button) at $($e.X),$($e.Y)" })
$form.Add_MouseUp({ param($s, $e) & $say "mouse up $($e.Button) at $($e.X),$($e.Y)" })
$form.Add_MouseWheel({ param($s, $e) & $say "wheel $($e.Delta)" })
$form.Add_Shown({ & $say "ready" })
[System.Windows.Forms.Application]::Run($form)
& $say "done"
'@
    $echoPath = Join-Path $runDir 'selftest_window.ps1'
    Set-Content -Encoding utf8 $echoPath $echo
    $fileName = (Get-Command powershell.exe).Source
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File $(Quote $echoPath)"
    if (-not $Actions.Count -and -not $ActionFile) {
        $reference = Join-Path $runDir 'selftest_reference.png'
        # Reference: the painted pattern (red | blue) at the client size.
        Add-Type -AssemblyName System.Drawing
        $bitmap = New-Object System.Drawing.Bitmap 320, 200
        $g = [System.Drawing.Graphics]::FromImage($bitmap)
        $g.FillRectangle([System.Drawing.Brushes]::Red, 0, 0, 160, 200)
        $g.FillRectangle([System.Drawing.Brushes]::Blue, 160, 0, 160, 200)
        $g.Dispose(); $bitmap.Save($reference, [System.Drawing.Imaging.ImageFormat]::Png); $bitmap.Dispose()
        $Actions = @('wait-line ready', 'focus', 'click 40 50', 'click 200 60 right', 'wheel 1 100 100', 'type hi',
                     'key shift+a', 'screenshot selftest.png client printwindow', "compare selftest.png $reference 8 0.5",
                     'key q', 'wait-exit 10')
        $Expect = @('mouse down Left at 40,50', 'mouse down Right at 200,60', 'wheel 120', 'char 104', 'char 105',
                    'key down A mods=Shift', 'done')
    }
} else {
    if (-not $Program) { throw 'Alchemy GUI harness: -Program is required (or -SelfTest)' }
    $fileName = (Resolve-Path $Exe).Path
    $all = @()
    if (-not $RuntimeDir -and (Test-Path "$R/artifacts/alchemy-llvm-arm64")) { $RuntimeDir = "$R/artifacts/alchemy-llvm-arm64" }
    if ($RuntimeDir) { $all += @('--runtime-dir', (Resolve-Path $RuntimeDir).Path) }
    $all += @((Resolve-Path $Program).Path) + $ProgramArgs
    $arguments = ($all | ForEach-Object { Quote $_ }) -join ' '
}
if ($ActionFile) {
    $Actions = @(Get-Content $ActionFile | ForEach-Object { ($_ -replace '#.*$', '').Trim() } | Where-Object { $_ })
}

# ---- launch ------------------------------------------------------------------------
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $fileName
$psi.Arguments = $arguments
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.WorkingDirectory = $R
if (-not $NoTracePresent -and -not $SelfTest) { $psi.EnvironmentVariables['ALCHEMY_TRACE_PRESENT'] = '1' }
# QuartzCore's on-demand present capture (screenshot fallback for Vulkan
# swapchains) and exact presented-frame counts.
$presentCapture = ''
$presentStats = ''
if (-not $SelfTest) {
    $presentCapture = Join-Path $runDir 'present-capture'
    New-Item -ItemType Directory -Force $presentCapture | Out-Null
    $psi.EnvironmentVariables['ALCHEMY_PRESENT_CAPTURE'] = $presentCapture
    $presentStats = Join-Path $runDir 'present-stats.json'
    $psi.EnvironmentVariables['ALCHEMY_PRESENT_STATS'] = $presentStats
    if (-not $AllowActivate) { $psi.EnvironmentVariables['ALCHEMY_GUI_NO_ACTIVATE'] = '1' }
}
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $psi
$sw = [System.Diagnostics.Stopwatch]::StartNew()
[void]$process.Start()
# One reader thread per stream keeps lines in order (DataReceived events may not).
$stdoutPump = [AlchemyLinePump]::new($process.StandardOutput)
$stderrPump = [AlchemyLinePump]::new($process.StandardError)

$window = [IntPtr]::Zero
$results = New-Object System.Collections.Generic.List[object]
$failures = New-Object System.Collections.Generic.List[string]
$skips = New-Object System.Collections.Generic.List[string]
$seen = New-Object System.Collections.Generic.List[string]
function Remaining { [Math]::Max(0, $TimeoutSeconds - $sw.Elapsed.TotalSeconds) }
function Drain { $line = $null; while ($stdoutPump.TryDequeue([ref]$line)) { $seen.Add($line) }; Check-Dialogs $false }
function Find-Window {
    if (-not $WindowTitle) { return [AlchemyGuiWin32]::MainWindow([uint32]$process.Id) }
    $candidates = @([AlchemyGuiHarness]::Windows([uint32]$process.Id) |
        Where-Object { [AlchemyGuiWin32]::ClassName($_) -ne '#32770' -and [AlchemyGuiHarness]::Title($_) -match $WindowTitle })
    if (-not $candidates.Count) { return [IntPtr]::Zero }
    $candidates | Sort-Object { $b = [AlchemyGuiHarness]::Bounds($_); - $b[2] * $b[3] } | Select-Object -First 1
}
$dialogs = New-Object System.Collections.Generic.List[object]
$dialogByHandle = @{}
# Records new modal dialogs; with $press (or -DismissDialogs) answers them.
function Check-Dialogs([bool] $press) {
    if ($SelfTest -or $process.HasExited) { return }
    foreach ($handle in @([AlchemyGuiWin32]::Dialogs([uint32]$process.Id))) {
        $key = $handle.ToInt64()
        if (-not $dialogByHandle.ContainsKey($key)) {
            Start-Sleep -Milliseconds 300
            $entry = [ordered]@{ text = [AlchemyGuiWin32]::DialogText($handle); seen_ms = $sw.ElapsedMilliseconds; pressed = $null }
            $dialogByHandle[$key] = $entry
            $dialogs.Add($entry)
        }
        $entry = $dialogByHandle[$key]
        if (($press -or $DismissDialogs) -and -not $entry.pressed -and [AlchemyGuiWin32]::Alive($handle)) {
            $entry.pressed = [AlchemyGuiWin32]::PressDefault($handle)
        }
    }
}
function Wait-Window {
    while ($script:window -eq [IntPtr]::Zero -or -not [AlchemyGuiHarness]::Alive($script:window)) {
        if ($process.HasExited) { throw "the program exited (status $($process.ExitCode)) before showing a window" }
        if ((Remaining) -le 0) { throw 'timed out waiting for the window' }
        $script:window = Find-Window
        if ($script:window -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 100 }
    }
    $script:window
}
function Focus-Window { $w = Wait-Window; if (-not [AlchemyGuiHarness]::Focus($w)) { throw 'could not bring the window to the foreground' }; Start-Sleep -Milliseconds 50; $w }
$vk = @{ enter = 0x0D; return = 0x0D; esc = 0x1B; escape = 0x1B; tab = 0x09; space = 0x20; backspace = 0x08; delete = 0x2E
         left = 0x25; up = 0x26; right = 0x27; down = 0x28; home = 0x24; end = 0x23; pageup = 0x21; pagedown = 0x22
         insert = 0x2D; ctrl = 0x11; control = 0x11; alt = 0x12; option = 0x12; shift = 0x10; win = 0x5B; cmd = 0x5B }
function Key-Code([string] $name) {
    $n = $name.ToLowerInvariant()
    if ($vk.ContainsKey($n)) { return [uint16]$vk[$n] }
    if ($n -match '^[a-z0-9]$') { return [uint16][int][char]$n.ToUpperInvariant() }
    if ($n -match '^f([1-9]|1[0-2])$') { return [uint16](0x6F + [int]$Matches[1]) }
    throw "unknown key '$name'"
}
function Compare-Images([string] $capture, [string] $reference, [int] $tolerance, [double] $percent) {
    if (-not (Test-Path $reference)) {
        if ($NoBless) { $script:skips.Add("no reference $reference"); return "SKIPPED: no reference $reference" }
        New-Item -ItemType Directory -Force (Split-Path -Parent $reference) | Out-Null
        Copy-Item $capture $reference
        return "reference created: $reference"
    }
    $tool = Join-Path $R 'tools/fx_imgdiff.py'
    $report = $null
    try { $report = & $Python $tool $capture $reference 2>&1 | Out-String } catch { $report = $null }
    if ($report -and $report -match 'shape mismatch') { throw "compare: $($report.Trim())" }
    if ($report -and $report -match 'max (\d+)') {
        $max = [int]$Matches[1]
        # fx_imgdiff reports channels > 8; the tolerance applies to that count.
        $over = if ($report -match '>8: (\d+)') { [long]$Matches[1] } else { 0 }
        $total = if ($report -match '/ (\d+)') { [long]$Matches[1] } else { 1 }
        if ($tolerance -lt 8 -and $max -gt $tolerance) {
            $exact = [AlchemyGuiHarness]::Compare($capture, $reference, $tolerance); $over = $exact[1]; $total = $exact[2]
        }
    } else {
        $exact = [AlchemyGuiHarness]::Compare($capture, $reference, $tolerance)
        if ($exact[0] -lt 0) { throw 'compare: image sizes differ' }
        $max = $exact[0]; $over = $exact[1]; $total = $exact[2]
        $report = "max $max, $over / $total channels over $tolerance (System.Drawing fallback)"
    }
    $share = 100.0 * $over / [Math]::Max(1, $total)
    $summary = "$($report.Trim()) => max $max, $([Math]::Round($share, 3))% channels over tolerance"
    if ($max -gt $tolerance -and $share -gt $percent) { throw "compare failed: $summary" }
    $summary
}

# ---- actions ---------------------------------------------------------------------
try {
    foreach ($action in $Actions) {
        Drain
        $words = @($action -split '\s+' | Where-Object { $_ })
        $verb = $words[0].ToLowerInvariant()
        $rest = if ($action.Trim().Length -gt $words[0].Length) { $action.Trim().Substring($words[0].Length).Trim() } else { '' }
        $note = ''
        switch ($verb) {
            'wait-window' { [void](Wait-Window) }
            'wait-line' {
                while (-not ($seen | Where-Object { $_ -match $rest })) {
                    if ($process.HasExited) { Start-Sleep -Milliseconds 200; Drain; if ($seen | Where-Object { $_ -match $rest }) { break }
                        throw "the program exited before printing /$rest/" }
                    if ((Remaining) -le 0) { throw "timed out waiting for /$rest/" }
                    Start-Sleep -Milliseconds 50; Drain
                }
            }
            'sleep' { Start-Sleep -Milliseconds ([int]$words[1]) }
            'focus' { [void](Focus-Window) }
            'key' { [void](Focus-Window); [AlchemyGuiHarness]::Chord([uint16[]]@($rest -split '\+' | ForEach-Object { Key-Code $_ })) }
            'keydown' { [void](Focus-Window); [AlchemyGuiHarness]::KeyDown((Key-Code $words[1])) }
            'keyup' { [void](Focus-Window); [AlchemyGuiHarness]::KeyUp((Key-Code $words[1])) }
            'type' { [void](Focus-Window); [AlchemyGuiHarness]::Text($rest) }
            'move' { $w = Focus-Window; [AlchemyGuiHarness]::MoveClient($w, [int]$words[1], [int]$words[2]) }
            'click' {
                $w = Focus-Window
                $button = if ($words.Count -gt 3) { $words[3] } else { 'left' }
                $count = if ($words.Count -gt 4) { [int]$words[4] } else { 1 }
                [AlchemyGuiHarness]::MoveClient($w, [int]$words[1], [int]$words[2]); Start-Sleep -Milliseconds 30
                for ($i = 0; $i -lt $count; ++$i) {
                    [AlchemyGuiHarness]::Button($w, [int]$words[1], [int]$words[2], $button, $true)
                    [AlchemyGuiHarness]::Button($w, [int]$words[1], [int]$words[2], $button, $false)
                }
            }
            'down' { $w = Focus-Window; [AlchemyGuiHarness]::Button($w, [int]$words[1], [int]$words[2], $(if ($words.Count -gt 3) { $words[3] } else { 'left' }), $true) }
            'up' { $w = Focus-Window; [AlchemyGuiHarness]::Button($w, [int]$words[1], [int]$words[2], $(if ($words.Count -gt 3) { $words[3] } else { 'left' }), $false) }
            'drag' {
                $w = Focus-Window
                $button = if ($words.Count -gt 5) { $words[5] } else { 'left' }
                $steps = if ($words.Count -gt 6) { [int]$words[6] } else { 10 }
                $x1 = [int]$words[1]; $y1 = [int]$words[2]; $x2 = [int]$words[3]; $y2 = [int]$words[4]
                [AlchemyGuiHarness]::MoveClient($w, $x1, $y1)
                [AlchemyGuiHarness]::Button($w, $x1, $y1, $button, $true)
                for ($i = 1; $i -le $steps; ++$i) {
                    [AlchemyGuiHarness]::MoveClient($w, [int]($x1 + ($x2 - $x1) * $i / $steps), [int]($y1 + ($y2 - $y1) * $i / $steps))
                    Start-Sleep -Milliseconds 15
                }
                [AlchemyGuiHarness]::Button($w, $x2, $y2, $button, $false)
            }
            { $_ -in 'wheel', 'hwheel' } {
                $w = Focus-Window
                $client = [AlchemyGuiHarness]::Client($w)
                $x = if ($words.Count -gt 3) { [int]$words[2] } else { [int]($client[2] / 2) }
                $y = if ($words.Count -gt 3) { [int]$words[3] } else { [int]($client[3] / 2) }
                [AlchemyGuiHarness]::Wheel($w, $x, $y, [int]$words[1], $verb -eq 'hwheel')
            }
            'resize' { $w = Wait-Window; [AlchemyGuiHarness]::ResizeClient($w, [int]$words[1], [int]$words[2]) }
            'screenshot' {
                $w = Wait-Window
                $path = Join-Path $runDir $words[1]
                $client = -not ($words -contains 'window')
                if ($words -contains 'printwindow' -or $words -contains 'screen') {
                    [AlchemyGuiHarness]::Capture($w, $path, $client, $words -contains 'screen')
                    $note = $path
                } else {
                    $note = "$path ($([AlchemyGuiWin32]::Capture($w, $path, $client, $presentCapture)))"
                }
            }
            'compare' {
                $capture = Join-Path $runDir $words[1]
                $reference = if ([System.IO.Path]::IsPathRooted($words[2])) { $words[2] } else { Join-Path $R $words[2] }
                if ($reference.Contains('{size}')) {
                    $image = [System.Drawing.Image]::FromFile($capture)
                    try { $reference = $reference.Replace('{size}', "$($image.Width)x$($image.Height)") } finally { $image.Dispose() }
                }
                $tolerance = if ($words.Count -gt 3) { [int]$words[3] } else { 8 }
                $percent = if ($words.Count -gt 4) { [double]$words[4] } else { 0.1 }
                $note = Compare-Images $capture $reference $tolerance $percent
            }
            'post-key' { $w = Wait-Window; [AlchemyGuiHarness]::PostKey($w, [char]$words[1][0]) }
            'place' { $w = Wait-Window; [AlchemyGuiHarness]::Place($w, [int]$words[1], [int]$words[2]) }
            'wait-frames' {
                $wanted = [long]$words[1]
                while (-not ((Get-AlchemyPresentedFrames $stderrPump.Text $presentStats) -ge $wanted)) {
                    if ($process.HasExited) { throw "the program exited after $(Get-AlchemyPresentedFrames $stderrPump.Text $presentStats) presented frames" }
                    if ((Remaining) -le 0) { throw "timed out waiting for $wanted presented frames (ALCHEMY_PRESENT_STATS)" }
                    Start-Sleep -Milliseconds 50; Drain
                }
                $note = "$(Get-AlchemyPresentedFrames $stderrPump.Text $presentStats) frames"
            }
            'wait-stderr' {
                while (-not ($stderrPump.Text -split "`r?`n" | Where-Object { $_ -match $rest })) {
                    if ($process.HasExited) {
                        Start-Sleep -Milliseconds 200
                        if ($stderrPump.Text -split "`r?`n" | Where-Object { $_ -match $rest }) { break }
                        throw "the program exited before printing /$rest/ on stderr"
                    }
                    if ((Remaining) -le 0) { throw "timed out waiting for stderr /$rest/" }
                    Start-Sleep -Milliseconds 50; Drain
                }
            }
            'dialog' {
                $press = $words.Count -gt 1 -and $words[1] -eq 'press'
                while (-not ($dialogs | Where-Object { -not $_.pressed })) {
                    if ($process.HasExited) { throw 'the program exited without showing a dialog' }
                    if ((Remaining) -le 0) { throw 'timed out waiting for a dialog' }
                    Start-Sleep -Milliseconds 100; Check-Dialogs $false
                }
                $note = ($dialogs | Where-Object { -not $_.pressed } | Select-Object -First 1).text
                Check-Dialogs $press
            }
            'close' { $w = Wait-Window; [AlchemyGuiHarness]::Close($w) }
            'wait-exit' {
                $limit = if ($words.Count -gt 1) { [Math]::Min([double]$words[1], (Remaining)) } else { Remaining }
                if (-not $process.WaitForExit([int]($limit * 1000))) { throw "the program did not exit within $limit s" }
                $process.WaitForExit()   # flush the asynchronous readers
            }
            default { throw "unknown action '$action'" }
        }
        Start-Sleep -Milliseconds 40   # let the target process the input
        $results.Add([ordered]@{ action = $action; ok = $true; ms = $sw.ElapsedMilliseconds; note = $note })
    }
} catch {
    $failures.Add("$action -> $($_.Exception.Message)")
    $results.Add([ordered]@{ action = $action; ok = $false; ms = $sw.ElapsedMilliseconds; note = $_.Exception.Message })
}

# ---- finish ------------------------------------------------------------------------
$stopped = $false
if (-not $process.HasExited -and -not $KeepRunning) { $process.Kill(); $stopped = $true; $process.WaitForExit() }
if ($process.HasExited) { $process.WaitForExit() }
Start-Sleep -Milliseconds 200
[void]$stdoutPump.Wait(3000); [void]$stderrPump.Wait(3000)
$out = $stdoutPump.Text; $err = $stderrPump.Text
foreach ($pattern in $Expect) { if ($out -notmatch $pattern) { $failures.Add("stdout lacks /$pattern/") } }
Set-Content -Encoding utf8 (Join-Path $runDir 'log.txt') ("== command`n$fileName $arguments`n== stdout`n$out`n== stderr`n$err")
$stop = Get-AlchemyStopPoint $out $err
$status = [ordered]@{
    timestamp = $stamp
    command = "$fileName $arguments"
    exit_code = if ($process.HasExited) { $process.ExitCode } else { $null }
    stopped_by_harness = $stopped
    elapsed_ms = $sw.ElapsedMilliseconds
    window = if ($window -ne [IntPtr]::Zero) { [ordered]@{ title = [AlchemyGuiHarness]::Title($window)
        class = [AlchemyGuiWin32]::ClassName($window)
        client = if ([AlchemyGuiHarness]::Alive($window)) { [AlchemyGuiHarness]::Client($window) } else { $null } } } else { $null }
    dialogs = $dialogs
    presented_frames = Get-AlchemyPresentedFrames $err $presentStats
    present_stats = if ($presentStats) { Get-AlchemyPresentSummary $presentStats $err } else { $null }
    actions = $results
    failures = $failures
    stop_point = if ($stop.kind -ne 'none') { $stop } else { $null }
}
$status | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $runDir 'status.json')
foreach ($result in $results) { '{0,-6} {1}{2}' -f $(if ($result.ok) { 'ok' } else { 'FAIL' }), $result.action, $(if ($result.note) { "  ($($result.note))" } else { '' }) }
foreach ($d in $dialogs) { "dialog: $($d.text)$(if ($d.pressed) { " -> [$($d.pressed)]" })" }
if ($null -ne $status.presented_frames) { "presented frames: $($status.presented_frames)" }
if ($status.stop_point) { "stop ($($stop.kind)): $($stop.line)" }
"logs: $runDir"
if ($failures.Count) { $failures | ForEach-Object { "failure: $_" }; exit 1 }
if ($skips.Count) { $skips | ForEach-Object { "skipped: $_" }; exit 77 }
'harness: all actions and expectations passed'
exit 0
