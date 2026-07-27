# hft-crypto-bot

A low-latency market-making bot for Binance USD-M Futures, written in C++20.

Built as a learning project — every component is documented in [`docs/`](docs/)
with the reasoning behind it, not just the implementation.

---

## Status

| Phase | Topic | State |
|---|---|---|
| 0 | Market microstructure fundamentals | ✅ [notes](docs/00-market-microstructure.md) |
| 1 | Market data ingestion & order book reconstruction | ✅ [book](docs/01-order-book.md) · [transport](docs/02-transport.md) |
| 2 | Recorder + deterministic replay harness | 🔄 [record/replay](docs/03-record-replay.md) done — fill sim next |
| 3 | Strategy: naive quoting → Avellaneda–Stoikov | ⬜ |
| 4 | Execution engine, risk limits, latency measurement | ⬜ |

---

## The 4-week plan

Aggressive but achievable. Scope is deliberately cut to fit — see
[Non-goals](#non-goals).

### Week 1 — Market data
Build a correct, live order book.
- Project skeleton, CMake, dependency setup
- Binance Futures WebSocket client (Boost.Beast + OpenSSL)
- Depth stream parsing, snapshot + delta reconstruction
- Sequence gap detection and resynchronisation
- **Deliverable:** a live, verifiably correct L2 book printing to console

### Week 2 — Replay & simulation
You cannot iterate on a strategy you cannot test.
- Record raw market data to disk in a replayable binary format
- Deterministic replay engine (same input → same output, every time)
- Fill simulator with a queue-position model
- **Deliverable:** backtest harness that runs a strategy over recorded data

### Week 3 — Strategy
- v1: naive symmetric quoting around the microprice (this will lose money)
- v2: Avellaneda–Stoikov — inventory skewing and optimal spread
- Backtest both, compare P&L, analyse in Python
- **Deliverable:** a strategy with P&L curves and an explanation of the delta

### Week 4 — Live execution & risk
- Order placement via REST/WebSocket, order lifecycle state machine
- Rate limiting, state reconciliation against the exchange
- Position limits, max drawdown, kill switch
- Latency instrumentation (`rdtsc`, histograms) and targeted optimisation
- **Deliverable:** running live on testnet, plus a project write-up

## Non-goals

Explicitly out of scope for 4 weeks. Cutting these is what makes the rest
possible:

- Kernel bypass, custom allocators, FPGA — the network dominates our latency
  budget, so these would be wasted effort here
- Multi-venue arbitrage
- Machine-learned alpha signals
- Hand-rolled WebSocket/TLS/JSON — we use battle-tested libraries

## Layout

```
src/        engine source
include/    public headers
docs/       lesson notes — read these in order
tools/      Python research, backtest analysis, plotting
data/       recorded market data (gitignored)
```

## Build

Requires Boost, OpenSSL 3 and simdjson:

```bash
brew install boost openssl@3 simdjson

cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/test_core     # 60 assertions, no network needed
```

## Run

```bash
./build/hftbot                     # live BTCUSDT book from mainnet market data
./build/hftbot --symbol ethusdt
./build/hftbot --levels 10
./build/hftbot --testnet           # testnet feed (thin, synthetic)
```

Market data requires no API key and places no orders — it is read-only.

## Record and replay

```bash
./build/hftbot --record data/btcusdt.jsonl.gz     # Ctrl-C stops cleanly
./build/hftreplay data/btcusdt.jsonl.gz --twice   # replay + prove determinism
zcat < data/btcusdt.jsonl.gz | head -3            # recordings are inspectable
```

Recordings are gzipped JSONL holding the exchange's bytes verbatim, covering
both the depth stream and the trade stream over one combined connection — see
[Lesson 3](docs/03-record-replay.md).

## Debugging

```bash
./build/wsdump /ws/btcusdt@trade                 # dump raw messages from a stream
./build/wsdump "/stream?streams=a/b" 20          # combined stream, 20 messages
```

Built after `@aggTrade` silently delivered nothing for 45 seconds. When a feed
produces no data, look at the wire.

