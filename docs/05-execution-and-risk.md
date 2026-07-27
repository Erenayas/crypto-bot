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

## 8. A debugging note worth keeping

The long recording initially failed the determinism check. It was not a bug in
the replay engine — the recorder was **still writing the file**. Two passes read
different amounts of data.

Worth internalising because it generalises: when a deterministic system suddenly
is not, question the *inputs* before the logic. The file was growing 25 KB every
three seconds and `ls` showed it in two commands.

## 9. Getting testnet keys

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
