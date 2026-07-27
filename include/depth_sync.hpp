#pragma once

#include "order_book.hpp"

#include <cstdint>
#include <vector>

namespace hft {

// One diff-depth event off the WebSocket stream.
//   U  = first update id covered by this event
//   u  = final update id covered by this event
//   pu = final update id of the PREVIOUS event  (USD-M futures only; spot omits it)
//
// pu is the gift the futures stream gives us: it makes gaps explicitly
// detectable instead of something you infer. Every event states what it expects
// to follow, so a single dropped message is caught on the very next event.
struct DepthUpdate {
    std::int64_t       U  = 0;
    std::int64_t       u  = 0;
    std::int64_t       pu = 0;
    std::vector<Level> bids;
    std::vector<Level> asks;
};

// REST snapshot from /fapi/v1/depth
struct DepthSnapshot {
    std::int64_t       last_update_id = 0;
    std::vector<Level> bids;
    std::vector<Level> asks;
};

// What the caller must do after feeding us data.
enum class SyncAction {
    None,             // all good, book is up to date
    RequestSnapshot,  // we are out of sync -- fetch a fresh REST snapshot
};

// Implements Binance's "How to manage a local order book correctly" for USD-M
// futures. This class owns the correctness of the book and nothing else: no
// sockets, no JSON, no threads. That is what makes it unit-testable, and the
// reason this is the first thing we built.
//
// Protocol (from the official docs):
//   1. Open the stream and buffer events.
//   2. Fetch a REST snapshot -> lastUpdateId.
//   3. Drop any buffered event where u < lastUpdateId.
//   4. The first event to apply must satisfy: U <= lastUpdateId AND u >= lastUpdateId.
//   5. Every event after that must satisfy: pu == previous event's u.
//      If not, restart from step 2.
class DepthSync {
public:
    enum class State {
        NeedSnapshot,       // buffering events, waiting for a REST snapshot
        AwaitingFirstEvent, // snapshot applied, hunting for the event that bridges it
        Synced,             // book is live and verified
    };

    const OrderBook& book() const { return book_; }
    State            state() const { return state_; }
    bool             synced() const { return state_ == State::Synced; }
    std::int64_t     last_update_id() const { return last_u_; }
    std::uint64_t    resync_count() const { return resyncs_; }

    [[nodiscard]] SyncAction on_event(const DepthUpdate& e) {
        switch (state_) {
            case State::NeedSnapshot:
                // No snapshot yet -- we cannot validate anything, so just hold on
                // to the event. Bounded, so a stalled snapshot can't eat memory.
                if (buffer_.size() < kMaxBuffer) buffer_.push_back(e);
                return SyncAction::None;

            case State::AwaitingFirstEvent:
                // Step 3: this event is entirely older than the snapshot. Discard.
                if (e.u < last_u_) return SyncAction::None;

                // Step 4: does this event straddle the snapshot's lastUpdateId?
                if (e.U <= last_u_ && e.u >= last_u_) {
                    apply(e);
                    last_u_ = e.u;
                    state_  = State::Synced;
                    return SyncAction::None;
                }

                // e.U > lastUpdateId: the events that would have bridged the gap
                // never arrived, so the snapshot is unusable. Start over.
                return trigger_resync(e);

            case State::Synced:
                // Step 5: the chain must be unbroken.
                if (e.pu != last_u_) return trigger_resync(e);
                apply(e);
                last_u_ = e.u;
                return SyncAction::None;
        }
        return SyncAction::None;
    }

    // Full reset, for when the transport itself is replaced (reconnect). Events
    // buffered from a dead connection tell us nothing about the new one.
    void reset() {
        book_.clear();
        state_  = State::NeedSnapshot;
        last_u_ = 0;
        buffer_.clear();
    }

    [[nodiscard]] SyncAction on_snapshot(const DepthSnapshot& snap) {
        book_.clear();
        for (const auto& l : snap.bids) book_.apply_bid(l.px, l.qty);
        for (const auto& l : snap.asks) book_.apply_ask(l.px, l.qty);

        last_u_ = snap.last_update_id;
        state_  = State::AwaitingFirstEvent;

        // Replay whatever arrived while the REST call was in flight. Draining
        // through on_event() means the buffered path and the live path share
        // exactly one implementation -- no second copy of the rules to drift.
        std::vector<DepthUpdate> pending;
        pending.swap(buffer_);
        for (const auto& e : pending) {
            if (on_event(e) == SyncAction::RequestSnapshot) {
                return SyncAction::RequestSnapshot;
            }
        }
        return SyncAction::None;
    }

private:
    static constexpr std::size_t kMaxBuffer = 4096;

    SyncAction trigger_resync(const DepthUpdate& e) {
        ++resyncs_;
        state_ = State::NeedSnapshot;
        book_.clear();  // the book is untrustworthy now; do not quote off it
        buffer_.clear();
        buffer_.push_back(e);  // keep this event, it may bridge the next snapshot
        return SyncAction::RequestSnapshot;
    }

    void apply(const DepthUpdate& e) {
        for (const auto& l : e.bids) book_.apply_bid(l.px, l.qty);
        for (const auto& l : e.asks) book_.apply_ask(l.px, l.qty);
    }

    OrderBook                book_;
    State                    state_   = State::NeedSnapshot;
    std::int64_t             last_u_  = 0;
    std::uint64_t            resyncs_ = 0;
    std::vector<DepthUpdate> buffer_;
};

}  // namespace hft
