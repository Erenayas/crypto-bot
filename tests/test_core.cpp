// Dependency-free test runner. No gtest, no fetch, no build complexity --
// Week 1 should not be spent debugging CMake.
//
//   ./build/test_core   ->  exits 0 if everything passes

#include "depth_sync.hpp"
#include "order_book.hpp"
#include "price.hpp"

#include <cmath>
#include <cstdio>

using namespace hft;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            std::printf("  FAIL  %s:%d   %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) < (eps))

static Level lvl(const char* px, const char* qty) {
    return Level{*parse_fixed(px), *parse_fixed(qty)};
}

// ---------------------------------------------------------------- price

static void test_parse_fixed() {
    std::printf("test_parse_fixed\n");

    CHECK(parse_fixed("60199.90") == 6019990000000LL);
    CHECK(parse_fixed("0.001") == 100000LL);
    CHECK(parse_fixed("1") == kScale);
    CHECK(parse_fixed("0") == 0LL);
    CHECK(parse_fixed("-1.5") == -150000000LL);

    // Excess precision is truncated, not rejected -- the exchange occasionally
    // sends more digits than our scale holds.
    CHECK(parse_fixed("1.0000000012345") == kScale);

    // Malformed input must be rejected, never coerced.
    CHECK(parse_fixed("") == std::nullopt);
    CHECK(parse_fixed("abc") == std::nullopt);
    CHECK(parse_fixed("1.2.3") == std::nullopt);
    CHECK(parse_fixed("1.") == std::nullopt);
    CHECK(parse_fixed(".5") == std::nullopt);
    CHECK(parse_fixed("1e5") == std::nullopt);

    // The reason we do any of this: 0.1 + 0.2 != 0.3 in binary floating point,
    // but it is exact in fixed point.
    CHECK(*parse_fixed("0.1") + *parse_fixed("0.2") == *parse_fixed("0.3"));
    CHECK(0.1 + 0.2 != 0.3);  // ... and here is the bug we avoided
}

// ------------------------------------------------------------ order book

static void test_order_book() {
    std::printf("test_order_book\n");

    OrderBook b;
    CHECK(!b.best_bid().has_value());
    CHECK(!b.mid().has_value());

    b.apply_bid(*parse_fixed("99.99"), *parse_fixed("50"));
    b.apply_bid(*parse_fixed("99.98"), *parse_fixed("10"));
    b.apply_ask(*parse_fixed("100.01"), *parse_fixed("10"));
    b.apply_ask(*parse_fixed("100.02"), *parse_fixed("20"));

    CHECK(b.best_bid()->px == *parse_fixed("99.99"));   // highest bid
    CHECK(b.best_ask()->px == *parse_fixed("100.01"));  // lowest ask
    CHECK(b.bid_levels() == 2);
    CHECK(!b.crossed());

    CHECK_NEAR(*b.mid(), 100.00, 1e-9);

    // Lesson 0, question 4: 50 on the bid vs 10 on the ask. Heavy buying
    // interest, so fair value sits ABOVE the mid.
    //   (99.99*10 + 100.01*50) / 60 = 100.006666...
    CHECK_NEAR(*b.microprice(), 100.00666666, 1e-6);
    CHECK(*b.microprice() > *b.mid());

    // qty == 0 deletes the level.
    b.apply_bid(*parse_fixed("99.99"), 0);
    CHECK(b.best_bid()->px == *parse_fixed("99.98"));
    CHECK(b.bid_levels() == 1);

    // Deleting a level we never had is a legal no-op, not an error.
    b.apply_ask(*parse_fixed("12345.6"), 0);
    CHECK(b.ask_levels() == 2);

    // The corruption alarm.
    OrderBook c;
    c.apply_bid(*parse_fixed("101"), *parse_fixed("1"));
    c.apply_ask(*parse_fixed("100"), *parse_fixed("1"));
    CHECK(c.crossed());
}

// ------------------------------------------------------------- depth sync

static DepthSnapshot make_snapshot(std::int64_t id) {
    DepthSnapshot s;
    s.last_update_id = id;
    s.bids           = {lvl("99.99", "5"), lvl("99.98", "7")};
    s.asks           = {lvl("100.01", "5"), lvl("100.02", "7")};
    return s;
}

static void test_sync_happy_path() {
    std::printf("test_sync_happy_path\n");

    DepthSync s;
    CHECK(s.state() == DepthSync::State::NeedSnapshot);

    // Events arriving before the snapshot are buffered, not applied.
    DepthUpdate e1;
    e1.U = 99, e1.u = 101, e1.pu = 98;
    e1.bids = {lvl("99.99", "9")};
    CHECK(s.on_event(e1) == SyncAction::None);
    CHECK(!s.synced());

    // Snapshot arrives and the buffer drains through the same rules.
    CHECK(s.on_snapshot(make_snapshot(100)) == SyncAction::None);
    CHECK(s.synced());
    CHECK(s.last_update_id() == 101);
    // The buffered event was applied on top of the snapshot.
    CHECK(s.book().best_bid()->qty == *parse_fixed("9"));

    // Chained event: pu matches the previous u.
    DepthUpdate e2;
    e2.U = 102, e2.u = 105, e2.pu = 101;
    e2.asks = {lvl("100.01", "3")};
    CHECK(s.on_event(e2) == SyncAction::None);
    CHECK(s.last_update_id() == 105);
    CHECK(s.book().best_ask()->qty == *parse_fixed("3"));
    CHECK(!s.book().crossed());
    CHECK(s.resync_count() == 0);
}

static void test_sync_drops_stale_events() {
    std::printf("test_sync_drops_stale_events\n");

    DepthSync s;
    CHECK(s.on_snapshot(make_snapshot(100)) == SyncAction::None);
    CHECK(s.state() == DepthSync::State::AwaitingFirstEvent);

    // Entirely older than the snapshot -> discard, keep waiting. Applying this
    // would resurrect a price level the snapshot already superseded.
    DepthUpdate old;
    old.U = 80, old.u = 90, old.pu = 79;
    old.bids = {lvl("99.99", "999")};
    CHECK(s.on_event(old) == SyncAction::None);
    CHECK(s.state() == DepthSync::State::AwaitingFirstEvent);
    CHECK(s.book().best_bid()->qty == *parse_fixed("5"));  // untouched

    // The event that straddles lastUpdateId is the one that bridges us.
    DepthUpdate bridge;
    bridge.U = 95, bridge.u = 103, bridge.pu = 94;
    CHECK(s.on_event(bridge) == SyncAction::None);
    CHECK(s.synced());
}

static void test_sync_detects_gap() {
    std::printf("test_sync_detects_gap\n");

    DepthSync s;
    CHECK(s.on_snapshot(make_snapshot(100)) == SyncAction::None);

    DepthUpdate first;
    first.U = 99, first.u = 101, first.pu = 98;
    CHECK(s.on_event(first) == SyncAction::None);
    CHECK(s.synced());

    // A message was dropped: pu (108) != our last u (101).
    DepthUpdate gapped;
    gapped.U = 109, gapped.u = 115, gapped.pu = 108;
    gapped.bids = {lvl("99.50", "1")};
    CHECK(s.on_event(gapped) == SyncAction::RequestSnapshot);

    // Critical: we must NOT keep quoting off a book we know is wrong.
    CHECK(!s.synced());
    CHECK(s.state() == DepthSync::State::NeedSnapshot);
    CHECK(!s.book().best_bid().has_value());
    CHECK(s.resync_count() == 1);

    // Recovery: a fresh snapshot re-syncs us, and the event we held is replayed.
    CHECK(s.on_snapshot(make_snapshot(114)) == SyncAction::None);
    CHECK(s.synced());
    CHECK(s.last_update_id() == 115);
}

static void test_sync_rejects_stale_snapshot() {
    std::printf("test_sync_rejects_stale_snapshot\n");

    DepthSync s;
    CHECK(s.on_snapshot(make_snapshot(100)) == SyncAction::None);

    // U (200) > lastUpdateId (100): the bridging events never arrived, so this
    // snapshot can never be reconciled. Ask for a newer one.
    DepthUpdate too_new;
    too_new.U = 200, too_new.u = 210, too_new.pu = 199;
    CHECK(s.on_event(too_new) == SyncAction::RequestSnapshot);
    CHECK(s.state() == DepthSync::State::NeedSnapshot);
}

int main() {
    test_parse_fixed();
    test_order_book();
    test_sync_happy_path();
    test_sync_drops_stale_events();
    test_sync_detects_gap();
    test_sync_rejects_stale_snapshot();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
