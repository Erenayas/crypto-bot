#pragma once

#include "order_book.hpp"
#include "price.hpp"

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
};

// The quoting strategy.
//
// This is Avellaneda-Stoikov reduced to the two ideas that actually matter,
// with the parameters made dimensionless so they can be tuned:
//
//   1. RESERVATION PRICE. Do not quote around fair value -- quote around fair
//      value shifted AGAINST your inventory:
//
//          r = fair - q * gamma * sigma^2
//
//      Long inventory pushes r down, so both quotes move down, making us more
//      likely to sell and less likely to buy. We give up a little expected
//      spread to pull back toward flat. A-S derives this as the optimal
//      trade-off; we normalise q by max_position so gamma is unitless.
//
//   2. SPREAD WIDENS WITH VOLATILITY. A-S's optimal spread grows with
//      gamma*sigma^2. Intuitively: volatility is when you get run over, so
//      that is when you demand more compensation.
//
// Setting gamma = 0 and use_microprice = false gives the naive symmetric
// market maker -- deliberately, so the two can be compared on identical data.
class MarketMaker {
public:
    explicit MarketMaker(const MMParams& p) : p_(p) {}

    Quote quote(const OrderBook& book, Qty position, double sigma) const {
        Quote      q;
        const auto bb = book.best_bid();
        const auto ba = book.best_ask();
        if (!bb || !ba) return q;

        const auto fair_opt = p_.use_microprice ? book.microprice() : book.mid();
        if (!fair_opt) return q;
        const double fair = *fair_opt;

        const double tick_px = to_double(p_.tick);
        const double q_norm =
            static_cast<double>(position) / static_cast<double>(p_.max_position);

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
        const double reservation = fair - q_norm * p_.gamma * tick_px;

        const double half = (p_.base_half_ticks * tick_px) + (p_.vol_coeff * sigma);

        Price bid_px = floor_to_tick(reservation - half, p_.tick);
        Price ask_px = ceil_to_tick(reservation + half, p_.tick);

        // We are a MAKER. A quote that crosses the book would execute as a
        // taker: worse fee, no spread captured, and none of the queue dynamics
        // this strategy is built on. Cap at one tick inside the opposite touch.
        bid_px = std::min(bid_px, ba->px - p_.tick);
        ask_px = std::max(ask_px, bb->px + p_.tick);

        // Hard inventory limits. Beyond them we quote only the side that
        // reduces the position -- the skew above is a preference, this is a
        // constraint, and a market maker needs both.
        q.bid    = position < p_.max_position;
        q.ask    = position > -p_.max_position;
        q.bid_px = bid_px;
        q.ask_px = ask_px;
        return q;
    }

    const MMParams& params() const { return p_; }

private:
    MMParams p_;
};

}  // namespace hft
