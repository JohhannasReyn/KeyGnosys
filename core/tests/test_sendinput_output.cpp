// What the output backend believes it is holding.
//
// The invariant under test:
//
//     heldKeys_ and heldButtons_ reflect SUCCESSFUL OS-visible transitions
//     only, never attempted ones.
//
// SendInput can be refused -- UIPI blocks injection into a higher-integrity
// foreground window, and the input desktop can be switched out from under it.
// Provoking a real refusal needs an elevated foreground window, which a test
// suite may not arrange, so the dispatch point is overridden instead. That is
// the smallest seam that makes the failure path deterministic.

#if defined(_WIN32)

#include "../src/platform/windows/sendinput_output.hpp"

#include "kgn_test.hpp"

#include <string>
#include <vector>

using namespace kgn;
using kgn::win::SendInputOutput;

namespace {

// Refuses on demand and records what it was asked to do.
class ControllableOutput : public SendInputOutput {
public:
    bool refuse = false;
    int attempts = 0;

protected:
    bool dispatch(const INPUT&) override {
        ++attempts;
        return !refuse;
    }
};

int countCode(const Diagnostics& diagnostics, const char* code) {
    int found = 0;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) ++found;
    }
    return found;
}

}  // namespace

KGN_TEST(a_press_the_os_refused_is_not_recorded_as_held) {
    // Recording it would make releaseAll() emit a release for a key the OS
    // never saw pressed.
    ControllableOutput output;
    const KeyCode a = KeyCode::fromString("KeyA");

    output.refuse = true;
    output.sendKey(a, true);
    KGN_CHECK_EQ(output.attempts, 1);

    output.refuse = false;
    output.attempts = 0;
    output.releaseAll();
    KGN_CHECK_EQ(output.attempts, 0);   // nothing was held, so nothing is lifted
}

KGN_TEST(a_release_the_os_refused_keeps_the_key_held_so_it_can_be_retried) {
    // The dangerous direction. Clearing the record on a failed release
    // discards the only knowledge in the process that the key is still down.
    ControllableOutput output;
    const KeyCode a = KeyCode::fromString("KeyA");

    output.sendKey(a, true);            // succeeds; now held
    output.refuse = true;
    output.sendKey(a, false);           // refused; must stay held
    output.refuse = false;

    output.attempts = 0;
    output.releaseAll();
    KGN_CHECK_EQ(output.attempts, 1);   // retried

    output.attempts = 0;
    output.releaseAll();
    KGN_CHECK_EQ(output.attempts, 0);   // and now genuinely released
}

KGN_TEST(a_button_press_the_os_refused_is_not_recorded_as_held) {
    ControllableOutput output;
    output.refuse = true;
    output.button(MouseButton::Left, true);
    output.refuse = false;

    output.attempts = 0;
    output.releaseAll();
    KGN_CHECK_EQ(output.attempts, 0);
}

KGN_TEST(a_button_release_the_os_refused_keeps_the_button_held) {
    ControllableOutput output;
    output.button(MouseButton::Right, true);
    output.refuse = true;
    output.button(MouseButton::Right, false);
    output.refuse = false;

    output.attempts = 0;
    output.releaseAll();
    KGN_CHECK_EQ(output.attempts, 1);
}

KGN_TEST(release_all_lifts_every_key_and_button_it_actually_holds) {
    ControllableOutput output;
    output.sendKey(KeyCode::fromString("KeyA"), true);
    output.sendKey(KeyCode::fromString("KeyB"), true);
    output.sendKey(KeyCode::fromString("ShiftLeft"), true);
    output.button(MouseButton::Left, true);
    output.button(MouseButton::Middle, true);

    output.attempts = 0;
    output.releaseAll();
    KGN_CHECK_EQ(output.attempts, 5);

    output.attempts = 0;
    output.releaseAll();
    KGN_CHECK_EQ(output.attempts, 0);
}

KGN_TEST(a_key_with_no_scancode_is_refused_rather_than_approximated) {
    // Pause has no unambiguous scancode, so it is not synthesised at all. It
    // must also not be recorded as held, or releaseAll() would try forever.
    ControllableOutput output;
    output.sendKey(KeyCode::fromString("Pause"), true);
    KGN_CHECK_EQ(output.attempts, 0);

    output.releaseAll();
    KGN_CHECK_EQ(output.attempts, 0);
}

KGN_TEST(refused_synthesis_is_reported_rather_than_swallowed) {
    // P6. A refused RELEASE is an error, because something may still be held;
    // a refused press is a warning, because nothing is.
    ControllableOutput output;
    const KeyCode a = KeyCode::fromString("KeyA");

    output.refuse = true;
    output.sendKey(a, true);
    Diagnostics found;
    output.drainDiagnostics(found);
    KGN_CHECK_EQ(countCode(found, "output.send_failed"), 1);
    KGN_CHECK(!found.empty() && found[0].level == DiagLevel::Warn);

    // Drained, so it is reported once per episode rather than forever.
    found.clear();
    output.drainDiagnostics(found);
    KGN_CHECK(found.empty());

    output.refuse = false;
    output.sendKey(a, true);
    output.refuse = true;
    output.sendKey(a, false);
    found.clear();
    output.drainDiagnostics(found);
    KGN_CHECK_EQ(countCode(found, "output.send_failed"), 1);
    KGN_CHECK(!found.empty() && found[0].level == DiagLevel::Error);
}

KGN_TEST(motion_and_scroll_carry_no_obligation_and_record_nothing) {
    // A lost pointer step is a stutter, not a stranded button, so a refused
    // motion must not leave anything for releaseAll() to lift.
    ControllableOutput output;
    output.refuse = true;
    output.moveCursorBy(10, 10);
    output.scroll(0, 3);
    output.refuse = false;

    output.attempts = 0;
    output.releaseAll();
    KGN_CHECK_EQ(output.attempts, 0);
}

int main() { return kgn::test::runAll(); }

#else

int main() { return 0; }

#endif
