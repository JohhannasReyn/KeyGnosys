# KeyGnosys manual-test PANIC script.
# Mouse-only recovery: kill the core, then force-release every key and button
# that a broken release path could have left synthesized-down.
# Killing the process removes the hook (physical keys work again) but does NOT
# undo a SendInput key-down that was never matched with an up. Hence part 2.

Write-Host "== KeyGnosys PANIC ==" -ForegroundColor Yellow

$p = Get-Process keygnosys-core -ErrorAction SilentlyContinue
if ($p) { $p | Stop-Process -Force; Write-Host "killed keygnosys-core (pid $($p.Id -join ','))" -ForegroundColor Green }
else    { Write-Host "keygnosys-core was not running" -ForegroundColor DarkGray }

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Panic {
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, IntPtr e);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int v);
}
"@

$KEYUP = 0x2
# every modifier, both sides, plus the generic aliases
$vks = @(0x11,0xA2,0xA3, 0x12,0xA4,0xA5, 0x10,0xA0,0xA1, 0x5B,0x5C, 0x14)
foreach ($vk in $vks) { [Panic]::keybd_event([byte]$vk, 0, $KEYUP, [IntPtr]::Zero) }

# left/right/middle up
foreach ($f in @(0x0004, 0x0010, 0x0040)) { [Panic]::mouse_event([uint32]$f, 0, 0, 0, [IntPtr]::Zero) }

Write-Host "released modifiers and mouse buttons" -ForegroundColor Green
Write-Host ""
Write-Host "If a NON-modifier letter key still repeats, tap that physical key once." -ForegroundColor Cyan
Write-Host "Press Enter to close."
[void][System.Console]::ReadLine()
