# Phase 2A: pre-trade risk controls

Phase 2A is complete and frozen by the local tag risk-controls-verified on branch project/risk-controls. The branch starts at reproduction-verified (585a00a). No upstream src file changed. WAL, snapshots, networking, and PostgreSQL remain pending.

## Architecture and supported scope

The new code is in extensions/risk/risk_gateway.h.

Incoming OrderRequest is checked by PreTradeRiskEngine. RiskGateway reserves exposure before calling Liquibook and consumes synchronous fill, cancel, and replace callbacks to update account and order state. RiskGateway owns its book and exposes a value snapshot rather than mutable book pointers.

This implementation is one instrument per gateway, with serialized calls from one thread. It supports positive-price limit orders, cancellation, and replacement. Prices are integer ticks in one configured unit; quantities are integer units; notional uses their product in those same configured units. Market, stop, IOC, and AON orders are outside this interface. It does not calculate futures contract multipliers, margin, cash balances, portfolio offsets, or self-trade prevention.

The risk engine validates without mutation. submit always revalidates and then reserves; a separate validate call is advisory and never authorizes a later unchecked submission.

## Rules

| Check | Exact policy |
| --- | --- |
| Order ID | Accepted IDs are globally unique within the gateway session, across accounts. Filled and cancelled IDs remain unavailable. A risk-rejected ID can be retried. |
| Quantity | Signed input must be positive and within the account's maximum order quantity. |
| Price | Signed input must be positive and within the inclusive account price band. |
| Notional | Limit price times total order quantity must fit the per-order cap. A division-based comparison prevents multiplication overflow. |
| Position | Filled position plus all remaining buys must fit the long limit. Filled position minus all remaining sells must fit the short limit. Opposite pending orders are not netted. |
| Rate | Accepted submissions and replacements share a rolling one-second account window. An admission exactly one second old expires. Rejections consume no rate quota. Cancellation is always available regardless of quota. |

Limits are configured when an account is created. Invalid limits, duplicate account registrations, and initial positions outside limits throw invalid_argument.
max_short_position is a nonnegative magnitude. Quantity is capped at INT32_MAX because upstream replacement logic narrows open quantity to int; exceeding that supported domain is rejected at configuration time. Position projections use widened integer arithmetic on GCC/Clang to avoid signed overflow.

The caller supplies monotonic nanoseconds; steady_now() provides the production clock. Tests and benchmarks inject a deterministic clock. Timestamps must not precede that account's last accepted admission. These timestamps are trusted application clock values, not client-supplied wall-clock timestamps.

## Account lifecycle

- Accepted buy/sell quantities reserve the corresponding side's exposure.
- Each fill releases that quantity from both orders' reservations and updates both accounts' signed positions.
- Partial cancellation releases only the unfilled remainder; the filled position stays unchanged.
- Replacement quantity is the new total, including historical fills. Only its new unfilled remainder is reserved, with the old remainder removed during the check. The quantity must exceed already filled quantity; use cancel to remove the remainder entirely.
- Replacement rechecks quantity, price, notional, position, clock, and rate. Its notional cap is conservatively applied to the entire amended total at the new limit price.
- Unknown IDs, commands from the wrong account, and commands on terminal orders are rejected before the book is called.
- On any validation rejection, the book, callbacks count, order state, account exposure, ID registry, and admission history are unchanged.

Allocation failure before book submission propagates without submitting. Unexpected matching-engine rejection or callback exceptions halt subsequent gateway commands; the wrapper does not claim rollback or crash recovery after an engine failure. Liquibook catches callback exceptions internally, so the gateway latches its logger notifications to detect those failures.

Order objects and accepted IDs are retained for the session. Memory therefore grows with accepted order history. This is deliberate for exact ID uniqueness and observable order state in Phase 2A; no durable or bounded historical retention scheme is claimed.

## Correctness evidence

Thirty focused Boost cases, containing 325 assertions, pass in:
- GCC Debug and Release.
- GCC AddressSanitizer and UndefinedBehaviorSanitizer, run separately.
- Clang Debug.

Each configuration also runs the Phase 1 suite and seven standalone regressions. All ten CTest entries pass. The upstream suite still has 2671 passing assertions and the previously documented disabled-body placeholder among its 157 registered cases.

Risk tests cover valid orders, all six checks and boundaries, multiplication overflow, extreme positions, duplicate active and terminal IDs, read-only validation, account isolation, backwards timestamps, partial fills, cancellation, size increases/decreases, repricing into a fill, replacement rejection, and simultaneous buy/sell reservations. Rejection checks compare book snapshots and account state; replacement checks also verify unchanged order quantity.

Raw logs and machine-readable verification are in ../results/risk-controls/. scripts/check_risk_evidence.py verifies the case names, assertion totals, sanitizer output, paired benchmark hashes, and unchanged upstream src.

## Benchmark method

The executable benchmarks/risk_benchmark.cpp runs one mode per fresh process.

Bare mode uses the unchanged Liquibook matcher, the same OrderRecord, an owning ID map, and fill callbacks that update order outcomes and account positions. Risk mode uses RiskGateway with all six checks, rate-window storage, the accepted-ID set, and exposure callbacks. Thus this measures the incremental full risk wrapper over a comparable owning matching adapter, not the cost of a single predicate or a header-only function in isolation.

Each mode receives exactly the same generated request array:
- One million orders, fixed seed 20261001.
- Crossing workload: paired sell/buy orders at the same random price and quantity, fully executing each pair.
- Resting workload: noncrossing buys and sells that accumulate in the book.
- Two accounts, alternated across pairs. Limits are permissive enough that every generated order is accepted.
- Injected event time advances by ten microseconds per request, independent of measured runtime.
- Seven measured repetitions per workload/mode; mode ordering alternates between pairs.
- One untabulated warm-up process per workload/mode.
- Both request hashes and final per-order/account outcome hashes must match for every pair.

Request generation, account setup, and result checking are outside the timed region. Per-order latency uses steady_clock around submission and includes order allocation, matching, and synchronous callbacks. Throughput measures the entire submission loop, including timing instrumentation. CPU is process user plus system time for that loop; utilization is normalized to one CPU core. Peak RSS is the Linux process high-water mark, including requests, latency samples, and retained order/risk state.

Reported percentiles are medians of seven per-run percentiles, not percentiles pooled across runs. Memory and CPU values are also per-run medians. The raw CSV preserves every trial and throughput ranges.

Environment: Ubuntu 24.04 ARM64 in Docker on an Apple M4 Pro, GCC 13.3, Release with -O3 -DNDEBUG and C++11. No CPU affinity or real-time scheduling was used. Virtualization, allocation, timer granularity, and host scheduling affect these values. They are local workload observations, not production latency guarantees or comparisons against upstream's historical laptop benchmark.

The 200,000-order pilot remains in ../results/risk-controls/pilot-200k. Final results below use the longer one-million-order runs.

## Final benchmark results

| Workload / mode | Orders per second | p50 ns | p95 ns | p99 ns | p99.9 ns | CPU seconds | CPU utilization | Peak RSS KiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| crossing / bare | 9,227,316 | 42 | 84 | 666 | 1291 | 0.108358 | 100.00% | 170,388 |
| crossing / risk | 5,030,842 | 84 | 167 | 709 | 1750 | 0.198734 | 100.00% | 213,768 |
| resting / bare | 3,580,550 | 208 | 459 | 792 | 1917 | 0.274563 | 99.99% | 264,080 |
| resting / risk | 1,858,397 | 209 | 625 | 917 | 4375 | 0.533961 | 99.97% | 307,464 |

- Crossing: median throughput reduced by 45.5%; median per-run p99 increased by 43 ns.
- Resting: median throughput reduced by 48.1%; median per-run p99 increased by 125 ns.

The risk layer costs additional time and memory in both workloads. These figures include its duplicate-ID registry and reservation bookkeeping. No performance improvement over bare matching is claimed.

## Reproduce and inspect

From the repository:

    docker build -t liquibook-repro:ubuntu24.04 .
    docker build -f Dockerfile.diagnostics -t liquibook-diagnostics:ubuntu24.04 .
    docker run --rm -v "$PWD:/workspace" liquibook-diagnostics:ubuntu24.04 bash scripts/verify_risk.sh
    docker run --rm -v "$PWD:/workspace" liquibook-diagnostics:ubuntu24.04 python3 scripts/benchmark_risk.py
    python3 scripts/check_risk_evidence.py

These commands overwrite the named Phase 2A result files; preserve them separately when comparing another environment. source-commit.txt identifies the tested implementation; benchmark-source-commit.txt also includes the final workload count and Python runtime setup. test-image-id.txt and image-id.txt record the test and benchmark image IDs respectively; the latter adds the Python benchmark runner.

The next phase is WAL and crash recovery, followed by snapshots. It has not started in this branch.
