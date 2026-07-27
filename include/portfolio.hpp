#pragma once

#include "fill_sim.hpp"
#include "price.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace hft {

// Position and P&L accounting.
//
// Quantities stay in fixed point; cash and P&L are doubles. That split is
// deliberate: position must be exact because it is compared against hard risk
// limits and reconciled against the exchange, whereas cash is a derived
// statistic we only ever read. See docs/01-order-book.md section 1.
struct Portfolio {
    Qty           position  = 0;    // signed, fixed point
    double        cash      = 0.0;  // quote currency
    double        fees_paid = 0.0;
    std::uint64_t fills     = 0;
    Qty           volume    = 0;
    Qty           max_long  = 0;
    Qty           max_short = 0;

    // Volume-weighted entry price of the CURRENT inventory, in quote currency.
    // Needed by any rule that reasons about whether closing would be profitable;
    // 0 when flat.
    double        avg_entry = 0.0;

    void on_fill(const Fill& f, double maker_fee_rate) {
        const double notional = to_double(f.px) * to_double(f.qty);
        const double px       = to_double(f.px);

        const Qty signed_qty = f.side == Side::Buy ? f.qty : -f.qty;
        const Qty new_pos    = position + signed_qty;

        // Maintain the cost basis of the inventory we are carrying.
        if (new_pos == 0) {
            avg_entry = 0.0;  // flat: there is no inventory to have a basis
        } else if (position == 0 || (position > 0) == (signed_qty > 0)) {
            // Opening or adding: blend the new fill into the average.
            const double held     = std::abs(to_double(position));
            const double added    = to_double(f.qty);
            avg_entry = (held * avg_entry + added * px) / (held + added);
        } else if ((new_pos > 0) != (position > 0)) {
            // Flipped through zero: the old basis is gone, this fill is the new one.
            avg_entry = px;
        }
        // Reducing without flipping leaves the basis unchanged, which is what
        // makes "am I closing at a profit?" a well-posed question.

        if (f.side == Side::Buy) {
            position += f.qty;
            cash -= notional;
        } else {
            position -= f.qty;
            cash += notional;
        }

        // Maker fee. On Binance USD-M futures this is roughly 2bp, and it is
        // the number that decides whether spread capture is profitable at all:
        // a 1-tick spread on BTCUSDT is worth about 0.15bp round trip.
        const double fee = notional * maker_fee_rate;
        cash -= fee;
        fees_paid += fee;

        ++fills;
        volume += f.qty;
        max_long  = std::max(max_long, position);
        max_short = std::min(max_short, position);
    }

    // Mark to market. Starting equity is 0, so this IS the net P&L.
    double equity(double mark) const { return cash + to_double(position) * mark; }
};

// Measures adverse selection directly.
//
// For each fill, compare the mid price `horizon` later against the price we
// traded at, signed by direction. If we bought at 100 and the mid is 100.05 a
// second later, that fill was good (+). If it is 99.95, we were picked off (-).
//
// This is the single most important diagnostic for a market maker. Spread
// capture is easy to measure and easy to fool yourself with; markout tells you
// whether the people trading against you knew something you did not.
//
// A market maker with positive spread capture and strongly negative markout is
// not profitable -- it is a slow-motion loss with good bookkeeping.
class MarkoutTracker {
public:
    explicit MarkoutTracker(std::int64_t horizon_ns) : horizon_(horizon_ns) {}

    void on_fill(const Fill& f) { pending_.push_back(f); }

    // Call with the current time and mid on every book update.
    void on_mark(std::int64_t ts, double mid) {
        while (!pending_.empty() && ts - pending_.front().ts >= horizon_) {
            const Fill&  f    = pending_.front();
            const double sign = f.side == Side::Buy ? 1.0 : -1.0;
            total_ += (mid - to_double(f.px)) * to_double(f.qty) * sign;
            notional_ += to_double(f.qty) * to_double(f.px);
            ++count_;
            pending_.pop_front();
        }
    }

    double        total() const { return total_; }
    std::uint64_t count() const { return count_; }
    // Normalised, so it is comparable across runs and against the fee rate.
    double bps() const { return notional_ > 0 ? total_ / notional_ * 10000.0 : 0.0; }

private:
    std::int64_t     horizon_;
    std::deque<Fill> pending_;
    double           total_    = 0.0;
    double           notional_ = 0.0;
    std::uint64_t    count_    = 0;
};

}  // namespace hft
