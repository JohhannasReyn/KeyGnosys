# keygnosys-core

The native input core: interception, suppression, pointer synthesis, window
management, and the JSON Lines IPC server the overlay connects to.

## Status

**Milestone M2 is complete.** `keygnosys-core` builds and runs on both
platforms, owns its IPC endpoint and serves the protocol. What it does **not**
do yet is see or synthesise a single key: there are no platform backends until
M3 and M4, and the core says so — in `hello`, in a diagnostic and on stderr —
rather than running as something that merely looks like it is working.

| | |
|---|---|
| ✅ | `kgn_engine`: the platform-free library — key vocabulary, layer engine, motion, actions, config, IPC protocol |
| ✅ | `src/keycode.cpp` — the key vocabulary, interning, modifier grouping |
| ✅ | `src/layer_engine.cpp` — the CapsLock state machine, the grace window, and the P7 forwarded-release invariant with its mirror |
| ✅ | `src/motion.cpp` — the 60 Hz integrator: ramp, diagonal normalisation, fractional accumulation |
| ✅ | `src/actions.cpp` — the action catalog and the dispatcher, with its fail-safe obligation rule |
| ✅ | `src/config.cpp` — bindings documents, where a bad binding costs exactly that binding |
| ✅ | `src/json.cpp` — the JSON subset the protocol needs |
| ✅ | `src/ipc.cpp` — the JSON Lines server: envelopes, sequences, bounded queues, reply routing |
| ✅ | `kgn_ipc` — endpoint ownership and the platform transports (SPEC 5.1.1–5.1.3) |
| ✅ | `keygnosys-core` — the executable |
| 🚧 | Windows backend: hook, `SendInput`, Win32 windows (**M3**) |
| 🚧 | Linux/X11 backend: evdev, uinput, EWMH, XRandR (**M4**) |

The overlay runs today without any of this, on the mock backend
(`keygnosys --backend mock`). It will connect to the core automatically once a
backend gives the core something to report. Nothing in the Python side changes
when it does.

## Running it

```sh
./build/default/core/keygnosys-core          # Linux
build\default\core\keygnosys-core.exe        # Windows
```

It takes the endpoint, starts listening, and prints what it cannot do. A second
instance refuses rather than joining, and names the condition it hit:

```
keygnosys-core: ipc.endpoint_in_use: another process already owns \\.\pipe\keygnosys
```

| Option | |
|--------|--|
| `--endpoint <address>` | Listen here instead of the resolved endpoint |
| `--bindings <id>` | Bindings document id (default `default`) |
| `--bindings-file <path>` | Load this document, ignoring the id |
| `--config-dir <path>` · `--data-dir <path>` | Where to look for documents |

Exit codes: `0` clean stop, `2` bad usage, `3` the endpoint is already owned,
`4` the endpoint could not be taken. These are the core's own codes and are
distinct from the launcher's ([LAUNCHING.md §10](../docs/LAUNCHING.md#10-exit-codes)),
which is a different program.

### The endpoint

Resolution, ownership and stale recovery are specified in
[SPEC §5.1.1–5.1.3](../docs/SPEC.md#5-ipc-protocol), and this implements them
rather than approximating them. On Linux the core verifies every directory
component it owns against a *descriptor* rather than a path, holds `core.lock`
with `flock` for the lifetime of the process, and only then probes and binds —
the lock is what makes the single-instance guarantee hold, because `connect()`
returns `ECONNREFUSED` for a perfectly healthy core caught between `bind()` and
`listen()`. On Windows `FILE_FLAG_FIRST_PIPE_INSTANCE` does the same job in one
kernel call, so there is no lock and nothing to recover.

## Building

### Prerequisites

A C++17 compiler, CMake 3.20+, and Ninja.

**Windows.** If you already have MSYS2, one command gets all three:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc \
                   mingw-w64-ucrt-x86_64-cmake \
                   mingw-w64-ucrt-x86_64-ninja
```

Then put `C:\msys64\ucrt64\bin` on `PATH`. Without MSYS2, either
`winget install Kitware.CMake Ninja-build.Ninja` alongside a compiler, or
install the **Visual Studio 2022 Build Tools** with the "Desktop development
with C++" workload and use the `msvc` preset below.

**Linux.**

```sh
sudo apt install build-essential cmake ninja-build     # Debian/Ubuntu
sudo dnf install gcc-c++ cmake ninja-build             # Fedora
```

M4 additionally needs `libevdev`, `libx11` and `libxrandr` development packages.
They are not required to build the engine or run its tests.

### Build and test

From the repository root:

```sh
cmake --preset default        # configure
cmake --build --preset default
ctest --preset default
```

Presets live in [`CMakePresets.json`](../CMakePresets.json):

| Preset | What it is for |
|--------|----------------|
| `default` | RelWithDebInfo, Ninja, whatever compiler is on `PATH` |
| `debug` | Debug build with `-Werror`. **Run this before committing** — the default build only warns |
| `msvc` | Visual Studio 2022 generator, if you would rather not use MinGW |

Build output goes to `build/<preset>/`, which is gitignored. `ctest` prints
failing output automatically; to run a test binary directly for more detail:

```sh
./build/default/core/test_keycode
```

> **Windows, running from Git Bash.** Git for Windows ships its own
> `mingw64in` on `PATH`, and its `libstdc++-6.dll` is not the one a UCRT64
> build links against. Whichever comes first wins, and the losing case is a
> segfault before the first test prints. Put the toolchain first when running
> the binaries from that shell:
>
> ```sh
> PATH=/c/msys64/ucrt64/bin:$PATH ctest --preset default
> ```
>
> Running from the MSYS2 UCRT64 shell, from PowerShell, or from an IDE that
> uses the configured toolchain does not have the problem.

### Why it is split this way

**`kgn_engine` touches no OS API.** No device, no thread, no system call. It
takes events and a clock and returns decisions. `test_layer_engine` exercises
the whole CapsLock state machine on a synthetic timeline, including a property
test that replays 200 random event sequences and asserts no key is ever left
stranded down. That is a design constraint, not
an accident: it is the only way the concurrency-sensitive logic inherited from
the original prototype — the CapsLock race window, the forwarded-release
invariant — can be tested at all. `ctest` needs no display, no privileges and no
hardware, and it will stay that way.

**`kgn_ipc` is endpoint ownership and the transports.** It sits apart from
`kgn_engine` because it must touch an operating system, while the IPC
*protocol* stays in the engine behind an abstract transport. That is what lets
every envelope, queue and reply-routing rule be tested against an in-memory
fake with no socket in sight, and leaves only the ownership rules needing a
real one.

**`kgn_platform` is where operating systems live**, one implementation per
platform behind the interfaces in [`include/kgn/backends.hpp`](include/kgn/backends.hpp).
It is not built yet; the sources arrive with M3 and M4. Configuring on a
platform with no backend is deliberately not an error — `backends_none.cpp`
supplies the factory returning null members, which is exactly what
`backends.hpp` specifies for a capability a platform does not have. It is not a
fake backend: a stub that swallowed calls and returned plausible values would
let the core look like it was driving the pointer while nothing moved.

### Tests

[`tests/kgn_test.hpp`](tests/kgn_test.hpp) is a ~100-line harness. Not Catch2 or
GoogleTest, because both want a network fetch at configure time or a vendored
copy, and neither earns that for asserting on a pure state machine. Swapping it
later is mechanical — the assertions already read the same.

```cpp
KGN_TEST(name_of_the_thing_being_asserted) {
    KGN_CHECK(condition);
    KGN_CHECK_EQ(actual, expected);
}
```

Add a test file to `tests/`, then one `kgn_add_test(name)` line in
`CMakeLists.txt` — or `kgn_add_test_with(name kgn_ipc)` if it needs a real
operating system, as the endpoint and core tests do.

## The prototype

[`../legacy/kbd_layer.c`](../legacy/kbd_layer.c) is where this started. It is
kept for reference and is not built. The ideas that carry forward are documented
in [OUTLINE.md](../docs/OUTLINE.md) section 9.
