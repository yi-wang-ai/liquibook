# Liquibook reproduction

## Scope
Upstream: https://github.com/enewhuis/liquibook
Pinned source commit: 2427613b32f1667abae68a01df6af9ba8270f8e7
Local branch: project/reproduction

The matching engine and upstream workloads are unchanged. Added CMake and an Ubuntu Docker environment. Fixed only the test listener signature: int32_t became int64_t to match OrderListener::on_replace, with override to catch future mismatches. The original failed compilation remains in results/20260930T233630Z.

## Run
From this repository:
    docker build -t liquibook-repro:ubuntu24.04 .
    docker run --rm --mount type=bind,source="$PWD",target=/workspace liquibook-repro:ubuntu24.04

The runner builds Release/C++11, runs all upstream unit tests, runs all three manual-order scripts, and executes throughput and latency workloads. It records test failures, continues collecting benchmark evidence, and returns the failing test status. No failing test is excluded.

## Current test results
153 of 157 upstream unit tests pass; 13 of 2671 assertions fail.
Failing cases:
- TestTradeCallbacks
- TestStopOrdersOffMarketNoTrade
- TestStopMarketOrdersTradeWhenStopPriceReached
- TestStopOrdersCancelBeforeTrigger

The trade test expects total cost where the callback interface now specifies fill price. This needs a separate contract review before changing the assertion.
Stop-order failures involve acceptance and cancellation of held stop orders; root cause and intended lifecycle remain to be investigated.
The README labels STOP support experimental through the pinned commit message.

The scripted order-entry CTest passes. The optional QuickFAST market-data publisher/subscriber is outside this first milestone; it is not built or claimed as reproduced.

## Benchmark limits
Host: Apple M4 Pro, macOS; execution: Ubuntu 24.04 ARM64 in Docker.
Environment, versions, build flags, and raw results are saved in each results directory.
Upstream throughput uses clock() process CPU time and a seeded workload. It includes a timer call per insertion and retries with larger preallocated arrays.
Upstream latency uses CLOCK_REALTIME. Its interval measurements include instrumentation; they are not a monotonic production end-to-end latency benchmark.
Results from this host are not directly comparable with the upstream historical Intel laptop numbers.
Percentiles can be generated with:
    python3 scripts/summarize_latency.py results/<run>/latency.txt

## Next milestones
1. Diagnose the four failing cases against the callback and stop-order contracts. Add focused regression evidence before fixing engine behavior.
2. Add a deterministic benchmark with a monotonic clock, explicit workload configuration, repeated runs, and correctness checks.
3. Add pre-trade validation and risk limits.
4. Add WAL, snapshots, and crash-recovery tests.
5. Add QuickFIX connectivity and asynchronous PostgreSQL audit storage.

Do not claim a production exchange system or verified reliability before these milestones are complete.

## Recorded first complete run
Evidence: results/20260930T233722Z
All three manual-order scripts completed. Unit suite remains failing as listed above.
Upstream throughput, one-second CPU-time windows:
- Five-level depth: 3,151,084 insertions per second
- BBO: 3,365,447 insertions per second
- No depth: 3,208,052 insertions per second
Raw latency samples: 100,000 orders per configuration. See latency_summary.json.
These are one-run observations, not a performance improvement claim.
