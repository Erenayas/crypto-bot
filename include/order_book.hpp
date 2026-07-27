#pragma once

#include "price.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>

namespace hft {

struct Level {
    Price px;
    Qty   qty;
};

// An L2 (aggregated-by-price) order book.
//
// Storage choice: std::map, deliberately.
//   Pro: ordered, so begin() is always the best price. Obviously correct.
//   Con: node-based, so every level is a separate heap allocation and every
//        traversal is a cache miss. This is NOT what a production book uses.
//
// We start here because in Week 1 the enemy is *correctness*, not latency, and
// the network round trip dwarfs anything happening in this class. In Week 4 we
// measure, and if it matters we swap the internals for a flat array indexed by
// tick. The public interface below is designed so that swap changes nothing
// outside this file.
class OrderBook {
public:
    // Binance sends the ABSOLUTE quantity at a price level, not a delta.
    // qty == 0 means "this level is gone".
    void apply_bid(Price px, Qty qty) { apply(bids_, px, qty); }
    void apply_ask(Price px, Qty qty) { apply(asks_, px, qty); }

    void clear() {
        bids_.clear();
        asks_.clear();
    }

    std::optional<Level> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return Level{bids_.begin()->first, bids_.begin()->second};
    }

    std::optional<Level> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return Level{asks_.begin()->first, asks_.begin()->second};
    }

    // (bid + ask) / 2. Naive: ignores how much size sits on each side.
    std::optional<double> mid() const {
        const auto b = best_bid();
        const auto a = best_ask();
        if (!b || !a) return std::nullopt;
        return (to_double(b->px) + to_double(a->px)) / 2.0;
    }

    // Size-weighted fair value. Note the CROSSED weighting: bid price is
    // weighted by ASK size and vice versa, so a heavy bid pulls fair value UP.
    // This is our first alpha signal -- see docs/00-market-microstructure.md.
    std::optional<double> microprice() const {
        const auto b = best_bid();
        const auto a = best_ask();
        if (!b || !a) return std::nullopt;
        const double total = static_cast<double>(b->qty) + static_cast<double>(a->qty);
        if (total <= 0.0) return std::nullopt;
        return (to_double(b->px) * static_cast<double>(a->qty) +
                to_double(a->px) * static_cast<double>(b->qty)) /
               total;
    }

    std::optional<Price> spread() const {
        const auto b = best_bid();
        const auto a = best_ask();
        if (!b || !a) return std::nullopt;
        return a->px - b->px;
    }

    // An invariant, not a market condition. On a sane venue the best bid is
    // always strictly below the best ask. If this ever returns true, OUR BOOK
    // IS CORRUPT -- we dropped or misapplied an update. Cheap to check, and it
    // catches the exact class of bug that otherwise stays silent for hours.
    bool crossed() const {
        const auto b = best_bid();
        const auto a = best_ask();
        return b && a && b->px >= a->px;
    }

    std::size_t bid_levels() const { return bids_.size(); }
    std::size_t ask_levels() const { return asks_.size(); }

    // Ordered best-first iteration, for printing and for depth-based signals.
    const std::map<Price, Qty, std::greater<Price>>& bids() const { return bids_; }
    const std::map<Price, Qty, std::less<Price>>&    asks() const { return asks_; }

private:
    template <class Side>
    static void apply(Side& side, Price px, Qty qty) {
        if (qty == 0) {
            side.erase(px);  // erasing an absent key is a no-op, which is correct:
                             // the exchange may delete a level we never had.
        } else {
            side.insert_or_assign(px, qty);
        }
    }

    std::map<Price, Qty, std::greater<Price>> bids_;  // descending: begin() = highest
    std::map<Price, Qty, std::less<Price>>    asks_;  // ascending:  begin() = lowest
};

}  // namespace hft
