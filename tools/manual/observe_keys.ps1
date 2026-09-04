# Measures the grace window from outside the core.
#
# The synthetic press the grace expiry emits is INJECTED, so it never appears in
# the core's own key stream (the hook skips injected events by design). An
# independent hook does see it. Pairing the physical down with the injected down
# for the same key measures real end-to-end grace latency with no instrumentation
# inside the product.
#
# Flushes on a 1s timer rather than at the end: a fixed window that expires
# before the operator is ready produces an empty file and no evidence, which has
# already cost one run. The hook itself only appends to memory.
param([int]$Seconds = 1800, [string]$Out = "")

Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public class G {
  public delegate IntPtr HookProc(int c, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SetWindowsHookExW(int i, HookProc f, IntPtr m, uint t);
  [DllImport("user32.dll")] public static extern IntPtr CallNextHookEx(IntPtr h,int c,IntPtr w,IntPtr l);
  [DllImport("user32.dll")] public static extern bool UnhookWindowsHookEx(IntPtr h);
  [DllImport("kernel32.dll")] public static extern IntPtr GetModuleHandleW(string n);
  [DllImport("user32.dll")] public static extern int GetMessageW(out MSG m, IntPtr h, uint a, uint b);
  [DllImport("user32.dll")] public static extern bool PeekMessageW(out MSG m, IntPtr h, uint a, uint b, uint f);
  [DllImport("user32.dll")] public static extern UIntPtr SetTimer(IntPtr h, UIntPtr i, uint ms, IntPtr f);
  [StructLayout(LayoutKind.Sequential)] public struct MSG { public IntPtr hwnd; public uint msg; public IntPtr w; public IntPtr l; public uint t; public int x; public int y; }
  public static List<string> Buf = new List<string>();
  public static int Total = 0;
  public static string Path = "";
  static HookProc _p; static IntPtr _h = IntPtr.Zero;
  static System.Diagnostics.Stopwatch sw = System.Diagnostics.Stopwatch.StartNew();
  static IntPtr On(int c, IntPtr w, IntPtr l) {
    if (c >= 0) {
      int vk = Marshal.ReadInt32(l,0); int fl = Marshal.ReadInt32(l,8);
      uint m = (uint)w.ToInt64();
      lock (Buf) {
        Buf.Add(String.Format("{0:F2},{1},{2},{3}", sw.Elapsed.TotalMilliseconds, vk,
                              ((fl & 0x10)!=0)?1:0, (m==0x100||m==0x104)?"D":"U"));
      }
    }
    return CallNextHookEx(IntPtr.Zero,c,w,l);
  }
  static void Flush() {
    lock (Buf) {
      if (Buf.Count == 0) return;
      System.IO.File.AppendAllLines(Path, Buf);
      Total += Buf.Count; Buf.Clear();
    }
  }
  public static string Install() {
    _p = new HookProc(On);
    _h = SetWindowsHookExW(13,_p,GetModuleHandleW(null),0);
    return _h == IntPtr.Zero ? ("FAILED err=" + Marshal.GetLastWin32Error())
                             : ("ok handle=0x" + _h.ToInt64().ToString("X"));
  }
  public static void Run(int ms) {
    MSG m; PeekMessageW(out m, IntPtr.Zero, 0x400,0x400,0);
    SetTimer(IntPtr.Zero, UIntPtr.Zero, 1000, IntPtr.Zero);
    DateTime end = DateTime.Now.AddMilliseconds(ms);
    while(true){
      int g = GetMessageW(out m, IntPtr.Zero, 0, 0);
      if (g==0 || g==-1) break;
      if (m.msg == 0x0113) { Flush(); if (DateTime.Now >= end) break; }
    }
    Flush();
    if (_h != IntPtr.Zero) UnhookWindowsHookEx(_h);
  }
}
"@
if ($Out -eq "") { "need -Out"; exit 1 }
if (Test-Path $Out) { Remove-Item $Out }
New-Item -ItemType File $Out | Out-Null
[G]::Path = (Resolve-Path $Out).Path
"probe: " + [G]::Install()
"measuring for $Seconds s, flushing every 1s"
[G]::Run($Seconds*1000)
"logged " + [G]::Total + " events"
