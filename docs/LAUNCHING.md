# KeyGnosys — Launcher Contract

*Master Keys to Your System*

**Status:** contract · **Implementation:** deferred until after **M4**

This document specifies the launcher scripts: their option surface, what each
option is allowed to do, and what each exit code means. **Nothing described here
is implemented yet**, and nothing here is a licence to implement it early — see
§1.2 for why the timing matters.

It is written as a contract rather than as documentation of existing behaviour
so that the implementation, when it arrives, has no semantics left to invent.
Requirement keywords **MUST**, **MUST NOT**, **SHOULD** and **MAY** are used in
the RFC 2119 sense, as in [SPEC.md](SPEC.md).

---

## Table of contents

1. [Scope and timing](#1-scope-and-timing)
2. [The launcher surface](#2-the-launcher-surface)
3. [Invocation model](#3-invocation-model)
4. [Launching](#4-launching)
5. [Detecting an existing instance](#5-detecting-an-existing-instance)
6. [Prerequisites and privilege](#6-prerequisites-and-privilege)
7. [Dependency and build policy](#7-dependency-and-build-policy)
8. [Diagnostics](#8-diagnostics)
9. [Autostart](#9-autostart)
10. [Exit codes](#10-exit-codes)
11. [Output](#11-output)
12. [Deliberately not specified](#12-deliberately-not-specified)

---

## 1. Scope and timing

### 1.1 What the launcher is for

KeyGnosys is two processes (OUTLINE §3): the native core, which must be running
and listening before the overlay can attach to it, and the overlay itself. Today
a developer starts each by hand. The launcher exists to make "get KeyGnosys
running on this machine, from this checkout" a single command that behaves the
same on both supported platforms — including the case where the checkout has
never been built and the Python environment does not exist yet.

It is a **developer and self-installer convenience**, not the shipping installer.
Packaged installation is M7 and is a separate concern: an installer places files
and system-level integration, whereas the launcher prepares and starts a
checkout. In particular the Linux udev rule and `input` group membership that
the core's evdev backend needs (SPEC §12.5) are installer work and are **never**
performed by the launcher (§6.2).

### 1.2 Why implementation waits for M4

M3 makes Windows runnable and M4 makes Linux runnable. Writing the launcher
after M3 would mean designing it against one real platform and one imagined one,
and the predictable result is a Windows-shaped design with a Linux script
retrofitted into it. The two platforms disagree on almost everything the
launcher touches — process detachment, endpoint semantics, autostart mechanism,
toolchain discovery — so the implementation waits until both backends exist and
can be developed against together.

The **contract** is settled now, before M2, because M2 creates the
`keygnosys-core` executable and the IPC server that the launcher will manage,
and it is cheaper to know what the launcher needs from them than to discover it
two milestones later.

### 1.3 What this document does not change

This is a contract for future work. It specifies no change to the core, the
overlay, the IPC protocol, the configuration formats, or any current runtime
behaviour, and it introduces no new commands or events.

---

## 2. The launcher surface

| Path | Platform | Role |
|------|----------|------|
| `scripts/start-keygnosys.sh` | Linux | The Linux implementation |
| `scripts/start-keygnosys.ps1` | Windows | The Windows implementation |
| `scripts/start-keygnosys.cmd` | Windows | Shim only |
| `docs/LAUNCHING.md` | — | This contract |

There are **two implementations, not three.** `start-keygnosys.cmd` exists so
that the launcher can be double-clicked and invoked from `cmd.exe` without the
caller reasoning about execution policy; it **MUST** contain no logic of its own
beyond locating `start-keygnosys.ps1`, invoking it with the arguments it was
given, and propagating its exit code. Two implementations of this contract per
platform would drift, and the drift would be silent.

Both implementations **MUST** accept the identical option surface of §3 and
**MUST** use the identical exit-code table of §10. An option that is meaningless
on a platform is still parsed and still validated there; it does not become an
unknown option.

Each implementation resolves the repository root from its own location, so the
launcher works from any working directory and is safe to invoke by absolute path
— which is what autostart does (§9).

---

## 3. Invocation model

### 3.1 Verbs and modifiers

The surface is four **verbs** and four **modifiers**. A verb selects what the
launcher does and terminates when it is done; a modifier adjusts how.

| Verb | Effect |
|------|--------|
| *(none)* | **Launch.** Prepare if needed, then start KeyGnosys (§4) |
| `--diagnostics` | Inspect and report. Strictly side-effect-free (§8) |
| `--install-autostart` | Register per-user autostart (§9) |
| `--remove-autostart` | Remove per-user autostart (§9) |
| `--autostart-status` | Report autostart registration (§9) |

| Modifier | Effect |
|----------|--------|
| `--background` | Detach the started processes from the invoking terminal (§4.3) |
| `--repair` | Force preparation to run from scratch, then launch (§7.3) |
| `--no-install` | Install nothing and build nothing; fail instead (§7.2) |
| `--non-interactive` | Never prompt (§7.4) |

At most one verb **MAY** be given. Zero verbs means launch.

### 3.2 Legal combinations

| | launch | `--diagnostics` | `--install-autostart` | `--remove-autostart` | `--autostart-status` |
|---|---|---|---|---|---|
| `--background` | ✅ | ❌ | ❌ | ❌ | ❌ |
| `--repair` | ✅ | ❌ | ❌ | ❌ | ❌ |
| `--no-install` | ✅ | ⭕ | ⭕ | ⭕ | ⭕ |
| `--non-interactive` | ✅ | ✅ | ✅ | ✅ | ✅ |

✅ meaningful · ⭕ accepted, no effect · ❌ rejected as a usage error

Rules, normatively:

- Two or more verbs **MUST** be rejected with exit **2**.
- An unknown option **MUST** be rejected with exit **2**.
- `--background` with any verb **MUST** be rejected with exit **2**. Every verb
  is a report or a registration that runs in the foreground and terminates;
  there is nothing to detach.
- `--repair` with any verb **MUST** be rejected with exit **2**. Repair is
  defined as preparation followed by a launch (§7.3), so it is only meaningful
  when a launch is what is happening.
- `--repair` together with `--no-install` **MUST** be rejected with exit **2**.
  They are direct contradictions: one forces preparation, the other forbids it.
- `--no-install` on any verb other than a launch **MUST** be accepted and have
  no effect. No verb installs anything, so the flag is already true; rejecting
  it would break the ordinary habit of putting a fixed set of flags in a wrapper
  script or shell alias.
- `--non-interactive` is legal everywhere.
- Repeating an option is not an error; it is the same as giving it once.

Usage errors are detected **before** any other work. A rejected command line
**MUST NOT** have inspected the environment, started a process, or written
anything.

---

## 4. Launching

### 4.1 Order of operations

A launch proceeds in this order and stops at the first step that fails:

1. **Parse and validate** the command line (§3.2). Failure → exit **2**.
2. **Establish the environment is supported** — platform, and on Linux a usable
   X11 display (SPEC §8.5). Failure → exit **3**.
3. **Check system prerequisites** (§6). Failure → exit **4**.
4. **Prepare the Python environment** (§7). Failure → exit **5**.
5. **Build the core** (§7). Failure → exit **6**.
6. **Probe for a running core** (§5). If one is live, skip step 7.
7. **Start the core** and wait for it to become ready. Failure → exit **7**.
8. **Start the overlay.** Failure → exit **8**.
9. Exit **0**.

### 4.2 The core starts first, and readiness is not existence

The launcher **MUST NOT** start the overlay until the core is **ready**, where
ready means: a client connection to the IPC endpoint succeeds and the core's
`hello` event (SPEC §5.3) has been received.

This is stricter than checking that the endpoint exists, and deliberately so.
The overlay chooses its backend exactly once, at startup: `_make_client` in
`python/keygnosys/app.py` tests for the endpoint's presence and falls back to
the mock backend if it is absent, and that choice is never revisited. An overlay
started a moment too early therefore runs against the mock **permanently** — it
lights up as you type and does nothing else. That is precisely the plausible-
looking wrong state that P6 forbids, and the only reliable defence is not to
start the overlay until the core is genuinely answering.

The launcher **SHOULD** apply a bounded wait for readiness — 10 seconds is the
default — and on expiry **MUST** exit **7** rather than start an overlay it
knows will attach to nothing.

The `hello` payload also carries `protocol`. If its major differs from the
overlay's, the launcher **SHOULD** report the mismatch naming both versions and
exit **7**, rather than starting an overlay that will refuse the connection
itself (SPEC §5.5). Discovering a version mismatch from the launcher is strictly
more useful than discovering it from a GUI that has already opened.

### 4.3 Foreground and `--background`

**Foreground** (the default) keeps the launcher attached. It **MUST** stop the
processes *it started*, in reverse order — overlay first, then core — when it
exits or is interrupted. A `Ctrl-C` that leaves an orphaned core holding a
keyboard grab is the single worst failure this contract can permit.

**Ownership is what decides this.** If the launcher attached to a core that was
already running (§5), it **MUST NOT** stop that core, on any exit path. It did
not start it and does not know who depends on it.

`--background` detaches the started processes from the invoking terminal,
redirects their standard output and error to the log root of SPEC §3.1, and
returns as soon as the outcome is known — exit **0** once both are up, or the
appropriate failure code. Under `--background` the launcher does not own the
processes it started and **MUST NOT** stop them on exit; that is the point of
detaching.

### 4.4 Idempotence

A launch is a request for a steady state, not for a state transition. Running it
twice **MUST** be safe: the second run finds the core already live, does not
start a second one, and exits **0** (§5, §10.2).

---

## 5. Detecting an existing instance

The launcher **MUST** determine whether a core is running by connecting to the
KeyGnosys IPC endpoint. It **MUST NOT** invent a lock file, a PID file, a mutex,
or a process-name scan for this purpose.

**The endpoint rule is [SPEC §5.1.1](SPEC.md#5-ipc-protocol), and this document
does not restate it.** The core, the overlay and the launcher are all bound by
that one rule — including the fallback applied on Linux when
`$XDG_RUNTIME_DIR` is unset — and a launcher that probed a path its peers do not
resolve to would report a running core the overlay cannot find. On the Python
side the rule is `keygnosys.paths.ipc_endpoint()`; the launcher **MUST** obtain
the endpoint from that same rule rather than composing a path of its own.

**A connection that succeeds and yields `hello` is the only proof of life.** The
endpoint merely existing proves nothing: a Unix socket file outlives the process
that bound it, so a stale path is indistinguishable from a live one by
inspection. `hello` additionally tells the launcher the core version, protocol
version, active backends, capabilities and limitations, all of which the
diagnostics report wants anyway (§8.2).

The launcher **MUST NOT** create, delete, move, or recreate the endpoint, under
any option including `--repair`. Only the core does any of that, and recovering
an endpoint left behind by a crashed core is the core's own responsibility at
bind time — specified in [SPEC §5.1.3](SPEC.md#5-ipc-protocol), which also fixes
the rule that a live endpoint is never stolen. The launcher connects and reads;
that is the whole of its relationship with the endpoint.

**Only the core is single-instance.** Two cores are harmful — two device grabs
on Linux, two hooks on Windows — and the launcher's job is to make that
impossible. The overlay is a viewer (P4); a second one is a cosmetic nuisance,
not a correctness problem, and there is no IPC command that enumerates connected
clients. So the launcher does not attempt overlay single-instance detection, and
**MAY** start an overlay whether or not one is already running. If overlay
single-instance is wanted later it belongs in the overlay, not here (§12).

---

## 6. Prerequisites and privilege

### 6.1 Two classes of prerequisite

| Class | Examples | Launcher may install? |
|-------|----------|-----------------------|
| **System** | CMake, Ninja, a C++17 compiler, X11/evdev development headers, Python itself | **Never** |
| **User-space** | The project virtual environment, PySide6, `python-xlib`, the editable install of `keygnosys` | Yes, subject to §7 |

The line is not "big versus small". It is **whether satisfying it requires
authority over the machine rather than over the user's own files.**

### 6.2 The launcher never elevates

The launcher **MUST NOT**, under any option:

- invoke `sudo`, `doas`, `pkexec`, `runas`, or request UAC elevation;
- invoke a system package manager — `apt`, `apt-get`, `dnf`, `yum`, `pacman`,
  `zypper`, `apk`, `brew`, `winget`, `choco`, `scoop`, or MSYS2 `pacman`;
- write outside the repository, the project virtual environment, the user
  configuration root, the log root, and the user's own autostart registration;
- install a udev rule, create or modify a group, or change group membership;
- re-launch itself elevated.

On a missing system prerequisite it **MUST** report the prerequisite by name,
why it is needed, and — where the platform and package manager are reliably
detectable — the exact command **for the user to run**, then exit **4**.

Where the environment cannot be determined reliably, it **MUST** name the
prerequisite without guessing a command. A confidently wrong install command is
worse than none: it gets pasted into a root shell.

The install commands already documented in
[`core/README.md`](../core/README.md) are the source for this text; the launcher
**SHOULD NOT** maintain a second, divergent list.

This rule holds identically under `--non-interactive`. Automation is a reason to
skip the prompt, never a reason to widen the blast radius.

---

## 7. Dependency and build policy

### 7.1 The matrix

| Condition | Launch (interactive) | `--non-interactive` | `--no-install` | `--repair` |
|-----------|----------------------|---------------------|----------------|------------|
| Virtual environment missing | Explain, confirm once, create | Create | Exit **5** | Recreate |
| Python dependencies missing or unimportable | Explain, confirm once, install | Install | Exit **5** | Reinstall |
| `keygnosys-core` absent or out of date | Explain, confirm once, build | Build | Exit **6** if absent | Reconfigure from scratch, build |
| System prerequisite missing | Report, exit **4** | Report, exit **4** | Report, exit **4** | Report, exit **4** |

Confirmation is asked **once** per launch, covering everything the launcher
proposes to do, with a default of yes. A launcher that asks four questions is a
launcher people stop running.

### 7.2 `--no-install`

`--no-install` means: **use what is here, or fail saying what is missing.** The
launcher installs nothing, builds nothing, and creates no virtual environment.
It reports the first unmet condition and exits **5** (environment) or **6**
(build).

It exists for two callers who both need the launcher to be cheap and predictable
rather than helpful: autostart at login (§9.3), and any script that must not
have a compile or a package download appear inside it.

### 7.3 `--repair`

`--repair` forces every preparation step to run from scratch rather than being
skipped as already satisfied — recreate the virtual environment, reinstall the
dependencies, reconfigure the build tree and rebuild — and then **continues into
the ordinary launch**. A repair that leaves the user not running has done half
the job.

`--repair` is scoped to the **development and runtime environment**: the virtual
environment, the Python dependencies, and the native build tree. It **MUST NOT**
touch the user configuration root, user documents, settings, or logs, and it
**MUST NOT** touch the IPC endpoint (§5). SPEC §3.4 guarantees that an update
never costs the user their configuration; a repair command is exactly where that
guarantee would be lost by accident, so it is stated here explicitly.

Reconfiguring from scratch is what makes `--repair` worth having: a CMake cache
records absolute paths, so a checkout that has been moved or renamed produces a
build tree that fails in ways no incremental build recovers from.

### 7.4 `--non-interactive`

`--non-interactive` means **never prompt**. It proceeds with the same user-space
preparation it would otherwise have proposed, without asking.

It is not a security boundary and **MUST NOT** be treated as one. The boundary
is §6.2 — user-space only, never elevated — and it holds identically whether or
not anyone is at the keyboard. Making `--non-interactive` refuse to install
would make it useless to its main caller, autostart, which then pairs it with
`--no-install` (§9.3) when a stricter guarantee is actually wanted.

Under `--non-interactive` the launcher **MUST NOT** read from standard input and
**MUST NOT** block waiting for a response on any code path.

### 7.5 Which build

The launcher builds the `default` preset (RelWithDebInfo). The `debug` preset is
the pre-commit warnings gate described in [`core/README.md`](../core/README.md)
and is not what a user launching KeyGnosys wants.

Because `cmake --build` is already incremental and costs milliseconds when
nothing changed, the launcher **SHOULD** simply run it rather than implementing
a staleness heuristic of its own. Under `--no-install` it runs nothing and
requires the executable to exist.

### 7.6 Network access

SPEC §12.2 commits that KeyGnosys makes **no network access, ever**. That
commitment is about the running application — the core and the overlay open no
socket but the local IPC endpoint, and there is no telemetry, no update check
and no crash upload. It is not weakened here.

The launcher is neither the core nor the overlay. Its only network use is
installing user-space Python dependencies that the user asked for, at
preparation time, from the ordinary package index — which `--no-install`
forbids outright. It performs no other network access of any kind, and
`--diagnostics` performs none at all (§8.1).

---

## 8. Diagnostics

### 8.1 `--diagnostics` is strictly side-effect-free

`--diagnostics` inspects and reports. **It changes nothing.**

It **MUST NOT**:

- install anything, or create or modify a virtual environment;
- download anything, or access the network at all;
- configure or build anything;
- start KeyGnosys, or any part of it;
- stop KeyGnosys, or any part of it;
- install, modify, or remove autostart;
- create, modify, or delete user configuration, user documents, or settings;
- create, modify, or delete the user configuration root or any directory in it;
- modify the repository, the build tree, or the IPC endpoint;
- write anything anywhere, other than its own report on standard output and
  standard error.

The value of a diagnostic command is that it is safe to run when you do not know
what state the machine is in. A command that repairs while it reports cannot be
trusted at exactly the moment it is needed, and its report describes a machine
that no longer exists.

**One concrete consequence, recorded so it is not rediscovered as a bug:**
`--diagnostics` **MUST NOT** obtain document validation by invoking
`keygnosys --check`. That path calls `paths.ensure_user_dirs()` in
`load_registry` (`python/keygnosys/app.py`), which creates the user
configuration tree. Diagnostics reports whether that tree exists; it does not
bring it into existence. Until a validation entry point that does not create
directories is available, diagnostics reports document validation as not
performed rather than performing it unsafely.

### 8.2 What it reports

The report **SHOULD** cover, marking each item found, missing, or not
applicable:

| Group | Contents |
|-------|----------|
| Environment | Platform and version; display environment (X11, Wayland, headless); whether it is supported (SPEC §8.5) |
| Repository | Resolved root; current revision, read-only |
| Python | Interpreter and version; virtual environment path and presence; whether `keygnosys` is importable; PySide6 version; `python-xlib` on Linux |
| Native | CMake, Ninja and compiler versions; whether the build tree exists; whether `keygnosys-core` exists |
| Core | Endpoint path; whether a core answered; and if it did, the `hello` payload — core version, protocol, backends, capabilities, limitations |
| Paths | User configuration root and log root, and whether each exists |
| Autostart | The same information `--autostart-status` reports (§9.4) |

The report **MUST NOT** contain key codes or keystroke content (SPEC §12.1).
This is trivially true of everything listed above, and is stated so that it
stays true of anything added later.

### 8.3 What it exits with

`--diagnostics` exits **0** when the report was produced and nothing was found
that would block a launch.

When something would block a launch, it exits with the code that a launch would
have failed with — **3**, **4**, **5** or **6** — for the **earliest-blocking**
condition found. Earliest, not most numerous: that condition is the one to fix
first, and the others may well be consequences of it.

It exits **1** if diagnostics itself could not complete. It never exits **7**,
**8** or **9**, because it never starts anything and never registers anything.

This makes `--diagnostics` a prediction of what a launch would do, which is what
makes it worth putting in a bug report.

---

## 9. Autostart

### 9.1 Common rules

All three autostart verbs are **per-user**, require no administrator or root
privilege, and are subject to §6.2 in full.

- They **MUST NOT** start or stop KeyGnosys. Registering autostart and running
  KeyGnosys now are different requests.
- `--install-autostart` is idempotent: when a registration already exists it is
  updated to the command that would be registered today, and the verb exits
  **0**.
- `--remove-autostart` is idempotent: when no registration exists it exits **0**,
  because the requested state already holds.
- A failure of the platform mechanism exits **9**.

### 9.2 Mechanism

**Windows.** A Task Scheduler task named `KeyGnosys`, per-user, with an
`ONLOGON` trigger and a limited (non-elevated) run level. Registering a logon
task for the current user needs no elevation.

Task Scheduler is preferred over `HKCU\...\Run` and the Startup folder because
it records a last-run result that `--autostart-status` can report, and supports
a startup delay and restart-on-failure. A `Run` key entry that silently fails at
logon leaves the user with nothing to inspect.

**Linux.** An XDG desktop entry at `~/.config/autostart/keygnosys.desktop`,
`Type=Application`, with `Exec` set to the absolute path of
`scripts/start-keygnosys.sh` and its flags.

The desktop entry carries no display guarding of its own. The launcher already
refuses to run without a supported display environment and exits **3** (§4.1
step 2), so guarding in the entry as well would mean two rules that can disagree.

### 9.3 The registered command

The registered command is the launcher's absolute path, invoked with:

```
--background --non-interactive --no-install
```

`--background` because a login is not a terminal session. `--non-interactive`
because there is nobody to answer a prompt, and a blocked prompt at logon is
invisible. `--no-install` because **a login must never trigger a compile or a
package download** — the failure mode is a machine that appears to hang while
logging in, and the user has no way to connect that to a `git pull` from last
week.

The consequence is accepted deliberately: after a change that requires a
rebuild, autostart stops working until the user runs the launcher once by hand.
That is a visible, diagnosable failure — `--autostart-status` and
`--diagnostics` both surface it — and is preferable to an invisible one.

### 9.4 `--autostart-status`

Reports:

- whether a registration exists;
- the exact command registered;
- whether that command matches what `--install-autostart` would register today,
  and whether its target still exists — **registration drift**;
- the last run result, where the platform exposes one.

Drift detection is not incidental. The registration stores an absolute path, so
moving or renaming the checkout leaves a registration that points at nothing and
fails silently at every login. Reporting it is the only way the user finds out.

`--autostart-status` is a report, and is bound by §8.1: it **MUST NOT** create
or repair a registration it finds missing or broken.

---

## 10. Exit codes

### 10.1 The table

| Code | Meaning |
|------|---------|
| **0** | Success — the requested state now holds |
| **1** | Unexpected internal failure of the launcher itself |
| **2** | Invalid usage — unknown, contradictory, or excess options |
| **3** | Unsupported environment — platform or display environment not supported |
| **4** | Missing system prerequisite — reported, never installed (§6.2) |
| **5** | User-space environment preparation failed or was forbidden |
| **6** | Native build failed or was forbidden |
| **7** | The core failed to start or to become ready |
| **8** | The overlay failed to start |
| **9** | An autostart operation failed |

These are chosen to stay clear of the shell's reserved range (126, 127, 128+N)
and of 255. `2` for a usage error follows the `getopt`/`argparse` convention;
`1` remains the generic catch-all so that an unforeseen failure is never
mistaken for a specific one.

Codes are **stable**. A later addition to the surface takes the next free code
and does not renumber these.

### 10.2 Already running is exit 0

A launch that finds a live core already running exits **0**.

The alternative was considered and rejected. A launch is a request for a steady
state — "KeyGnosys should be running" — and that state holds. Every caller that
matters here is one for which failure would be actively wrong: autostart, which
may fire alongside a session already restored; a shell alias someone types
twice; a `--non-interactive` script whose whole purpose is to be safe to run
repeatedly. An idempotent command that reports failure for being idempotent is a
command people wrap in `|| true`, which throws away the codes that do matter.

The distinction is not lost, only moved: the exit code carries the outcome, and
the human-readable output says plainly that an existing core was found and
reused rather than started (§11).

### 10.3 Which failures share a code

**Separate, because the remedies differ.** `5` and `6` both mean "preparation
failed", but a failed `pip install` and a failed compile send the user to
entirely different places. Likewise `4` is not folded into `5`: the whole point
of `4` is that the launcher will *not* fix it and the user must.

**Separate, because the consequences differ.** `7` and `8` both mean "a process
did not start", but a core that failed to start means nothing works, whereas an
overlay that failed to start means the input layer is running without its map —
which OUTLINE §1 explicitly treats as a usable state. Accordingly, **when the
core started successfully and the overlay then failed, the launcher exits 8 and
leaves the core running.** It does not tear down working functionality because
an optional viewer failed.

**Shared, deliberately.** `--diagnostics` reuses `3`/`4`/`5`/`6` rather than
taking a code of its own (§8.3), so that the code means the same condition
whether it was predicted or hit. And every autostart failure shares `9`,
because the caller's next step — inspect the registration with
`--autostart-status` — is the same for all of them.

---

## 11. Output

**The exit code is the machine contract.** It is the only part of the launcher's
output this document fixes, and the only part a script should branch on.

Everything printed is **human-readable diagnostics**, intended to be read or
pasted into a bug report. Its wording is not a contract and **MAY** change.
Scripts **MUST NOT** parse it.

- Progress, results and reports go to standard output.
- Warnings and failures go to standard error.
- Every non-zero exit **MUST** be preceded by a message on standard error saying
  what failed and, where one exists, what the user can do about it. An exit code
  alone is not a diagnostic.
- A launch that reused an already-running core **MUST** say so (§10.2).
- Nothing the launcher prints ever contains key codes or keystroke content
  (SPEC §12.1).

A machine-readable diagnostics format is deliberately not specified (§12).

---

## 12. Deliberately not specified

Recorded so a later reader can tell an omission from an oversight.

| Left open | Why |
|-----------|-----|
| **Stopping a running instance** | No `--stop` verb in this surface. Stopping is done from the overlay's control bar or by ending the process. If a verb is wanted later it takes the next free exit code (§10.1) |
| **A machine-readable report** | No `--json`. There is no consumer for it yet, and a format shipped without one is a format that will be wrong |
| **Overlay single-instance** | No IPC command enumerates connected clients, and a second overlay is harmless (§5). If wanted, it belongs in the overlay |
| **macOS** | No backend exists (OUTLINE §8). The launcher does not pretend otherwise; it exits **3** |
| **Wayland** | X11 is the supported Linux display environment for v1 (SPEC §8.5). The launcher exits **3** rather than starting something that will half-work |
| **Packaged installation** | M7. An installer places files and system integration; the launcher prepares and starts a checkout (§1.1) |
