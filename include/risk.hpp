#pragma once

#include "price.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace hft {

// Every order passes through here. Nothing reaches the exchange without a
// verdict, and the default verdict is no.
//
// This is deliberately header-only and dependency-free so it can be unit tested
// exhaustively. The risk layer is the one component where a bug is not a bad
// backtest -- it is a liquidated account.
struct RiskConfig {
    Qty          max_position     = 0;      // absolute inventory cap
    Qty          max_order_size   = 0;
    std::int64_t max_orders_per_s = 20;     // stay under the venue's rate limit
    double       max_drawdown     = 1e9;    // quote currency, from peak equity
    std::int64_t max_book_age_ms  = 2000;   // refuse to quote on stale data
};

enum class RiskVerdict {
    Allow,
    RejectPosition,
    RejectSize,
    RejectRate,
    RejectDrawdown,
    RejectStaleBook,
    RejectHalted,
};

inline const char* to_string(RiskVerdict v) {
    switch (v) {
        case RiskVerdict::Allow:           return "allow";
        case RiskVerdict::RejectPosition:  return "position limit";
        case RiskVerdict::RejectSize:      return "order size";
        case RiskVerdict::RejectRate:      return "rate limit";
        case RiskVerdict::RejectDrawdown:  return "max drawdown -- HALTED";
        case RiskVerdict::RejectStaleBook: return "stale book";
        case RiskVerdict::RejectHalted:    return "halted";
    }
    return "?";
}

class RiskGate {
public:
    explicit RiskGate(const RiskConfig& c) : cfg_(c) {}

    // Call on every equity update. Trips the kill switch permanently once the
    // drawdown limit is breached: a strategy that has lost its budget does not
    // get to decide whether it should keep trading.
    void on_equity(double equity) {
        peak_ = std::max(peak_, equity);
        if (peak_ - equity > cfg_.max_drawdown) halted_ = true;
    }

    void halt() { halted_ = true; }
    bool halted() const { return halted_; }
    std::uint64_t rejections() const { return rejections_; }

    // `now_ms` and `book_ms` come from the event stream, never from the system
    // clock -- same discipline as the backtester (docs/03 section 4).
    RiskVerdict check(Side side, Qty size, Qty position, std::int64_t now_ms,
                      std::int64_t book_ms) {
        const RiskVerdict v = evaluate(side, size, position, now_ms, book_ms);
        if (v != RiskVerdict::Allow) ++rejections_;
        return v;
    }

    // Only call after check() returned Allow.
    void record_sent(std::int64_t now_ms) {
        if (now_ms - window_start_ms_ >= 1000) {
            window_start_ms_ = now_ms;
            in_window_       = 0;
        }
        ++in_window_;
    }

private:
    RiskVerdict evaluate(Side side, Qty size, Qty position, std::int64_t now_ms,
                         std::int64_t book_ms) const {
        if (halted_) return RiskVerdict::RejectHalted;
        if (size <= 0 || size > cfg_.max_order_size) return RiskVerdict::RejectSize;

        // Stale data is worse than no data: it looks actionable.
        if (now_ms - book_ms > cfg_.max_book_age_ms) return RiskVerdict::RejectStaleBook;

        // Check the position we would hold if this order filled COMPLETELY,
        // not the one we hold now. Sizing against current inventory is how you
        // wake up past your limit.
        const Qty projected = side == Side::Buy ? position + size : position - size;
        if (projected > cfg_.max_position || projected < -cfg_.max_position) {
            return RiskVerdict::RejectPosition;
        }

        const std::int64_t in_window =
            (now_ms - window_start_ms_ >= 1000) ? 0 : in_window_;
        if (in_window >= cfg_.max_orders_per_s) return RiskVerdict::RejectRate;

        return RiskVerdict::Allow;
    }

    RiskConfig    cfg_;
    double        peak_            = 0.0;
    bool          halted_          = false;
    std::int64_t  window_start_ms_ = 0;
    std::int64_t  in_window_       = 0;
    std::uint64_t rejections_      = 0;
};

}  // namespace hft
