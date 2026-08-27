# mtk-core

The native input core: interception, suppression, pointer synthesis, window
management, and the JSON Lines IPC server the overlay connects to.

## Status

The build works end to end — configure, compile, test — but most of the core is
not written yet. What exists today:

| | |
|---|---|
| ✅ | `mtk_engine`: the platform-free library, CMake target, warning setup |
| ✅ | `src/keycode.cpp` — the key vocabulary, interning, modifier grouping |
| ✅ | `src/layer_engine.cpp` — the CapsLock state machine, the grace window, and the P7 forwarded-release invariant with its mirror |
| ✅ | Test harness and `ctest` wiring; 96 tests, clean under `-Werror -Wconversion` |
| ✅ | Interface headers defining the seam ([`include/mtk/`](include/mtk/)) |
| 🚧 | Motion integrator, action dispatch, IPC server (rest of **M2**) |
| 🚧 | Windows backend: hook, `SendInput`, Win32 windows (**M3**) |
| 🚧 | Linux/X11 backend: evdev, uinput, EWMH, XRandR (**M4**) |

`CMakeLists.txt` lists the M2 sources as comments where they will go, so adding
one is uncommenting a line rather than working out where it belongs.

The overlay runs today without any of this, on the mock backend
(`mousetrapkeys --backend mock`), and will connect to the core automatically once
it starts listening. Nothing in the Python side changes when it does.

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

### Why it is split this way

**`mtk_engine` touches no OS API.** No device, no thread, no system call. It
takes events and a clock and returns decisions. `test_layer_engine` exercises
the whole CapsLock state machine on a synthetic timeline, including a property
test that replays 200 random event sequences and asserts no key is ever left
stranded down. That is a design constraint, not
an accident: it is the only way the concurrency-sensitive logic inherited from
the original prototype — the CapsLock race window, the forwarded-release
invariant — can be tested at all. `ctest` needs no display, no privileges and no
hardware, and it will stay that way.

**`mtk_platform` is where operating systems live**, one implementation per
platform behind the interfaces in [`include/mtk/backends.hpp`](include/mtk/backends.hpp).
It is not built yet. Configuring on a platform with no backend is deliberately
not an error — you get the engine and its tests, which is what M2 needs.

### Tests

[`tests/mtk_test.hpp`](tests/mtk_test.hpp) is a ~100-line harness. Not Catch2 or
GoogleTest, because both want a network fetch at configure time or a vendored
copy, and neither earns that for asserting on a pure state machine. Swapping it
later is mechanical — the assertions already read the same.

```cpp
MTK_TEST(name_of_the_thing_being_asserted) {
    MTK_CHECK(condition);
    MTK_CHECK_EQ(actual, expected);
}
```

Add a test file to `tests/`, then one `mtk_add_test(name)` line in
`CMakeLists.txt`.

## The prototype

[`../legacy/kbd_layer.c`](../legacy/kbd_layer.c) is where this started. It is
kept for reference and is not built. The ideas that carry forward are documented
in [OUTLINE.md](../docs/OUTLINE.md) section 9.
