# Limit Order Book: C++17
![CI](https://github.com/rvparin20/limit-order-book/actions/workflows/c-cpp.yml/badge.svg)

A single-instrument, price-time priority matching engine supporting **limit**, **market** and **cancel** orders. It is built for low, predictable latency: no heap allocation on the hot path, O(1) price-level access and O(1) cancels.

It ships with a textbook `std::map` / `std::list` implementation used both as a **benchmark baseline** and as a **correctness oracle**: both books are fed the same random order flow and must produce an identical trade stream.

## Build & run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/lob_tests              # unit tests + differential test vs baseline
./build/lob_bench 2000000      # 2M ops; optional 2nd arg = seed
```

No dependencies beyond a C++17 compiler (GCC or Clang) and CMake.

## Design

| Component | Choice | Why |
|---|---|---|
| Prices | `int64` ticks | No floating-point equality bugs |
| Price levels | Flat array per side, indexed by `price - min_price` | Level lookup is one subtraction, with no tree walk and no pointer chasing |
| Queue at a level | Intrusive doubly linked FIFO (`prev`/`next` inside the order) | Time priority for free; cancel is O(1) unlink with no search |
| Order storage | Pre-allocated `ObjectPool` with a LIFO free list | Zero `malloc`/`free` after start-up; recently freed slots stay cache-warm |
| Id → order lookup | Flat `vector<Order*>` | O(1) with no hashing |
| Best bid/ask | Cached index, scanned outward when a level empties | O(1) top of book; scan cost = tick gap to next level |
| Trade reporting | Template callback | Inlined by the compiler, no virtual call |

**Matching rules:** an incoming order matches against the best opposite price, oldest order first, and executes at the **resting (maker) order's price**. A limit order rests any unfilled remainder. A market order's unfilled remainder is discarded.

## Results

2M operations of synthetic flow (55% limit, 38% cancel, 7% market; 10% of limits cross the spread). Mid price follows a random walk. Run in a GitHub Codespace (GCC 13.3, `-O3`, seed 42):

| | Throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|
| **OrderBook** (pool + flat arrays) | **45.0 M ops/s** | **45 ns** | 150 ns | 440 ns |
| Baseline (`std::map` + `std::list`) | 12.2 M ops/s | 81 ns | 265 ns | 386 ns |

About **3.7x throughput** and about **1.8x lower median and p99 latency** than the standard-container baseline. Latency includes ~20 ns of `steady_clock` overhead per sample, and results vary by machine and run, so run `./build/lob_bench` yourself.

**Honest observation:** the p99.9 tail is comparable (440 ns vs 386 ns). Tail operations are mostly market and crossing orders that sweep several price levels, so the cost is the matching work itself rather than data-structure overhead. Further work would target that.

## Tests

- Rests when not crossing, price-time priority across levels, execution at the maker's price
- Partial fills, market orders larger than available liquidity
- Cancel: mid-queue, best-level empties (best price moves), already-filled, unknown id
- A cancelled order loses its queue priority if resubmitted
- Rejections: zero quantity, out-of-range price/id, duplicate id, pool exhausted
- **Differential test:** 200k random operations through both books, comparing every trade and the top of book after every operation

Also clean under `-fsanitize=address,undefined`.

## Limitations / next steps

- Fixed price range and dense order ids (the trade-off that buys O(1) lookup). A production book would map exchange ids through a hash table.
- Best-price scan is O(gap) after a level empties. A bitset over levels with `__builtin_ctzll` would make it O(levels/64) worst case.
- No order modify (amend quantity down keeps priority; price change = cancel + new).
- Single-threaded. Next step: feed it from a lock-free SPSC queue, i.e. the real exchange-gateway → matching-engine pattern.
- Measure with `rdtsc`, pin to a core, and use `perf stat` to count cache misses.
