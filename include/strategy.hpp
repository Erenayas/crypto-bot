#pragma once

#include "order_book.hpp"
#include "price.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hft {

inline Price floor_to_tick(double px, Price tick) {
    const double fixed = px * static_cast<double>(kScale);
    return static_cast<Price>(std::floor(fixed / static_cast<double>(tick))) * tick;
}

inline Price ceil_to_tick(double px, Price tick) {
    const double fixed = px * static_cast<double>(kScale);
    return static_cast<Price>(std::ceil(fixed / static_cast<double>(tick))) * tick;
}

// Exponentially weighted volatility of the mid, in price units per update.
//
// EWMA rather than a rolling window because it needs no history buffer and
// adapts immediately when the market changes regime -- which is exactly when a
// market maker needs to widen.
class VolEstimator {
public:
    explicit VolEstimator(double alpha) : alpha_(alpha) {}

    void update(double mid) {
        if (last_ > 0.0) {
            const double r = mid - last_;
            var_ = (1.0 - alpha_) * var_ + alpha_ * r * r;
            ++n_;
        }
        last_ = mid;
    }

    double sigma() const { return std::sqrt(var_); }
    bool   ready() const { return n_ > 200; }

private:
    double        alpha_;
    double        var_  = 0.0;
    double        last_ = 0.0;
    std::uint64_t n_    = 0;
};

struct Quote {
    bool  bid    = false;
    bool  ask    = false;
    Price bid_px = 0;
    Price ask_px = 0;
};

struct MMParams {
    Qty    size            = 0;
    Price  tick            = 0;
    Qty    max_position    = 0;
    double base_half_ticks = 1.0;   // half-spread floor, in ticks
    double vol_coeff       = 0.0;   // widen proportionally to volatility
    double gamma           = 0.0;   // inventory risk aversion; 0 = no skew
    bool   use_microprice  = true;  // false => plain mid (the naive baseline)

    // Minimum half-spread as basis points of price. Set this to (round-trip fee
    // / 2) and every filled round trip covers its own fees by construction.
    double min_edge_bp     = 0.0;

    // Never quote the exit side of an open position below its cost basis plus
    // the round-trip fee. See the warning in quote().
    bool   breakeven_exit  = false;
    double fee_bp          = 2.0;   // maker fee PER SIDE, for the breakeven line

    // Averaging down. >1 lets the position grow to (max_position * this) while
    // it is UNDERWATER, and switches off the inventory skew so we keep quoting
    // the side that adds to it -- "buy more cheaply to improve the average".
    // 1.0 disables it. See the warning in quote(); it is off for a reason.
    double avg_down_mult   = 1.0;
};

// The quoting strategy.
//
// This is Avellaneda-Stoikov reduced to the two ideas that actually matter,
// with the parameters made dimensionless so they can be tuned:
//
//   1. RESERVATION PRICE. Do not quote around fair value -- quote around fair
//      value shifted AGAINST your inventory:
//
//          r = fair - q_norm * gamma * tick
//
//      Long inventory pushes r down, so both quotes move down, making us more
//      likely to sell and less likely to buy. We give up a little expected
//      spread to pull back toward flat. A-S writes this as q*gamma*sigma^2,
//      where the units cancel against gamma's own; ours is in TICKS, for the
//      reason documented at the call site.
//
//   2. SPREAD WIDENS WITH VOLATILITY. A-S's optimal spread grows with
//      volatility. Intuitively: volatility is when you get run over, so that is
//      when you demand more compensation.
//
// Setting gamma = 0 and use_microprice = false gives the naive symmetric
// market maker -- deliberately, so the two can be compared on identical data.
class MarketMaker {
public:
    explicit MarketMaker(const MMParams& p) : p_(p) {}

    // avg_entry is the cost basis of the current inventory (0 when flat).
    Quote quote(const OrderBook& book, Qty position, double sigma,
                double avg_entry = 0.0) const {
        Quote      q;
        const auto bb = book.best_bid();
        const auto ba = book.best_ask();
        if (!bb || !ba) return q;

        const auto fair_opt = p_.use_microprice ? book.microprice() : book.mid();
        if (!fair_opt) return q;
        const double fair = *fair_opt;

        const double tick_px = to_double(p_.tick);

        // AVERAGING DOWN.
        //
        // The idea: the position is losing, we still have balance, so add more
        // at a better price and improve the average entry.
        //
        // What it actually does: increases exposure precisely when the market
        // is disagreeing with the position, and switches OFF the one mechanism
        // (inventory skew) whose entire job is to stop that happening. The
        // "improved average" is an accounting illusion -- the average moved
        // because the SIZE grew, and the total money at risk grew with it.
        //
        // It converts a small, certain, closable loss into a small probability
        // of a very large one. Most days it looks like it works, which is what
        // makes it dangerous. Measured in docs/04.
        Qty    max_pos = p_.max_position;
        double gamma   = p_.gamma;
        if (p_.avg_down_mult > 1.0 && position != 0 && avg_entry > 0.0) {
            const bool underwater = position > 0 ? (fair < avg_entry) : (fair > avg_entry);
            if (underwater) {
                max_pos = static_cast<Qty>(static_cast<double>(p_.max_position) * p_.avg_down_mult);
                gamma   = 0.0;  // stop skewing away from the losing position
            }
        }

        const double q_norm = static_cast<double>(position) / static_cast<double>(max_pos);

        // Inventory skew, expressed in TICKS.
        //
        // A-S writes this as q*gamma*sigma^2, where the units cancel against
        // gamma's own units. Making gamma dimensionless and normalising q (as
        // we do, so the parameter is tunable) breaks that cancellation: sigma is
        // in price units, so sigma^2 on a 65,000-priced asset is ~133 -- and the
        // skew shoves quotes hundreds of dollars through the opposite touch.
        //
        // Scaling by the TICK instead keeps gamma meaningful and independent of
        // the instrument's price scale: gamma is "how many ticks to shift the
        // quotes when inventory is at its limit".
        const double reservation = fair - q_norm * gamma * tick_px;

        double half = (p_.base_half_ticks * tick_px) + (p_.vol_coeff * sigma);

        // A round trip pays the maker fee twice. Quoting closer than that to
        // fair value means every completed round trip loses money on
        // arithmetic alone, no matter how good the timing is.
        if (p_.min_edge_bp > 0.0) {
            half = std::max(half, fair * p_.min_edge_bp / 10000.0);
        }

        Price bid_px = floor_to_tick(reservation - half, p_.tick);
        Price ask_px = ceil_to_tick(reservation + half, p_.tick);

        // We are a MAKER. A quote that crosses the book would execute as a
        // taker: worse fee, no spread captured, and none of the queue dynamics
        // this strategy is built on. Cap at one tick inside the opposite touch.
        bid_px = std::min(bid_px, ba->px - p_.tick);
        ask_px = std::max(ask_px, bb->px + p_.tick);

        // Never close an open position at a loss: hold the exit quote at or
        // above (below) the cost basis plus the round-trip fee.
        //
        // WARNING, and it is the important part: this does not make losses go
        // away, it converts them into INVENTORY. If the market moves against
        // the position, the exit quote simply never fills and we carry the
        // position indefinitely -- turning a small, closable loss into an open
        // one with no bound. "Never take a loss" is how small losses become
        // large ones. Measured in docs/04.
        if (p_.breakeven_exit && position != 0 && avg_entry > 0.0) {
            const double round_trip = avg_entry * (p_.fee_bp * 2.0) / 10000.0;
            if (position > 0) {
                ask_px = std::max(ask_px, ceil_to_tick(avg_entry + round_trip, p_.tick));
            } else {
                bid_px = std::min(bid_px, floor_to_tick(avg_entry - round_trip, p_.tick));
            }
        }

        // Hard inventory limits. Beyond them we quote only the side that
        // reduces the position -- the skew above is a preference, this is a
        // constraint, and a market maker needs both.
        q.bid    = position < max_pos;
        q.ask    = position > -max_pos;
        q.bid_px = bid_px;
        q.ask_px = ask_px;
        return q;
    }

    const MMParams& params() const { return p_; }

private:
    MMParams p_;
};

}  // namespace hft
