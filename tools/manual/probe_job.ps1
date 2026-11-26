# probe_job.ps1 -- measurement M1 for the crash-release guardian.
#
# Run it, by hand, from the host you want to characterise (Windows Terminal,
# a plain conhost window, VS Code's terminal, a detached start). It reports
# whether this shell runs inside a job object, which limit flags that job has,
# and whether a child can be started with CREATE_BREAKAWAY_FROM_JOB.
#
# It sends no input and changes nothing. With -OutFile it also writes the
# report to that file, which is how the detached run is read back.
#
# See docs/superpowers/specs/2026-09-15-crash-release-guardian-design.md,
# "Measurements come first".

param([string]$Host_ = "", [string]$OutFile = "")

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class KgnJobProbe {
    [StructLayout(LayoutKind.Sequential)]
    struct BASIC_LIMIT {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass, SchedulingClass;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct IO_COUNTERS { public ulong a, b, c, d, e, f; }
    [StructLayout(LayoutKind.Sequential)]
    struct EXTENDED_LIMIT {
        public BASIC_LIMIT Basic;
        public IO_COUNTERS Io;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string r, d, t;
        public int x, y, w, h, cx, cy, fill, flags; public short show, r2;
        public IntPtr r3, i, o, e;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int pid, tid; }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool result);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool QueryInformationJobObject(IntPtr job, int cls, out EXTENDED_LIMIT info, int len, IntPtr ret);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcessW(string app, System.Text.StringBuilder cmd, IntPtr pa, IntPtr ta,
        bool inherit, uint flags, IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

    const uint BREAKAWAY_OK = 0x800, SILENT_BREAKAWAY_OK = 0x1000, KILL_ON_JOB_CLOSE = 0x2000;
    const uint CREATE_BREAKAWAY_FROM_JOB = 0x01000000, CREATE_NO_WINDOW = 0x08000000;

    public static string Run() {
        var sb = new System.Text.StringBuilder();
        bool inJob;
        IsProcessInJob(GetCurrentProcess(), IntPtr.Zero, out inJob);
        sb.AppendLine("in_job=" + inJob);
        if (inJob) {
            EXTENDED_LIMIT info;
            if (QueryInformationJobObject(IntPtr.Zero, 9, out info, Marshal.SizeOf(typeof(EXTENDED_LIMIT)), IntPtr.Zero)) {
                uint f = info.Basic.LimitFlags;
                sb.AppendLine(string.Format("limit_flags=0x{0:X}", f));
                sb.AppendLine("breakaway_ok=" + ((f & BREAKAWAY_OK) != 0));
                sb.AppendLine("silent_breakaway_ok=" + ((f & SILENT_BREAKAWAY_OK) != 0));
                sb.AppendLine("kill_on_job_close=" + ((f & KILL_ON_JOB_CLOSE) != 0));
            } else {
                sb.AppendLine("query_failed_error=" + Marshal.GetLastWin32Error());
            }
        }
        var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        PROCESS_INFORMATION pi;
        var cmd = new System.Text.StringBuilder("cmd.exe /c exit 0");
        bool ok = CreateProcessW(null, cmd, IntPtr.Zero, IntPtr.Zero, false,
                                 CREATE_BREAKAWAY_FROM_JOB | CREATE_NO_WINDOW, IntPtr.Zero, null, ref si, out pi);
        if (ok) {
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            sb.AppendLine("breakaway_spawn=ok");
        } else {
            sb.AppendLine("breakaway_spawn=failed error=" + Marshal.GetLastWin32Error());
        }
        return sb.ToString();
    }
}
'@

$report = "host=$Host_`n" + [KgnJobProbe]::Run()
Write-Output $report
if ($OutFile) { Set-Content -Path $OutFile -Value $report -Encoding utf8 }
