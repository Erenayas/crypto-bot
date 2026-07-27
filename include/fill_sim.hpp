#pragma once

#include "order_book.hpp"
#include "price.hpp"
#include "trade.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace hft {

struct Fill {
    Side         side;
    Price        px;
    Qty          qty;
    std::int64_t ts;
};

// A queue-position fill model.
//
// The naive backtest says "price touched my level, therefore I filled". That is
// the single biggest reason market-making backtests look profitable and then
// lose money live. In reality you fill only after EVERYONE AHEAD OF YOU at that
// price fills first -- and the fills that reach deep into a queue are exactly
// the ones where price is genuinely moving, i.e. the toxic ones.
//
// So we track, per resting order, how much volume sits ahead of it:
//
//   place()    queue_ahead = the entire size currently at that price. Everyone
//              already there is in front of us.
//   on_trade() a trade at our price on our side eats the queue ahead first.
//              Only what is left over fills US.
//   on_book()  the level shrank without a trade => cancellations. We do NOT
//              credit those to our queue position (see below).
//
// The cancellation rule is the conservative half of the model and deserves its
// own justification. When a level shrinks with no trade to explain it, someone
// cancelled -- but the depth stream never says WHO. Assuming the cancels were
// ahead of us would advance our queue position for free and flatter every
// result. We assume they were behind us instead, and only clamp when arithmetic
// forces our hand (queue_ahead can never exceed the level's total size).
//
// This makes the model PESSIMISTIC: in reality some cancels really are ahead of
// you, so true fill rates are a little better than what we report. That is the
// right direction to be wrong in. An optimistic backtest is a bill you pay
// later, with real money.
class FillSimulator {
public:
    struct Order {
        Price px          = 0;
        Qty   qty         = 0;  // remaining
        Qty   queue_ahead = 0;
        bool  active      = false;
    };

    // Places (or replaces) our single resting order on `side`.
    //
    // Replacing at a DIFFERENT price throws away queue position: the new order
    // goes to the back of the new level. That cost is real, it is why a market
    // maker cannot requote on every tick, and it falls out of this model for
    // free rather than having to be bolted on.
    void place(Side side, Price px, Qty qty, const OrderBook& book) {
        Order& o = order(side);
        if (o.active && o.px == px) {
            o.qty = qty;  // same price: keep our place in the queue
            return;
        }
        o.px          = px;
        o.qty         = qty;
        o.queue_ahead = book.size_at(side, px);
        o.active      = true;
        ++placements_;
    }

    void cancel(Side side) { order(side).active = false; }

    void cancel_all() {
        bid_.active = false;
        ask_.active = false;
    }

    const Order& bid() const { return bid_; }
    const Order& ask() const { return ask_; }
    std::uint64_t placements() const { return placements_; }

    // Cancellations only ever clamp our queue position; they never improve it.
    void on_book(const OrderBook& book) {
        clamp(bid_, Side::Buy, book);
        clamp(ask_, Side::Sell, book);
    }

    // Returns whatever this trade filled of ours. Append-only into `out` so the
    // caller controls the allocation.
    void on_trade(const Trade& t, std::int64_t ts, std::vector<Fill>& out) {
        // t.hit_bid() means a resting BID was consumed, so it can only touch our
        // buy order. The trade stream is the only thing that tells us this --
        // the depth stream alone cannot distinguish a trade from a cancel.
        apply_trade(t.hit_bid() ? bid_ : ask_, t.hit_bid() ? Side::Buy : Side::Sell, t, ts,
                    out);
    }

private:
    Order& order(Side s) { return s == Side::Buy ? bid_ : ask_; }

    static void clamp(Order& o, Side side, const OrderBook& book) {
        if (!o.active) return;
        o.queue_ahead = std::min(o.queue_ahead, book.size_at(side, o.px));
    }

    static void apply_trade(Order& o, Side side, const Trade& t, std::int64_t ts,
                            std::vector<Fill>& out) {
        if (!o.active) return;

        // Trades strictly past our price mean the book swept THROUGH our level.
        // That cannot happen without everything resting there -- us included --
        // being taken out first, so we are fully filled.
        const bool swept = side == Side::Buy ? t.px < o.px : t.px > o.px;
        if (swept) {
            out.push_back(Fill{side, o.px, o.qty, ts});
            o.qty    = 0;
            o.active = false;
            return;
        }

        if (t.px != o.px) return;  // trade at a price that is not ours: irrelevant

        Qty       remaining = t.qty;
        const Qty from_queue = std::min(o.queue_ahead, remaining);
        o.queue_ahead -= from_queue;
        remaining -= from_queue;
        if (remaining <= 0) return;  // the queue ahead absorbed all of it

        const Qty filled = std::min(remaining, o.qty);
        o.qty -= filled;
        out.push_back(Fill{side, o.px, filled, ts});
        if (o.qty == 0) o.active = false;
    }

    Order         bid_;
    Order         ask_;
    std::uint64_t placements_ = 0;
};

}  // namespace hft
