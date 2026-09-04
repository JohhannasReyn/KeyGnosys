# WH_MOUSE_LL observer: timestamps left-button events and whether injected.
# Used to measure the gap the core actually delivers between the two pairs of
# button.double_click, rather than trusting how it feels.
param([int]$Seconds = 28800, [string]$Out = "")
Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public class M {
  public delegate IntPtr HookProc(int c, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SetWindowsHookExW(int i, HookProc f, IntPtr m, uint t);
  [DllImport("user32.dll")] public static extern IntPtr CallNextHookEx(IntPtr h,int c,IntPtr w,IntPtr l);
  [DllImport("user32.dll")] public static extern bool UnhookWindowsHookEx(IntPtr h);
  [DllImport("kernel32.dll")] public static extern IntPtr GetModuleHandleW(string n);
  [DllImport("user32.dll")] public static extern int GetMessageW(out MSG m, IntPtr h, uint a, uint b);
  [DllImport("user32.dll")] public static extern bool PeekMessageW(out MSG m, IntPtr h, uint a, uint b, uint f);
  [DllImport("user32.dll")] public static extern UIntPtr SetTimer(IntPtr h, UIntPtr i, uint ms, IntPtr f);
  [StructLayout(LayoutKind.Sequential)] public struct MSG { public IntPtr hwnd; public uint msg; public IntPtr w; public IntPtr l; public uint t; public int x; public int y; }
  public static List<string> Buf = new List<string>(); public static int Total=0; public static string Path="";
  static HookProc _p; static IntPtr _h=IntPtr.Zero;
  static System.Diagnostics.Stopwatch sw = System.Diagnostics.Stopwatch.StartNew();
  static IntPtr On(int c, IntPtr w, IntPtr l) {
    if (c >= 0) {
      uint msg=(uint)w.ToInt64();
      // WM_LBUTTONDOWN 0x201, WM_LBUTTONUP 0x202
      if (msg==0x201 || msg==0x202) {
        int fl = Marshal.ReadInt32(l, 12);        // MSLLHOOKSTRUCT.flags
        lock(Buf){ Buf.Add(String.Format("{0:F2},{1},{2}", sw.Elapsed.TotalMilliseconds,
                    msg==0x201?"D":"U", ((fl & 0x01)!=0)?1:0)); }
      }
    }
    return CallNextHookEx(IntPtr.Zero,c,w,l);
  }
  static void Flush(){ lock(Buf){ if(Buf.Count==0) return; System.IO.File.AppendAllLines(Path,Buf); Total+=Buf.Count; Buf.Clear(); } }
  public static string Install(){ _p=new HookProc(On); _h=SetWindowsHookExW(14,_p,GetModuleHandleW(null),0);
    return _h==IntPtr.Zero ? ("FAILED err="+Marshal.GetLastWin32Error()) : ("ok 0x"+_h.ToInt64().ToString("X")); }
  public static void Run(int ms){ MSG m; PeekMessageW(out m,IntPtr.Zero,0x400,0x400,0);
    SetTimer(IntPtr.Zero,UIntPtr.Zero,1000,IntPtr.Zero); DateTime end=DateTime.Now.AddMilliseconds(ms);
    while(true){ int g=GetMessageW(out m,IntPtr.Zero,0,0); if(g==0||g==-1) break;
      if(m.msg==0x0113){ Flush(); if(DateTime.Now>=end) break; } }
    Flush(); if(_h!=IntPtr.Zero) UnhookWindowsHookEx(_h); }
}
"@
if ($Out -eq "") { "need -Out"; exit 1 }
if (Test-Path $Out) { Remove-Item $Out }
New-Item -ItemType File $Out | Out-Null
[M]::Path = (Resolve-Path $Out).Path
"mouse probe: " + [M]::Install()
[M]::Run($Seconds*1000)
