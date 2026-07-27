# Lesson 4 — Fill Simulation, Strategy, and What the Numbers Say

The part where the project stops being infrastructure and starts answering the
actual question: *does this make money?*

Files: [`include/fill_sim.hpp`](../include/fill_sim.hpp),
[`include/portfolio.hpp`](../include/portfolio.hpp),
[`include/strategy.hpp`](../include/strategy.hpp),
[`src/backtest_main.cpp`](../src/backtest_main.cpp)

---

## 1. The fill model is the backtest

Everything else can be perfect and a bad fill model still makes the result
fiction. The naive version — *"price touched my level, so I filled"* — is the
single biggest reason market-making backtests look profitable and then lose
money live.

In reality you fill only after **everyone ahead of you at that price** fills
first. So we track, per resting order, how much volume sits in front of it:

```cpp
place()     queue_ahead = size currently at that price   // everyone there is ahead
on_trade()  a trade at our price on our side eats the queue FIRST;
            only the leftover fills us
on_book()   the level shrank with no trade => cancellations
```

The test that captures the whole idea:

```cpp
sim.place(Side::Buy, 100.00, 1, book);        // 10 units already resting
sim.on_trade(trade_at("100.00", "4", true));  // price "touched" our level...
CHECK(fills.empty());                          // ...and we did NOT fill
CHECK(sim.bid().queue_ahead == 6);
```

### The cancellation rule

When a level shrinks with no trade to explain it, someone cancelled — but the
depth stream never says *whose* order it was. This is exactly the ambiguity that
made us record the trade stream in Lesson 3.

We assume cancels came from **behind** us, so our queue position does not
improve. We only advance when arithmetic forces it (`queue_ahead` can never
exceed the level's total size).

That makes the model **pessimistic**: in reality some cancels really are ahead
of you, so true fill rates are a little better than what we report. That is the
right direction to be wrong in. An optimistic backtest is a bill you pay later,
with real money.

### Requoting costs queue position

```cpp
sim.place(Side::Buy, 100.00, 1, book);   // queue_ahead falls to 1 after trades
sim.place(Side::Buy, 100.00, 1, book);   // same price: keeps our place
CHECK(sim.bid().queue_ahead == 1);

sim.place(Side::Buy,  99.90, 1, book);   // one tick away: back of a 25-unit queue
CHECK(sim.bid().queue_ahead == 25);
```

Queue position is the asset a market maker spends. A strategy that requotes on
every tick throws it away continuously and never reaches the front of anything.
That is why the backtester has a hysteresis threshold, and why this cost falls
out of the model naturally instead of being bolted on.

## 2. Markout: measuring adverse selection directly

Spread capture is easy to measure and easy to fool yourself with. **Markout** is
the number that matters: for each fill, compare the mid price one second later
against what we traded at, signed by direction.

- Bought at 100, mid is 100.05 later → **+** good fill
- Bought at 100, mid is 99.95 later → **−** we were picked off

```cpp
mo.on_fill(Fill{Side::Buy, 100.00, 2, t});
mo.on_mark(t + horizon, 99.50);
CHECK(mo.total() == -1.0);   // (99.50 - 100.00) * 2
```

A market maker with positive spread capture and strongly negative markout is not
profitable — it is a slow-motion loss with good bookkeeping. This is Lesson 0's
adverse selection, finally measurable.

## 3. The strategy

Avellaneda–Stoikov reduced to the two ideas that carry the weight, with
parameters made scale-free so they can be tuned:

**Reservation price.** Don't quote around fair value — quote around fair value
shifted *against* your inventory:

```
r = fair − q_norm · γ · tick
```

Long inventory pushes `r` down, moving both quotes down, making us more likely
to sell and less likely to buy. We give up a little expected spread to pull back
toward flat.

A-S writes this term as `q·γ·σ²`, where the units cancel against γ's own. Ours
is expressed in **ticks** — γ means "how many ticks to shift the quotes at full
inventory" — because normalising `q` and making γ dimensionless breaks that
cancellation. §7 covers what happened when it did not.

**Spread widens with volatility.** A-S's optimal spread grows with volatility.
Volatility is when you get run over, so that is when you demand compensation.
(`vol_coeff`, off by default in these runs.)

Plus two things A-S doesn't give you but reality demands: hard inventory limits
(stop quoting the side that makes it worse), and never crossing the book (a
maker that crosses becomes a taker — worse fee, no spread, none of the queue
dynamics the strategy is built on).

Setting `γ = 0` and `use_microprice = false` yields the naive symmetric market
maker, deliberately, so the two can be compared on identical bytes.

## 4. Results

**15 minutes of live BTCUSDT**: 133,865 records, 8,808 depth events, 125,055
trades, 0 resyncs, 0 undecodable. Quote size 0.002 BTC, max position 0.010 BTC.
Around 1,000 simulated fills per configuration — enough to mean something.

Over this window the market drifted **down** roughly 0.6% (65,590 → 65,185),
with trade flow imbalance at −0.28. Keep that in mind reading the numbers: a
market maker in a falling market accumulates longs and bleeds. See §6.

### Varying only the fee (γ=2, half-spread 0.5 tick)

| maker fee | **net P&L** | net in bp | markout |
|---|---|---|---|
| 2.0 bp (base tier) | **−49.08** | −3.04 | −0.89 bp |
| 1.0 bp | **−32.87** | −2.04 | −0.89 bp |
| 0.0 bp | **−16.65** | −1.03 | −0.89 bp |
| −0.5 bp (rebate) | **−8.55** | −0.53 | −0.89 bp |

### Inventory skew (fee = 0, half-spread 0.5 tick)

| γ (ticks at full inventory) | fills | **net (bp)** | max position | markout |
|---|---|---|---|---|
| 0 (no skew) | 1251 | −1.069 | 0.0080 | −0.885 bp |
| 1 | 1309 | −1.028 | 0.0060 | −0.890 bp |
| 2 | 1309 | −1.031 | 0.0060 | −0.885 bp |
| 5 | 1299 | −1.008 | 0.0060 | −0.873 bp |
| 10 | 1317 | −0.990 | 0.0070 | −0.860 bp |
| 20 | 1274 | **−0.968** | 0.0040 | −0.853 bp |

A **real but modest** effect: about 9% better net P&L from γ=0 to γ=20, a small
markout improvement, and a clear reduction in peak inventory (0.008 → 0.004),
which is what the skew is actually for. It does not come close to overcoming the
fee.

Note that fills go *up* slightly with skew (1251 → ~1300). Gentle skewing keeps
both sides quoted instead of running into the hard position limit and going
one-sided.

### Half-spread — the parameter that does not (fee = 0)

| half-spread | fills | net (bp) |
|---|---|---|
| 0.5 tick | 1309 | −1.031 |
| 1 tick | 1292 | −1.034 |
| 2 tick | 1245 | −1.039 |
| 4 tick | 1182 | −1.018 |
| 8 tick | 1144 | −0.976 |

Flat within noise across a 16× range. See finding 4 below.

## 5. What this actually means

**It loses money.** That is the correct result, it is what Lesson 0 predicted
from arithmetic alone, and it is worth more than a backtest that "worked".

Four findings, in order of importance:

**1. This is a fee-tier problem before it is a strategy problem.** The whole
range from −3.04 bp to −0.53 bp is driven by the fee, not by anything the
strategy does. BTCUSDT's book sits at a one-tick spread — about 0.15 bp round
trip — while the base maker fee is 2 bp *per side*. **You cannot capture 0.15 bp
and pay 4 bp.** Real market makers on this venue operate at VIP tiers where the
maker fee is near zero or negative. Without that, no amount of cleverness fixes
the arithmetic. Any conversation about this strategy that does not start with
the fee schedule is not a serious conversation.

**2. Inventory skew helps, modestly.** γ=0 → γ=20 improves net P&L by about 9%
(−1.069 → −0.968 bp), improves markout slightly, and halves peak inventory. The
direction matches the theory; the magnitude is small on a one-tick book, for the
same reason as finding 4 — there is very little room to express a preference in
a price. **See §7 for why an earlier version of this document claimed a much
larger effect.**

**3. Gross P&L is negative before fees at all.** Even at zero fee we lose 1.03 bp,
and markout says why: −0.89 bp of adverse selection. Nearly the entire gross loss
*is* adverse selection. The fills we get are systematically the ones we don't
want, exactly as Lesson 0 predicted.

**4. Half-spread does essentially nothing, across a 16× range.** Because on a
one-tick book, quotes get rounded and clamped into nearly the same places
regardless. The related finding: the microprice edge is ~0.03 against a 0.10
tick — a real signal that **cannot be expressed in a price** on this instrument.
To use it you would have to express it in **size**, in **whether to quote at
all**, or trade a wider-spread instrument. That is a genuine microstructure
constraint, not a bug.

### What would come next, given more time

- **Quote sizing / quote suppression** driven by the microprice and trade-flow
  imbalance, since price cannot carry a sub-tick signal
- Test on a wider-spread symbol where a tick is worth more in bp
- A model of our own latency (we currently assume our quote is in the book
  instantly, which is optimistic in the one place the model is not conservative)
- Fee-tier sensitivity as an explicit input to strategy selection

## 6. Honest limitations

Stated plainly, because a backtest whose assumptions are hidden is worse than no
backtest:

- **No latency model.** Quotes are assumed to reach the book instantly. Real
  round trip is 1–50 ms, during which the book moves. This is optimistic.
- **We do not affect the market.** Our orders are simulated alongside the real
  book, not inside it. At 0.002 BTC on BTCUSDT that is a fair approximation;
  at size it would not be.
- **Cancel/replace is instant and free** beyond the queue-position cost.
- **One 15-minute sample, in a downtrend.** The market fell 0.6% during this
  window with trade flow imbalance at −0.28. A market maker in a falling market
  accumulates longs and bleeds, so these numbers are pessimistic for reasons
  that have nothing to do with the strategy's quality. **This sample cannot show
  that the strategy is unprofitable in general** — only that it was unprofitable
  here, and that fees dominate its P&L regardless of regime. Establishing
  anything stronger needs hours of data across rising, falling and flat markets.
  That costs nothing but wall-clock time, and it is the first thing I would do
  with another day.

## 7. Minimum edge — the largest strategy improvement found

The question that produced this: *"can it only close a trade once the profit
exceeds the commission?"* Three plausible-sounding rules came out of it. Two
were traps and one was the best result in the project, and only measurement
separated them.

### The rule that worked: refuse to quote too close to fair value

`min_edge_bp` sets a floor on the half-spread as basis points of price. Swept at
the 2 bp base maker fee:

| min edge | fills | net (bp) | markout | time holding inventory |
|---|---|---|---|---|
| 0 (off) | 973 | −2.898 | −0.838 bp | 81% |
| 0.5 bp | 713 | −2.731 | −0.661 bp | 71% |
| 1.0 bp | 317 | −2.458 | −0.342 bp | 54% |
| **1.25 bp** | 241 | **−2.318** | **−0.070 bp** | 64% |
| 1.5 bp | 172 | −2.386 | +0.092 bp | 77% |
| 2.0 bp | 56 | −3.873 | +0.577 bp | 95% |

**Markout goes from −0.838 bp to −0.070 bp.** Adverse selection — the thing
Lesson 0 named as the central problem and every measurement since has confirmed —
is almost entirely eliminated by refusing to quote inside 1.25 bp of fair value.
Gross P&L improves from −10.89 to −0.92 USDT: before fees, the strategy is now
close to flat.

And past 1.5 bp it collapses. Markout keeps improving (+0.577 at 2 bp: the fills
you get out there are genuinely *good*) but you get 56 fills in 15 minutes
instead of 973, and the losses that remain are no longer amortised over anything.

**The edge is real, and it is inaccessible.** Quote tight and every fill is
someone who knew more than you; quote wide and nobody trades with you. 1.25 bp
is where those two curves cross, on this instrument, in this sample.

### The first profitable configuration

Same setting across fee tiers:

| min edge | 2 bp fee | 1 bp fee | 0 bp fee | −0.5 bp rebate |
|---|---|---|---|---|
| 0 (off) | −2.898 | −1.894 | −0.891 | −0.389 |
| 1.0 bp | −2.458 | −1.455 | −0.451 | **+0.051** |
| **1.25 bp** | **−2.318** | **−1.314** | **−0.311** | **+0.191** |

**+0.191 bp.** The only positive number the project ever produced, and it still
needs a maker rebate to get there. The conclusion from §5 survives intact — this
is a fee-tier business — but the strategy now clears the bar *when the fee
structure allows it to*, which it previously did not at any tier.

These are now the defaults.

### The trap: "never close at a loss"

The most natural reading of the original question — hold the exit quote at or
above the position's cost basis plus fees — is implemented as `--breakeven 1`.
It is off by default, and here is why:

| | fills | net (bp) | markout | time holding inventory |
|---|---|---|---|---|
| off | 973 | −2.898 | −0.838 bp | 81% |
| **on** | **9** | **−18.926** | −1.464 bp | **97.6%** |

Nine fills in fifteen minutes, and 97.6% of the run spent holding inventory.

The absolute loss is smaller (−1.97 vs −35.44 USDT) purely because it barely
trades. Per unit of risk taken it is **six times worse**, and markout gets worse
too, because the positions it refuses to close are exactly the ones the market
has already moved against.

**The rule does not remove losses. It converts them into inventory.** When the
market moves against you, the exit quote simply never fills, and a small
closable loss becomes an open-ended one. "Never take a loss" is the mechanism by
which small losses become large ones — and `time in market` is the column where
it shows up first, which is why the backtester now reports it.

### The other trap: quoting more slowly

| min gap between requotes | fills | net (bp) | markout |
|---|---|---|---|
| 0 | 317 | −2.458 | −0.342 bp |
| 1 s | 345 | −2.663 | −0.490 bp |
| 3 s | 242 | −2.709 | −0.531 bp |

Slowing down makes it *worse* once a minimum edge is in place. Stale quotes are
adversely selected quotes: the longer a price sits unrevised, the more likely it
is that the market has moved and you are the last one offering it. Requoting has
a cost in queue position, but not requoting has a cost in getting picked off,
and here the second is larger.

## 8. The units bug that faked a result

Worth recording in full, because the failure mode is more instructive than the
fix.

The skew was originally written straight from the paper:

```cpp
reservation = fair - q_norm * gamma * sigma * sigma;   // WRONG
```

In Avellaneda–Stoikov the units of `q·γ·σ²` cancel against γ's own units. Making
γ dimensionless and normalising `q` — which we did so the parameter would be
tunable — breaks that cancellation. `sigma` is in absolute price units, so on a
65,000-priced asset `sigma²` is around **133**. The skew was shoving quotes
*hundreds of dollars* through the opposite touch.

**The backtest never noticed.** It happily quoted 133 dollars away, filled less
on one side, and reported *better* numbers — so γ looked like a monotonic
improvement all the way to γ=20. It was not measuring inventory skew at all; it
was measuring "stop quoting the side that increases inventory", implemented by
accident.

What caught it was the **exchange**, on the very first live run:

```
mid 65184.50  →  BID 65317.90
{"code":-5022,"msg":"Due to the order could not be executed as maker,
                     the Post Only order will be rejected."}
```

Three lessons:

1. **The unit test that "covered" this passed `sigma = 0.5`.** At that scale
   `sigma²` is 0.25 and the bug is invisible. A test with unrealistic inputs
   certifies nothing. `test_skew_is_scale_free` now runs at real BTCUSDT prices
   with a measured `sigma = 11.5` and asserts quotes stay within a bounded number
   of ticks of fair value.
2. **A backtest cannot reject a quote.** The simulator has no opinion about
   whether a price is sane — it just models what happens if you quote there.
   Post-only rejection is a constraint that only exists at the venue, which is
   an argument for reaching the venue early even when the strategy is not ready.
3. **Dimensionless parameters need a stated unit.** γ is now defined as *"how
   many ticks to shift the quotes at full inventory"*: scale-free, bounded, and
   obviously wrong if it ever produces a 133-dollar shift.

The corrected form:

```cpp
const double reservation = fair - q_norm * p_.gamma * tick_px;
```

---

*Run it:*

```bash
./build/hftbacktest data/btcusdt.jsonl.gz --gamma 5 --half 0.5
./build/hftbacktest data/btcusdt.jsonl.gz --gamma 0 --microprice 0   # naive baseline
./build/hftbacktest data/btcusdt.jsonl.gz --fee 0 --csv pnl.csv      # zero-fee scenario
```
