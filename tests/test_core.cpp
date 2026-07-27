// Dependency-free test runner. No gtest, no fetch, no build complexity --
// Week 1 should not be spent debugging CMake.
//
//   ./build/test_core   ->  exits 0 if everything passes

#include "depth_sync.hpp"
#include "fill_sim.hpp"
#include "portfolio.hpp"
#include "strategy.hpp"
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

// ------------------------------------------------------------- fill model

static OrderBook two_sided_book() {
    OrderBook b;
    b.apply_bid(*parse_fixed("100.00"), *parse_fixed("10"));
    b.apply_ask(*parse_fixed("100.10"), *parse_fixed("10"));
    return b;
}

static Trade trade_at(const char* px, const char* qty, bool hit_bid) {
    Trade t;
    t.px             = *parse_fixed(px);
    t.qty            = *parse_fixed(qty);
    t.buyer_is_maker = hit_bid;
    return t;
}

static void test_queue_position() {
    std::printf("test_queue_position\n");

    OrderBook     book = two_sided_book();
    FillSimulator sim;
    std::vector<Fill> fills;

    // Joining a level with 10 resting means 10 units are ahead of us.
    sim.place(Side::Buy, *parse_fixed("100.00"), *parse_fixed("1"), book);
    CHECK(sim.bid().queue_ahead == *parse_fixed("10"));

    // A 4-unit sell into the bid eats queue, not us. This is the whole point:
    // price "touched" our level and we did NOT fill.
    sim.on_trade(trade_at("100.00", "4", true), 1, fills);
    CHECK(fills.empty());
    CHECK(sim.bid().queue_ahead == *parse_fixed("6"));

    // Still not enough.
    sim.on_trade(trade_at("100.00", "6", true), 2, fills);
    CHECK(fills.empty());
    CHECK(sim.bid().queue_ahead == 0);

    // Now the queue is gone, so the next trade reaches us.
    sim.on_trade(trade_at("100.00", "0.4", true), 3, fills);
    CHECK(fills.size() == 1);
    if (fills.size() == 1) {
        CHECK(fills[0].side == Side::Buy);
        CHECK(fills[0].qty == *parse_fixed("0.4"));
        CHECK(fills[0].ts == 3);
    }
    CHECK(sim.bid().qty == *parse_fixed("0.6"));
    CHECK(sim.bid().active);
}

static void test_trades_on_the_other_side_are_ignored() {
    std::printf("test_trades_on_the_other_side_are_ignored\n");

    OrderBook     book = two_sided_book();
    FillSimulator sim;
    std::vector<Fill> fills;

    sim.place(Side::Buy, *parse_fixed("100.00"), *parse_fixed("1"), book);

    // An aggressive BUY lifts asks. It cannot touch our resting bid, no matter
    // how large. Getting this backwards would invent fills out of nothing.
    sim.on_trade(trade_at("100.00", "999", false), 1, fills);
    CHECK(fills.empty());
    CHECK(sim.bid().queue_ahead == *parse_fixed("10"));

    // A trade at a price that is not ours is equally irrelevant.
    sim.on_trade(trade_at("99.99", "999", true), 2, fills);
    // ...unless it swept PAST us, which the next test covers.
    CHECK(fills.size() == 1);
}

static void test_sweep_fills_us() {
    std::printf("test_sweep_fills_us\n");

    OrderBook     book = two_sided_book();
    FillSimulator sim;
    std::vector<Fill> fills;

    sim.place(Side::Buy, *parse_fixed("100.00"), *parse_fixed("2"), book);

    // A sell printing BELOW our bid means the book was swept through our level.
    // Everything resting at 100.00 -- us included -- must already be gone.
    sim.on_trade(trade_at("99.95", "5", true), 7, fills);
    CHECK(fills.size() == 1);
    if (fills.size() == 1) {
        CHECK(fills[0].qty == *parse_fixed("2"));
        CHECK(fills[0].px == *parse_fixed("100.00"));
    }
    CHECK(!sim.bid().active);
}

static void test_cancellations_never_improve_queue() {
    std::printf("test_cancellations_never_improve_queue\n");

    OrderBook     book = two_sided_book();
    FillSimulator sim;

    sim.place(Side::Buy, *parse_fixed("100.00"), *parse_fixed("1"), book);
    CHECK(sim.bid().queue_ahead == *parse_fixed("10"));

    // Level shrinks 10 -> 8 with no trade: two units were CANCELLED. We refuse
    // to credit that to our queue position, because the feed never says whose
    // orders they were. Assuming they were ahead of us would flatter every
    // backtest we ever run.
    book.apply_bid(*parse_fixed("100.00"), *parse_fixed("8"));
    sim.on_book(book);
    CHECK(sim.bid().queue_ahead == *parse_fixed("8"));  // clamped, not credited

    // Clamping is forced by arithmetic once the level is smaller than what we
    // believed was ahead of us -- it cannot exceed the level's total size.
    book.apply_bid(*parse_fixed("100.00"), *parse_fixed("3"));
    sim.on_book(book);
    CHECK(sim.bid().queue_ahead == *parse_fixed("3"));
}

static void test_requote_costs_queue_position() {
    std::printf("test_requote_costs_queue_position\n");

    OrderBook     book = two_sided_book();
    FillSimulator sim;
    std::vector<Fill> fills;

    sim.place(Side::Buy, *parse_fixed("100.00"), *parse_fixed("1"), book);
    sim.on_trade(trade_at("100.00", "9", true), 1, fills);
    CHECK(sim.bid().queue_ahead == *parse_fixed("1"));  // nearly at the front

    // Re-placing at the SAME price keeps our hard-won place in the queue.
    sim.place(Side::Buy, *parse_fixed("100.00"), *parse_fixed("1"), book);
    CHECK(sim.bid().queue_ahead == *parse_fixed("1"));
    CHECK(sim.placements() == 1);  // not counted as a new placement

    // Moving one tick sends us to the BACK of the new level. This is the cost
    // that makes requoting on every tick a losing strategy, and the reason the
    // backtester has a hysteresis threshold.
    book.apply_bid(*parse_fixed("99.90"), *parse_fixed("25"));
    sim.place(Side::Buy, *parse_fixed("99.90"), *parse_fixed("1"), book);
    CHECK(sim.bid().queue_ahead == *parse_fixed("25"));
    CHECK(sim.placements() == 2);
}

static void test_portfolio_accounting() {
    std::printf("test_portfolio_accounting\n");

    Portfolio pf;
    const double fee = 0.0002;  // 2 bp

    // Buy 1 @ 100, sell 1 @ 100.10 -- textbook spread capture.
    pf.on_fill(Fill{Side::Buy, *parse_fixed("100.00"), *parse_fixed("1"), 1}, fee);
    CHECK(pf.position == *parse_fixed("1"));
    CHECK_NEAR(pf.cash, -100.02, 1e-9);  // 100 paid + 0.02 fee

    pf.on_fill(Fill{Side::Sell, *parse_fixed("100.10"), *parse_fixed("1"), 2}, fee);
    CHECK(pf.position == 0);
    CHECK(pf.fills == 2);

    // Gross spread captured is 0.10; fees are 0.0200 + 0.0200 = 0.0400.
    CHECK_NEAR(pf.fees_paid, 0.04002, 1e-9);
    CHECK_NEAR(pf.equity(100.05), 0.10 - 0.04002, 1e-9);
    CHECK(pf.equity(100.05) > 0);  // at a 10-tick spread this still works...

    // ...but at a ONE tick spread it does not. This is Lesson 0's arithmetic,
    // and it is why a naive spread-capture market maker loses money.
    Portfolio thin;
    thin.on_fill(Fill{Side::Buy, *parse_fixed("100.00"), *parse_fixed("1"), 1}, fee);
    thin.on_fill(Fill{Side::Sell, *parse_fixed("100.01"), *parse_fixed("1"), 2}, fee);
    CHECK(thin.equity(100.005) < 0);
}

static void test_markout_detects_adverse_selection() {
    std::printf("test_markout_detects_adverse_selection\n");

    MarkoutTracker mo(1000);  // 1000 ns horizon, for the test

    // We bought at 100. A second later the mid is 99.50: we were picked off.
    mo.on_fill(Fill{Side::Buy, *parse_fixed("100.00"), *parse_fixed("2"), 0});
    mo.on_mark(500, 99.50);   // too early, nothing settles yet
    CHECK(mo.count() == 0);
    mo.on_mark(1000, 99.50);  // horizon reached
    CHECK(mo.count() == 1);
    CHECK_NEAR(mo.total(), -1.0, 1e-9);  // (99.50 - 100.00) * 2
    CHECK(mo.bps() < 0);                 // negative markout = toxic flow

    // A sell that the market then moves down for us is a GOOD fill.
    MarkoutTracker good(1000);
    good.on_fill(Fill{Side::Sell, *parse_fixed("100.00"), *parse_fixed("2"), 0});
    good.on_mark(1000, 99.50);
    CHECK_NEAR(good.total(), 1.0, 1e-9);
    CHECK(good.bps() > 0);
}

static void test_inventory_skew() {
    std::printf("test_inventory_skew\n");

    MMParams p;
    p.tick            = *parse_fixed("0.10");
    p.size            = *parse_fixed("0.002");
    p.max_position    = *parse_fixed("0.010");
    p.base_half_ticks = 2.0;
    p.gamma           = 0.0;
    p.use_microprice  = false;

    OrderBook book;
    book.apply_bid(*parse_fixed("100.00"), *parse_fixed("10"));
    book.apply_ask(*parse_fixed("100.10"), *parse_fixed("10"));

    // Flat, no skew: quotes straddle the mid symmetrically.
    const Quote flat = MarketMaker(p).quote(book, 0, 0.0);
    CHECK(flat.bid && flat.ask);
    const double mid = *book.mid();
    CHECK_NEAR((to_double(flat.bid_px) + to_double(flat.ask_px)) / 2.0, mid, 1e-6);

    // Long inventory with gamma > 0 must push BOTH quotes down, so we are more
    // likely to sell and less likely to buy.
    p.gamma = 5.0;
    const Quote lng = MarketMaker(p).quote(book, *parse_fixed("0.008"), 0.5);
    CHECK(lng.bid_px < flat.bid_px);
    CHECK(lng.ask_px <= flat.ask_px);

    // Short inventory skews the other way.
    const Quote sht = MarketMaker(p).quote(book, -*parse_fixed("0.008"), 0.5);
    CHECK(sht.bid_px >= flat.bid_px);
    CHECK(sht.ask_px > flat.ask_px);

    // At the hard limit we stop quoting the side that would make it worse.
    const Quote maxed = MarketMaker(p).quote(book, *parse_fixed("0.010"), 0.5);
    CHECK(!maxed.bid);
    CHECK(maxed.ask);
}

static void test_skew_is_scale_free() {
    std::printf("test_skew_is_scale_free\n");

    // REGRESSION TEST for a units bug that only appeared at real market scale.
    //
    // The skew was originally q*gamma*sigma^2 with sigma in absolute price
    // units. On BTCUSDT, sigma is around 11, so sigma^2 is ~133 -- the skew
    // pushed quotes 133 DOLLARS through the opposite touch and every order came
    // back rejected as a would-be taker.
    //
    // The unit test that "covered" this passed a toy sigma of 0.5, where
    // sigma^2 is 0.25 and the bug is invisible. A test with unrealistic inputs
    // is a test that certifies nothing.
    MMParams p;
    p.tick            = *parse_fixed("0.10");
    p.size            = *parse_fixed("0.002");
    p.max_position    = *parse_fixed("0.010");
    p.base_half_ticks = 1.0;
    p.gamma           = 5.0;
    p.use_microprice  = false;

    OrderBook book;  // realistic BTCUSDT prices, not 100.00
    book.apply_bid(*parse_fixed("65184.40"), *parse_fixed("10"));
    book.apply_ask(*parse_fixed("65184.50"), *parse_fixed("10"));

    const double kRealSigma = 11.5;  // measured on live BTCUSDT
    const double mid        = *book.mid();

    for (const Qty pos : {*parse_fixed("-0.010"), *parse_fixed("-0.002"), Qty{0},
                          *parse_fixed("0.002"), *parse_fixed("0.008")}) {
        const Quote q = MarketMaker(p).quote(book, pos, kRealSigma);

        // Whatever the inventory, quotes must stay within a few ticks of fair
        // value. gamma is a number of TICKS, so the bound is knowable.
        const double max_shift = (p.gamma + p.base_half_ticks + 1.0) * to_double(p.tick);
        if (q.bid) CHECK(std::fabs(to_double(q.bid_px) - mid) <= max_shift);
        if (q.ask) CHECK(std::fabs(to_double(q.ask_px) - mid) <= max_shift);

        // And they must never cross -- the condition the exchange rejects with
        // -5022 "could not be executed as maker".
        if (q.bid) CHECK(q.bid_px < book.best_ask()->px);
        if (q.ask) CHECK(q.ask_px > book.best_bid()->px);
    }
}

static void test_quotes_never_cross() {
    std::printf("test_quotes_never_cross\n");

    MMParams p;
    p.tick            = *parse_fixed("0.10");
    p.size            = *parse_fixed("0.002");
    p.max_position    = *parse_fixed("0.010");
    p.base_half_ticks = 0.0;  // absurdly tight: would cross without the clamp
    p.use_microprice  = false;

    OrderBook book;
    book.apply_bid(*parse_fixed("100.00"), *parse_fixed("10"));
    book.apply_ask(*parse_fixed("100.10"), *parse_fixed("10"));

    // A maker that crosses the book becomes a taker: worse fee, no spread, and
    // none of the queue dynamics the strategy is built on.
    const Quote q = MarketMaker(p).quote(book, 0, 0.0);
    CHECK(q.bid_px < *parse_fixed("100.10"));
    CHECK(q.ask_px > *parse_fixed("100.00"));
    CHECK(q.bid_px < q.ask_px);
}

int main() {
    test_parse_fixed();
    test_order_book();
    test_sync_happy_path();
    test_sync_drops_stale_events();
    test_sync_detects_gap();
    test_sync_rejects_stale_snapshot();
    test_queue_position();
    test_trades_on_the_other_side_are_ignored();
    test_sweep_fills_us();
    test_cancellations_never_improve_queue();
    test_requote_costs_queue_position();
    test_portfolio_accounting();
    test_markout_detects_adverse_selection();
    test_inventory_skew();
    test_skew_is_scale_free();
    test_quotes_never_cross();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
