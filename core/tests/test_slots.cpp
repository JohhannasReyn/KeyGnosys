// Window slot stability.
//
// SPEC 6.5 makes this a correctness property rather than a nicety: the number
// keys are bound to slots, so a slot that moves is a binding that lies.

#include "kgn/slots.hpp"

#include "kgn_test.hpp"

#include <string>
#include <vector>

using namespace kgn;

namespace {

WindowInfo win(WindowId id) {
    WindowInfo info;
    info.id = id;
    info.title = "window " + std::to_string(id);
    return info;
}

}  // namespace

KGN_TEST(a_window_keeps_its_slot_for_as_long_as_it_exists) {
    SlotRegistry registry;
    registry.update({win(10), win(20), win(30)});
    KGN_CHECK_EQ(*registry.at(1), WindowId{10});
    KGN_CHECK_EQ(*registry.at(2), WindowId{20});
    KGN_CHECK_EQ(*registry.at(3), WindowId{30});

    // 20 closes, and the backend happens to report the survivors in a
    // different order. Neither may move: a map that changes under the user's
    // fingers cannot be learned.
    registry.update({win(30), win(10)});
    KGN_CHECK_EQ(*registry.at(1), WindowId{10});
    KGN_CHECK(!registry.at(2).has_value());
    KGN_CHECK_EQ(*registry.at(3), WindowId{30});
}

KGN_TEST(a_new_window_takes_the_lowest_free_index) {
    SlotRegistry registry;
    registry.update({win(10), win(20), win(30)});
    registry.update({win(10), win(30)});
    registry.update({win(10), win(30), win(40)});
    KGN_CHECK_EQ(*registry.at(2), WindowId{40});
    KGN_CHECK_EQ(*registry.at(1), WindowId{10});
    KGN_CHECK_EQ(*registry.at(3), WindowId{30});
}

KGN_TEST(only_nine_windows_carry_an_index) {
    SlotRegistry registry;
    std::vector<WindowInfo> windows;
    for (WindowId i = 1; i <= 12; ++i) windows.push_back(win(i));
    registry.update(windows);

    KGN_CHECK(registry.at(9).has_value());
    KGN_CHECK(!registry.at(10).has_value());
    KGN_CHECK_EQ(registry.occupied().size(), std::size_t{9});
    // The tenth window has no slot, and is reported without one rather than
    // displacing a slot the user has learned.
    KGN_CHECK(!registry.indexOf(WindowId{10}).has_value());
}

KGN_TEST(an_index_outside_the_range_is_refused_rather_than_wrapped) {
    SlotRegistry registry;
    registry.update({win(10)});
    KGN_CHECK(!registry.at(0).has_value());
    KGN_CHECK(!registry.at(-1).has_value());
    KGN_CHECK(!registry.at(10).has_value());
    KGN_CHECK(!registry.at(1000).has_value());
}

KGN_TEST(occupied_walks_in_slot_order_not_report_order) {
    // window.cycle walks this, so it must follow the numbers the user sees.
    SlotRegistry registry;
    registry.update({win(10), win(20), win(30)});
    registry.update({win(30), win(20), win(10)});

    const std::vector<WindowId> ids = registry.occupied();
    KGN_CHECK_EQ(ids.size(), std::size_t{3});
    KGN_CHECK_EQ(ids[0], WindowId{10});
    KGN_CHECK_EQ(ids[1], WindowId{20});
    KGN_CHECK_EQ(ids[2], WindowId{30});
}

KGN_TEST(a_reopened_window_id_is_treated_as_new) {
    SlotRegistry registry;
    registry.update({win(10), win(20)});
    registry.update({win(20)});
    KGN_CHECK(!registry.at(1).has_value());
    registry.update({win(20), win(10)});
    KGN_CHECK_EQ(*registry.at(1), WindowId{10});
}

KGN_TEST(an_empty_list_frees_every_slot) {
    SlotRegistry registry;
    registry.update({win(10), win(20)});
    registry.update({});
    KGN_CHECK(registry.occupied().empty());
    registry.update({win(99)});
    KGN_CHECK_EQ(*registry.at(1), WindowId{99});
}

int main() { return kgn::test::runAll(); }
