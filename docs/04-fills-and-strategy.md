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
dimensionless parameters:

**Reservation price.** Don't quote around fair value — quote around fair value
shifted *against* your inventory:

```
r = fair − q · γ · σ²
```

Long inventory pushes `r` down, moving both quotes down, making us more likely
to sell and less likely to buy. We give up a little expected spread to pull back
toward flat.

**Spread widens with volatility.** A-S's optimal spread grows with `γσ²`.
Volatility is when you get run over, so that is when you demand compensation.

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

### Varying only the fee (γ=5, half-spread 0.5 tick)

| maker fee | fees paid | **net P&L** | net in bp | markout |
|---|---|---|---|---|
| 2.0 bp (base tier) | 25.49 | **−35.85** | −2.82 | −0.79 bp |
| 1.0 bp | 12.75 | **−23.11** | −1.82 | −0.79 bp |
| 0.0 bp | 0.00 | **−10.36** | −0.82 | −0.79 bp |
| −0.5 bp (rebate) | −6.37 | **−3.99** | −0.31 | −0.79 bp |

### Inventory skew — the one parameter that clearly works (fee = 0)

| γ | fills | **net (bp)** | markout |
|---|---|---|---|
| 0 (no skew) | 1251 | **−1.069** | −0.885 bp |
| 1 | 1239 | **−0.942** | −0.830 bp |
| 5 | 1012 | **−0.816** | −0.790 bp |
| 20 | 814 | **−0.713** | −0.675 bp |

Monotonic, in both P&L and markout. Avellaneda–Stoikov's reservation-price skew
does exactly what the theory says: quoting away from inventory reduces both how
much you accumulate and how badly you get selected against. This is the clearest
positive result in the project.

### Half-spread — the parameter that does not (fee = 0)

| half-spread | fills | net (bp) |
|---|---|---|
| 0.5 tick | 1012 | −0.816 |
| 1 tick | 1015 | −0.817 |
| 2 tick | 979 | −0.829 |
| 4 tick | 976 | −0.819 |
| 8 tick | 945 | −0.813 |

Flat within noise across a 16× range. See finding 3 below.

## 5. What this actually means

**It loses money.** That is the correct result, it is what Lesson 0 predicted
from arithmetic alone, and it is worth more than a backtest that "worked".

Three findings, in order of importance:

**1. This is a fee-tier problem before it is a strategy problem.** The whole
range from −2.82 bp to −0.31 bp is driven by the fee, not by anything the
strategy does. BTCUSDT's book sits at a one-tick spread — about 0.15 bp round
trip — while the base maker fee is 2 bp *per side*. **You cannot capture 0.15 bp
and pay 4 bp.** Real market makers on this venue operate at VIP tiers where the
maker fee is near zero or negative. Without that, no amount of cleverness fixes
the arithmetic. Any conversation about this strategy that does not start with
the fee schedule is not a serious conversation.

**2. Inventory skew works, measurably.** Going from γ=0 to γ=20 improves net P&L
from −1.07 bp to −0.71 bp and markout from −0.885 bp to −0.675 bp, monotonically.
This is the theory paying off: skewing quotes away from inventory both limits
what you accumulate and reduces how badly you get selected against. It is not
enough to overcome the fee, but it is a real effect, in the predicted direction,
with the predicted mechanism.

**3. Gross P&L is negative before fees at all.** Even at zero fee we lose 0.82 bp,
and markout says why: −0.79 bp of adverse selection. Nearly the entire gross loss
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

---

*Run it:*

```bash
./build/hftbacktest data/btcusdt.jsonl.gz --gamma 5 --half 0.5
./build/hftbacktest data/btcusdt.jsonl.gz --gamma 0 --microprice 0   # naive baseline
./build/hftbacktest data/btcusdt.jsonl.gz --fee 0 --csv pnl.csv      # zero-fee scenario
```
