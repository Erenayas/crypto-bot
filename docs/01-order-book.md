# Lesson 1 — Reconstructing the Order Book

The exchange never sends you "the order book." It sends a snapshot, then a
stream of deltas, and expects you to maintain the book yourself. Every design
decision below exists to make that reconstruction *provably* correct.

Files: [`include/price.hpp`](../include/price.hpp),
[`include/order_book.hpp`](../include/order_book.hpp),
[`include/depth_sync.hpp`](../include/depth_sync.hpp),
[`tests/test_core.cpp`](../tests/test_core.cpp)

---

## 1. Never use `double` for a price

This is not stylistic. It is the difference between a bot that works and one
that mysteriously misprices orders at 3am.

Binary floating point cannot represent most decimal fractions. `0.1` is stored
as approximately `0.1000000000000000055511151231257827`. So:

```cpp
0.1 + 0.2 == 0.3     // false
```

Three concrete ways this bites a trading system:

1. **Equality breaks.** An order book is a map keyed by price. If `99.99`
   computed one way isn't bit-identical to `99.99` computed another way, you get
   two "different" levels at the same price. Your book silently splits.
2. **Error accumulates.** P&L is a long sum of small numbers. Floating point
   error compounds, and your position value drifts from the exchange's.
3. **Rounding hits the wire.** Send `99.98999999999999` to an exchange with a
   `0.01` tick size and the order is rejected — or worse, rounded to a price you
   didn't intend.

**The fix: fixed-point integers.** Store price as an `int64_t` scaled by `1e8`,
so `60199.90` becomes `6019990000000`. Integer arithmetic is exact, equality is
exact, and `int64_t` holds up to ~9.2×10¹⁸ — around 92 billion units at our
scale, with enormous headroom for any crypto price.

`parse_fixed()` converts the exchange's decimal strings directly to fixed point
without ever touching a float. It returns `std::optional` and rejects malformed
input rather than coercing it — **a misparsed price is worse than no price**,
because it looks perfectly valid all the way down to the order you send.

Doubles are permitted for exactly one thing: *derived statistics* like mid and
microprice, which are estimates, not accounting values.

## 2. Absolute quantities, not deltas

Binance's depth events carry the **absolute** resting quantity at each price
level, not the change. So the update rule is simply:

```
qty == 0   ->  erase the level
qty != 0   ->  overwrite the level
```

This is a mercy: it means a correctly-applied event fully determines the level,
so errors cannot accumulate the way they would with true deltas. Note that
erasing a level you never had is a legal no-op — the exchange may delete a level
that was outside your snapshot's depth limit.

## 3. The synchronisation algorithm

This is the heart of Phase 1. Each event carries three ids:

| Field | Meaning |
|---|---|
| `U` | first update id covered by this event |
| `u` | final update id covered by this event |
| `pu` | final update id of the **previous** event |

`pu` is what the USD-M futures stream gives you that spot does not. Every event
declares what it expects to follow, so a single dropped message is caught on the
very next event instead of being inferred later — or never.

The official procedure, implemented in `DepthSync`:

1. Open the stream and **buffer** events. You cannot validate anything yet.
2. Fetch a REST snapshot → `lastUpdateId`.
3. Drop any buffered event where `u < lastUpdateId` — entirely superseded.
4. The first event you apply must satisfy `U <= lastUpdateId AND u >= lastUpdateId`.
   It must *straddle* the snapshot, bridging it to the live stream.
5. Every subsequent event must satisfy `pu == previous event's u`.
   If not, **restart from step 2**.

Why the buffer in step 1: the REST snapshot takes a network round trip. Events
published during that window would otherwise be lost, leaving a hole between the
snapshot and the first event you see.

### The state machine

```
   NeedSnapshot ──on_snapshot()──> AwaitingFirstEvent ──straddling event──> Synced
        ^                                  │                                  │
        └──────── pu mismatch, or U > lastUpdateId (snapshot too old) ────────┘
```

Three states, not two. `AwaitingFirstEvent` is separate from `Synced` because
having a snapshot is *not* the same as having a verified live book — you still
need the event that bridges them.

Buffered events drain back through `on_event()` rather than through a second
code path. One implementation of the rules, so the buffered and live paths can
never drift apart.

### What we do on a gap

`trigger_resync()` **clears the book**. This looks aggressive; it is the whole
point. A book with a missed update is not "slightly stale" — it may be missing
the very level you are about to quote against. Quoting off it is how you send
orders into a market that no longer exists.

The rule: **a book you cannot verify is a book you must not trade on.** The
strategy layer will refuse to quote unless `DepthSync::synced()` is true.

`resync_count()` is exposed because resync frequency is a health metric. Rising
resyncs mean network trouble, and you want to see it on a dashboard rather than
discover it in the P&L.

## 4. Why `std::map` (for now)

`std::map` is a red-black tree: node-based, so every price level is a separate
heap allocation scattered across memory, and every traversal is a cache miss.
No production HFT book uses it.

We use it anyway, for now, because:

- In Week 1 the enemy is **correctness**, not latency. An ordered container
  where `begin()` is the best price is obviously correct at a glance.
- The network round trip to Binance is ~1–50 ms. Nanoseconds inside this class
  are invisible against that.
- **We have not measured yet.** Optimising before profiling is guessing.

The public interface is deliberately narrow (`apply_bid`, `best_bid`,
`microprice`, …) so that in Week 4 we can replace the internals with a flat
array indexed by tick — O(1), cache-friendly — and no calling code changes.

That is the actual lesson: *design the seam now, cross it when the profiler
tells you to.*

## 5. `crossed()` — the cheapest bug detector you will write

On a functioning venue, the best bid is always strictly below the best ask. A
crossed book is not a market condition; it means **our reconstruction is wrong**.

One comparison, checked every update, catches the entire class of bugs that
otherwise stays silent for hours while quietly losing money. Trading systems are
full of invariants like this. Assert them loudly and early.

## 6. Tests

`tests/test_core.cpp` covers the paths that matter, with no external test
framework — Week 1 should not be spent debugging CMake:

- fixed-point parsing, including malformed input and the `0.1 + 0.2` case
- book updates, level deletion, microprice, crossed detection
- sync: pre-snapshot buffering, stale-event dropping, the straddling first event
- **gap detection via `pu` mismatch, and recovery from it**
- rejection of a snapshot too old to ever reconcile

Run with `./build/test_core`.

Note what this test file proves: the correctness-critical logic of this project
is verified **without a network connection**. That is not an accident — it is why
`DepthSync` owns no sockets, no JSON, and no threads. Untestable trading code is
how people lose money.

---

*Next: Lesson 2 — WebSocket transport and JSON parsing*
