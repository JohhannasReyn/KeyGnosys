# M3 Windows Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give KeyGnosys a working Windows input path — low-level keyboard hook, `SendInput` output, Win32 windows/monitors — with the layer engine owned solely by the hook thread and every obligation provably deliverable.

**Architecture:** The hook thread becomes the sole mutating owner of `LayerEngine` and answers Windows' suppression question synchronously. Two preallocated SPSC rings cross to the 60 Hz core thread: an *obligation* ring of work items that can never overflow (proved in the spec §4), and a *publication* ring of physical key records that may coalesce. Core→hook control is a third ring woken by an auto-reset event whose wait timeout doubles as the grace-window deadline timer. Backend construction moves to the executable so tests keep linking `kgn_ipc` alone.

**Tech Stack:** C++17, CMake 3.20+, Ninja; Win32 (`user32`, `dwmapi`, `shcore`); the in-repo `kgn_test.hpp` harness.

**Spec:** `docs/superpowers/specs/2026-08-28-m3-threading-design.md` (read it first — the capacity proof in its §4 is the reason several tasks look the way they do). Authoritative product spec: `docs/SPEC.md` §6.2, §7, §8.2–8.4, §11, §12.

## Global Constraints

- **P6 — fail visibly.** An unavailable capability is reported and disabled, never emulated. No fake backends ship.
- **P7 — never strand a key.** Every suppressed or synthesized press has a guaranteed matching release on every exit path.
- **No allocation, no logging, no IPC, no blocking on the hook callback path.** `LowLevelHooksTimeout` defaults to 300 ms; exceeding it silently unhooks us.
- **No network access, ever.** No telemetry, no update check.
- **Positional codes only on the wire.** The core never resolves a keystroke to a character.
- **`kgn_engine` touches no OS API.** New pure code (rings, translation, slot registry) belongs there; anything calling Win32 belongs in `kgn_platform`.
- Milestone discipline: **no Linux/evdev/X11 work, no launcher scripts, no autostart, no installer, no M5+ editor work.**
- C++17. Warnings are errors under the `debug` preset: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`.
- Build and test on **both** platforms before claiming clean. On Windows/Git Bash prefix with `PATH=/c/msys64/ucrt64/bin:$PATH`.
- Every fix to an invariant gets a test that **fails against the old code** — verify that it does.
- Focused, reviewable commits. Open a PR and stop for review; never merge unless told.

**Verification commands** (run both, both platforms):

```sh
cmake --preset debug && cmake --build --preset debug && PATH=/c/msys64/ucrt64/bin:$PATH ctest --preset debug
cmake --preset default && cmake --build --preset default && PATH=/c/msys64/ucrt64/bin:$PATH ctest --preset default
```

Baseline at `ee26cec`: 9/9 test binaries pass; 246 C++ tests on Windows, 261 on Linux; `pytest` 90 passed.

---

# Phase P — Prerequisites (must land before any thread exists)

### Task P1: Stable backing store for the key intern table

`KeyCode::toString()` returns a `std::string_view` into `table.names[id-1]` and then
releases the mutex (`core/src/keycode.cpp:110-115`). `names` is a `std::vector<std::string>`,
so a concurrent intern that reallocates moves every element. Short names live in the
`std::string`'s own storage (SSO), so the view dangles. Single-threaded today; M3 introduces
the second thread. This is a correctness precondition, not a cleanup.

**Files:**
- Modify: `core/src/keycode.cpp:63-77` (the `Intern` struct), `:110-115` (`toString`)
- Test: `core/tests/test_keycode.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: no signature change. `KeyCode::fromString` / `KeyCode::toString` keep their
  declarations in `core/include/kgn/keycode.hpp`. The guarantee added is that a
  `string_view` returned by `toString()` stays valid for the process lifetime.

- [ ] **Step 1: Write the failing test**

Add to `core/tests/test_keycode.cpp`, before `int main`:

```cpp
KGN_TEST(a_returned_name_survives_interning_many_more_keys) {
    // toString() hands back a view into the intern table's storage after
    // releasing its lock. If that storage can move, the view dangles -- which
    // becomes reachable the moment a second thread interns anything.
    const kgn::KeyCode first = kgn::KeyCode::fromString("KeyA");
    const std::string_view view = first.toString();
    const char* data = view.data();

    // Force many reallocations of whatever backs the table.
    for (int i = 0; i < 4096; ++i) {
        kgn::KeyCode::fromString("Synthetic" + std::to_string(i));
    }

    KGN_CHECK_EQ(std::string(view), std::string("KeyA"));
    KGN_CHECK(view.data() == data);
}
```

Ensure `<string>` and `<string_view>` are included at the top of the file.

- [ ] **Step 2: Run it and confirm it fails**

```sh
cmake --build --preset debug && PATH=/c/msys64/ucrt64/bin:$PATH ./build/debug/core/test_keycode
```

Expected: `FAIL a_returned_name_survives_interning_many_more_keys` — the pointer comparison
fails (and possibly the string comparison, reading freed memory). If it passes, the vector
happened not to reallocate; raise 4096 until it fails, and leave it at that value.

- [ ] **Step 3: Change the backing store to `std::deque`**

In `core/src/keycode.cpp`, replace `#include <vector>` usage for the intern table with
`#include <deque>` (keep `<vector>` if `builtinNames()` still returns one) and change:

```cpp
struct Intern {
    // std::deque, not std::vector: toString() returns a string_view into this
    // storage and then releases the lock, so the storage must never move. A
    // deque's push_back never invalidates references to existing elements, and
    // entries are never erased, so a view stays valid for the process lifetime.
    // Not a detail about short-string optimisation -- that is an implementation
    // accident, not a lifetime guarantee.
    std::deque<std::string> names;                  // id - 1 -> name
    std::unordered_map<std::string, std::uint16_t> ids;
    std::mutex mutex;

    Intern() {
        const auto& builtin = builtinNames();
        names.assign(builtin.begin(), builtin.end());
        ids.reserve(builtin.size() * 2);
        for (std::size_t i = 0; i < builtin.size(); ++i) {
            ids.emplace(builtin[i], static_cast<std::uint16_t>(i + 1));
        }
    }
};
```

`fromString` and `toString` bodies are unchanged — `push_back`, `size()` and `operator[]`
all exist on `std::deque`.

- [ ] **Step 4: Run the test and the full suite**

```sh
cmake --build --preset debug && PATH=/c/msys64/ucrt64/bin:$PATH ctest --preset debug
```

Expected: PASS, 9/9 binaries.

- [ ] **Step 5: Commit**

```bash
git add core/src/keycode.cpp core/tests/test_keycode.cpp
git commit -m "Give the key intern table storage that never moves"
```

---

### Task P2: Documentation corrections

Four stale or missing pieces of documentation, none touching code. Grouped so the M3 PR
stays purely M3.

**Files:**
- Modify: `core/README.md` (option table), `docs/SPEC.md` (§5.3 table, §5.3 cross-references at lines 990 and 997, §11 table)
- Test: none — documentation only. Verified by grep.

**Interfaces:**
- Consumes: nothing. Produces: the diagnostic codes `ipc.bad_message`,
  `input.queue_overflow`, `input.publication_dropped` become specified, so later tasks may
  emit them.

- [ ] **Step 1: Remove the stale `--endpoint` row from `core/README.md`**

Delete this row from the option table (it was removed from `main.cpp`; the comment at
`core/src/main.cpp:98` records why):

```
| `--endpoint <address>` | Listen here instead of the resolved endpoint |
```

Add immediately below the table:

```markdown
There is deliberately no option to move the endpoint. [SPEC §5.1.1](../docs/SPEC.md#51-transport)
gives one endpoint rule that the core, the overlay and the launcher all derive from, and a
core listening anywhere else is a core its own clients cannot find.
```

- [ ] **Step 2: Fix the two stale cross-references in `docs/SPEC.md`**

Line 990: `| \`diagnostic\` | ... | §10 |` → `§11`.
Line 997: `... Only positional codes are transmitted. See §11.` → `See §12.`

- [ ] **Step 3: Add the `overlay_toggle` row to the §5.3 event table**

The core already broadcasts it (`core/src/core.cpp:303`) and SPEC §7.6 specifies the
`overlay.toggle` action, but the event table omits it. Insert after the `drag_lock` row:

```markdown
| `overlay_toggle` | `{}` | The `overlay.toggle` action fired; the overlay shows/hides itself |
```

Add after the table:

```markdown
`mode` and `modifiers` MAY be coalesced within one core tick: two rapid transitions inside
16.7 ms produce one event carrying the final state.
```

- [ ] **Step 4: Add the three missing diagnostic codes to the §11 table**

```markdown
| `ipc.bad_message` | warn | A malformed line, a non-command message, or a line past the size limit |
| `input.queue_overflow` | error | The hook could not hand work to the core loop; interception degraded and `release_all` issued |
| `input.publication_dropped` | info | Key-event publication fell behind; overlay feedback may have missed a key |
```

- [ ] **Step 5: Handle `overlay_toggle` in the Python client**

In `python/keygnosys/coreclient/ipc.py`, add a signal next to the existing ones:

```python
overlayToggleRequested = Signal()
```

and a branch in `_dispatch` (alongside the existing `elif name == "focus":` chain):

```python
elif name == "overlay_toggle":
    self.overlayToggleRequested.emit()
```

Connect it in `python/keygnosys/app.py` wherever the other client signals are wired, to the
same handler the control bar's overlay toggle already uses.

- [ ] **Step 6: Verify**

```sh
grep -n 'endpoint <address>' core/README.md          # expect no output
grep -c 'ipc.bad_message' docs/SPEC.md               # expect 1
grep -c 'overlay_toggle' docs/SPEC.md                # expect 1
python -m pytest -q
```

- [ ] **Step 7: Commit**

```bash
git add core/README.md docs/SPEC.md python/keygnosys/coreclient/ipc.py python/keygnosys/app.py
git commit -m "Correct four stale or missing pieces of documentation"
```

---

# Phase F — Composition boundary

### Task F1: Move backend construction to the executable

`createBackends()` currently lives in `backends.hpp` and is compiled into `kgn_ipc` via
`backends_none.cpp`. Once `kgn_platform` exists, `test_core` (which links `kgn_ipc` alone)
would fail to link. Fix the dependency direction rather than spraying `user32` across test
targets.

**Files:**
- Create: `core/include/kgn/platform.hpp`, `core/src/platform_none.cpp`
- Delete: `core/src/backends_none.cpp`
- Modify: `core/include/kgn/backends.hpp`, `core/include/kgn/core.hpp`, `core/src/core.cpp`, `core/src/main.cpp`, `core/CMakeLists.txt`
- Test: `core/tests/test_core.cpp`

**Interfaces:**
- Consumes: `kgn::Backends`, `kgn::Capabilities` from `backends.hpp`.
- Produces:
  - `kgn::Backends kgn::createBackends();` declared in `kgn/platform.hpp`, defined in `kgn_platform`.
  - `kgn::Core::Core(CoreOptions options, Backends backends = {})`.
  - `Capabilities` loses `canWarpAbsolute` and `canMoveWindows`; `InputBackend::capabilities()`
    keeps `canSuppress`; new `OutputBackend::capabilities()` and `WindowBackend::capabilities()`
    return `Capabilities` too.

- [ ] **Step 1: Write the failing test**

Add to `core/tests/test_core.cpp`:

```cpp
namespace {

// A fake output backend. This is a TEST double, not a shipped backend: P6
// forbids shipping something that looks like it works, not writing one to
// assert on what the core asked for.
class RecordingOutput : public kgn::OutputBackend {
public:
    std::vector<std::pair<kgn::KeyCode, bool>> keys;
    int releaseAllCalls = 0;

    void moveCursorBy(int, int) override {}
    void moveCursorTo(int, int) override {}
    kgn::Point cursorPosition() override { return {}; }
    void button(kgn::MouseButton, bool) override {}
    void scroll(int, int) override {}
    void sendKey(kgn::KeyCode code, bool down) override { keys.emplace_back(code, down); }
    void releaseAll() override { ++releaseAllCalls; }
    kgn::Capabilities capabilities() const override {
        kgn::Capabilities c;
        c.canWarpAbsolute = true;
        return c;
    }
};

}  // namespace

KGN_TEST(hello_reports_warp_absolute_from_the_output_backend_not_the_input_one) {
    kgn::CoreOptions options;
    options.endpointOverrideForTests = uniqueEndpoint();
    kgn::Backends backends;
    backends.output = std::make_unique<RecordingOutput>();

    kgn::Core core(options, std::move(backends));
    KGN_CHECK(core.start().ok());

    const auto& capabilities = core.hello().capabilities;
    KGN_CHECK(std::find(capabilities.begin(), capabilities.end(), "warp_absolute")
              != capabilities.end());
    core.stop("test over");
}
```

Reuse whatever helper the file already uses to make a unique endpoint; if there is none,
add `static std::string uniqueEndpoint()` following the pattern already in
`core/tests/test_endpoint.cpp`.

- [ ] **Step 2: Run it and confirm it fails**

```sh
cmake --build --preset debug 2>&1 | tail -20
```

Expected: compile error — `Core` has no two-argument constructor, and `OutputBackend` has no
`capabilities()`.

- [ ] **Step 3: Split `Capabilities` across the three interfaces**

In `core/include/kgn/backends.hpp`, change the struct and add the two methods:

```cpp
// What a backend can actually do. Reported to the UI verbatim so an
// unavailable capability is visible rather than silently absent (P6).
//
// Each interface reports its OWN properties. Reading a pointer-warp capability
// off the input backend would be asserting something the input backend cannot
// know.
struct Capabilities {
    bool canSuppress = false;        // InputBackend
    bool canWarpAbsolute = false;    // OutputBackend
    bool canMoveWindows = false;     // WindowBackend
    std::vector<std::string> limitations;
};
```

Add to `OutputBackend` and `WindowBackend`, alongside their existing members:

```cpp
    [[nodiscard]] virtual Capabilities capabilities() const = 0;
```

Remove the `createBackends()` declaration and the `Backends` struct's factory comment from
this header; keep the `Backends` struct itself.

- [ ] **Step 4: Create `core/include/kgn/platform.hpp`**

```cpp
// The backend factory: the single place that knows which platform this build
// targets.
//
// It lives here rather than in backends.hpp so that the DEPENDENCY DIRECTION
// stays honest. kgn_ipc composes a core out of backends it is given; it does
// not know how to make them. Only the executable -- the composition root --
// calls this, which is why a test can build a Core out of fakes without
// linking user32, dwmapi or libevdev.
//
// See docs/SPEC.md section 6.2.

#pragma once

#include "kgn/backends.hpp"

namespace kgn {

// Returns null members for capabilities unavailable on this platform; the
// caller reports them rather than substituting something that merely looks
// similar (P6).
Backends createBackends();

}  // namespace kgn
```

- [ ] **Step 5: Create `core/src/platform_none.cpp` and delete `core/src/backends_none.cpp`**

```cpp
// The backend factory on a build with no platform backends.
//
// Returning null members is exactly what backends.hpp specifies for a
// capability this platform does not have, and the core reports each absence in
// `hello` rather than substituting something that merely looks similar (P6).
//
// It is deliberately NOT a fake backend. A stub that swallowed calls and
// returned plausible values would let the core look like it was driving the
// pointer while nothing moved, which is the state P6 exists to forbid.

#include "kgn/platform.hpp"

namespace kgn {

Backends createBackends() { return {}; }

}  // namespace kgn
```

```bash
git rm core/src/backends_none.cpp
```

- [ ] **Step 6: Give `Core` an injected `Backends`**

In `core/include/kgn/core.hpp`:

```cpp
    explicit Core(CoreOptions options, Backends backends = {});
```

In `core/src/core.cpp`, change the constructor and store the argument:

```cpp
Core::Core(CoreOptions options, Backends backends)
    : impl_(std::make_unique<Impl>(std::move(options))) {
    impl_->backends = std::move(backends);
}
```

Delete the `impl_->backends = createBackends();` line from `Core::start()` (currently
`core.cpp:537`), and delete the `#include "kgn/platform.hpp"` if you added one — `core.cpp`
must not reference the factory at all.

- [ ] **Step 7: Read capabilities from the right backend**

Replace `core/src/core.cpp:539-560` with:

```cpp
    impl_->hello.coreVersion = "0.1.0";
    impl_->hello.platform = platformName();

    auto absorb = [&](const Capabilities& capabilities) {
        if (capabilities.canSuppress) impl_->hello.capabilities.emplace_back("suppress");
        if (capabilities.canWarpAbsolute) {
            impl_->hello.capabilities.emplace_back("warp_absolute");
        }
        if (capabilities.canMoveWindows) {
            impl_->hello.capabilities.emplace_back("move_windows");
        }
        for (const auto& limitation : capabilities.limitations) {
            impl_->hello.limitations.push_back(limitation);
        }
    };

    if (impl_->backends.input) {
        impl_->hello.inputBackend = impl_->backends.input->name();
        absorb(impl_->backends.input->capabilities());
    }
    if (impl_->backends.output) {
        impl_->hello.outputBackend = impl_->backends.output->name();
        absorb(impl_->backends.output->capabilities());
    }
    if (impl_->backends.window) {
        impl_->hello.windowBackend = impl_->backends.window->name();
        absorb(impl_->backends.window->capabilities());
    }
    if (!impl_->backends.input) {
        impl_->hello.limitations.emplace_back(
            "No input backend on this build: keys are not intercepted and the "
            "pointer is not driven (milestones M3 and M4).");
    }
```

Add `[[nodiscard]] virtual std::string_view name() const = 0;` to all three interfaces in
`backends.hpp` (with `#include <string_view>`), so `hello` reports a real name instead of
the placeholder literals `"input"`/`"output"`/`"window"`. Implement it in the test fake as
`return "recording";`.

- [ ] **Step 8: Call the factory from the executable**

In `core/src/main.cpp`, add `#include "kgn/platform.hpp"` and change the `Core` construction
to:

```cpp
    // The composition root, and the only place that knows what platform this
    // build targets (SPEC section 6.2).
    kgn::Core core(options, kgn::createBackends());
```

- [ ] **Step 9: Restructure `core/CMakeLists.txt`**

Replace the `KGN_PLATFORM_SOURCES` block and the `else()` that added `backends_none.cpp`:

```cmake
set(KGN_PLATFORM_SOURCES "")

if(WIN32)
    set(KGN_PLATFORM_NAME "windows")
    # M3 lands these in Phase W.
elseif(UNIX AND NOT APPLE)
    set(KGN_PLATFORM_NAME "linux-x11")
    # M4: evdev_input.cpp uinput_output.cpp x11_window.cpp evdev_keymap.cpp
else()
    set(KGN_PLATFORM_NAME "unsupported")
endif()

# kgn_platform ALWAYS exists. With no real sources it supplies the factory
# returning null members, which is what backends.hpp specifies for a capability
# a platform does not have. Only the executable links it, so a test can build a
# Core out of fakes without linking user32 or libevdev.
if(NOT KGN_PLATFORM_SOURCES)
    set(KGN_PLATFORM_SOURCES src/platform_none.cpp)
    set(KGN_PLATFORM_STUB TRUE)
endif()

add_library(kgn_platform ${KGN_PLATFORM_SOURCES})
target_link_libraries(kgn_platform PUBLIC kgn_engine PRIVATE kgn_warnings)

if(NOT KGN_PLATFORM_STUB)
    if(WIN32)
        target_link_libraries(kgn_platform PRIVATE user32 dwmapi shcore)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(EVDEV REQUIRED IMPORTED_TARGET libevdev)
        pkg_check_modules(X11   REQUIRED IMPORTED_TARGET x11 xrandr)
        target_link_libraries(kgn_platform PRIVATE PkgConfig::EVDEV PkgConfig::X11)
    endif()
endif()
```

and unconditionally link it into the executable:

```cmake
add_executable(keygnosys-core src/main.cpp)
target_link_libraries(keygnosys-core PRIVATE kgn_ipc kgn_platform kgn_warnings)
```

Update the status message to use `KGN_PLATFORM_STUB` instead of `KGN_PLATFORM_SOURCES`.

- [ ] **Step 10: Run the tests on both platforms**

```sh
cmake --preset debug && cmake --build --preset debug && PATH=/c/msys64/ucrt64/bin:$PATH ctest --preset debug
```

Expected: PASS, 9/9. Confirm `test_core` still links `kgn_ipc` only — check
`core/CMakeLists.txt` still says `kgn_add_test_with(test_core kgn_ipc)`.

- [ ] **Step 11: Commit**

```bash
git add -A core/
git commit -m "Move backend construction to the composition root"
```

---

# Phase C — The channels (pure, `kgn_engine`, no OS)

### Task C2: `LayerEngine::nextDeadline()`

The hook thread's wait timeout is the grace-window deadline. `tick()` does exactly one thing
— expire pending presses (`core/src/layer_engine.cpp:522-537`) — so the engine needs to say
when that next matters instead of being polled at 60 Hz.

**Files:**
- Modify: `core/include/kgn/layer_engine.hpp`, `core/src/layer_engine.cpp`
- Test: `core/tests/test_layer_engine.cpp`

**Interfaces:**
- Produces: `std::optional<TimePoint> LayerEngine::nextDeadline() const;`

- [ ] **Step 1: Write the failing test**

```cpp
KGN_TEST(the_engine_reports_when_its_next_grace_window_lapses) {
    kgn::EngineConfig config;
    config.grace = std::chrono::milliseconds(50);
    kgn::LayerEngine engine(config);
    engine.setBindings({{kgn::KeyCode::fromString("KeyD"), kgn::BindingKind::Action},
                        {kgn::KeyCode::fromString("KeyF"), kgn::BindingKind::Action}});

    KGN_CHECK(!engine.nextDeadline().has_value());

    engine.onKey(kgn::KeyCode::fromString("KeyD"), kgn::KeyState::Down, at(100));
    engine.onKey(kgn::KeyCode::fromString("KeyF"), kgn::KeyState::Down, at(120));

    // The EARLIEST pending press governs, not the latest.
    KGN_CHECK(engine.nextDeadline().has_value());
    KGN_CHECK(*engine.nextDeadline() == at(150));

    engine.tick(at(150));
    KGN_CHECK(*engine.nextDeadline() == at(170));

    engine.tick(at(170));
    KGN_CHECK(!engine.nextDeadline().has_value());
}
```

`at(ms)` is the existing helper in this file for building a `TimePoint`; reuse it.

- [ ] **Step 2: Run it and confirm it fails**

Expected: compile error, `nextDeadline` is not a member.

- [ ] **Step 3: Declare it**

In `core/include/kgn/layer_engine.hpp`, after `heldActions()`:

```cpp
    // When the next buffered press's grace window lapses, or nullopt when
    // nothing is buffered.
    //
    // This is what lets the owner wait on a deadline instead of polling: the
    // engine's only timed work is expiring these, so a 60 Hz tick would be
    // 59 wakeups a second doing nothing. Allocation-free and const.
    [[nodiscard]] std::optional<TimePoint> nextDeadline() const;
```

- [ ] **Step 4: Define it**

In `core/src/layer_engine.cpp`, next to `heldActions()`:

```cpp
std::optional<TimePoint> LayerEngine::nextDeadline() const {
    if (pendingCount_ == 0) return std::nullopt;
    // pending_ is kept in press order and `grace` is uniform, so the head entry
    // is always the earliest to lapse.
    return pending_[0].pressedAt + config_.grace;
}
```

- [ ] **Step 5: Run the tests**

```sh
cmake --build --preset debug && PATH=/c/msys64/ucrt64/bin:$PATH ./build/debug/core/test_layer_engine
```

Expected: PASS, 88 tests.

- [ ] **Step 6: Commit**

```bash
git add core/include/kgn/layer_engine.hpp core/src/layer_engine.cpp core/tests/test_layer_engine.cpp
git commit -m "Let the layer engine report its next grace deadline"
```

---

### Task C1: The channel types and the SPSC ring

Pure, allocation-free, testable with no thread and no OS. Lives in `kgn_engine`.

**Files:**
- Create: `core/include/kgn/hookchannel.hpp`, `core/src/hookchannel.cpp`, `core/tests/test_hookchannel.cpp`
- Modify: `core/CMakeLists.txt`
- Test: `core/tests/test_hookchannel.cpp`

**Interfaces:**
- Produces: `kgn::WorkItem`, `kgn::PhysicalRecord`, `kgn::PublishedState`,
  `kgn::SpscRing<T, N>`, `kgn::kWorkRingCapacity`, `kgn::kWorkAdmissionGate`,
  `kgn::kMaxReleaseWork`, `kgn::kPublicationRingCapacity`,
  `kgn::translateDecisions(const DecisionBuffer&, KeyCode, KeyState, SpscRing<WorkItem,…>&) -> bool`.

- [ ] **Step 1: Write the header**

`core/include/kgn/hookchannel.hpp`:

```cpp
// The two streams that cross from the input-owning thread to the core loop.
//
// An engine DECISION is not a physical KEY EVENT, and conflating them is a bug
// waiting for a backend to exist:
//   - one physical event can produce several decisions (a grace replay, a
//     CapsLock press promoting buffered keys);
//   - grace expiry, release_all and config changes produce decisions with NO
//     physical event at all;
//   - a decision satisfied by native passthrough needs publication but no
//     synthesis, and re-synthesising it would deliver the key twice.
//
// So there are two streams:
//
//   Stream A -- WorkItem. Things the core must DO. Ordered, and it must never
//   overflow, because a dropped release is a key or a mouse button held down
//   forever (P7). The capacity argument that makes overflow structurally
//   impossible is below.
//
//   Stream B -- PhysicalRecord. Exactly one per physical hook callback, and
//   none for timer or control work. This is what produces the `key` IPC event.
//   Losing one costs an overlay highlight and nothing else, so it may drop.
//
// Everything here is pure: no OS API, no thread, no allocation after
// construction. See docs/superpowers/specs/2026-08-28-m3-threading-design.md.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "kgn/keycode.hpp"
#include "kgn/layer_engine.hpp"

namespace kgn {

// ---------------------------------------------------------------------------
// Stream A -- work items

struct WorkItem {
    enum class Kind : std::uint8_t {
        SendKey,        // OutputBackend::sendKey(code, down)
        RunAction,      // Dispatcher: start the binding on `code`
        ReleaseAction,  // Dispatcher: stop the binding on `code`
    };

    Kind kind = Kind::SendKey;
    bool down = false;          // SendKey only
    std::uint16_t code = 0;
};

// ---------------------------------------------------------------------------
// Stream B -- physical resolution records

struct PhysicalRecord {
    std::uint16_t code = 0;
    KeyState state = KeyState::Down;   // the PHYSICAL state, as the OS delivered it
    bool suppressed = false;           // true iff the hook returned non-zero
    std::uint32_t version = 0;         // published-state version after this callback
};

// ---------------------------------------------------------------------------
// Capacities
//
// The theorem this ring's size comes from, in short:
//
// Let U := heldActions + 2 * pendingPresses. Every Up path emits at most -dU
// work items -- releasing a held action costs 1 item and drops U by 1;
// resolving a buffered press costs at most 2 and drops U by 2; and every path
// that emits NO work item consumes no capacity either, because Suppress and
// Buffer decisions never become work items. So once no new obligation is
// admitted, ALL remaining Up events together emit at most U <= kMaxReleaseWork
// items.
//
// Admit an obligation-creating operation (a Down, a Repeat, tick(), any
// control) only while free >= kWorkAdmissionGate. It then emits at most
// kDecisionCapacity items, leaving free >= kMaxReleaseWork >= U. Up events are
// never refused, and each one preserves free - U >= 0. Therefore the ring
// never overflows, and no release is ever dropped.
//
// Rows that emit no work item consuming no capacity is exactly why mode and
// modifier publication is NOT in this ring: an Up for a key with no obligation
// would otherwise spend reserve capacity while discharging nothing, and the
// bound would not exist.

// The worst case for the whole release drain.
inline constexpr std::size_t kMaxReleaseWork = kMaxHeld + 2 * kMaxPending;
static_assert(kMaxReleaseWork == 384, "the capacity proof is stated for these bounds");

// Free slots required before an obligation-creating operation may run.
inline constexpr std::size_t kWorkAdmissionGate = kDecisionCapacity + kMaxReleaseWork;

// Power of two so the ring can mask instead of divide. The proof needs only
// kWorkAdmissionGate; the rest is slack so the gate is unreachable in practice.
inline constexpr std::size_t kWorkRingCapacity = 2048;
static_assert(kWorkRingCapacity >= kWorkAdmissionGate,
              "the ring must be able to satisfy its own admission gate");

// Publication may drop; it is sized for comfort, not for a proof.
inline constexpr std::size_t kPublicationRingCapacity = 1024;

// ---------------------------------------------------------------------------
// A single-producer / single-consumer ring.
//
// Two atomics and a fixed array. Deliberately the smallest thing that
// establishes the ownership rather than the cleverest: exactly one thread
// pushes and exactly one pops, which is a property of the architecture rather
// than something this class enforces.
template <typename T, std::size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "capacity must be a power of two");

public:
    // Producer side.
    [[nodiscard]] std::size_t free() const {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return N - 1 - (head - tail);
    }

    // Returns false when full. The producer checks free() first for anything
    // that must not be refused, so a false here is a programming error on the
    // work ring and an accepted drop on the publication ring.
    bool push(const T& item) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= N - 1) return false;
        items_[head & (N - 1)] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side.
    bool pop(T& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = items_[tail & (N - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t size() const {
        return head_.load(std::memory_order_acquire)
               - tail_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool empty() const { return size() == 0; }

private:
    std::array<T, N> items_{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
};

using WorkRing = SpscRing<WorkItem, kWorkRingCapacity>;
using PublicationRing = SpscRing<PhysicalRecord, kPublicationRingCapacity>;

// ---------------------------------------------------------------------------
// Published engine state
//
// Mode, latch, activation and the four modifier groups, plus a version, in one
// 64-bit atomic. The core reads it freely; ordering against `key` events comes
// from the version stamped into each PhysicalRecord, so no ordered queue entry
// is needed and none of the work ring's reserved capacity is spent on the
// overlay's benefit.
struct PublishedState {
    Mode mode = Mode::Normal;
    bool latched = false;
    ActivationMode activation = ActivationMode::Hybrid;
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool meta = false;

    [[nodiscard]] std::uint32_t pack() const;
    static PublishedState unpack(std::uint32_t bits);
};

class StatePublisher {
public:
    // Producer: stores only when something actually changed, and bumps the
    // version when it does.
    void publish(const PublishedState& state);
    // Consumer.
    [[nodiscard]] PublishedState state() const;
    [[nodiscard]] std::uint32_t version() const;

private:
    // High 32 bits version, low 32 bits packed state, so a reader cannot see a
    // version from one store paired with the state from another.
    std::atomic<std::uint64_t> value_{0};
};

// ---------------------------------------------------------------------------
// Decision translation
//
// Turns one DecisionBuffer into work items, applying the native-passthrough
// rule. Returns TRUE when the current physical event should be passed to the
// OS untouched -- exactly when the engine emitted one decision and it is
// Forward for this very code and state. In that case NOTHING is pushed,
// because the OS is about to receive the event itself and synthesising it too
// would deliver it twice.
//
// `code` and `state` describe the physical event in hand. For the timer and
// control paths there is none: pass KeyCode{} and the result is always false.
bool translateDecisions(const DecisionBuffer& decisions, KeyCode code,
                        KeyState state, WorkRing& out);

}  // namespace kgn
```

- [ ] **Step 2: Write the failing tests**

`core/tests/test_hookchannel.cpp`:

```cpp
#include "kgn/hookchannel.hpp"

#include "kgn_test.hpp"

namespace {

kgn::Decision forward(const char* name, kgn::KeyState state) {
    return {kgn::Decision::Kind::Forward, kgn::KeyCode::fromString(name), state};
}
kgn::Decision suppress(const char* name, kgn::KeyState state) {
    return {kgn::Decision::Kind::Suppress, kgn::KeyCode::fromString(name), state};
}
kgn::Decision runAction(const char* name) {
    return {kgn::Decision::Kind::RunAction, kgn::KeyCode::fromString(name),
            kgn::KeyState::Down};
}

}  // namespace

KGN_TEST(a_lone_matching_forward_passes_through_and_synthesises_nothing) {
    kgn::DecisionBuffer decisions;
    decisions.push(forward("KeyA", kgn::KeyState::Down));
    kgn::WorkRing ring;

    const bool native = kgn::translateDecisions(
        decisions, kgn::KeyCode::fromString("KeyA"), kgn::KeyState::Down, ring);

    KGN_CHECK(native);
    KGN_CHECK(ring.empty());
}

KGN_TEST(a_forward_for_a_different_state_is_synthesised_not_passed_through) {
    // The engine turns a duplicate Down on a forwarded key into a Repeat. The
    // OS must see a repeat, not a second press, so this cannot pass through.
    kgn::DecisionBuffer decisions;
    decisions.push(forward("KeyA", kgn::KeyState::Repeat));
    kgn::WorkRing ring;

    const bool native = kgn::translateDecisions(
        decisions, kgn::KeyCode::fromString("KeyA"), kgn::KeyState::Down, ring);

    KGN_CHECK(!native);
    KGN_CHECK_EQ(ring.size(), std::size_t{1});
}

KGN_TEST(the_grace_replay_synthesises_both_halves_in_order) {
    // The physical event is the Up, but a synthetic Down must reach the OS
    // first. Passing the Up through natively would deliver up-then-down.
    kgn::DecisionBuffer decisions;
    decisions.push(forward("KeyD", kgn::KeyState::Down));
    decisions.push(forward("KeyD", kgn::KeyState::Up));
    kgn::WorkRing ring;

    const bool native = kgn::translateDecisions(
        decisions, kgn::KeyCode::fromString("KeyD"), kgn::KeyState::Up, ring);

    KGN_CHECK(!native);
    kgn::WorkItem first{};
    kgn::WorkItem second{};
    KGN_CHECK(ring.pop(first));
    KGN_CHECK(ring.pop(second));
    KGN_CHECK(first.kind == kgn::WorkItem::Kind::SendKey);
    KGN_CHECK(first.down);
    KGN_CHECK(second.kind == kgn::WorkItem::Kind::SendKey);
    KGN_CHECK(!second.down);
}

KGN_TEST(suppress_and_buffer_decisions_cost_no_capacity) {
    // This is what the release-capacity proof rests on: an Up for a key with
    // no obligation must not spend a ring slot, or no bound exists.
    kgn::DecisionBuffer decisions;
    decisions.push(suppress("KeyQ", kgn::KeyState::Up));
    decisions.push({kgn::Decision::Kind::Buffer, kgn::KeyCode::fromString("KeyW"),
                    kgn::KeyState::Down});
    kgn::WorkRing ring;

    kgn::translateDecisions(decisions, kgn::KeyCode::fromString("KeyQ"),
                            kgn::KeyState::Up, ring);

    KGN_CHECK(ring.empty());
}

KGN_TEST(actions_become_work_items) {
    kgn::DecisionBuffer decisions;
    decisions.push(suppress("CapsLock", kgn::KeyState::Down));
    decisions.push(runAction("KeyJ"));
    kgn::WorkRing ring;

    const bool native = kgn::translateDecisions(
        decisions, kgn::KeyCode::fromString("CapsLock"), kgn::KeyState::Down, ring);

    KGN_CHECK(!native);
    kgn::WorkItem item{};
    KGN_CHECK(ring.pop(item));
    KGN_CHECK(item.kind == kgn::WorkItem::Kind::RunAction);
    KGN_CHECK(ring.empty());
}

KGN_TEST(the_ring_reports_its_free_capacity_and_refuses_when_full) {
    kgn::SpscRing<kgn::WorkItem, 4> ring;
    KGN_CHECK_EQ(ring.free(), std::size_t{3});
    KGN_CHECK(ring.push({}));
    KGN_CHECK(ring.push({}));
    KGN_CHECK(ring.push({}));
    KGN_CHECK_EQ(ring.free(), std::size_t{0});
    KGN_CHECK(!ring.push({}));

    kgn::WorkItem out{};
    KGN_CHECK(ring.pop(out));
    KGN_CHECK_EQ(ring.free(), std::size_t{1});
}

KGN_TEST(the_published_state_round_trips_and_versions_monotonically) {
    kgn::StatePublisher publisher;
    KGN_CHECK_EQ(publisher.version(), std::uint32_t{0});

    kgn::PublishedState state;
    state.mode = kgn::Mode::Cursor;
    state.latched = true;
    state.shift = true;
    publisher.publish(state);

    KGN_CHECK_EQ(publisher.version(), std::uint32_t{1});
    KGN_CHECK(publisher.state().mode == kgn::Mode::Cursor);
    KGN_CHECK(publisher.state().latched);
    KGN_CHECK(publisher.state().shift);
    KGN_CHECK(!publisher.state().control);

    // Republishing the same state must not churn the version, or the core
    // would emit a `mode` event on every tick.
    publisher.publish(state);
    KGN_CHECK_EQ(publisher.version(), std::uint32_t{1});
}

int main() { return kgn::test::runAll(); }
```

- [ ] **Step 3: Run and confirm failure**

Add `kgn_add_test(test_hookchannel)` to `core/CMakeLists.txt` next to the others, and
`src/hookchannel.cpp` to the `kgn_engine` source list. Then:

```sh
cmake --preset debug && cmake --build --preset debug 2>&1 | tail -20
```

Expected: link error — `translateDecisions`, `PublishedState::pack`, `StatePublisher::publish` undefined.

- [ ] **Step 4: Implement `core/src/hookchannel.cpp`**

```cpp
#include "kgn/hookchannel.hpp"

namespace kgn {

std::uint32_t PublishedState::pack() const {
    std::uint32_t bits = 0;
    if (mode == Mode::Cursor) bits |= 1u << 0;
    if (latched) bits |= 1u << 1;
    bits |= static_cast<std::uint32_t>(activation) << 2;   // two bits
    if (shift) bits |= 1u << 4;
    if (control) bits |= 1u << 5;
    if (alt) bits |= 1u << 6;
    if (meta) bits |= 1u << 7;
    return bits;
}

PublishedState PublishedState::unpack(std::uint32_t bits) {
    PublishedState state;
    state.mode = (bits & (1u << 0)) ? Mode::Cursor : Mode::Normal;
    state.latched = (bits & (1u << 1)) != 0;
    state.activation = static_cast<ActivationMode>((bits >> 2) & 0x3u);
    state.shift = (bits & (1u << 4)) != 0;
    state.control = (bits & (1u << 5)) != 0;
    state.alt = (bits & (1u << 6)) != 0;
    state.meta = (bits & (1u << 7)) != 0;
    return state;
}

void StatePublisher::publish(const PublishedState& state) {
    const std::uint64_t current = value_.load(std::memory_order_relaxed);
    const std::uint32_t bits = state.pack();
    if (static_cast<std::uint32_t>(current) == bits
        && (current >> 32) != 0) {
        return;   // nothing changed; do not churn the version
    }
    const std::uint64_t version = (current >> 32) + 1;
    value_.store((version << 32) | bits, std::memory_order_release);
}

PublishedState StatePublisher::state() const {
    return PublishedState::unpack(
        static_cast<std::uint32_t>(value_.load(std::memory_order_acquire)));
}

std::uint32_t StatePublisher::version() const {
    return static_cast<std::uint32_t>(value_.load(std::memory_order_acquire) >> 32);
}

bool translateDecisions(const DecisionBuffer& decisions, KeyCode code,
                        KeyState state, WorkRing& out) {
    // Native passthrough: the engine asked for exactly this event and nothing
    // else, so the cheapest way to make it true is to let Windows deliver it.
    // Anything more than one decision means something must be ordered around
    // it -- the grace replay's synthetic Down being the case that matters --
    // and then the physical event has to be suppressed and re-synthesised so
    // the order is ours to control.
    const bool native = decisions.size() == 1
                        && decisions[0].kind == Decision::Kind::Forward
                        && decisions[0].code == code
                        && decisions[0].state == state;
    if (native) return true;

    for (const Decision& decision : decisions) {
        switch (decision.kind) {
            case Decision::Kind::Forward:
                out.push(WorkItem{WorkItem::Kind::SendKey,
                                  decision.state != KeyState::Up,
                                  decision.code.id()});
                break;
            case Decision::Kind::RunAction:
                out.push(WorkItem{WorkItem::Kind::RunAction, false,
                                  decision.code.id()});
                break;
            case Decision::Kind::ReleaseAction:
                out.push(WorkItem{WorkItem::Kind::ReleaseAction, false,
                                  decision.code.id()});
                break;
            case Decision::Kind::Suppress:
            case Decision::Kind::Buffer:
                // Nothing for the core to do, and -- load-bearing -- no ring
                // capacity spent. The release-capacity proof depends on this.
                break;
        }
    }
    return false;
}

}  // namespace kgn
```

- [ ] **Step 5: Run the tests**

```sh
cmake --build --preset debug && PATH=/c/msys64/ucrt64/bin:$PATH ./build/debug/core/test_hookchannel
```

Expected: PASS, 7 tests.

- [ ] **Step 6: Commit**

```bash
git add core/include/kgn/hookchannel.hpp core/src/hookchannel.cpp core/tests/test_hookchannel.cpp core/CMakeLists.txt
git commit -m "Add the two hook-to-core streams and the decision translation"
```

---

### Task C3: Prove the release-capacity bound with a test

The proof in the spec is only as good as the code agreeing with it. This test drives the
engine to its worst case and asserts the drain fits.

**Files:**
- Test: `core/tests/test_hookchannel.cpp`

**Interfaces:**
- Consumes: `kgn::translateDecisions`, `kgn::WorkRing`, `kgn::kMaxReleaseWork`, `kgn::LayerEngine`.
- Produces: nothing new.

- [ ] **Step 1: Write the test**

Append to `core/tests/test_hookchannel.cpp`, before `int main`:

```cpp
KGN_TEST(the_whole_release_drain_fits_inside_the_reserved_capacity) {
    // Drive the engine to its worst case -- as many held actions and buffered
    // presses as it can hold -- then admit nothing further and drain it
    // entirely through Up events, counting the work items produced.
    //
    // The theorem says that total is at most kMaxReleaseWork. If this ever
    // exceeds it, the admission gate is too small and a release can be lost,
    // which is P7's failure.
    kgn::EngineConfig config;
    config.activation = kgn::ActivationMode::Hold;
    config.grace = std::chrono::milliseconds(50);
    kgn::LayerEngine engine(config);

    kgn::BindingMap bindings;
    std::vector<kgn::KeyCode> actionKeys;
    for (int i = 0; i < 300; ++i) {
        const kgn::KeyCode code = kgn::KeyCode::fromString("Cap" + std::to_string(i));
        bindings[code] = kgn::BindingKind::Action;
        actionKeys.push_back(code);
    }
    engine.setBindings(bindings);

    kgn::WorkRing ring;
    kgn::DecisionBuffer buffer;
    auto feed = [&](kgn::KeyCode code, kgn::KeyState state, kgn::TimePoint now) {
        buffer.clear();
        engine.onKey(code, state, now, buffer);
        kgn::translateDecisions(buffer, code, state, ring);
    };

    // Engage the layer, then fill the held-action list to capacity.
    const kgn::KeyCode caps = kgn::KeyCode::fromString("CapsLock");
    feed(caps, kgn::KeyState::Down, at(0));
    for (std::size_t i = 0; i < kgn::kMaxHeld + 20; ++i) {
        feed(actionKeys[i], kgn::KeyState::Down, at(1));
    }
    // Drain what filling produced; we are measuring the RELEASE only.
    kgn::WorkItem discard{};
    while (ring.pop(discard)) {}

    // From here nothing new is admitted. Releasing CapsLock in hold mode
    // unwinds every held action in one call -- the largest single Up in the
    // engine.
    feed(caps, kgn::KeyState::Up, at(2));
    for (std::size_t i = 0; i < kgn::kMaxHeld + 20; ++i) {
        feed(actionKeys[i], kgn::KeyState::Up, at(3));
    }

    std::size_t produced = 0;
    while (ring.pop(discard)) ++produced;

    KGN_CHECK(produced <= kgn::kMaxReleaseWork);
    KGN_CHECK(produced > 0);   // the generator must not be degenerate
}

KGN_TEST(an_up_for_a_key_with_no_obligation_costs_no_capacity) {
    // Row U6 of the trace. If this ever spends a slot, the release bound is
    // unbounded: a user can produce arbitrarily many such Ups.
    kgn::LayerEngine engine;
    engine.setBindings({});

    kgn::WorkRing ring;
    kgn::DecisionBuffer buffer;
    const kgn::KeyCode code = kgn::KeyCode::fromString("KeyZ");
    for (int i = 0; i < 1000; ++i) {
        buffer.clear();
        engine.onKey(code, kgn::KeyState::Up, at(i), buffer);
        kgn::translateDecisions(buffer, code, kgn::KeyState::Up, ring);
    }
    KGN_CHECK(ring.empty());
}
```

Add an `at(ms)` helper at the top of the file matching the one in `test_layer_engine.cpp`,
plus `#include <string>`, `#include <vector>` and `#include "kgn/layer_engine.hpp"`.

- [ ] **Step 2: Run it**

```sh
cmake --build --preset debug && PATH=/c/msys64/ucrt64/bin:$PATH ./build/debug/core/test_hookchannel
```

Expected: PASS, 9 tests. If `produced > kMaxReleaseWork`, **stop** — the proof and the code
disagree and the design must be revisited before continuing.

- [ ] **Step 3: Verify the test is load-bearing**

Temporarily change `translateDecisions` to also push a `WorkItem` for
`Decision::Kind::Suppress`, rebuild, and confirm
`an_up_for_a_key_with_no_obligation_costs_no_capacity` **fails**. Revert the change.

- [ ] **Step 4: Commit**

```bash
git add core/tests/test_hookchannel.cpp
git commit -m "Prove the release drain fits the reserved queue capacity"
```

---

# Phase R — Rewire the core

### Task R1: The core consumes streams instead of calling the engine

**Files:**
- Modify: `core/include/kgn/core.hpp`, `core/src/core.cpp`
- Test: `core/tests/test_core.cpp`

**Interfaces:**
- Consumes: `kgn::WorkRing`, `kgn::PublicationRing`, `kgn::StatePublisher`, `kgn::WorkItem`, `kgn::PhysicalRecord`.
- Produces: `Core::Impl` gains `WorkRing work; PublicationRing publication; StatePublisher published;`
  and the methods `void drainWork(); void drainPublication();`. `Impl::applyDecisions` is deleted.

- [ ] **Step 1: Write the failing test**

```cpp
KGN_TEST(a_key_event_is_published_once_per_physical_event_not_once_per_decision) {
    // The grace replay produces TWO Forward decisions from ONE physical Up.
    // M2 broadcast one `key` event per decision, which would report a key the
    // user never pressed.
    kgn::CoreOptions options;
    options.endpointOverrideForTests = uniqueEndpoint();
    kgn::Core core(options);
    KGN_CHECK(core.start().ok());

    FakeClient client(options.endpointOverrideForTests);
    client.connect();
    core.step(kgn::Clock::now());
    client.drain();

    core.publishPhysical({kgn::KeyCode::fromString("KeyD").id(),
                          kgn::KeyState::Up, true, 0});
    core.pushWork({kgn::WorkItem::Kind::SendKey, true,
                   kgn::KeyCode::fromString("KeyD").id()});
    core.pushWork({kgn::WorkItem::Kind::SendKey, false,
                   kgn::KeyCode::fromString("KeyD").id()});
    core.step(kgn::Clock::now());

    KGN_CHECK_EQ(client.countEvents("key"), 1);
    core.stop("test over");
}
```

`FakeClient` and `countEvents` follow the pattern already in `test_core.cpp`; extend the
existing helper rather than writing a new one. Add two test-only seams to `core.hpp`:

```cpp
    // TEST-ONLY seams. They exist so the core's consumption of the two hook
    // streams can be driven without a hook thread; nothing in production calls
    // them.
    void publishPhysical(const PhysicalRecord& record);
    void pushWork(const WorkItem& item);
```

- [ ] **Step 2: Run and confirm failure**

Expected: compile error (`publishPhysical` undefined), then after adding the seams, a
failure showing 2 `key` events instead of 1.

- [ ] **Step 3: Replace `applyDecisions` with two drains**

In `core/src/core.cpp`, delete `Impl::applyDecisions` entirely and add:

```cpp
    // Stream B. One `key` event per PHYSICAL event -- never one per decision
    // (INV-PUB). A grace replay is two decisions and one physical event; a
    // release_all is many decisions and none at all.
    void drainPublication() {
        PhysicalRecord record{};
        while (publication.pop(record)) {
            if (server) {
                Json data = Json::object();
                data.set("code",
                         Json(std::string(KeyCode(record.code).toString())));
                data.set("state", Json(record.state == KeyState::Up       ? "up"
                                       : record.state == KeyState::Repeat ? "repeat"
                                                                          : "down"));
                data.set("suppressed", Json(record.suppressed));
                // Positional codes only. The core never resolves a keystroke
                // to a character, so this stream cannot reconstruct typed text
                // (SPEC section 12.4).
                server->broadcast("key", std::move(data));
            }
            // The state this record was stamped with, published straight after
            // the key that caused it, so the causal order is right without any
            // ordered queue entry.
            if (record.version != publishedVersion) publishState();
        }
        if (published.version() != publishedVersion) publishState();

        const std::uint64_t dropped = publicationDrops.exchange(0);
        if (dropped != 0) {
            note(DiagLevel::Info, "input.publication_dropped",
                 "key event publication fell behind by " + std::to_string(dropped)
                     + " event(s); overlay feedback may have missed a key");
        }
    }

    // Stream A. Things the core must DO. Never dropped: the ring's capacity
    // makes overflow structurally impossible (hookchannel.hpp).
    void drainWork() {
        WorkItem item{};
        while (work.pop(item)) {
            const KeyCode code{item.code};
            switch (item.kind) {
                case WorkItem::Kind::SendKey:
                    if (backends.output) {
                        backends.output->sendKey(code, item.down);
                    }
                    break;
                case WorkItem::Kind::RunAction:
                case WorkItem::Kind::ReleaseAction: {
                    const Decision decision{
                        item.kind == WorkItem::Kind::RunAction
                            ? Decision::Kind::RunAction
                            : Decision::Kind::ReleaseAction,
                        code,
                        item.kind == WorkItem::Kind::RunAction ? KeyState::Down
                                                               : KeyState::Up};
                    effects.clear();
                    dispatcher.onDecision(decision, Clock::now(), effects);
                    applyEffects(Clock::now());
                    break;
                }
            }
        }
    }

    void publishState() {
        const PublishedState state = published.state();
        publishedVersion = published.version();
        if (!server) return;

        Json mode = Json::object();
        mode.set("mode", Json(state.mode == Mode::Cursor ? "cursor" : "normal"));
        mode.set("latched", Json(state.latched));
        mode.set("activation", Json(activationName(state.activation)));
        server->broadcast("mode", std::move(mode));

        Json modifiers = Json::object();
        modifiers.set("shift", Json(state.shift));
        modifiers.set("control", Json(state.control));
        modifiers.set("alt", Json(state.alt));
        modifiers.set("meta", Json(state.meta));
        modifiers.set("caps_layer", Json(state.mode == Mode::Cursor));
        server->broadcast("modifiers", std::move(modifiers));
    }
```

Add the members to `Impl`:

```cpp
    WorkRing work;
    PublicationRing publication;
    StatePublisher published;
    std::atomic<std::uint64_t> publicationDrops{0};
    std::uint32_t publishedVersion = 0;
```

Delete the now-unused `bool shift/control/alt/meta` members (`core.cpp:161-164`) and make
`stateSnapshot()` read from `published.state()` instead.

- [ ] **Step 4: Update `Core::step`**

```cpp
void Core::step(TimePoint now) {
    if (!impl_->started || impl_->stopped) return;

    // The layer engine is no longer ticked here: it is owned by the input
    // thread, which wakes on its own grace deadline instead of at 60 Hz
    // (design note section 7). What remains on this beat is motion, which
    // SPEC section 6.4 does specify at 60 Hz.
    impl_->drainPublication();
    impl_->drainWork();

    if (impl_->enabled) {
        const TickResult motion = impl_->dispatcher.tick(now);
        if (!motion.pointer.zero() && impl_->backends.output) {
            impl_->backends.output->moveCursorBy(motion.pointer.x, motion.pointer.y);
        }
        if (!motion.scroll.zero() && impl_->backends.output) {
            impl_->backends.output->scroll(motion.scroll.x, motion.scroll.y);
        }
    }

    impl_->server->poll();
}
```

- [ ] **Step 5: Add the test seams**

```cpp
void Core::publishPhysical(const PhysicalRecord& record) {
    if (!impl_->publication.push(record)) ++impl_->publicationDrops;
}

void Core::pushWork(const WorkItem& item) { impl_->work.push(item); }
```

- [ ] **Step 6: Run the tests**

```sh
cmake --build --preset debug && PATH=/c/msys64/ucrt64/bin:$PATH ctest --preset debug
```

Expected: PASS. `test_core` gains 1 test.

- [ ] **Step 7: Commit**

```bash
git add core/include/kgn/core.hpp core/src/core.cpp core/tests/test_core.cpp
git commit -m "Drive the core from the two hook streams"
```

---

### Task R2: The control channel and bounded acknowledgement

**Files:**
- Modify: `core/include/kgn/hookchannel.hpp`, `core/src/hookchannel.cpp`, `core/include/kgn/core.hpp`, `core/src/core.cpp`
- Test: `core/tests/test_hookchannel.cpp`, `core/tests/test_core.cpp`

**Interfaces:**
- Produces: `kgn::Control`, `kgn::ControlRing`, and an abstract `kgn::EngineOwner` with
  `bool submit(const Control&)`, `bool awaitApplied(std::uint32_t seq, std::chrono::milliseconds)`.
  `Core` holds a `std::unique_ptr<EngineOwner>`; when null (no input backend) it applies
  configuration to a core-owned `LayerEngine` directly, which is what keeps every existing
  `test_core` case working unchanged.

- [ ] **Step 1: Add the control types to `hookchannel.hpp`**

```cpp
struct Control {
    enum class Kind : std::uint8_t {
        SetConfig,     // payload: const EngineConfig*
        SetBindings,   // payload: const BindingMap*
        ReleaseAll,
        SetEnabled,    // flag
        Stop,
    };

    Kind kind = Kind::ReleaseAll;
    bool flag = false;
    std::uint32_t seq = 0;
    const void* payload = nullptr;
};

using ControlRing = SpscRing<Control, 64>;

// Whoever owns the LayerEngine on this build. With an input backend that is
// the hook thread; with none, the core owns it directly and applies control
// inline. Either way the core only ever SUBMITS -- it never calls a mutating
// engine method itself.
class EngineOwner {
public:
    virtual ~EngineOwner() = default;

    // Never blocks. Returns false when the control ring is full.
    virtual bool submit(const Control& control) = 0;

    // Blocks the CALLER (always the core thread, never the hook thread) until
    // the owner reports `seq` applied, or the timeout lapses. Waiting in this
    // direction is safe: it stalls the core loop, never a hook callback.
    virtual bool awaitApplied(std::uint32_t seq,
                              std::chrono::milliseconds timeout) = 0;
};
```

Add `#include <chrono>` and `#include <memory>`.

- [ ] **Step 2: Write the failing test**

```cpp
KGN_TEST(a_setting_change_is_refused_when_the_engine_owner_cannot_apply_it) {
    // P6: reply with the truth rather than a false success. A core that says
    // "applied" for a change the engine never saw is worse than one that
    // refuses.
    kgn::CoreOptions options;
    options.endpointOverrideForTests = uniqueEndpoint();
    kgn::Core core(options);
    core.setEngineOwnerForTests(std::make_unique<NeverAcknowledging>());
    KGN_CHECK(core.start().ok());

    FakeClient client(options.endpointOverrideForTests);
    client.connect();
    core.step(kgn::Clock::now());
    client.drain();

    client.send("set_activation_mode", R"({"mode":"toggle"})", "c1");
    core.step(kgn::Clock::now());

    const Reply reply = client.replyFor("c1");
    KGN_CHECK(!reply.ok);
    KGN_CHECK_EQ(reply.code, std::string("input.queue_overflow"));
    core.stop("test over");
}
```

with, in the anonymous namespace:

```cpp
class NeverAcknowledging : public kgn::EngineOwner {
public:
    bool submit(const kgn::Control&) override { return true; }
    bool awaitApplied(std::uint32_t, std::chrono::milliseconds) override {
        return false;
    }
};
```

Use a 5 ms timeout in tests by exposing `CoreOptions::controlTimeout` defaulting to
250 ms, and set it to 5 ms in this test so the suite stays fast.

- [ ] **Step 3: Run and confirm failure**

Expected: compile error (`setEngineOwnerForTests` undefined).

- [ ] **Step 4: Route the engine-mutating commands through the owner**

In `core/src/core.cpp`, add to `Impl`:

```cpp
    std::unique_ptr<EngineOwner> owner_of_engine;
    std::uint32_t controlSeq = 0;

    // Every command whose reply means APPLIED goes through here. With no
    // engine owner the core owns the engine itself and applies inline, which
    // is what a build with no input backend does.
    Reply applyControl(Control control) {
        if (!owner_of_engine) {
            applyControlInline(control);
            return Reply::success();
        }
        control.seq = ++controlSeq;
        if (!owner_of_engine->submit(control)) {
            return Reply::failure("input.queue_overflow",
                                  "the input thread is not accepting work");
        }
        if (!owner_of_engine->awaitApplied(control.seq, options.controlTimeout)) {
            return Reply::failure("input.queue_overflow",
                                  "the input thread did not apply the change in time");
        }
        return Reply::success();
    }
```

`applyControlInline` contains what the M2 handlers did directly: `engine.setConfig`,
`engine.setBindings`, `engine.releaseAll` into `decisions`, then the existing
`releaseEverything()` path. Rewrite `handleSetActivationMode`, `handleSetSetting`,
`handleSetEnabled`, `handleSetBindings`, `reload()` and `handleReleaseAll` to build a
`Control` and return `applyControl(...)`.

Add the test seam:

```cpp
void Core::setEngineOwnerForTests(std::unique_ptr<EngineOwner> owner) {
    impl_->owner_of_engine = std::move(owner);
}
```

- [ ] **Step 5: Run the tests**

```sh
cmake --build --preset debug && PATH=/c/msys64/ucrt64/bin:$PATH ctest --preset debug
```

Expected: PASS, all previous `test_core` cases still green (they run with no owner, so they
apply inline exactly as before).

- [ ] **Step 6: Commit**

```bash
git add core/include/kgn/hookchannel.hpp core/src/hookchannel.cpp core/include/kgn/core.hpp core/src/core.cpp core/tests/test_core.cpp core/tests/test_hookchannel.cpp
git commit -m "Marshal engine mutation through a control channel"
```

---

### Task R3: The shutdown fallback

**Files:**
- Modify: `core/src/core.cpp`
- Test: `core/tests/test_core.cpp`

**Interfaces:**
- Consumes: `EngineOwner`, `Dispatcher::releaseAll`, `OutputBackend::releaseAll`, `InputBackend::stop`.
- Produces: no new signatures.

- [ ] **Step 1: Write the failing test**

```cpp
KGN_TEST(shutdown_releases_everything_even_when_the_engine_owner_never_answers) {
    // The formal fallback. OS-visible obligations partition into three sets:
    // natively-forwarded physical presses (discharged by uninstalling the
    // hook), dispatcher state, and backend-synthesized keys. Only the first
    // needs the engine, and uninstalling covers it -- so an unresponsive
    // engine owner must not be able to leave anything held.
    kgn::CoreOptions options;
    options.endpointOverrideForTests = uniqueEndpoint();
    options.controlTimeout = std::chrono::milliseconds(5);

    auto output = std::make_unique<RecordingOutput>();
    RecordingOutput* recorded = output.get();
    auto input = std::make_unique<RecordingInput>();
    RecordingInput* recordedInput = input.get();

    kgn::Backends backends;
    backends.output = std::move(output);
    backends.input = std::move(input);

    kgn::Core core(options, std::move(backends));
    core.setEngineOwnerForTests(std::make_unique<NeverAcknowledging>());
    KGN_CHECK(core.start().ok());

    core.stop("test over");

    KGN_CHECK_EQ(recordedInput->stopCalls, 1);
    KGN_CHECK(recorded->releaseAllCalls >= 1);
    // The input backend must be stopped BEFORE the output is released, or new
    // work can arrive after the drain.
    KGN_CHECK(recordedInput->stopOrder < recorded->releaseAllOrder);
}
```

`RecordingInput` mirrors `RecordingOutput`: it records `start`/`stop` calls and a global
monotonic order counter shared with `RecordingOutput`.

- [ ] **Step 2: Run and confirm failure**

Expected: FAIL — `stopCalls` is 0, because `Core::stop` never stops the input backend.

- [ ] **Step 3: Implement the ordered shutdown**

```cpp
void Core::stop(const std::string& reason) {
    if (!impl_ || impl_->stopped) return;
    impl_->stopped = true;
    if (!impl_->started) {
        if (impl_->server) impl_->server->shutdown(reason);
        impl_->owner.release();
        return;
    }

    // 1. Ask the engine owner to unwind, but do not depend on an answer.
    if (impl_->owner_of_engine) {
        Control control{Control::Kind::ReleaseAll, false, ++impl_->controlSeq, nullptr};
        if (impl_->owner_of_engine->submit(control)) {
            impl_->owner_of_engine->awaitApplied(control.seq, impl_->options.controlTimeout);
        }
    } else {
        impl_->releaseEverything();
    }

    // 2. Stop seeing input BEFORE draining, so nothing arrives mid-drain.
    //    Uninstalling the hook is also what discharges every natively
    //    forwarded press: the user's own physical release now reaches the OS
    //    directly.
    if (impl_->backends.input) impl_->backends.input->stop();

    // 3. Execute whatever the owner managed to hand over.
    impl_->drainWork();

    // 4. The two obligation sets the core owns outright. Neither needs the
    //    engine, which is why an unresponsive owner cannot strand anything.
    impl_->effects.clear();
    impl_->dispatcher.releaseAll(impl_->effects);
    for (const auto& effect : impl_->effects) {
        if (effect.kind == Effect::Kind::Button && impl_->backends.output) {
            impl_->backends.output->button(effect.button, effect.down);
        }
    }
    impl_->effects.clear();
    if (impl_->backends.output) impl_->backends.output->releaseAll();

    if (impl_->server) impl_->server->shutdown(reason);
    impl_->owner.release();
}
```

- [ ] **Step 4: Run the tests**

Expected: PASS, `ctest --preset debug` 10/10 binaries.

- [ ] **Step 5: Commit**

```bash
git add core/src/core.cpp core/tests/test_core.cpp
git commit -m "Make shutdown independent of the engine owner answering"
```

---

# Phase W — The Windows backend

### Task W1: The scancode keymap

**Files:**
- Create: `core/src/platform/windows/scancode_keymap.hpp`, `core/src/platform/windows/scancode_keymap.cpp`, `core/tests/test_scancode_keymap.cpp`
- Modify: `core/CMakeLists.txt`

**Interfaces:**
- Produces: `class kgn::win::ScancodeKeymap` with
  `ScancodeKeymap();` (resolves every `KeyCode` once, at construction),
  `[[nodiscard]] KeyCode toKeyCode(std::uint32_t scanCode, bool extended) const;`
  `[[nodiscard]] bool toScanCode(KeyCode code, std::uint32_t& scanCode, bool& extended) const;`

- [ ] **Step 1: Write the failing test**

```cpp
KGN_TEST(every_mapped_scancode_round_trips_and_the_table_is_a_bijection) {
    // SPEC section 13: round-trip every code through the backend's table,
    // assert bijection and no unmapped entries.
    const kgn::win::ScancodeKeymap map;
    std::set<std::pair<std::uint32_t, bool>> seen;
    int mapped = 0;

    for (std::uint32_t scan = 0; scan < 0x200; ++scan) {
        for (bool extended : {false, true}) {
            const kgn::KeyCode code = map.toKeyCode(scan, extended);
            if (!code.valid()) continue;
            ++mapped;

            std::uint32_t back = 0;
            bool backExtended = false;
            KGN_CHECK(map.toScanCode(code, back, backExtended));
            KGN_CHECK_EQ(back, scan);
            KGN_CHECK(backExtended == extended);

            // No two scancodes may claim one KeyCode.
            KGN_CHECK(seen.insert({scan, extended}).second);
        }
    }
    KGN_CHECK(mapped >= 100);
}

KGN_TEST(the_extended_flag_separates_the_keys_that_share_a_scancode) {
    const kgn::win::ScancodeKeymap map;
    KGN_CHECK(map.toKeyCode(0x1C, false) == kgn::KeyCode::fromString("Enter"));
    KGN_CHECK(map.toKeyCode(0x1C, true) == kgn::KeyCode::fromString("NumpadEnter"));
    KGN_CHECK(map.toKeyCode(0x1D, false) == kgn::KeyCode::fromString("ControlLeft"));
    KGN_CHECK(map.toKeyCode(0x1D, true) == kgn::KeyCode::fromString("ControlRight"));
    KGN_CHECK(map.toKeyCode(0x48, false) == kgn::KeyCode::fromString("Numpad8"));
    KGN_CHECK(map.toKeyCode(0x48, true) == kgn::KeyCode::fromString("ArrowUp"));
}

KGN_TEST(an_unmapped_scancode_is_invalid_rather_than_guessed) {
    const kgn::win::ScancodeKeymap map;
    KGN_CHECK(!map.toKeyCode(0x1FF, false).valid());
}
```

Guard the whole test file with `#ifdef _WIN32` so the Linux build skips it cleanly, and
register it in CMake only under `if(WIN32)`.

- [ ] **Step 2: Run and confirm failure**

Expected: the header does not exist.

- [ ] **Step 3: Implement the keymap**

The table is a `constexpr` array of `{scanCode, extended, name}` covering the full SPEC §2.1
vocabulary that a PC keyboard can produce. Build two lookup structures in the constructor:
a flat `std::array<KeyCode, 512>` indexed by `scan | (extended << 8)`, and a
`std::unordered_map<KeyCode, std::pair<std::uint32_t,bool>>` for the reverse direction.

```cpp
// Every KeyCode is resolved ONCE, here, at construction. KeyCode::fromString
// takes the intern table's mutex and builds a std::string, so calling it per
// event would put both an allocation and a contended lock inside
// WH_KEYBOARD_LL -- the two things SPEC section 8.2 forbids by name.
ScancodeKeymap::ScancodeKeymap() {
    for (const Entry& entry : kTable) {
        const KeyCode code = KeyCode::fromString(entry.name);
        forward_[index(entry.scanCode, entry.extended)] = code;
        reverse_.emplace(code, std::make_pair(entry.scanCode, entry.extended));
    }
}
```

Include at minimum: `KeyA`–`KeyZ`, `Digit0`–`Digit9`, `F1`–`F24`, all modifiers, `Space`,
`Tab`, `Enter`, `Backspace`, the punctuation set, `IntlBackslash`, the navigation cluster,
the arrows, the full numpad including `NumpadEnter` and `NumLock`, `Escape`, `PrintScreen`,
`ScrollLock`, `Pause`, `ContextMenu`, `CapsLock`. **Do not map `Fn`** — SPEC §8.4 says it
never produces a scancode and must not be promised as bindable.

- [ ] **Step 4: Run the tests, both platforms**

Expected: PASS on Windows; the file is skipped on Linux.

- [ ] **Step 5: Commit**

```bash
git add core/src/platform/windows/ core/tests/test_scancode_keymap.cpp core/CMakeLists.txt
git commit -m "Map Windows scancodes to the key vocabulary"
```

---

### Task W2: The low-level hook input backend

**Files:**
- Create: `core/src/platform/windows/hook_input.hpp`, `core/src/platform/windows/hook_input.cpp`
- Modify: `core/CMakeLists.txt`

**Interfaces:**
- Produces: `class kgn::win::HookInput : public InputBackend, public EngineOwner`, constructed with
  `HookInput(WorkRing&, PublicationRing&, StatePublisher&, EngineConfig)`.

- [ ] **Step 1: Write the hook thread**

Key requirements, each of which must appear in the code with the comment explaining it:

```cpp
// The hook procedure. Everything it is allowed to do is here: derive the
// physical state, ask the engine, translate, publish, return. No IPC, no
// logging, no allocation, no lock the core thread can hold.
LRESULT CALLBACK HookInput::proc(int code, WPARAM wParam, LPARAM lParam) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wParam, lParam);
    const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

    // Our own SendInput output, or it feeds straight back into this hook
    // (SPEC section 8.2).
    if ((event->flags & LLKHF_INJECTED) != 0) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
    const KeyCode key = keymap_.toKeyCode(event->scanCode,
                                          (event->flags & LLKHF_EXTENDED) != 0);
    if (!key.valid()) return CallNextHookEx(nullptr, code, wParam, lParam);

    // KBDLLHOOKSTRUCT carries no repeat count, so autorepeat arrives as a
    // second Down. Deriving Repeat here keeps ordinary held-key autorepeat on
    // the native passthrough path; without it the engine would answer
    // Forward(c, Repeat) for a Down event, the passthrough rule would not
    // match, and every autorepeat of every key would be suppressed and
    // re-injected.
    KeyState state = up ? KeyState::Up : KeyState::Down;
    const std::size_t slot = key.id();
    if (!up && physicallyDown_[slot]) state = KeyState::Repeat;
    physicallyDown_[slot] = !up;

    if (!enabled_) return CallNextHookEx(nullptr, code, wParam, lParam);

    // The admission gate. A Down or Repeat may create an obligation, so it is
    // admitted only while the ring can certainly carry whatever the engine
    // emits. An Up DISCHARGES obligations and is never refused -- refusing one
    // is how a key gets stranded (see the capacity proof in hookchannel.hpp).
    if (state != KeyState::Up && work_.free() < kWorkAdmissionGate) {
        ++admissionRefusals_;
        publish(key, state, true);
        return 1;   // suppress: a missing keystroke, never a stranded one
    }

    decisions_.clear();
    engine_.onKey(key, state, Clock::now(), decisions_);
    const bool native = translateDecisions(decisions_, key, state, work_);
    republish();
    publish(key, state, !native);
    return native ? CallNextHookEx(nullptr, code, wParam, lParam) : 1;
}
```

The pump:

```cpp
void HookInput::run() {
    // Force the message queue into existence before anything can post to it.
    MSG message;
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    install();
    ready_.set_value();

    while (!stopping_) {
        const DWORD timeout = waitTimeout();
        MsgWaitForMultipleObjectsEx(1, &wake_, timeout, QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
        // Retrieving messages is what lets the system call our hook.
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        drainControl();
        expireGrace();
        reinstallIfLost();
    }
    uninstall();
}
```

`waitTimeout()` returns `INFINITE` when `engine_.nextDeadline()` is empty, otherwise the
clamped milliseconds to the deadline. `expireGrace()` runs `engine_.tick()` only while
`work_.free() >= kWorkAdmissionGate`, translating with `KeyCode{}` so `native` is always
false and no `PhysicalRecord` is emitted.

`reinstallIfLost()` calls `SetWindowsHookExW` again when the handle is gone
(SPEC §8.2: "MUST detect having been unhooked and re-install automatically") and emits
`input.hook_lost` through a flag the core reads — never a log call on this thread.

`capabilities()` returns `canSuppress = true` and these limitations verbatim:

```cpp
    c.limitations.emplace_back(
        "Keys cannot be intercepted while an elevated window has focus, "
        "because this process is not elevated.");
    c.limitations.emplace_back(
        "Ctrl+Alt+Del and the Secure Attention Sequence are never "
        "interceptable. This is by design in Windows, not a defect.");
```

- [ ] **Step 2: Wire it into the factory**

`core/src/platform/windows/platform_windows.cpp`:

```cpp
#include "kgn/platform.hpp"
// ... the three Windows backends

namespace kgn {

Backends createBackends() {
    Backends backends;
    backends.output = std::make_unique<win::SendInputOutput>();
    backends.window = std::make_unique<win::Win32Window>();
    backends.input = std::make_unique<win::HookInput>(/* channels wired by Core */);
    return backends;
}

}  // namespace kgn
```

The channels live in `Core::Impl`, so `HookInput` is constructed by `Core` rather than by
the factory. Give `Backends` an optional `std::function<std::unique_ptr<InputBackend>(WorkRing&, PublicationRing&, StatePublisher&)> makeInput`
that the factory sets and `Core::start()` calls, so the factory stays the one place that
knows the platform while the channels stay owned by the core.

- [ ] **Step 3: Add the sources to CMake**

```cmake
    set(KGN_PLATFORM_SOURCES
        src/platform/windows/scancode_keymap.cpp
        src/platform/windows/hook_input.cpp
        src/platform/windows/sendinput_output.cpp
        src/platform/windows/win32_window.cpp
        src/platform/windows/platform_windows.cpp)
```

- [ ] **Step 4: Build and smoke test**

```sh
cmake --preset debug && cmake --build --preset debug
./build/debug/core/keygnosys-core.exe
```

Expected: it starts, `hello` reports `input: "windows-hook"`, and typing in another window
still works. Press CapsLock+the bound cursor keys and confirm the pointer moves.

- [ ] **Step 5: Commit**

```bash
git add core/src/platform/windows/ core/CMakeLists.txt
git commit -m "Add the Windows low-level keyboard hook backend"
```

---

### Task W3: `SendInput` output, with its own release tracking

**Files:**
- Create: `core/src/platform/windows/sendinput_output.hpp`, `core/src/platform/windows/sendinput_output.cpp`

**Interfaces:**
- Produces: `class kgn::win::SendInputOutput : public OutputBackend`.

- [ ] **Step 1: Implement, with the tracking that the shutdown fallback depends on**

```cpp
// releaseAll() is the third of the three obligation sets the shutdown fallback
// discharges, and the only one nothing else can reach. A key this backend
// synthesised may have no physical key behind it -- a grace-window expiry
// forwards a press the user is holding, but release_all can also put one down
// with nothing to lift it. So every synthesised press is recorded here until
// its release is emitted.
void SendInputOutput::sendKey(KeyCode code, bool down) {
    std::uint32_t scan = 0;
    bool extended = false;
    if (!keymap_.toScanCode(code, scan, extended)) return;

    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = static_cast<WORD>(scan);
    input.ki.dwFlags = KEYEVENTF_SCANCODE
                       | (extended ? KEYEVENTF_EXTENDEDKEY : 0u)
                       | (down ? 0u : KEYEVENTF_KEYUP);
    if (SendInput(1, &input, sizeof(input)) == 1) {
        heldKeys_[code.id()] = down;
    }
}

void SendInputOutput::releaseAll() {
    for (std::size_t id = 0; id < heldKeys_.size(); ++id) {
        if (heldKeys_[id]) sendKey(KeyCode(static_cast<std::uint16_t>(id)), false);
    }
    for (std::size_t i = 0; i < heldButtons_.size(); ++i) {
        if (heldButtons_[i]) button(static_cast<MouseButton>(i), false);
    }
}
```

Absolute warp must normalise against the **virtual desktop**:

```cpp
// SPEC section 8.3 names normalising against the primary monitor as the
// classic bug that makes warp land on the wrong screen.
void SendInputOutput::moveCursorTo(int x, int y) {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 1 || height <= 1) return;

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE
                       | MOUSEEVENTF_VIRTUALDESK;
    input.mi.dx = static_cast<LONG>((x - left) * 65535LL / (width - 1));
    input.mi.dy = static_cast<LONG>((y - top) * 65535LL / (height - 1));
    SendInput(1, &input, sizeof(input));
}
```

`scroll` uses `MOUSEEVENTF_WHEEL`/`MOUSEEVENTF_HWHEEL` in `WHEEL_DELTA` units.
`capabilities()` returns `canWarpAbsolute = true`.

- [ ] **Step 2: Manual verification**

Run the core, engage the layer, and confirm: pointer moves in all eight directions; a
diagonal is the same speed as a cardinal; left/right/middle click work; drag lock holds
across motion and releases on layer exit; page scroll moves about a screen.

- [ ] **Step 3: Commit**

```bash
git add core/src/platform/windows/sendinput_output.hpp core/src/platform/windows/sendinput_output.cpp
git commit -m "Add the Windows SendInput output backend"
```

---

### Task W4: Win32 windows and monitors

**Files:**
- Create: `core/src/platform/windows/win32_window.hpp`, `core/src/platform/windows/win32_window.cpp`

**Interfaces:**
- Produces: `class kgn::win::Win32Window : public WindowBackend`.

- [ ] **Step 1: Implement enumeration with the cloaked check**

```cpp
// DWMWA_CLOAKED is not optional. Without it, UWP applications leave ghost
// top-level windows in the list that are invisible on screen but occupy slots,
// and the numbered window bindings then point at nothing (SPEC section 8.4).
bool Win32Window::isEligible(HWND window) {
    if (!IsWindowVisible(window)) return false;
    if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    if (GetWindowTextLengthW(window) == 0) return false;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked)))
        && cloaked != 0) {
        return false;
    }
    return true;
}
```

`focus()` uses `SetForegroundWindow` with the `AttachThreadInput` workaround.
`monitors()` uses `EnumDisplayMonitors`/`GetMonitorInfoW`, marking `MONITORINFOF_PRIMARY`.
`moveWindowToMonitor()` restores if maximised, `SetWindowPos` to the scaled relative
position on the target, re-maximises, and accounts for per-monitor DPI via
`GetDpiForMonitor` (this is what `shcore` is linked for).
`WindowInfo::process` is the lowercased executable name from `GetWindowThreadProcessId` +
`QueryFullProcessImageNameW`; `wmClass` stays empty on Windows.
`capabilities()` returns `canMoveWindows = true`.

- [ ] **Step 2: Manual verification**

Confirm the slot list contains only real windows (no UWP ghosts), focus switching works
including to a window on another monitor, and moving a maximised window between monitors of
different DPI preserves its relative size.

- [ ] **Step 3: Commit**

```bash
git add core/src/platform/windows/win32_window.hpp core/src/platform/windows/win32_window.cpp
git commit -m "Add the Win32 window and monitor backend"
```

---

### Task W5: The window slot registry

Pure and testable; SPEC §6.5 requires stable ordering or the number bindings are unusable.

**Files:**
- Create: `core/include/kgn/slots.hpp`, `core/src/slots.cpp`, `core/tests/test_slots.cpp`
- Modify: `core/CMakeLists.txt`

**Interfaces:**
- Produces: `class kgn::SlotRegistry` with
  `void update(const std::vector<WindowInfo>&);`
  `[[nodiscard]] std::optional<WindowId> at(int index) const;`   // 1-based
  `[[nodiscard]] const std::vector<std::optional<WindowId>>& slots() const;`

- [ ] **Step 1: Write the failing tests**

```cpp
KGN_TEST(a_window_keeps_its_slot_for_as_long_as_it_exists) {
    kgn::SlotRegistry registry;
    registry.update({win(10), win(20), win(30)});
    KGN_CHECK_EQ(*registry.at(1), kgn::WindowId{10});
    KGN_CHECK_EQ(*registry.at(2), kgn::WindowId{20});

    // 20 closes. 10 and 30 must NOT shuffle: a map that changes under the
    // user's fingers cannot be learned (SPEC section 6.5).
    registry.update({win(30), win(10)});
    KGN_CHECK_EQ(*registry.at(1), kgn::WindowId{10});
    KGN_CHECK(!registry.at(2).has_value());
    KGN_CHECK_EQ(*registry.at(3), kgn::WindowId{30});
}

KGN_TEST(a_new_window_takes_the_lowest_free_index) {
    kgn::SlotRegistry registry;
    registry.update({win(10), win(20), win(30)});
    registry.update({win(10), win(30)});
    registry.update({win(10), win(30), win(40)});
    KGN_CHECK_EQ(*registry.at(2), kgn::WindowId{40});
}

KGN_TEST(only_nine_windows_carry_an_index) {
    kgn::SlotRegistry registry;
    std::vector<kgn::WindowInfo> windows;
    for (int i = 1; i <= 12; ++i) windows.push_back(win(kgn::WindowId(i)));
    registry.update(windows);
    KGN_CHECK(registry.at(9).has_value());
    KGN_CHECK(!registry.at(10).has_value());
}
```

- [ ] **Step 2: Run, confirm failure, implement, run again**

- [ ] **Step 3: Commit**

```bash
git add core/include/kgn/slots.hpp core/src/slots.cpp core/tests/test_slots.cpp core/CMakeLists.txt
git commit -m "Give windows stable slot indices"
```

---

### Task W6: Warp and window effects

`Effect::Kind::Warp` and `Effect::Kind::Window` are currently unimplemented — `core.cpp:288-297`
only emits a diagnostic when there is no window backend.

**Files:**
- Modify: `core/src/core.cpp`
- Test: `core/tests/test_core.cpp`

**Interfaces:**
- Consumes: `SlotRegistry`, `WindowBackend`, `OutputBackend`.
- Produces: `Impl::applyWarp(const Action&)`, `Impl::applyWindow(const Action&)`.

- [ ] **Step 1: Write the failing tests using a fake window backend**

Cover: `warp.grid` cell 5 lands at the centre of the monitor the pointer is currently on
(not the primary); `warp.corner` for all five; `warp.monitor next` wraps; `window.slot`
focuses the registry's entry; `window.cycle` moves in slot order not recency;
`window.move_to_monitor` calls `moveWindowToMonitor` with the focused id.

- [ ] **Step 2: Implement, run, confirm green**

Every operation the backend cannot perform emits `window.unsupported` and does nothing —
never an approximation (P6).

- [ ] **Step 3: Commit**

```bash
git commit -am "Implement the warp and window actions"
```

---

### Task W7: Non-blocking double-click

**Files:**
- Modify: `core/src/core.cpp`
- Test: `core/tests/test_core.cpp`

**Interfaces:**
- Produces: `Impl::pendingDoubleClicks` — a small fixed array of
  `{MouseButton button; TimePoint dueAt; int remaining;}` serviced from `Core::step`.

- [ ] **Step 1: Write the failing test**

Assert that one `Effect::Kind::DoubleClick` produces two press/release pairs separated by at
least the configured interval, that `step` never blocks, and that a `release_all` between
the pairs cancels the second and leaves the button **up**.

- [ ] **Step 2: Implement**

The interval comes from `GetDoubleClickTime()` on Windows, surfaced through a new
`OutputBackend::doubleClickInterval()` returning `std::chrono::milliseconds` so the core
stays platform-neutral. Schedule on the core's own timeline; never sleep.

Cancellation on layer release is P7 over fidelity: the button is guaranteed up.

- [ ] **Step 3: Commit**

```bash
git commit -am "Schedule double-clicks without blocking the loop"
```

---

### Task W8: The remaining events and a real `get_state`

**Files:**
- Modify: `core/src/core.cpp`
- Test: `core/tests/test_core.cpp`

**Interfaces:**
- Produces: `Impl::publishWindows()`, `Impl::publishMonitors()`, `Impl::publishFocus()`,
  `Impl::publishPointer()`.

- [ ] **Step 1: Write the failing tests**

`focus` fires when the focused window changes and not otherwise; `windows` is debounced to
at most one emission per 250 ms; `monitors` fires on topology change; `pointer` is emitted
only while the cursor layer is engaged and at no more than 20 Hz; `get_state` returns the
real focus, windows and monitors rather than `null`/`[]`.

- [ ] **Step 2: Implement, run, confirm green**

Replace the hardcoded `stateSnapshot()` values at `core.cpp:341-347`.

- [ ] **Step 3: Commit**

```bash
git commit -am "Publish focus, windows, monitors and pointer"
```

---

### Task W9: The manual test matrix

SPEC §13's last row makes this the entire test strategy for platform backends, and the file
does not exist.

**Files:**
- Create: `docs/manual-tests.md`

- [ ] **Step 1: Write the matrix**

One row per behaviour that cannot be automated without real hardware and a real display,
each with exact steps and the expected result. At minimum:

- The hook survives `LowLevelHooksTimeout` under load; `input.hook_lost` appears and the
  hook re-installs.
- Interception is inert while an elevated window has focus, and the UI says so.
- `Ctrl+Alt+Del` is never intercepted, and this is documented as correct.
- Ordinary typing is unaffected while the layer is off, including autorepeat and IME.
- A grace-window tap of an action-bound key types the character.
- Every action in SPEC §7 performs its documented behaviour.
- Warp lands correctly on a **secondary** monitor and on a monitor left of the primary
  (negative virtual-desktop coordinates).
- Moving a maximised window between monitors of different DPI preserves relative size.
- UWP ghost windows do not appear in the slot list.
- P7: kill the core with the layer engaged and keys held; no key or button stays down.
- Two cores: the second refuses with exit 3.

- [ ] **Step 2: Execute the matrix and record the results**

- [ ] **Step 3: Commit**

```bash
git add docs/manual-tests.md
git commit -m "Add the manual platform test matrix"
```

---

### Task W10: SPEC amendments and the M3 README update

**Files:**
- Modify: `docs/SPEC.md`, `core/README.md`

- [ ] **Step 1: Apply the eight amendments from the design note §12**

- [ ] **Step 2: Update `core/README.md`'s status table**

Mark the Windows backend ✅, describe the thread split and the two streams, and note that
`kgn_platform` is now always built and linked only by the executable.

- [ ] **Step 3: Full verification, both platforms**

```sh
cmake --preset debug && cmake --build --preset debug && PATH=/c/msys64/ucrt64/bin:$PATH ctest --preset debug
cmake --preset default && cmake --build --preset default && PATH=/c/msys64/ucrt64/bin:$PATH ctest --preset default
python -m pytest -q
keygnosys --check
git diff --check
grep -rEn 'mousetrapkeys|mousetrap|mtk' --exclude-dir=legacy . | grep -v '^\./\.git'
```

Expected: both presets clean under `-Werror`; all ctest binaries pass; pytest 90 passed;
the legacy-name sweep returns nothing.

- [ ] **Step 4: Commit and open the PR**

```bash
git add -A
git commit -m "Document the M3 threading model and Windows backend"
git push -u origin feat/m3-windows
gh pr create --title "M3: Windows backend" --body "..."
```

**Stop for review. Do not merge.**

---

## Self-review notes

**Spec coverage.** Design note §1→Task W2; §2→C1, R1; §3→C1; §4→C1, C3; §5→C1, R1; §6→R2;
§7→C2, W2; §8→R2; §9→R3; §10→W2; §11→F1; §12→W10; §13→no task (rejected alternatives).
SPEC §8.2→W2; §8.3→W3; §8.4→W4; §6.5→W5; §7.1–7.4→W6, W7; §7.5→W6; §11→P2; §13→W9.

**Known ordering dependency.** Task C2 (`nextDeadline`) is listed after C1 in the document
but must be implemented **first**, because C1's `hookchannel.hpp` does not depend on it while
W2's pump does. Either order works; C2 is the smaller change and makes a good warm-up.

**Deliberately out of scope**, per the milestone boundary: Linux/evdev/X11, launcher scripts,
autostart, installer, packaging, the M5 configuration UI and the M6 layout editor.
