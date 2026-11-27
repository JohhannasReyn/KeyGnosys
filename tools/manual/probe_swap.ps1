# probe_swap.ps1 -- measurement M2 for the crash-release guardian.
#
# Question: with the mouse buttons swapped, which virtual key does
# GetAsyncKeyState report down after an injected MOUSEEVENTF_LEFTDOWN?
# The guardian checks Windows' button state before it lifts anything, so it
# must ask about the right key.
#
# What it does, over a topmost window it owns, with the cursor parked inside:
#   1. remember the swap setting, then swap the buttons;
#   2. inject LEFTDOWN, read VK_LBUTTON and VK_RBUTTON;
#   3. inject LEFTUP, read again;
#   4. restore the original swap setting -- in a finally, so it is restored even
#      if anything above throws.
# The same is repeated unswapped as the control. Run it by hand; keep your
# hands off the mouse for the two seconds it takes.

Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class KgnSwapProbe {
    [StructLayout(LayoutKind.Sequential)]
    struct MOUSEINPUT { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)]
    // MOUSEINPUT is the largest member of the INPUT union, so this is exactly
    // sizeof(INPUT) (40 bytes on x64); SendInput rejects any other size.
    struct INPUT { public uint type; public MOUSEINPUT mi; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
    [DllImport("user32.dll")] public static extern bool SwapMouseButton(bool swap);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
    public static uint Button(bool down) {
        var i = new INPUT[1];
        i[0].type = 0;
        i[0].mi.flags = down ? 0x0002u : 0x0004u;  // LEFTDOWN / LEFTUP
        return SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static string Read() {
        bool l = (GetAsyncKeyState(0x01) & 0x8000) != 0;
        bool r = (GetAsyncKeyState(0x02) & 0x8000) != 0;
        return "VK_LBUTTON=" + (l ? "down" : "up") + " VK_RBUTTON=" + (r ? "down" : "up");
    }
}
'@

$form = New-Object System.Windows.Forms.Form
$form.Text = "KeyGnosys swap probe -- hands off the mouse"
$form.TopMost = $true
$form.Width = 500; $form.Height = 300
$form.StartPosition = "CenterScreen"
$form.Show()
[System.Windows.Forms.Application]::DoEvents()
Start-Sleep -Milliseconds 300

$original = [KgnSwapProbe]::GetSystemMetrics(23) -ne 0   # SM_SWAPBUTTON
$center = $form.PointToScreen((New-Object System.Drawing.Point(250, 150)))

function Invoke-SwapMeasure([bool]$swapped) {
    [void][KgnSwapProbe]::SwapMouseButton($swapped)
    [System.Windows.Forms.Cursor]::Position = $center
    [System.Windows.Forms.Application]::DoEvents()
    Start-Sleep -Milliseconds 150
    $sent = [KgnSwapProbe]::Button($true)
    Start-Sleep -Milliseconds 150
    $down = [KgnSwapProbe]::Read()
    [void][KgnSwapProbe]::Button($false)
    Start-Sleep -Milliseconds 150
    $up = [KgnSwapProbe]::Read()
    "swapped=$swapped sent=$sent after_LEFTDOWN: $down | after_LEFTUP: $up"
}

try {
    Invoke-SwapMeasure $false
    Invoke-SwapMeasure $true
} finally {
    [void][KgnSwapProbe]::SwapMouseButton($original)
    $form.Close()
    "restored swap=$original (now SM_SWAPBUTTON=$([KgnSwapProbe]::GetSystemMetrics(23)))"
}
