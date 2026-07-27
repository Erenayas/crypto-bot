# Lesson 5 — Execution and Risk

Everything so far was read-only. This is the part that can lose money, so it is
also the part with the most guard rails.

Files: [`include/signer.hpp`](../include/signer.hpp),
[`include/exec_client.hpp`](../include/exec_client.hpp),
[`include/risk.hpp`](../include/risk.hpp),
[`src/live_main.cpp`](../src/live_main.cpp)

---

## 1. Safety decisions that are not configurable

These are compiled in, not flags:

- **Testnet only.** There is no mainnet order path in the code. Trading real
  money should be a change someone makes on purpose, in a diff that gets
  reviewed — not a flag they can typo at 2am.
- **Dry run is the default.** `--live` is required before a single byte of order
  intent leaves the process.
- **Market data comes from the venue we trade on.** Quoting mainnet prices into
  the testnet book would place orders at prices that make no sense there. The
  two books are genuinely different instruments.
- **All orders are post-only.**
- **Every order passes the risk gate**, and any exception cancels everything.

## 2. Signing

Binance authenticates with HMAC-SHA256 over the raw query string, plus the key
in an `X-MBX-APIKEY` header.

The one rule that matters: **sign exactly the bytes you are about to send, in
the order you will send them.** Build the query one way and transmit it another
and you get error `-1022`, "Signature for this request is not valid", which
tells you nothing about which of the two was wrong.

```cpp
q += "&recvWindow=..." "&timestamp=...";
q += "&signature=" + hmac_sha256_hex(secret, q);   // last, over everything above
```

The test checks against the published RFC vector rather than against ourselves:

```cpp
CHECK(hmac_sha256_hex("key", "The quick brown fox jumps over the lazy dog") ==
      "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
```

A signature routine that is *self-consistently wrong* still passes any test that
compares it to itself, and then fails every request with an error that blames
the clock.

## 3. Post-only (`timeInForce=GTX`)

`GTX` — "Good Till Crossing" — tells the exchange to **reject the order
outright** if it would execute immediately.

That is exactly what a market maker wants. Crossing the spread makes us a taker:
2.5× the fee, no spread captured, and none of the queue dynamics the entire
strategy is built on. Better a rejected order than an accidental taker fill.

So a stream of GTX rejections in the log is **not a failure**. It means the book
moved between our decision and the exchange's receipt, and the protection did
its job. The live loop counts them rather than treating them as errors.

## 4. Decimal formatting is a correctness problem

```cpp
CHECK(format_fixed(*parse_fixed("65050.10"), 2) == "65050.10");
CHECK(*parse_fixed(format_fixed(px, 2)) == px);   // exact round trip
```

`format_fixed` goes through integer arithmetic rather than
`snprintf("%.2f", to_double(v))`. A value that survived the whole pipeline
exactly should not acquire a rounding error in the last step before the wire.
Binance rejects orders whose precision does not match the symbol's
`tickSize`/`stepSize`, and that rejection is the first wall everyone hits on
their first live order.

## 5. The risk gate

Every order passes through `RiskGate::check()`. It is header-only and
dependency-free specifically so it can be tested exhaustively — this is the one
component where a bug is not a bad backtest but a liquidated account.

**Position limits are checked against the projected position**, not the current
one:

```cpp
const Qty projected = side == Side::Buy ? position + size : position - size;
if (projected > cfg_.max_position || projected < -cfg_.max_position) reject;
```

Sizing against the position you hold *now* is how you wake up past your limit:
each individual order looks fine, and their sum does not.

**Stale books are refused.** Data that is 2 seconds old is worse than no data,
because it still looks actionable.

**The rate limiter is ours, not theirs.** Exceeding the venue's limit gets the
whole API key throttled or banned — a far worse outcome than skipping one
requote.

**The kill switch is permanent.**

```cpp
g.on_equity(88.0);      // 12 below a peak of 100, limit is 10
CHECK(g.halted());
g.on_equity(200.0);     // recovers?
CHECK(g.halted());      // still halted. Forever.
```

A strategy that has spent its loss budget does not get to decide it feels better
now. A human restarts it, after looking at why.

## 6. Position comes from the exchange

We poll `/fapi/v2/positionRisk` rather than inferring position from our own
fills. Two reasons:

1. Without a user-data stream (`listenKey`) we never see fill events at all.
2. **The exchange is authoritative.** A local position that has silently drifted
   from the real one makes every risk limit meaningless — they would be
   protecting a number that does not exist.

Polling every 5 seconds is the pragmatic version. A production system subscribes
to the user-data stream and reconciles against a poll periodically anyway,
because reconciliation catches the bugs that the stream alone hides.

## 7. What "shutdown" means

```cpp
// Never leave orders resting in a market you have stopped watching.
trader.flatten_quotes();
```

Called on SIGINT/SIGTERM, on a risk halt, on a book desync, on a network error,
and on any exception we do not understand. The pattern throughout: **when
something is wrong, the default is to have no orders in the market**, not to
hope.

## 8. Latency, measured against a real exchange

The first live run rejected almost every bid with `-5022`. Two separate causes,
and separating them took measurement rather than guessing.

**Cause 1: a units bug in the strategy**, which put the bid 133 dollars through
the ask. Covered in [Lesson 4 §7](04-fills-and-strategy.md) — the backtest could
never have caught it, because a simulator has no opinion about whether a quote
is sane. Only the venue does.

**Cause 2: order round-trip latency.** The original client opened a fresh TCP +
TLS connection per order:

```
new connection per request:  0.39 s
reused connection:           0.28 s
```

390 ms is long enough for BTCUSDT to move a tick, so a price computed as passive
arrives crossing, and post-only correctly rejects it. `HttpsSession` now holds
one connection open and reconnects transparently — servers close idle
keep-alives, and that failure surfaces on the *next* write, so the retry is part
of the design rather than error handling bolted on.

Measured over 75-second live runs:

| configuration | orders accepted | rejected | reject rate |
|---|---|---|---|
| new connection each time, 0.5 tick | 27 | 26 | 49% |
| **keep-alive**, 0.5 tick | 48 | 85 | 64% |
| **keep-alive**, 3 ticks from fair | 63 | 46 | **42%** |

Two things worth reading carefully:

1. Connection reuse **2.5×'d the throughput** (53 → 133 attempts in the same
   window) but *raised* the reject rate. Faster requoting means chasing the book
   more often, and every attempt carries the same in-flight risk. Throughput and
   hit rate are different problems.
2. Quoting further from the touch is what actually fixes rejections — 64% → 42%
   — because a wider margin absorbs a tick of movement during flight. It costs
   queue position, which is the trade-off the backtester models.

The honest conclusion: **at ~280 ms round trip over REST, you cannot reliably
join the touch on BTCUSDT.** Binance offers a WebSocket order-entry API that
would cut this substantially, and that is the correct next step — not more
strategy tuning.

## 9. Flattening, and what it costs

`flatten_quotes()` cancels orders. Closing the **position** is a separate,
opt-in decision:

```bash
./build/hftlive --live --flatten-on-exit   # close the position when stopping
./build/hftlive --flatten                  # close it now and exit (panic button)
```

Opt-in because a bot stopping is not always a reason to exit a position, and
exiting is not free. Whichever you choose, *choose it* — the failure mode is
leaving one open by accident, which is exactly what happened on the first live
session here.

`reduceOnly=true` is not optional on the closing order. It guarantees the order
can only shrink the position, never flip it. Without it, a position that moved
between reading it and sending the order leaves you short instead of flat —
worse than where you started.

### The measured cost of flattening 0.160 BTC

```
17:52:58  SELL 0.0858 @ 64665.60  maker=false  commission 2.219
17:52:58  SELL 0.0015 @ 64645.60  maker=false  commission 0.039
17:52:58  SELL 0.0043 @ 64643.40  maker=false  commission 0.111
17:52:58  SELL 0.0634 @ 64623.20  maker=false  commission 1.639
```

Three things in four lines:

1. **Every fill is `maker=false`.** A market order is a taker order: ~4 bp of
   commission here against 2 bp for maker. That is the price of certainty, and
   the reason a passive exit is preferable when you have the luxury of time.
2. **The order walked the book** — 64,665 down to 64,623, about 42 dollars of
   slippage on one order. This is **market impact**, the thing the backtester
   explicitly does not model ([Lesson 4 §6](04-fills-and-strategy.md)). Testnet
   depth is thin so the effect is exaggerated, but the mechanism is real.
3. **Total cost ~10.6 USDT** to exit a position that had accumulated over 90
   seconds of quoting.

The arc of that session, end to end: spread capture earned **+2.13**, the
inventory it accumulated lost **−4.17**, and closing that inventory cost
**−10.6**.

**Accumulating inventory is cheap. Getting rid of it is expensive.** That
asymmetry is the entire reason Avellaneda–Stoikov skews quotes against
inventory — preventing accumulation costs a fraction of a tick, while cleaning
it up afterwards costs the spread plus taker fees plus impact.

## 10. A debugging note worth keeping

The long recording initially failed the determinism check. It was not a bug in
the replay engine — the recorder was **still writing the file**. Two passes read
different amounts of data.

Worth internalising because it generalises: when a deterministic system suddenly
is not, question the *inputs* before the logic. The file was growing 25 KB every
three seconds and `ls` showed it in two commands.

## 11. Getting testnet keys

```bash
# 1. Register at https://testnet.binancefuture.com (separate from the real account)
# 2. Generate an API key/secret from the dashboard
export BINANCE_API_KEY=...
export BINANCE_API_SECRET=...

./build/hftlive                 # dry run: quotes computed and logged, nothing sent
./build/hftlive --live          # places real orders on TESTNET
```

Testnet keys are worthless if leaked, which is the point of starting there.
`.gitignore` excludes `.env` and `*.key` regardless.

---

*Read the results in [Lesson 4](04-fills-and-strategy.md) before running this
live. The strategy loses money at base fee tiers, and knowing that in advance is
the difference between an experiment and a mistake.*
