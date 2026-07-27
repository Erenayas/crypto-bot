#pragma once

#include "order_book.hpp"
#include "price.hpp"
#include "trade.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace hft {

struct Fill {
    Side         side;
    Price        px;
    Qty          qty;
    std::int64_t ts;
};

// A queue-position fill model, for an arbitrary ladder of resting orders.
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
    };

    // Replaces the whole ladder on one side with `desired`.
    //
    // Orders already resting at a wanted price are KEPT, queue position and
    // all. Everything else is cancelled, and new prices join at the back of
    // their level. That diff is the entire reason this function exists rather
    // than a cancel-all-then-place: re-placing an order at a price you already
    // hold would silently throw away the queue position you spent time earning,
    // which is the most valuable thing a market maker owns.
    void replace(Side side, const std::vector<Level>& desired, const OrderBook& book) {
        auto& live = side == Side::Buy ? bids_ : asks_;

        std::vector<Order> next;
        next.reserve(desired.size());

        for (const auto& want : desired) {
            const auto it = std::find_if(live.begin(), live.end(),
                                         [&](const Order& o) { return o.px == want.px; });
            if (it != live.end()) {
                Order kept = *it;
                kept.qty   = want.qty;  // resizing does not lose our place
                next.push_back(kept);
            } else {
                next.push_back(Order{want.px, want.qty, book.size_at(side, want.px)});
                ++placements_;
            }
        }
        live.swap(next);
    }

    // Single-level convenience. A one-order-per-side market maker is just a
    // ladder of length one, so these are wrappers rather than a second
    // implementation -- there is only one set of queue rules to get right.
    void place(Side side, Price px, Qty qty, const OrderBook& book) {
        replace(side, std::vector<Level>{Level{px, qty}}, book);
    }

    void cancel(Side side) { cancel_side(side); }

    struct OrderView {
        Price px          = 0;
        Qty   qty         = 0;
        Qty   queue_ahead = 0;
        bool  active      = false;
    };

    OrderView bid() const { return view(bids_); }
    OrderView ask() const { return view(asks_); }

    void cancel_all() {
        bids_.clear();
        asks_.clear();
    }

    void cancel_side(Side side) { (side == Side::Buy ? bids_ : asks_).clear(); }

    const std::vector<Order>& orders(Side side) const { return side == Side::Buy ? bids_ : asks_; }
    std::size_t   resting(Side side) const { return orders(side).size(); }
    std::uint64_t placements() const { return placements_; }

    // Cancellations only ever clamp our queue position; they never improve it.
    void on_book(const OrderBook& book) {
        clamp(bids_, Side::Buy, book);
        clamp(asks_, Side::Sell, book);
    }

    // Appends whatever this trade filled of ours. The caller owns the vector so
    // the hot path does not allocate.
    void on_trade(const Trade& t, std::int64_t ts, std::vector<Fill>& out) {
        // t.hit_bid() means a resting BID was consumed, so it can only touch our
        // buy orders. The trade stream is the only thing that tells us this --
        // the depth stream alone cannot distinguish a trade from a cancel.
        const Side side = t.hit_bid() ? Side::Buy : Side::Sell;
        auto&      live = t.hit_bid() ? bids_ : asks_;

        for (auto& o : live) apply_trade(o, side, t, ts, out);

        live.erase(std::remove_if(live.begin(), live.end(),
                                  [](const Order& o) { return o.qty <= 0; }),
                   live.end());
    }

private:
    static OrderView view(const std::vector<Order>& live) {
        if (live.empty()) return OrderView{};
        return OrderView{live.front().px, live.front().qty, live.front().queue_ahead, true};
    }

    static void clamp(std::vector<Order>& live, Side side, const OrderBook& book) {
        for (auto& o : live) o.queue_ahead = std::min(o.queue_ahead, book.size_at(side, o.px));
    }

    static void apply_trade(Order& o, Side side, const Trade& t, std::int64_t ts,
                            std::vector<Fill>& out) {
        if (o.qty <= 0) return;

        // Trades strictly past our price mean the book swept THROUGH our level.
        // That cannot happen without everything resting there -- us included --
        // being taken out first, so we are fully filled.
        const bool swept = side == Side::Buy ? t.px < o.px : t.px > o.px;
        if (swept) {
            out.push_back(Fill{side, o.px, o.qty, ts});
            o.qty = 0;
            return;
        }

        if (t.px != o.px) return;  // a trade at a price that is not ours

        Qty       remaining  = t.qty;
        const Qty from_queue = std::min(o.queue_ahead, remaining);
        o.queue_ahead -= from_queue;
        remaining -= from_queue;
        if (remaining <= 0) return;  // the queue ahead absorbed all of it

        const Qty filled = std::min(remaining, o.qty);
        o.qty -= filled;
        out.push_back(Fill{side, o.px, filled, ts});
    }

    std::vector<Order> bids_;
    std::vector<Order> asks_;
    std::uint64_t      placements_ = 0;
};

}  // namespace hft
