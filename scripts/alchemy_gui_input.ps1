# Input scenario toolkit for the Alchemy GUI (M4: input works). Dot-source it.
#
# Drives a GUI program (Blender.app under alchemy-fx, or native x64 Blender
# for reference images) with REAL input (SendInput), waits for presented
# frames, captures windows and compares captures. Used by scripts/m4/*.ps1.
#
# Every input function honours what-if mode ($AlchemyInputWhatIf = $true, or
# -WhatIf on the function): it prints the planned input ("[whatif] ...") and
# injects nothing. Unattended agents sharing a desktop must only use what-if
# mode; real input needs an unlocked desktop that nobody else is using.
#
# Coordinates are client-area physical pixels of the target window (top-left
# origin). This script runs per-monitor-DPI-aware (v2) on every thread it
# calls Win32 from, so ClientToScreen, the virtual-desktop metrics and
# SendInput's absolute coordinates are all physical pixels, whatever the
# monitor's scale.
#
# Session (Start-AlchemyGuiTarget -Target alchemy|native):
#   alchemy: alchemy-fx.exe [--runtime-dir] Blender.app <args>, with
#            ALCHEMY_GUI_NO_ACTIVATE=1 (windows never take the foreground on
#            their own; Activate-AlchemyWindow does it explicitly),
#            ALCHEMY_TRACE_PRESENT=1, ALCHEMY_PRESENT_CAPTURE=<dir> (present
#            capture fallback) and ALCHEMY_PRESENT_STATS=<file> (exact
#            presented-frame counts for Wait-Frame).
#   native:  blender.exe <args> --no-window-focus with BLENDER_USER_RESOURCES
#            set to a fresh directory (first-start Quick Setup splash, as on a
#            fresh Alchemy install) and no console window.
#
# Functions:
#   Start-AlchemyGuiTarget / Stop-AlchemyGuiTarget
#   Get-AlchemyWindows <session>        visible top-level windows of the process
#   Get-AlchemyClient <hwnd>            @{ X; Y; Width; Height; Dpi; Scale } (screen pixels)
#   ConvertTo-AlchemyScreenPoint <hwnd> <x> <y>
#   Activate-AlchemyWindow <hwnd>       foreground + keyboard focus (SetForegroundWindow, AttachThreadInput)
#   Send-AlchemyMove / Send-AlchemyClick / Send-AlchemyDrag / Send-AlchemyWheel
#   Send-AlchemyKey 'ctrl+shift+z' | 'f3' | 'num.'   (chord: modifiers down, key, release in reverse)
#   Send-AlchemyText 'text' [-Unicode]  (virtual keys from the keyboard layout, else KEYEVENTF_UNICODE)
#   Set-AlchemyClientSize <hwnd> <w> <h> (SetWindowPos, no activation)
#   Close-AlchemyWindow <hwnd>          WM_CLOSE (the title bar's close button)
#   Get-AlchemyFrameCount <session> [-Window]   exact presented frames (ALCHEMY_PRESENT_STATS)
#   Wait-Frame <session> [-Since n] [-Window] [-TimeoutMs] [-QuietMs]
#                                       waits until the presented-frame count advances past
#                                       -Since, then until no frame arrived for -QuietMs
#                                       (native: sleeps -NativeSettleMs)
#   Capture-AlchemyWindow <session> <path> [-Window] [-Whole]
#                                       PrintWindow(PW_RENDERFULLCONTENT) when it shows content
#                                       (unlocked desktop), else QuartzCore's present capture of the
#                                       last presented frame, else a screen BitBlt
#   Compare-AlchemyCapture <capture> <reference> [-Region x,y,w,h] [-Tolerance 8] [-Percent 1.0]
#                                       tools/fx_imgdiff.py (System.Drawing fallback) on the
#                                       (cropped) images: pass when at most -Percent % of the
#                                       channels differ by more than -Tolerance
#   Measure-AlchemyChange <before> <after> [-Region x,y,w,h] [-Threshold 24]
#                                       share of pixels in the region whose largest channel
#                                       difference exceeds -Threshold (the "visible change")
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/alchemy_gui_common.ps1"
if ($null -eq (Get-Variable -Name AlchemyInputWhatIf -Scope Script -ErrorAction SilentlyContinue)) { $script:AlchemyInputWhatIf = $false }

if (-not ('AlchemyGuiInput' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
public static class AlchemyGuiInput {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] struct INPUTUNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public INPUTUNION u; }
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr window, ref POINT point);
    [DllImport("user32.dll")] static extern bool GetCursorPos(out POINT point);
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint count, INPUT[] inputs, int size);
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] static extern uint MapVirtualKey(uint code, uint type);
    [DllImport("user32.dll")] static extern short VkKeyScan(char character);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint attach, uint to, bool attachFlag);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr window, uint flags);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    // WM_CLOSE, as the title bar's close button (no input, no activation).
    public static void Close(IntPtr window) { PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero); }

    // Per-monitor-aware v2 on the calling thread (PowerShell may call from
    // several threads; the process-wide setting may already be fixed).
    static void Aware() { try { SetThreadDpiAwarenessContext(new IntPtr(-4)); } catch { } }

    // ---- geometry --------------------------------------------------------
    // Client area in screen pixels plus the window's DPI: x, y, width, height, dpi.
    public static int[] Client(IntPtr window) {
        Aware();
        RECT rect;
        if (!GetClientRect(window, out rect)) throw new Exception("GetClientRect failed (window gone?)");
        var origin = new POINT();
        ClientToScreen(window, ref origin);
        uint dpi = 96;
        try { dpi = GetDpiForWindow(window); } catch { }
        return new int[] { origin.X, origin.Y, rect.Right - rect.Left, rect.Bottom - rect.Top, (int)dpi };
    }
    public static int[] ToScreen(IntPtr window, int x, int y) {
        Aware();
        var point = new POINT { X = x, Y = y };
        if (!ClientToScreen(window, ref point)) throw new Exception("ClientToScreen failed (window gone?)");
        return new int[] { point.X, point.Y };
    }
    // Resizes the client area (SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE).
    [DllImport("user32.dll")] static extern bool IsZoomed(IntPtr window);
    public static void SetClientSize(IntPtr window, int width, int height) {
        Aware();
        if (IsZoomed(window)) { ShowWindow(window, 4); Thread.Sleep(200); }   // SW_SHOWNOACTIVATE: restore without activation
        RECT bounds, client;
        GetWindowRect(window, out bounds);
        GetClientRect(window, out client);
        int extraX = (bounds.Right - bounds.Left) - (client.Right - client.Left);
        int extraY = (bounds.Bottom - bounds.Top) - (client.Bottom - client.Top);
        if (!SetWindowPos(window, IntPtr.Zero, 0, 0, width + extraX, height + extraY, 0x0002 | 0x0004 | 0x0010))
            throw new Exception("SetWindowPos failed");
    }

    // ---- focus ------------------------------------------------------------
    public static IntPtr Foreground() { return GetForegroundWindow(); }
    public static bool IsForeground(IntPtr window) {
        var foreground = GetForegroundWindow();
        return foreground == window || GetAncestor(foreground, 3) == window;   // GA_ROOTOWNER
    }
    public static bool Activate(IntPtr window) {
        if (IsIconic(window)) ShowWindow(window, 9);   // SW_RESTORE
        if (IsForeground(window)) return true;
        SetForegroundWindow(window);
        if (IsForeground(window)) return true;
        // Foreground lock: borrow the foreground thread's input state.
        uint ignored;
        var foreground = GetWindowThreadProcessId(GetForegroundWindow(), out ignored);
        var current = GetCurrentThreadId();
        bool attached = foreground != 0 && foreground != current && AttachThreadInput(current, foreground, true);
        try {
            BringWindowToTop(window);
            SetForegroundWindow(window);
        } finally {
            if (attached) AttachThreadInput(current, foreground, false);
        }
        Thread.Sleep(50);
        return IsForeground(window);
    }

    // ---- input ------------------------------------------------------------
    static void Send(INPUT[] inputs) {
        Aware();
        var sent = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUT)));
        if (sent != inputs.Length) throw new Exception("SendInput injected " + sent + " of " + inputs.Length +
            " events (error " + Marshal.GetLastWin32Error() + "; is the desktop locked, or the target elevated?)");
    }
    static INPUT MouseInput(int x, int y, uint flags, uint data) {
        // Absolute coordinates over the virtual desktop (0..65535).
        int left = GetSystemMetrics(76), top = GetSystemMetrics(77);
        int width = Math.Max(2, GetSystemMetrics(78)), height = Math.Max(2, GetSystemMetrics(79));
        var input = new INPUT { type = 0 };
        input.u.mi = new MOUSEINPUT {
            dx = (int)Math.Round((x - left) * 65535.0 / (width - 1)), dy = (int)Math.Round((y - top) * 65535.0 / (height - 1)),
            mouseData = data, dwFlags = flags | 0x8000u | 0x4000u   // ABSOLUTE | VIRTUALDESK
        };
        return input;
    }
    static uint ButtonFlag(string button, bool down) {
        switch (button) {
        case "right": return down ? 0x0008u : 0x0010u;
        case "middle": return down ? 0x0020u : 0x0040u;
        case "left": return down ? 0x0002u : 0x0004u;
        default: throw new Exception("unknown mouse button '" + button + "' (left, right, middle)");
        }
    }
    // Screen-pixel move; corrects absolute-coordinate rounding with SetCursorPos.
    public static void MoveTo(int x, int y) {
        Send(new[] { MouseInput(x, y, 0x0001u, 0) });
        POINT at;
        if (GetCursorPos(out at) && (Math.Abs(at.X - x) > 0 || Math.Abs(at.Y - y) > 0)) SetCursorPos(x, y);
    }
    public static void Button(int x, int y, string button, bool down) {
        Send(new[] { MouseInput(x, y, 0x0001u | ButtonFlag(button, down), 0) });
    }
    public static void Wheel(int x, int y, int notches, bool horizontal) {
        Send(new[] { MouseInput(x, y, 0x0001u, 0),
                     MouseInput(x, y, horizontal ? 0x1000u : 0x0800u, unchecked((uint)(notches * 120))) });
    }
    static bool Extended(ushort vk) {
        switch (vk) {
        case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28:   // page/home/end/arrows
        case 0x2D: case 0x2E: case 0x5B: case 0x5C: case 0x5D: case 0x6F: case 0xA3: case 0xA5:   // ins/del/win/apps/num÷/rctrl/ralt
        case 0x90: return true;                                                                   // numlock
        default: return false;
        }
    }
    // `vk` above 0xFFFF marks the numpad Enter (VK_RETURN, extended).
    static INPUT KeyInput(int code, bool down) {
        ushort vk = (ushort)(code & 0xFFFF);
        bool extended = Extended(vk) || code > 0xFFFF;
        uint flags = (extended ? 1u : 0u) | (down ? 0u : 2u);
        var input = new INPUT { type = 1 };
        input.u.ki = new KEYBDINPUT { wVk = vk, wScan = (ushort)MapVirtualKey(vk, 0), dwFlags = flags };
        return input;
    }
    public static void Key(int code, bool down) { Send(new[] { KeyInput(code, down) }); }
    // Modifiers and key down in order, released in reverse order.
    public static void Chord(int[] codes) {
        var inputs = new List<INPUT>();
        foreach (var code in codes) inputs.Add(KeyInput(code, true));
        for (int index = codes.Length - 1; index >= 0; --index) inputs.Add(KeyInput(codes[index], false));
        Send(inputs.ToArray());
    }
    // The virtual key and shift state that type `character` on the current
    // keyboard layout: { vk, shift, ctrl, alt } or null.
    public static int[] KeyFor(char character) {
        short scan = VkKeyScan(character);
        if (scan == -1) return null;
        int state = (scan >> 8) & 0xFF;
        return new int[] { scan & 0xFF, state & 1, (state >> 1) & 1, (state >> 2) & 1 };
    }
    public static void Unicode(string text) {
        foreach (var unit in text) {   // UTF-16 units; surrogate pairs arrive as two WM_CHARs
            var down = new INPUT { type = 1 };
            down.u.ki = new KEYBDINPUT { wVk = 0, wScan = unit, dwFlags = 4u };          // KEYEVENTF_UNICODE
            var up = new INPUT { type = 1 };
            up.u.ki = new KEYBDINPUT { wVk = 0, wScan = unit, dwFlags = 4u | 2u };
            Send(new[] { down, up });
            Thread.Sleep(8);
        }
    }

    // ---- images -----------------------------------------------------------
    public static int[] Size(string path) { using (var image = Image.FromFile(path)) return new int[] { image.Width, image.Height }; }
    public static void Crop(string source, string destination, int x, int y, int width, int height) {
        using (var image = new Bitmap(source)) {
            var area = Rectangle.Intersect(new Rectangle(x, y, width, height), new Rectangle(0, 0, image.Width, image.Height));
            if (area.Width <= 0 || area.Height <= 0) throw new Exception("crop region lies outside the " + image.Width + "x" + image.Height + " image");
            using (var part = image.Clone(area, PixelFormat.Format32bppArgb)) part.Save(destination, ImageFormat.Png);
        }
    }
    // { pixels whose largest channel difference exceeds `threshold`, pixels
    // compared, largest channel difference, channels over `tolerance`,
    // channels compared } in the region (clipped to both images).
    public static long[] Diff(string a, string b, int x, int y, int width, int height, int threshold, int tolerance) {
        using (var left = new Bitmap(a)) using (var right = new Bitmap(b)) {
            if (left.Width != right.Width || left.Height != right.Height)
                throw new Exception("image sizes differ: " + left.Width + "x" + left.Height + " vs " + right.Width + "x" + right.Height);
            var area = Rectangle.Intersect(new Rectangle(x, y, width, height), new Rectangle(0, 0, left.Width, left.Height));
            if (area.Width <= 0 || area.Height <= 0) throw new Exception("region lies outside the image");
            var p = left.LockBits(area, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            var q = right.LockBits(area, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            try {
                long changed = 0, max = 0, over = 0;
                var rowP = new byte[area.Width * 4];
                var rowQ = new byte[area.Width * 4];
                for (int row = 0; row < area.Height; ++row) {
                    Marshal.Copy(IntPtr.Add(p.Scan0, row * p.Stride), rowP, 0, rowP.Length);
                    Marshal.Copy(IntPtr.Add(q.Scan0, row * q.Stride), rowQ, 0, rowQ.Length);
                    for (int i = 0; i < rowP.Length; i += 4) {
                        int pixel = 0;
                        for (int c = 0; c < 3; ++c) {   // B, G, R (alpha is not shown)
                            int d = Math.Abs(rowP[i + c] - rowQ[i + c]);
                            if (d > pixel) pixel = d;
                            if (d > tolerance) ++over;
                        }
                        if (pixel > max) max = pixel;
                        if (pixel > threshold) ++changed;
                    }
                }
                long total = (long)area.Width * area.Height;
                return new long[] { changed, total, max, over, total * 3 };
            } finally { left.UnlockBits(p); right.UnlockBits(q); }
        }
    }
}
'@
}

# ---- key names -------------------------------------------------------------------
$script:AlchemyKeyCodes = @{
    enter = 0x0D; return = 0x0D; esc = 0x1B; escape = 0x1B; tab = 0x09; space = 0x20; backspace = 0x08
    delete = 0x2E; del = 0x2E; insert = 0x2D; ins = 0x2D; home = 0x24; end = 0x23; pageup = 0x21; pagedown = 0x22
    left = 0x25; up = 0x26; right = 0x27; down = 0x28
    ctrl = 0x11; control = 0x11; lctrl = 0xA2; rctrl = 0xA3; shift = 0x10; lshift = 0xA0; rshift = 0xA1
    alt = 0x12; option = 0x12; lalt = 0xA4; ralt = 0xA5; win = 0x5B; cmd = 0x5B; command = 0x5B; apps = 0x5D
    capslock = 0x14; numlock = 0x90
    comma = 0xBC; period = 0xBE; minus = 0xBD; equals = 0xBB; plus = 0xBB; slash = 0xBF; backslash = 0xDC
    semicolon = 0xBA; quote = 0xDE; lbracket = 0xDB; rbracket = 0xDD; grave = 0xC0
    'num.' = 0x6E; numdecimal = 0x6E; numperiod = 0x6E; 'num+' = 0x6B; numplus = 0x6B; 'num-' = 0x6D; numminus = 0x6D
    'num*' = 0x6A; nummultiply = 0x6A; 'num/' = 0x6F; numdivide = 0x6F; numenter = 0x1000D
}
# Virtual-key code (numpad Enter: 0x1000D) of a key name: a..z, 0..9, f1..f24,
# num0..num9 and the names above (case-insensitive).
function Get-AlchemyKeyCode([string] $Name) {
    $n = $Name.Trim().ToLowerInvariant()
    if ($script:AlchemyKeyCodes.ContainsKey($n)) { return [int]$script:AlchemyKeyCodes[$n] }
    if ($n -match '^[a-z0-9]$') { return [int][char]$n.ToUpperInvariant() }
    if ($n -match '^f([1-9]|1[0-9]|2[0-4])$') { return 0x6F + [int]$Matches[1] }
    if ($n -match '^num([0-9])$') { return 0x60 + [int]$Matches[1] }
    throw "unknown key '$Name'"
}

function Write-AlchemyPlan([string] $Text) { Write-Host "[whatif] $Text" }
function Test-AlchemyWhatIf([bool] $WhatIf) { return ($WhatIf -or $script:AlchemyInputWhatIf) }

# ---- geometry ----------------------------------------------------------------------
function Get-AlchemyClient([IntPtr] $Window) {
    $c = [AlchemyGuiInput]::Client($Window)
    [ordered]@{ X = $c[0]; Y = $c[1]; Width = $c[2]; Height = $c[3]; Dpi = $c[4]; Scale = $c[4] / 96.0 }
}
function ConvertTo-AlchemyScreenPoint([IntPtr] $Window, [int] $X, [int] $Y) {
    $p = [AlchemyGuiInput]::ToScreen($Window, $X, $Y)
    [ordered]@{ X = $p[0]; Y = $p[1] }
}
function Format-AlchemyPoint([IntPtr] $Window, [int] $X, [int] $Y) {
    if ($Window -eq [IntPtr]::Zero) { return "client ($X,$Y)" }
    try { $s = ConvertTo-AlchemyScreenPoint $Window $X $Y; return "client ($X,$Y) -> screen ($($s.X),$($s.Y))" }
    catch { return "client ($X,$Y)" }
}

# ---- focus ---------------------------------------------------------------------------
function Activate-AlchemyWindow([IntPtr] $Window, [switch] $WhatIf) {
    if (Test-AlchemyWhatIf $WhatIf) { Write-AlchemyPlan "activate window 0x$('{0:X}' -f $Window.ToInt64())"; return $true }
    if (-not [AlchemyGuiWin32]::DesktopVisible()) { throw 'the desktop is locked or not the input desktop: real input cannot reach the window' }
    if (-not [AlchemyGuiInput]::Activate($Window)) { throw "could not bring window 0x$('{0:X}' -f $Window.ToInt64()) to the foreground" }
    Start-Sleep -Milliseconds 60
    $true
}
# Real input goes to the foreground window: re-activate when focus moved away.
function Assert-AlchemyForeground([IntPtr] $Window) {
    if ($Window -ne [IntPtr]::Zero -and -not [AlchemyGuiInput]::IsForeground($Window)) { [void](Activate-AlchemyWindow $Window) }
}

# ---- mouse ---------------------------------------------------------------------------
function Send-AlchemyMove([IntPtr] $Window, [int] $X, [int] $Y, [switch] $WhatIf) {
    if (Test-AlchemyWhatIf $WhatIf) { Write-AlchemyPlan "move to $(Format-AlchemyPoint $Window $X $Y)"; return }
    Assert-AlchemyForeground $Window
    $s = ConvertTo-AlchemyScreenPoint $Window $X $Y
    [AlchemyGuiInput]::MoveTo($s.X, $s.Y)
}
function Send-AlchemyClick([IntPtr] $Window, [int] $X, [int] $Y, [ValidateSet('left', 'right', 'middle')] [string] $Button = 'left',
                           [int] $Count = 1, [string[]] $Modifiers = @(), [switch] $WhatIf) {
    $what = "$Button click$(if ($Count -gt 1) { " x$Count" })$(if ($Modifiers.Count) { " with $($Modifiers -join '+')" }) at $(Format-AlchemyPoint $Window $X $Y)"
    if (Test-AlchemyWhatIf $WhatIf) { Write-AlchemyPlan $what; return }
    Assert-AlchemyForeground $Window
    $s = ConvertTo-AlchemyScreenPoint $Window $X $Y
    $mods = @($Modifiers | ForEach-Object { Get-AlchemyKeyCode $_ })
    foreach ($m in $mods) { [AlchemyGuiInput]::Key($m, $true) }
    try {
        [AlchemyGuiInput]::MoveTo($s.X, $s.Y)
        Start-Sleep -Milliseconds 30
        for ($i = 0; $i -lt $Count; ++$i) {
            [AlchemyGuiInput]::Button($s.X, $s.Y, $Button, $true)
            [AlchemyGuiInput]::Button($s.X, $s.Y, $Button, $false)
            if ($i + 1 -lt $Count) { Start-Sleep -Milliseconds 40 }   # inside the double-click time
        }
    } finally {
        for ($i = $mods.Count - 1; $i -ge 0; --$i) { [AlchemyGuiInput]::Key($mods[$i], $false) }
    }
}
# Button down at From, -Steps intermediate moves (every -StepMs), up at To.
function Send-AlchemyDrag([IntPtr] $Window, [int[]] $From, [int[]] $To, [ValidateSet('left', 'right', 'middle')] [string] $Button = 'left',
                          [int] $Steps = 16, [int] $StepMs = 15, [string[]] $Modifiers = @(), [switch] $WhatIf) {
    $what = "$Button drag$(if ($Modifiers.Count) { " with $($Modifiers -join '+')" }) from $(Format-AlchemyPoint $Window $From[0] $From[1]) to $(Format-AlchemyPoint $Window $To[0] $To[1]) in $Steps steps"
    if (Test-AlchemyWhatIf $WhatIf) { Write-AlchemyPlan $what; return }
    Assert-AlchemyForeground $Window
    $a = ConvertTo-AlchemyScreenPoint $Window $From[0] $From[1]
    $b = ConvertTo-AlchemyScreenPoint $Window $To[0] $To[1]
    $mods = @($Modifiers | ForEach-Object { Get-AlchemyKeyCode $_ })
    foreach ($m in $mods) { [AlchemyGuiInput]::Key($m, $true) }
    try {
        [AlchemyGuiInput]::MoveTo($a.X, $a.Y)
        Start-Sleep -Milliseconds 30
        [AlchemyGuiInput]::Button($a.X, $a.Y, $Button, $true)
        try {
            for ($i = 1; $i -le $Steps; ++$i) {
                Start-Sleep -Milliseconds $StepMs
                [AlchemyGuiInput]::MoveTo([int]($a.X + ($b.X - $a.X) * $i / $Steps), [int]($a.Y + ($b.Y - $a.Y) * $i / $Steps))
            }
        } finally {
            [AlchemyGuiInput]::Button($b.X, $b.Y, $Button, $false)
        }
    } finally {
        for ($i = $mods.Count - 1; $i -ge 0; --$i) { [AlchemyGuiInput]::Key($mods[$i], $false) }
    }
}
# Positive notches: away from the user (Blender: zoom in).
function Send-AlchemyWheel([IntPtr] $Window, [int] $X, [int] $Y, [int] $Notches, [switch] $Horizontal, [switch] $WhatIf) {
    $what = "$(if ($Horizontal) { 'horizontal ' })wheel $Notches notch(es) at $(Format-AlchemyPoint $Window $X $Y)"
    if (Test-AlchemyWhatIf $WhatIf) { Write-AlchemyPlan $what; return }
    Assert-AlchemyForeground $Window
    $s = ConvertTo-AlchemyScreenPoint $Window $X $Y
    $sign = [Math]::Sign($Notches)
    for ($i = 0; $i -lt [Math]::Abs($Notches); ++$i) {   # one notch per event, as a real wheel
        [AlchemyGuiInput]::Wheel($s.X, $s.Y, $sign, [bool]$Horizontal)
        Start-Sleep -Milliseconds 40
    }
}

# ---- keyboard ------------------------------------------------------------------------
# 'f3', 'esc', 'ctrl+a', 'shift+num.', 'ctrl+shift+z'. -Window: re-activate first.
function Send-AlchemyKey([string] $Chord, [IntPtr] $Window = [IntPtr]::Zero, [switch] $WhatIf) {
    $codes = @($Chord -split '\+' | Where-Object { $_ } | ForEach-Object { Get-AlchemyKeyCode $_ })
    if (Test-AlchemyWhatIf $WhatIf) { Write-AlchemyPlan "key $Chord (vk $(($codes | ForEach-Object { '0x{0:X2}' -f ($_ -band 0xFFFF) }) -join ' '))"; return }
    Assert-AlchemyForeground $Window
    [AlchemyGuiInput]::Chord([int[]]$codes)
}
# Types `Text`: each character as the virtual key (with shift/ctrl/alt) the
# current keyboard layout uses for it, so the target sees real key codes;
# characters without a key (and everything with -Unicode) as
# KEYEVENTF_UNICODE packets.
function Send-AlchemyText([string] $Text, [IntPtr] $Window = [IntPtr]::Zero, [switch] $Unicode, [int] $DelayMs = 15, [switch] $WhatIf) {
    if (Test-AlchemyWhatIf $WhatIf) { Write-AlchemyPlan "type '$Text'$(if ($Unicode) { ' (unicode)' } else { ' (layout keys)' })"; return }
    Assert-AlchemyForeground $Window
    if ($Unicode) { [AlchemyGuiInput]::Unicode($Text); return }
    foreach ($ch in $Text.ToCharArray()) {
        $key = [AlchemyGuiInput]::KeyFor($ch)
        if ($null -eq $key) { [AlchemyGuiInput]::Unicode([string]$ch); continue }
        $codes = @()
        if ($key[1]) { $codes += 0x10 }
        if ($key[2]) { $codes += 0x11 }
        if ($key[3]) { $codes += 0x12 }
        $codes += $key[0]
        [AlchemyGuiInput]::Chord([int[]]$codes)
        Start-Sleep -Milliseconds $DelayMs
    }
}

function Close-AlchemyWindow([IntPtr] $Window, [switch] $WhatIf) {
    if (Test-AlchemyWhatIf $WhatIf) { Write-AlchemyPlan 'post WM_CLOSE to the window (its close button)'; return }
    [AlchemyGuiInput]::Close($Window)
}
function Set-AlchemyClientSize([IntPtr] $Window, [int] $Width, [int] $Height, [switch] $WhatIf) {
    if (Test-AlchemyWhatIf $WhatIf) { Write-AlchemyPlan "resize the client area to ${Width}x${Height} (SetWindowPos, no activation)"; return }
    [AlchemyGuiInput]::SetClientSize($Window, $Width, $Height)
}

# ---- session ---------------------------------------------------------------------------
function Get-AlchemyWindows($Session) {
    if (-not $Session -or -not $Session.Process -or $Session.Process.HasExited) { return @() }
    @([AlchemyGuiWin32]::Windows([uint32]$Session.Process.Id) | Where-Object {
        $c = [AlchemyGuiWin32]::ClassName($_); $c -ne '#32770' -and $c -ne 'tooltips_class32' -and $c -ne 'IME' -and $c -ne 'MSCTFIME UI' })
}

# Launches the target and waits for its main window, answering modal
# dialogs (the GPU warning) with their default button (a WM_COMMAND, no
# input), then for the first presented frame (alchemy) and -SettleSeconds
# without dialogs. Returns the session.
function Start-AlchemyGuiTarget {
    param(
        [ValidateSet('alchemy', 'native')] [string] $Target = 'alchemy',
        [string] $OutDir,
        [string] $Exe = '',
        [string] $App = '',
        [string] $RuntimeDir = '',
        [string] $NativeExe = '',
        [string[]] $Arguments = @('--factory-startup'),
        [int] $TimeoutSeconds = 180,
        [int] $SettleSeconds = 10
    )
    $root = (Resolve-Path "$PSScriptRoot/..").Path
    # Artifacts live in the main checkout (absent from worktrees).
    $artifacts = if (Test-Path "$root/artifacts/alchemy-blender") { "$root/artifacts" } else { 'C:/dev/transmute/artifacts' }
    New-Item -ItemType Directory -Force $OutDir | Out-Null
    $OutDir = (Resolve-Path $OutDir).Path
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.WorkingDirectory = $root
    $session = [ordered]@{ Target = $Target; OutDir = $OutDir; Process = $null; Window = [IntPtr]::Zero; StatsPath = $null
        CaptureDir = ''; Stdout = $null; Stderr = $null; Command = ''; Dialogs = New-Object System.Collections.Generic.List[object]
        NativeSettleMs = 600 }
    function Quote([string] $a) { if ($a -notmatch '[\s"]') { return $a }; '"' + ($a -replace '(\\*)"', '$1$1\"') + '"' }
    if ($Target -eq 'alchemy') {
        if (-not $Exe) { $Exe = "$root/build-wf-gui/Release/alchemy-fx.exe" }
        if (-not $App) { $App = "$artifacts/alchemy-blender/extracted/Blender/Blender.app" }
        if (-not $RuntimeDir) { $RuntimeDir = "$artifacts/alchemy-llvm-arm64" }
        $psi.FileName = (Resolve-Path $Exe).Path
        $all = @('--runtime-dir', (Resolve-Path $RuntimeDir).Path, (Resolve-Path $App).Path) + $Arguments
        $psi.EnvironmentVariables['ALCHEMY_GUI_NO_ACTIVATE'] = '1'
        # A host crash ends the process with a report on stderr, never a modal
        # WER dialog that would block the desktop (process-local SetErrorMode).
        $psi.EnvironmentVariables['ALCHEMY_GUI_NO_CRASH_DIALOG'] = '1'
        $psi.EnvironmentVariables['ALCHEMY_TRACE_PRESENT'] = '1'
        $session.CaptureDir = Join-Path $OutDir 'present-capture'
        New-Item -ItemType Directory -Force $session.CaptureDir | Out-Null
        $psi.EnvironmentVariables['ALCHEMY_PRESENT_CAPTURE'] = $session.CaptureDir
        $session.StatsPath = Join-Path $OutDir 'present-stats.json'
        $psi.EnvironmentVariables['ALCHEMY_PRESENT_STATS'] = $session.StatsPath
        # A fresh user resource directory, as for native: the first-start
        # Quick Setup splash every run, and no userpref or quit.blend
        # (Recover Last Session) from earlier runs. Guest path: /Volumes/<drive>/...
        $user = Join-Path $OutDir 'alchemy-user'
        if (Test-Path $user) { Remove-Item -Recurse -Force $user }
        New-Item -ItemType Directory -Force $user | Out-Null
        $full = (Resolve-Path $user).Path
        $psi.EnvironmentVariables['BLENDER_USER_RESOURCES'] = "/Volumes/$($full.Substring(0, 1))/$($full.Substring(3) -replace '\\', '/')"
    } else {
        if (-not $NativeExe) { $NativeExe = "$artifacts/x64/blender-4.5.14-windows-x64/blender.exe" }
        $psi.FileName = (Resolve-Path $NativeExe).Path
        $all = @($Arguments) + @('--no-window-focus')
        # A fresh user resource directory: the first-start Quick Setup splash.
        $user = Join-Path $OutDir 'native-user'
        if (Test-Path $user) { Remove-Item -Recurse -Force $user }
        New-Item -ItemType Directory -Force $user | Out-Null
        $psi.EnvironmentVariables['BLENDER_USER_RESOURCES'] = $user
    }
    $psi.Arguments = ($all | ForEach-Object { Quote $_ }) -join ' '
    $session.Command = "$($psi.FileName) $($psi.Arguments)"
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()
    $session.Process = $p
    $session.Stdout = [AlchemyLinePump]::new($p.StandardOutput)
    $session.Stderr = [AlchemyLinePump]::new($p.StandardError)
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $quietSince = $null
    $lastPresented = $null
    $seen = @{}
    try {
        for (;;) {
            if ($p.HasExited) { throw "the target exited (status $($p.ExitCode)) before its window settled" }
            if ($clock.Elapsed.TotalSeconds -ge $TimeoutSeconds) { throw "timed out after $TimeoutSeconds s waiting for the window to settle" }
            $open = @([AlchemyGuiWin32]::Dialogs([uint32]$p.Id))
            foreach ($d in $open) {
                $key = $d.ToInt64()
                if ($seen.ContainsKey($key)) { continue }
                Start-Sleep -Milliseconds 300
                $entry = [ordered]@{ text = [AlchemyGuiWin32]::DialogText($d); pressed = [AlchemyGuiWin32]::PressDefault($d)
                    after_s = [math]::Round($clock.Elapsed.TotalSeconds, 1) }
                $seen[$key] = $entry
                $session.Dialogs.Add($entry)
            }
            if ($session.Window -eq [IntPtr]::Zero -or -not [AlchemyGuiWin32]::Alive($session.Window)) {
                $session.Window = [AlchemyGuiWin32]::MainWindow([uint32]$p.Id)
            }
            $presented = if ($Target -eq 'alchemy') { Get-AlchemyFrameCount $session } else { 1 }
            $ready = $session.Window -ne [IntPtr]::Zero -and -not $open.Count -and $presented -gt 0
            # Settled: no dialog and no new frame for -SettleSeconds.
            if ($presented -ne $lastPresented) { $lastPresented = $presented; $quietSince = $null }
            if (-not $ready) { $quietSince = $null }
            elseif ($null -eq $quietSince) { $quietSince = $clock.Elapsed.TotalSeconds }
            elseif ($clock.Elapsed.TotalSeconds - $quietSince -ge $SettleSeconds) { break }
            Start-Sleep -Milliseconds 200
        }
    } catch {
        Stop-AlchemyGuiTarget $session | Out-Null
        throw
    }
    $session
}

# Stops the target (unless it already exited) and writes <OutDir>/log.txt.
function Stop-AlchemyGuiTarget($Session) {
    if (-not $Session -or -not $Session.Process) { return $null }
    $p = $Session.Process
    $killed = $false
    if (-not $p.HasExited) { try { $p.Kill(); $killed = $true } catch { } }
    try { $p.WaitForExit(10000) | Out-Null } catch { }
    [void]$Session.Stdout.Wait(3000); [void]$Session.Stderr.Wait(3000)
    $out = $Session.Stdout.Text; $err = $Session.Stderr.Text
    Set-Content -Encoding utf8 (Join-Path $Session.OutDir 'log.txt') ("== command`n$($Session.Command)`n== stdout`n$out`n== stderr`n$err")
    [ordered]@{ killed = $killed; exit_code = $(if ($p.HasExited) { $p.ExitCode } else { $null })
        presented_frames = Get-AlchemyPresentedFrames $err $Session.StatsPath; stop_point = Get-AlchemyStopPoint $out $err }
}

# ---- frames ------------------------------------------------------------------------------
# Exact presented frames (ALCHEMY_PRESENT_STATS); -Window: that window's own
# count. 0 before the first present; $null for a native target.
function Get-AlchemyFrameCount($Session, [IntPtr] $Window = [IntPtr]::Zero) {
    if (-not $Session.StatsPath) { return $null }
    $stats = Read-AlchemyPresentStats $Session.StatsPath
    if (-not $stats) { return [long]0 }
    if ($Window -eq [IntPtr]::Zero) { return [long]$stats.presented }
    $entry = @($stats.windows | Where-Object { [long]$_.window -eq $Window.ToInt64() }) | Select-Object -First 1
    if ($entry) { return [long]$entry.presented }
    [long]0
}
# Waits until the presented-frame count exceeds -Since (default: now), then
# until no frame arrived for -QuietMs (the redraw finished). Returns
# @{ advanced; frames; waited_ms }. Native targets have no counter: sleeps
# the session's NativeSettleMs.
function Wait-Frame($Session, [long] $Since = -1, [IntPtr] $Window = [IntPtr]::Zero, [int] $TimeoutMs = 5000, [int] $QuietMs = 350,
                    [switch] $WhatIf) {
    if (Test-AlchemyWhatIf $WhatIf) { Write-AlchemyPlan "wait for a presented frame$(if ($Since -ge 0) { " after #$Since" }), then $QuietMs ms without one"; return [ordered]@{ advanced = $true; frames = $null; waited_ms = 0 } }
    if (-not $Session.StatsPath) {
        Start-Sleep -Milliseconds $Session.NativeSettleMs
        return [ordered]@{ advanced = $null; frames = $null; waited_ms = $Session.NativeSettleMs }
    }
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $count = Get-AlchemyFrameCount $Session $Window
    if ($Since -lt 0) { $Since = $count }
    while ($count -le $Since) {
        if ($clock.ElapsedMilliseconds -ge $TimeoutMs) { return [ordered]@{ advanced = $false; frames = $count; waited_ms = $clock.ElapsedMilliseconds } }
        if ($Session.Process.HasExited) { throw "the target exited (status $($Session.Process.ExitCode)) while waiting for a frame" }
        Start-Sleep -Milliseconds 25
        $count = Get-AlchemyFrameCount $Session $Window
    }
    $changedAt = $clock.ElapsedMilliseconds
    while ($clock.ElapsedMilliseconds - $changedAt -lt $QuietMs -and $clock.ElapsedMilliseconds -lt $TimeoutMs + $QuietMs) {
        Start-Sleep -Milliseconds 25
        $now = Get-AlchemyFrameCount $Session $Window
        if ($now -ne $count) { $count = $now; $changedAt = $clock.ElapsedMilliseconds }
    }
    [ordered]@{ advanced = $true; frames = $count; waited_ms = $clock.ElapsedMilliseconds }
}

# ---- capture and compare ----------------------------------------------------------------
# The window's client area (-Whole: the whole window) to `Path` (PNG).
# Returns the method: printwindow (unlocked desktop, content), present
# (QuartzCore's copy of the last presented frame), screen, printwindow-blank.
function Capture-AlchemyWindow($Session, [string] $Path, [IntPtr] $Window = [IntPtr]::Zero, [switch] $Whole, [switch] $WhatIf) {
    if ($Window -eq [IntPtr]::Zero -and $Session) { $Window = $Session.Window }
    if (Test-AlchemyWhatIf $WhatIf) { Write-AlchemyPlan "capture $(if ($Whole) { 'window' } else { 'client area' }) to $Path"; return 'whatif' }
    $directory = if ($Session) { $Session.CaptureDir } else { '' }
    [AlchemyGuiWin32]::Capture($Window, $Path, -not $Whole, $directory)
}

# Region: @(x, y, width, height) in image pixels; empty = the whole image.
function Measure-AlchemyChange([string] $Before, [string] $After, [int[]] $Region = @(), [int] $Threshold = 24) {
    if (-not $Region.Count) { $size = [AlchemyGuiInput]::Size($Before); $Region = @(0, 0, $size[0], $size[1]) }
    $d = [AlchemyGuiInput]::Diff($Before, $After, $Region[0], $Region[1], $Region[2], $Region[3], $Threshold, 8)
    [ordered]@{ changed_pixels = $d[0]; pixels = $d[1]; percent = [math]::Round(100.0 * $d[0] / [math]::Max(1, $d[1]), 3)
        max_channel_diff = $d[2]; region = $Region; threshold = $Threshold }
}

# Capture vs reference through tools/fx_imgdiff.py (numpy) on the region,
# System.Drawing when Python is unavailable. Pass: at most -Percent % of the
# channels differ by more than -Tolerance (fx_imgdiff counts > 8).
function Compare-AlchemyCapture([string] $Capture, [string] $Reference, [int[]] $Region = @(), [int] $Tolerance = 8,
                                [double] $Percent = 1.0, [string] $Python = 'python') {
    if (-not (Test-Path $Reference)) { return [ordered]@{ ok = $null; summary = "no reference $Reference" } }
    $a = $Capture; $b = $Reference
    $temporary = @()
    $sa = [AlchemyGuiInput]::Size($Capture); $sb = [AlchemyGuiInput]::Size($Reference)
    if ($sa[0] -ne $sb[0] -or $sa[1] -ne $sb[1]) {
        return [ordered]@{ ok = $false; summary = "size $($sa[0])x$($sa[1]) differs from the reference's $($sb[0])x$($sb[1])" }
    }
    if ($Region.Count) {
        $a = [System.IO.Path]::GetTempFileName() + '.png'; $b = [System.IO.Path]::GetTempFileName() + '.png'
        $temporary = @($a, $b)
        [AlchemyGuiInput]::Crop($Capture, $a, $Region[0], $Region[1], $Region[2], $Region[3])
        [AlchemyGuiInput]::Crop($Reference, $b, $Region[0], $Region[1], $Region[2], $Region[3])
    }
    try {
        $report = $null
        $tool = Join-Path (Resolve-Path "$PSScriptRoot/..").Path 'tools/fx_imgdiff.py'
        try { $report = (& $Python $tool $a $b 2>&1 | Out-String).Trim() } catch { $report = $null }
        $over = $null; $total = $null; $max = $null
        if ($report -and $report -match 'max (\d+)' -and $Tolerance -eq 8) {
            $max = [int]$Matches[1]
            $over = if ($report -match '>8: (\d+)') { [long]$Matches[1] } else { 0 }
            $total = if ($report -match '/ (\d+)') { [long]$Matches[1] } else { 1 }
        } else {
            $size = [AlchemyGuiInput]::Size($a)
            $d = [AlchemyGuiInput]::Diff($a, $b, 0, 0, $size[0], $size[1], $Tolerance, $Tolerance)
            $max = $d[2]; $over = $d[3]; $total = $d[4]
            $report = "max $max, $over / $total channels over $Tolerance (System.Drawing)"
        }
        $share = 100.0 * $over / [math]::Max(1, $total)
        [ordered]@{ ok = ($share -le $Percent); percent_over = [math]::Round($share, 3); max = $max; tolerance = $Tolerance
            allowed_percent = $Percent; region = $Region; summary = $report }
    } finally { foreach ($t in $temporary) { Remove-Item -Force -ErrorAction SilentlyContinue $t } }
}
