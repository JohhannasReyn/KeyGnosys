// The hook thread's message-loop decisions.
//
// These exist because live M3 validation found a hook that installed cleanly,
// reported healthy, and never received a callback -- and the whole automated
// suite was green throughout. Nothing had pinned the pump's behaviour. These
// tests pin the part of it that can be reasoned about without a desktop.

#include "kgn/hookpump.hpp"

#include "kgn_test.hpp"

#include <atomic>
#include <deque>
#include <thread>
#include <vector>

using namespace kgn;

namespace {

// ---------------------------------------------------------------------------
// Doorbell

KGN_TEST(doorbell_first_arm_owns_the_post) {
    Doorbell bell;
    KGN_CHECK(bell.arm());
    KGN_CHECK(bell.pending());
}

KGN_TEST(doorbell_coalesces_until_consumed) {
    Doorbell bell;
    KGN_CHECK(bell.arm());
    // A burst of controls must not become a burst of thread messages: the
    // Windows queue has a 10 000-message cap we are not entitled to spend.
    for (int i = 0; i < 10000; ++i) KGN_CHECK(!bell.arm());
    bell.disarm();
    KGN_CHECK(bell.arm());   // a new episode gets a new post
}

KGN_TEST(doorbell_disarm_reopens_posting) {
    Doorbell bell;
    KGN_CHECK(bell.arm());
    KGN_CHECK(!bell.arm());
    bell.disarm();
    KGN_CHECK(!bell.pending());
    KGN_CHECK(bell.arm());
}

// The interleaving that the disarm-before-drain rule exists to survive.
//
// Modelled rather than threaded so it is deterministic: the producer pushes at
// the single most dangerous instant -- after the consumer has decided the ring
// is empty, but while the consumer's episode is still open.
KGN_TEST(doorbell_does_not_strand_work_pushed_during_a_drain) {
    Doorbell bell;
    std::deque<int> ring;
    int posts = 0;

    auto produce = [&](int item) {
        ring.push_back(item);
        if (bell.arm()) ++posts;
    };

    produce(1);
    KGN_CHECK_EQ(posts, 1);

    // Consumer wakes. CORRECT ORDER: disarm first, then drain.
    bell.disarm();
    std::vector<int> drained;
    while (!ring.empty()) {
        drained.push_back(ring.front());
        ring.pop_front();
        // The producer slips in here, after the last pop, while the consumer
        // still believes it is inside its drain.
        if (drained.size() == 1) produce(2);
    }

    // Item 2 arrived after the flag was cleared, so it got its own post and a
    // second wake is guaranteed. Nothing is stranded.
    KGN_CHECK_EQ(posts, 2);
    KGN_CHECK_EQ(drained.size(), static_cast<std::size_t>(2));
}

KGN_TEST(doorbell_drain_then_disarm_would_strand_work) {
    // The bug the ordering rule forbids, asserted so nobody "simplifies" the
    // consumer back into it. This models the WRONG order deliberately.
    Doorbell bell;
    std::deque<int> ring;
    int posts = 0;

    auto produce = [&](int item) {
        ring.push_back(item);
        if (bell.arm()) ++posts;
    };

    produce(1);
    KGN_CHECK_EQ(posts, 1);

    while (!ring.empty()) ring.pop_front();   // drain to empty FIRST
    produce(2);                               // flag still set -> no post
    bell.disarm();                            // ... and only now cleared

    KGN_CHECK_EQ(posts, 1);          // no second wake was ever sent
    KGN_CHECK_EQ(ring.size(), static_cast<std::size_t>(1));   // item 2 stranded
}

KGN_TEST(doorbell_under_concurrency_never_loses_the_last_item) {
    // Real threads, many episodes. The invariant checked is the one that
    // matters: when the producer stops, either the ring is empty or a post is
    // outstanding to come and get it. A silent stall fails this.
    for (int attempt = 0; attempt < 200; ++attempt) {
        Doorbell bell;
        std::atomic<int> produced{0};
        std::atomic<int> consumed{0};
        std::atomic<int> depth{0};
        std::atomic<bool> done{false};

        std::thread producer([&] {
            for (int i = 0; i < 500; ++i) {
                depth.fetch_add(1, std::memory_order_release);
                produced.fetch_add(1, std::memory_order_relaxed);
                if (bell.arm()) { /* would post */ }
                std::this_thread::yield();
            }
            done.store(true, std::memory_order_release);
        });

        std::thread consumer([&] {
            while (!done.load(std::memory_order_acquire) || bell.pending() ||
                   depth.load(std::memory_order_acquire) > 0) {
                if (!bell.pending()) { std::this_thread::yield(); continue; }
                bell.disarm();                       // BEFORE draining
                int n = depth.exchange(0, std::memory_order_acq_rel);
                consumed.fetch_add(n, std::memory_order_relaxed);
            }
        });

        producer.join();
        consumer.join();
        KGN_CHECK_EQ(consumed.load(), produced.load());
    }
}

}  // namespace

int main() { return kgn::test::runAll(); }
