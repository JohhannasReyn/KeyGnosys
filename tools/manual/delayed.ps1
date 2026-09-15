# Operator-launched delayed action, for matrix rows that need a command to land
# while both of the operator's hands are holding keys (section 10).
#
#   delayed.ps1 -Do <action> -Log <absolute path> [-Seconds 8] [-After 3]
#
# The OPERATOR launches this, locally (tools/manual/README.md method rule 7). It
# owns its own countdown, so no chat or relay latency sits inside the window.
#
# CANCEL before it fires: close this window with the mouse. Ctrl+C also works,
# but only while the cursor layer is not engaged -- an engaged layer swallows
# the C. Nothing persists: no process started here outlives the script -- except
# with start-core, which exists to leave a core running, and which logs the pid
# of the core and of the two evidence processes it starts so they can be stopped.
#
# Actions -- a fixed set, deliberately no arbitrary command:
#   noop                  fire nothing; exercises countdown, log and sampling
#   ping                  IPC ping; a harmless known-positive
#   release-all           IPC release_all                      (row 10.7)
#   set-enabled-false     IPC set_enabled {enabled:false}      (row 10.3)
#   set-enabled-true      IPC set_enabled {enabled:true}       (row 10.4)
#   reload-config         IPC reload_config                    (row 10.5)
#   set-bindings-unbound  IPC set_bindings {id:m3-s10-unbound} (row 10.6)
#   set-bindings-default  IPC set_bindings {id:default}        (restores 10.6)
#   drop-client           a separate process holds an IPC connection from launch
#                         and is force-terminated at the fire  (row 10.8)
#   kill-core             taskkill /F /IM keygnosys-core.exe   (row 10.2)
#   ctrl-c-core           CTRL_C_EVENT to the core's console   (row 10.1)
#   close-core-console    WM_CLOSE to the core's classic console window, which
#                         delivers CTRL_CLOSE_EVENT            (row 10.1 alternate)
#   close-window-titled   WM_CLOSE to the ONE top-level window whose title is
#                         exactly -Title (e.g. a dedicated Windows Terminal window
#                         hosting only the core). Refused unless exactly one window
#                         matches; no substring matching; the matched window is
#                         logged before the countdown and re-checked at the fire.
#                         WM_CLOSE is the only message it can send.
#   start-core            (O-1 measurements) start THE repository's
#                         build\default core alone in a classic console. Refused
#                         if any keygnosys-core is already running. After the fire
#                         it waits for the core to answer ping, then starts an
#                         independent key observer (<Log>.keys-post.csv) and a core
#                         event recorder (<Log>.core.jsonl) -- both installed AFTER
#                         the core, so the observer sees physical events before
#                         the core's hook can suppress them -- and only then prints
#                         the release prompt.
#
# Exit: 0 fired and the action succeeded; 1 fired and the action failed;
#       2 refused before the countdown (bad arguments or unmet precondition).
#
# Evidence. <Log> gets one line per step, stamped with QueryPerformanceCounter
# milliseconds -- a system-wide clock, comparable across processes. From launch
# until -After seconds past the fire, <Log>.samples.csv records the pointer
# position and the async state of the mouse buttons and modifiers roughly every
# 10 ms, so "motion stopped" and "the button came up" are measured on the same
# clock as the fire itself. The lshift/rshift columns are Windows' async state for
# VK_LSHIFT/VK_RSHIFT, read by this process independently of any KeyGnosys hook.
param(
    [string]$Do = "",
    [int]$Seconds = 8,
    [int]$After = 3,
    [string]$Log = "",
    # For close-window-titled only: the exact, whole window title to match.
    [string]$Title = "",
    # Internal: the console-signal child. Not for direct use.
    [string]$Signal = "",
    [int]$CorePid = 0
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System; using System.Text; using System.Runtime.InteropServices;
public static class KgnDelayed {
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int v);
  [DllImport("winmm.dll")] public static extern uint timeBeginPeriod(uint p);
  [DllImport("winmm.dll")] public static extern uint timeEndPeriod(uint p);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool FreeConsole();
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool AttachConsole(uint pid);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool SetConsoleCtrlHandler(IntPtr h, bool add);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool GenerateConsoleCtrlEvent(uint ev, uint group);
  [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
  [DllImport("user32.dll", SetLastError=true)] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  public static double Ms() {
    return System.Diagnostics.Stopwatch.GetTimestamp() * 1000.0 / System.Diagnostics.Stopwatch.Frequency;
  }
  public static int Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0 ? 1 : 0; }
  public static string ClassOf(IntPtr h) { var s = new StringBuilder(256); GetClassNameW(h, s, 256); return s.ToString(); }

  // Exact-title lookup over ALL top-level windows, visible or not.
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern int GetWindowTextLengthW(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public static string TitleOf(IntPtr h) {
    int n = GetWindowTextLengthW(h);
    var s = new StringBuilder(n + 2); GetWindowTextW(h, s, n + 1); return s.ToString();
  }
  public static IntPtr[] WindowsTitled(string title) {
    var found = new System.Collections.Generic.List<IntPtr>();
    EnumWindows((h, l) => { if (string.Equals(TitleOf(h), title, StringComparison.Ordinal)) found.Add(h); return true; }, IntPtr.Zero);
    return found.ToArray();
  }
}
"@

function Stamp([string]$path, [string]$text) {
    $line = "{0:F3}  {1}" -f [KgnDelayed]::Ms(), $text
    Add-Content -LiteralPath $path -Value $line -Encoding UTF8
    return $line
}

# ---------------------------------------------------------------------------
# Child mode: attach to the core's console and signal it. Runs in its own hidden
# process so the operator's window keeps its console. Writes only to the log.
if ($Signal -ne "") {
    $childLog = "$Log.child.txt"
    [void][KgnDelayed]::FreeConsole()
    if (-not [KgnDelayed]::AttachConsole([uint32]$CorePid)) {
        Stamp $childLog ("AttachConsole({0}) failed err={1}" -f $CorePid, [Runtime.InteropServices.Marshal]::GetLastWin32Error()) | Out-Null
        exit 3
    }
    $hwnd = [KgnDelayed]::GetConsoleWindow()
    $class = if ($hwnd -ne [IntPtr]::Zero) { [KgnDelayed]::ClassOf($hwnd) } else { "" }
    Stamp $childLog ("attached to pid {0}; console window 0x{1:X} class '{2}'" -f $CorePid, $hwnd.ToInt64(), $class) | Out-Null
    switch ($Signal) {
        'check-ctrl-c' { exit 0 }
        'check-close'  { if ($class -eq 'ConsoleWindowClass') { exit 0 } else { exit 4 } }
        'ctrl-c' {
            # Ignore the event in this process; the core and its shell receive it.
            [void][KgnDelayed]::SetConsoleCtrlHandler([IntPtr]::Zero, $true)
            Stamp $childLog "GenerateConsoleCtrlEvent(CTRL_C_EVENT) sending" | Out-Null
            $ok = [KgnDelayed]::GenerateConsoleCtrlEvent(0, 0)
            Stamp $childLog ("GenerateConsoleCtrlEvent returned {0}" -f $ok) | Out-Null
            if ($ok) { exit 0 } else { exit 5 }
        }
        'close' {
            if ($class -ne 'ConsoleWindowClass') { exit 4 }
            Stamp $childLog "PostMessage(WM_CLOSE) sending" | Out-Null
            $ok = [KgnDelayed]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            # Leave the console at once: the close event is delivered to every
            # attached process and cannot be ignored, so staying would end this
            # process too and lose the result.
            [void][KgnDelayed]::FreeConsole()
            Stamp $childLog ("PostMessage returned {0}" -f $ok) | Out-Null
            if ($ok) { exit 0 } else { exit 5 }
        }
    }
    exit 2
}

# ---------------------------------------------------------------------------
# Argument checks. Everything here refuses BEFORE any countdown.

$ipc = @{
    'ping'                 = @('ping',          '{}')
    'release-all'          = @('release_all',   '{}')
    'set-enabled-false'    = @('set_enabled',   '{"enabled":false}')
    'set-enabled-true'     = @('set_enabled',   '{"enabled":true}')
    'reload-config'        = @('reload_config', '{}')
    'set-bindings-unbound' = @('set_bindings',  '{"id":"m3-s10-unbound"}')
    'set-bindings-default' = @('set_bindings',  '{"id":"default"}')
}
$other = @('noop', 'drop-client', 'kill-core', 'ctrl-c-core', 'close-core-console', 'close-window-titled', 'start-core')
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$coreExe = Join-Path $repoRoot 'build\default\core\keygnosys-core.exe'

function Refuse([string]$why) {
    Write-Host "REFUSED: $why" -ForegroundColor Red
    if ($Log -ne "" -and [IO.Path]::IsPathRooted($Log)) { Stamp $Log "REFUSED: $why" | Out-Null }
    exit 2
}

if ($Log -eq "" -or -not [IO.Path]::IsPathRooted($Log)) { $Log = ""; Refuse "-Log must be an absolute path" }
if (-not ($ipc.ContainsKey($Do) -or $other -contains $Do)) {
    Refuse ("-Do must be one of: " + ((@($ipc.Keys) + $other | Sort-Object) -join ', '))
}
if ($Seconds -lt 3 -or $Seconds -gt 120) { Refuse "-Seconds must be 3..120" }
if ($After -lt 0 -or $After -gt 60) { Refuse "-After must be 0..60" }
if ($Do -eq 'close-window-titled' -and $Title -eq '') { Refuse "close-window-titled needs -Title with the exact window title" }
if ($Do -ne 'close-window-titled' -and $Title -ne '') { Refuse "-Title applies only to close-window-titled" }

$dir = Split-Path -Parent $Log
if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$samples = "$Log.samples.csv"
# Never delete a previous run's evidence. Re-running the same command line (an
# up-arrow and Enter) once wiped a fired run's log and samples before refusing.
foreach ($p in @($Log, $samples, "$Log.child.txt", "$Log.client.txt",
                 "$Log.keys-post.csv", "$Log.keys-post.out", "$Log.core.jsonl", "$Log.core.out")) {
    if (Test-Path -LiteralPath $p) {
        Write-Host "REFUSED: $p already exists -- use a new -Log name; earlier evidence is kept" -ForegroundColor Red
        exit 2
    }
}
Stamp $Log ("launch: -Do {0} -Seconds {1} -After {2} pid {3}" -f $Do, $Seconds, $After, $PID) | Out-Null

function Invoke-Kgn([string]$name, [string]$data, [int]$timeoutMs = 5000, [int]$connectMs = 2000) {
    # One connection per command: nothing is left connected and unread.
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'keygnosys', [System.IO.Pipes.PipeDirection]::InOut)
    try {
        $pipe.Connect($connectMs)
        $enc = New-Object System.Text.UTF8Encoding($false)
        $reader = New-Object System.IO.StreamReader($pipe, $enc)
        $writer = New-Object System.IO.StreamWriter($pipe, $enc)
        $writer.AutoFlush = $true
        $id = "d" + [int]([KgnDelayed]::Ms() % 1000000)
        $writer.Write('{"v":1,"t":"command","n":"' + $name + '","id":"' + $id + '","d":' + $data + "}`n")
        $sent = [KgnDelayed]::Ms()
        $deadline = $sent + $timeoutMs
        while ([KgnDelayed]::Ms() -lt $deadline) {
            $task = $reader.ReadLineAsync()
            if (-not $task.Wait([int][Math]::Max(1, $deadline - [KgnDelayed]::Ms()))) { break }
            $line = $task.Result
            if ($null -eq $line) { break }
            if ($line -match '"t"\s*:\s*"reply"' -and $line -match ('"id"\s*:\s*"' + $id + '"')) {
                return @{ sent = $sent; got = [KgnDelayed]::Ms(); line = $line; ok = ($line -match '"ok"\s*:\s*true') }
            }
        }
        return @{ sent = $sent; got = [KgnDelayed]::Ms(); line = "NO REPLY within $timeoutMs ms"; ok = $false }
    } finally {
        $pipe.Dispose()
    }
}

function Get-CorePid {
    $procs = @(Get-Process keygnosys-core -ErrorAction SilentlyContinue)
    if ($procs.Count -ne 1) { return 0 }
    return $procs[0].Id
}

function Start-SignalChild([string]$mode, [int]$corePid, [scriptblock]$whileWaiting = $null) {
    $argsList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
                  '-Signal', $mode, '-CorePid', $corePid, '-Log', $Log)
    $p = Start-Process powershell.exe -ArgumentList $argsList -WindowStyle Hidden -PassThru
    [void]$p.Handle   # without a handle taken early, ExitCode reads empty after exit
    # At the fire, keep sampling while the child starts and signals: a blocking
    # wait here left a ~1 s hole in samples.csv exactly where the release lands.
    while (-not $p.HasExited) {
        if ($null -ne $whileWaiting) { & $whileWaiting }
        [System.Threading.Thread]::Sleep(10)
    }
    $p.WaitForExit()
    return $p.ExitCode
}

# ---------------------------------------------------------------------------
# Preconditions: fail now, not at the fire.

$corePid = 0
$client = $null
if ($Do -eq 'start-core') {
    if (@(Get-Process keygnosys-core -ErrorAction SilentlyContinue).Count -ne 0) { Refuse "a keygnosys-core is already running; start-core needs none" }
    if (-not (Test-Path -LiteralPath $coreExe)) { Refuse "core executable not found: $coreExe" }
    Stamp $Log "precondition: no core running; will start $coreExe" | Out-Null
}
if ($Do -ne 'noop' -and $Do -ne 'start-core') {
    $corePid = Get-CorePid
    if ($corePid -eq 0) { Refuse "exactly one keygnosys-core must be running" }
    # Open the handle now: an exit code is only readable through a handle taken
    # before the process exits. It separates a clean unwind (0) from a kill.
    $coreProc = Get-Process -Id $corePid
    [void]$coreProc.Handle
    Stamp $Log "precondition: keygnosys-core pid $corePid" | Out-Null
}
if ($ipc.ContainsKey($Do) -or $Do -eq 'drop-client') {
    $probe = Invoke-Kgn 'ping' '{}'
    if (-not $probe.ok) { Refuse ("core did not answer ping: " + $probe.line) }
    Stamp $Log ("precondition: ping ok in {0:F1} ms" -f ($probe.got - $probe.sent)) | Out-Null
}
$targetWindow = [IntPtr]::Zero
if ($Do -eq 'close-window-titled') {
    $titled = @([KgnDelayed]::WindowsTitled($Title))
    if ($titled.Count -ne 1) {
        $list = ($titled | ForEach-Object { "0x{0:X}" -f $_.ToInt64() }) -join ', '
        Refuse ("exactly one top-level window must be titled exactly '{0}'; found {1} {2}" -f $Title, $titled.Count, $list)
    }
    $targetWindow = $titled[0]
    $windowPid = [uint32]0
    [void][KgnDelayed]::GetWindowThreadProcessId($targetWindow, [ref]$windowPid)
    $windowProc = Get-Process -Id $windowPid -ErrorAction SilentlyContinue
    Stamp $Log ("precondition: window 0x{0:X} class '{1}' owner pid {2} ({3}) title '{4}'" -f $targetWindow.ToInt64(),
        [KgnDelayed]::ClassOf($targetWindow), $windowPid, $(if ($windowProc) { $windowProc.ProcessName } else { '?' }),
        [KgnDelayed]::TitleOf($targetWindow)) | Out-Null
}
if ($Do -eq 'ctrl-c-core' -or $Do -eq 'close-core-console') {
    $mode = if ($Do -eq 'ctrl-c-core') { 'check-ctrl-c' } else { 'check-close' }
    $code = Start-SignalChild $mode $corePid
    if ($code -ne 0) {
        $why = if ($code -eq 4) { "the core's console is not a classic conhost window; launch the core with conhost.exe" }
               else { "cannot attach to the core's console (child exit $code; see $Log.child.txt)" }
        Refuse $why
    }
    Stamp $Log "precondition: core console attachable ($mode)" | Out-Null
}
if ($Do -eq 'drop-client') {
    # The client lives in its own process so the fire can terminate it abruptly,
    # exactly as a crashed client would vanish, while this script keeps sampling.
    $clientCode = @'
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'keygnosys', [System.IO.Pipes.PipeDirection]::InOut)
$pipe.Connect(2000)
$reader = New-Object System.IO.StreamReader($pipe)
$first = $reader.ReadLine()
Set-Content -LiteralPath $env:KGN_CLIENT_STATUS -Value ("connected: " + $first.Substring(0, [Math]::Min(60, $first.Length)))
while ($null -ne $reader.ReadLine()) { }
'@
    $status = "$Log.client.txt"
    if (Test-Path -LiteralPath $status) { Remove-Item -LiteralPath $status }
    $env:KGN_CLIENT_STATUS = $status
    # Encoded, because Start-Process joins its arguments with spaces and would
    # mangle a multi-line script's quoting.
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($clientCode))
    $client = Start-Process powershell.exe -ArgumentList @('-NoProfile', '-EncodedCommand', $encoded) -WindowStyle Hidden -PassThru
    $wait = [KgnDelayed]::Ms() + 10000
    while (-not (Test-Path -LiteralPath $status) -and [KgnDelayed]::Ms() -lt $wait) { Start-Sleep -Milliseconds 50 }
    if (-not (Test-Path -LiteralPath $status)) {
        Stop-Process -Id $client.Id -Force -ErrorAction SilentlyContinue
        Refuse "the client process did not connect within 10 s"
    }
    Stamp $Log ("precondition: client pid {0} {1}" -f $client.Id, (Get-Content -LiteralPath $status)) | Out-Null
}

# ---------------------------------------------------------------------------
# Countdown with sampling, then the fire, then post-fire sampling.

[void][KgnDelayed]::timeBeginPeriod(1)
$sw = New-Object System.IO.StreamWriter($samples, $false, (New-Object System.Text.UTF8Encoding($false)))
$sw.WriteLine("qpc_ms,rel_fire_ms,x,y,lbutton,rbutton,mbutton,shift,ctrl,alt,lwin,lshift,rshift")
$fireAt = [KgnDelayed]::Ms() + $Seconds * 1000.0
$endAt = $fireAt + $After * 1000.0
$fired = $false
$result = $null
$exitCode = 1
$lastShown = -1

function Sample {
    $pt = New-Object KgnDelayed+POINT
    [void][KgnDelayed]::GetCursorPos([ref]$pt)
    $now = [KgnDelayed]::Ms()
    $sw.WriteLine(("{0:F3},{1:F1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12}" -f $now, ($now - $fireAt),
        $pt.X, $pt.Y, [KgnDelayed]::Down(0x01), [KgnDelayed]::Down(0x02), [KgnDelayed]::Down(0x04),
        [KgnDelayed]::Down(0x10), [KgnDelayed]::Down(0x11), [KgnDelayed]::Down(0x12), [KgnDelayed]::Down(0x5B),
        [KgnDelayed]::Down(0xA0), [KgnDelayed]::Down(0xA1)))
}

# Starts one fixed evidence command hidden, its output redirected to a file.
# Encoded, because Start-Process would otherwise mangle the quoting.
function Start-Evidence([string]$script) {
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($script))
    $p = Start-Process powershell.exe -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $encoded) -WindowStyle Hidden -PassThru
    return $p
}

function Wait-Sampling([scriptblock]$done, [int]$timeoutMs) {
    $deadline = [KgnDelayed]::Ms() + $timeoutMs
    while ([KgnDelayed]::Ms() -lt $deadline) {
        if (& $done) { return $true }
        Sample
        [System.Threading.Thread]::Sleep(10)
    }
    return (& $done)
}

Stamp $Log ("armed: fires in {0} s" -f $Seconds) | Out-Null
Write-Host ("ARMED  -Do {0}  fires in {1} s.  Cancel: close this window." -f $Do, $Seconds) -ForegroundColor Yellow
try {
    while ([KgnDelayed]::Ms() -lt $fireAt) {
        Sample
        $left = [int][Math]::Ceiling(($fireAt - [KgnDelayed]::Ms()) / 1000.0)
        if ($left -ne $lastShown) { Write-Host ("  {0}" -f $left); $lastShown = $left }
        [System.Threading.Thread]::Sleep(10)
    }

    $fired = $true
    Write-Host "FIRE" -ForegroundColor Red
    Stamp $Log "FIRE $Do" | Out-Null
    $sw.Flush()

    if ($ipc.ContainsKey($Do)) {
        $spec = $ipc[$Do]
        $result = Invoke-Kgn $spec[0] $spec[1]
        Stamp $Log ("reply after {0:F1} ms: {1}" -f ($result.got - $result.sent), $result.line) | Out-Null
        $exitCode = if ($result.ok) { 0 } else { 1 }
    } elseif ($Do -eq 'noop') {
        $exitCode = 0
    } elseif ($Do -eq 'kill-core') {
        $tk = Start-Process taskkill.exe -ArgumentList @('/F', '/IM', 'keygnosys-core.exe') -WindowStyle Hidden -PassThru
        [void]$tk.Handle
        while (-not $tk.HasExited) { Sample; [System.Threading.Thread]::Sleep(10) }
        $tk.WaitForExit()
        $code = $tk.ExitCode
        Stamp $Log ("taskkill exit {0}" -f $code) | Out-Null
        $exitCode = if ($code -eq 0) { 0 } else { 1 }
    } elseif ($Do -eq 'ctrl-c-core' -or $Do -eq 'close-core-console') {
        $mode = if ($Do -eq 'ctrl-c-core') { 'ctrl-c' } else { 'close' }
        $code = Start-SignalChild $mode $corePid { Sample }
        Stamp $Log ("signal child exit {0}" -f $code) | Out-Null
        $exitCode = if ($code -eq 0) { 0 } else { 1 }
    } elseif ($Do -eq 'drop-client') {
        Stop-Process -Id $client.Id -Force
        Stamp $Log ("client pid {0} force-terminated" -f $client.Id) | Out-Null
        $exitCode = 0
    } elseif ($Do -eq 'close-window-titled') {
        # Re-check the identity matched before the countdown: a window closed or
        # retitled in the meantime is not closed by mistake.
        if (-not [KgnDelayed]::IsWindow($targetWindow) -or
            -not [string]::Equals([KgnDelayed]::TitleOf($targetWindow), $Title, [StringComparison]::Ordinal)) {
            Stamp $Log ("NOT SENT: window 0x{0:X} no longer exists or is no longer titled '{1}'" -f $targetWindow.ToInt64(), $Title) | Out-Null
            $exitCode = 1
        } else {
            $ok = [KgnDelayed]::PostMessageW($targetWindow, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            Stamp $Log ("PostMessage(WM_CLOSE) to window 0x{0:X} returned {1}" -f $targetWindow.ToInt64(), $ok) | Out-Null
            $exitCode = if ($ok) { 0 } else { 1 }
        }
    } elseif ($Do -eq 'start-core') {
        $env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
        $host1 = Start-Process conhost.exe -ArgumentList @($coreExe) -PassThru
        Stamp $Log ("conhost pid {0} started for the core" -f $host1.Id) | Out-Null
        $script:startedCore = $null
        $appeared = Wait-Sampling { $script:startedCore = Get-Process keygnosys-core -ErrorAction SilentlyContinue | Select-Object -First 1; $null -ne $script:startedCore } 10000
        if (-not $appeared) {
            Stamp $Log "START FAILED: no keygnosys-core process within 10 s" | Out-Null
        } else {
            $corePid = $script:startedCore.Id
            $coreProc = $script:startedCore; [void]$coreProc.Handle
            Stamp $Log ("core process pid {0} exists" -f $corePid) | Out-Null
            $script:readyReply = $null
            # Short connect timeout, so sampling keeps running while the core
            # comes up; a pipe that does not exist yet throws, which is "not ready".
            $ready = Wait-Sampling {
                $r = $null
                try { $r = Invoke-Kgn 'ping' '{}' 500 50 } catch { $r = @{ ok = $false } }
                if ($r.ok) { $script:readyReply = $r }
                $r.ok
            } 10000
            if (-not $ready) {
                Stamp $Log "START FAILED: core did not answer ping within 10 s" | Out-Null
            } else {
                Stamp $Log ("core READY (answered ping; hook installed during start)") | Out-Null
                $obsScript = "& '{0}' -Seconds 3600 -Out '{1}' *> '{2}'" -f (Join-Path $PSScriptRoot 'observe_keys.ps1'), "$Log.keys-post.csv", "$Log.keys-post.out"
                $recScript = "& '{0}' '{1}' --out '{2}' --timeout 3600 *> '{3}'" -f (Join-Path $repoRoot '.venv\Scripts\python.exe'), (Join-Path $PSScriptRoot 'record.py'), "$Log.core.jsonl", "$Log.core.out"
                $obs = Start-Evidence $obsScript
                $rec = Start-Evidence $recScript
                Stamp $Log ("evidence: post-core key observer wrapper pid {0}; recorder wrapper pid {1}" -f $obs.Id, $rec.Id) | Out-Null
                $armed = Wait-Sampling {
                    (Test-Path -LiteralPath "$Log.keys-post.out") -and ((Get-Content -LiteralPath "$Log.keys-post.out" -ErrorAction SilentlyContinue) -match 'probe: ok') -and
                    (Test-Path -LiteralPath "$Log.core.out") -and ((Get-Content -LiteralPath "$Log.core.out" -ErrorAction SilentlyContinue) -match 'recording to')
                } 20000
                if ($armed) {
                    Stamp $Log "evidence ARMED: post-core observer probe ok; recorder connected" | Out-Null
                    Stamp $Log "RELEASE PROMPT shown" | Out-Null
                    Write-Host ""
                    Write-Host "  >>> READY: core running and observed. RELEASE THE KEY UNDER TEST NOW. <<<" -ForegroundColor Green
                    Write-Host "  (then touch nothing until this window prints 'done')" -ForegroundColor Green
                    Write-Host ""
                    $endAt = [Math]::Max($endAt, [KgnDelayed]::Ms() + $After * 1000.0)
                    $exitCode = 0
                } else {
                    Stamp $Log "EVIDENCE NOT ARMED within 20 s -- run not valid" | Out-Null
                    Write-Host "  EVIDENCE NOT ARMED -- this run is not valid; tell the agent" -ForegroundColor Red
                }
            }
        }
    }

    while ([KgnDelayed]::Ms() -lt $endAt) { Sample; [System.Threading.Thread]::Sleep(10) }

    if ($corePid -ne 0) {
        $coreProc.Refresh()
        if ($coreProc.HasExited) {
            Stamp $Log ("after {0} s: core pid {1} EXITED, exit code {2}, exit time {3:HH:mm:ss.fff}" -f $After,
                $corePid, $coreProc.ExitCode, $coreProc.ExitTime) | Out-Null
        } else {
            Stamp $Log ("after {0} s: core pid {1} still running" -f $After, $corePid) | Out-Null
        }
        if ($Do -eq 'drop-client') {
            $check = Invoke-Kgn 'ping' '{}'
            Stamp $Log ("after drop: core ping ok={0}" -f $check.ok) | Out-Null
            if (-not $check.ok) { $exitCode = 1 }
        }
    }
} finally {
    $sw.Flush(); $sw.Close()
    [void][KgnDelayed]::timeEndPeriod(1)
    if ($null -ne $client -and -not $client.HasExited) { Stop-Process -Id $client.Id -Force -ErrorAction SilentlyContinue }
    if (-not $fired) { Stamp $Log "ended WITHOUT firing" | Out-Null }
}

Stamp $Log ("exit {0}" -f $exitCode) | Out-Null
Write-Host ("done, exit {0}. Log: {1}" -f $exitCode, $Log)
exit $exitCode
