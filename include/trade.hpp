#pragma once

#include "price.hpp"

#include <cstdint>

namespace hft {

enum class Side { Buy, Sell };

// One trade off the wire.
//
// Binance publishes two trade streams and we accept either:
//
//   @trade     one message per individual fill.
//   @aggTrade  consecutive fills of a single aggressive order at one price
//              collapsed into one message.
//
// We subscribe to @trade. It is the finer granularity -- every fill, separately
// -- which is strictly more information for a queue model, and empirically it
// is the one that actually delivers data on fstream.binance.com. (@aggTrade
// connects happily and then sends nothing; `wsdump` is how we found that out.)
struct Trade {
    Price        px             = 0;
    Qty          qty            = 0;
    std::int64_t id             = 0;      // "t" (trade) or "a" (aggTrade)
    std::int64_t exch_ms        = 0;      // "T" -- exchange trade time
    bool         buyer_is_maker = false;  // "m"
    bool         aggregated     = false;  // came from @aggTrade rather than @trade

    // The whole reason we record trades.
    //
    // A price level's size can drop for two completely different reasons:
    // someone TRADED against it, or someone CANCELLED. The depth stream reports
    // only the new total, so it cannot tell them apart -- but a queue model must,
    // because they affect your position in the queue very differently.
    //
    // Trades are the half we can observe directly. Whatever a level lost that
    // trades do not account for was cancellation.
    //
    // "m" tells us which side rested, hence which side got consumed:
    //   m == true  -> the resting order was a BID; the aggressor SOLD into it.
    //   m == false -> the resting order was an ASK; the aggressor BOUGHT.
    bool hit_bid() const { return buyer_is_maker; }

    // Which side crossed the spread. Aggressive buying lifts asks, and a run of
    // it is the signature of exactly the flow that adversely selects you.
    Side aggressor() const { return buyer_is_maker ? Side::Sell : Side::Buy; }
};

}  // namespace hft
