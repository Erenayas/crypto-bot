# Lesson 3 — Recording and Deterministic Replay

The most-skipped and most-regretted component in any trading project.

Files: [`include/gzfile.hpp`](../include/gzfile.hpp),
[`src/gzfile.cpp`](../src/gzfile.cpp),
[`include/recording.hpp`](../include/recording.hpp),
[`src/replay_main.cpp`](../src/replay_main.cpp)

---

## 1. Why this comes before the strategy

Without replay, testing a strategy means running it live, in real time, during
market hours. Change one parameter and you wait ten minutes for a result — and
the result isn't comparable to the last one, because the market moved.

With replay:

```
  wall span    44.7 s
  replay time  0.006 s   (7689x realtime)
```

The same 45 seconds of market, re-run in 6 milliseconds, identically, as many
times as you like. A three-hour recording backtests in about two seconds. That
is the difference between tuning a strategy and guessing at one.

This is also the only honest way to compare two strategies. Run both over the
identical byte stream and the difference in P&L is *the strategy*, not luck
about which minute you happened to be connected.

## 2. Format: gzipped JSONL

One JSON object per line, gzip level 1:

```json
{"t":1785157940427654000,"k":"e","d":{"e":"depthUpdate","U":11143204067767,...}}
```

| field | meaning |
|---|---|
| `t` | **our** receive time, nanoseconds since the Unix epoch |
| `k` | record kind: `h` header, `s` snapshot, `e` depth event |
| `d` | the exchange's payload, spliced in **verbatim** |

A binary format would be maybe 5× smaller and faster to parse. We chose JSONL
anyway, and the reason is worth stating plainly: **you can debug it with
`zcat file.jsonl.gz | head`.** In a four-week project, being able to answer "is
the data I recorded actually correct?" in one shell command is worth far more
than disk space. Measured on real data:

```
696 KB raw  ->  175 KB compressed     (~4x, and market data JSON is repetitive)
```

45 seconds of one symbol is 175 KB, so an hour is ~14 MB. Disk is not the
constraint.

Gzip level 1, not the default 6, because compression runs on the live data
path. Level 1 gets most of the ratio for a fraction of the CPU. We are
recording, not archiving.

## 3. Record raw bytes, never decoded structs

`d` holds the exchange's bytes untouched. `main.cpp` records **before** it
decodes, and records messages it *cannot* decode.

This is the single most important property of the format. If our decoder has a
bug, we fix the decoder and re-run the same files. Had we recorded decoded
structs, that bug would be permanently baked into every recording we ever made,
and no amount of later cleverness could recover the truth.

**The recording is ground truth. Everything downstream is an interpretation.**

The same principle drives `decode_depth_event` being shared between live and
replay (Lesson 2 §1): replay must interpret bytes exactly as live did, or a
backtest is measuring a program that doesn't exist.

## 4. Timestamps

`now_ns()` uses `system_clock`, not `steady_clock`. They are not
interchangeable:

- **`system_clock`** is wall time. It can jump when NTP corrects it. It is the
  only clock comparable to a timestamp from *another machine* — such as the
  exchange's `E` and `T` fields.
- **`steady_clock`** never jumps. It is the only clock valid for measuring a
  duration.

Using the wrong one produces impossible-looking latency numbers, including
negative ones. We record wall time (to correlate with the exchange) and measure
replay speed with `steady_clock`.

Nanoseconds are stored as an `int64`, and the round-trip is tested:

```cpp
CHECK(records[1].t == 1'700'000'000'000'000'001LL);
```

That value needs 61 bits of mantissa; a `double` has 53. Parsing timestamps as
floats silently truncates the low digits — the exact digits that matter when
you are measuring microseconds.

### The discipline this sets up

From here on, **strategy code must never call the system clock.** It takes
"now" from the timestamp on the event it is processing. A strategy that reads
the wall clock behaves differently in replay than in live, which destroys the
only property that makes backtesting meaningful.

## 5. Determinism, and proving it

`hftreplay --twice` replays the file twice and compares an FNV-1a checksum over
every level of the final book:

```
  checksum     0x928d6643532c7418
  determinism  PASS  (second pass checksum 0x928d6643532c7418)
```

Determinism is easy to *assume* and easy to lose — one `unordered_map`
iteration, one uninitialised variable, one wall-clock read, and two runs of the
same data diverge. Then a backtest result becomes unreproducible and you cannot
tell an improvement from noise. Checking it mechanically costs one line.

The same checksum is useful later for reconciling our book against a fresh
snapshot at runtime.

## 6. Surviving a kill

Two defences, because recordings are long-running and get interrupted:

**Clean shutdown.** `hftbot` handles `SIGINT`/`SIGTERM`/`SIGALRM`, breaks out
of the read loop, and lets `Recorder`'s destructor close the gzip stream
properly. Note the subtlety in the catch block: a signal interrupts the blocking
`ws.read()`, which surfaces as a *network* exception. We check the stop flag
before concluding the network broke.

**Truncation tolerance.** `GzWriter::flush()` issues `Z_SYNC_FLUSH` every 250 ms,
ending the deflate block so everything written so far is recoverable. If the
process is `SIGKILL`ed anyway, the file ends mid-block; `GzLineReader` treats
that as end-of-file and sets `truncated()` instead of throwing.

Losing three hours of market data because the last line was incomplete would be
an absurd way to fail.

## 7. Layering, again

zlib lives behind `hft_io` exactly as Boost lives behind `hft_net`. `gzfile.hpp`
forward-declares `struct gzFile_s;` rather than including `<zlib.h>`.

```
  hft_core   book, sync, fixed point       no dependencies
  hft_net    Boost.Beast + OpenSSL         hidden
  hft_io     zlib                          hidden
```

`DepthSync` still knows nothing about any of it, which is why `test_core` still
runs with no network and no files.

## 8. Known limitation

Replaying 45 seconds left the book with 1685 bid and 1566 ask levels, grown from
a 1000-level snapshot. That is *correct* — the diff stream carries levels beyond
the snapshot's depth — but nothing ever prunes levels far from the mid, so a
multi-hour run grows unbounded.

It doesn't affect correctness near the touch, which is all a market maker quotes
against, so it is not urgent. Noted here rather than fixed, because unrecorded
known issues are how projects rot.

---

## Try it

```bash
./build/hftbot --symbol btcusdt --record data/btcusdt.jsonl.gz    # Ctrl-C to stop
./build/hftreplay data/btcusdt.jsonl.gz --twice
zcat < data/btcusdt.jsonl.gz | head -3
```

---

*Next: Lesson 4 — Fill simulation and queue position*
