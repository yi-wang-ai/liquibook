# Phase 2A.1: risk overhead
The risk-controls-verified tag is unchanged. Hardware perf counters are unavailable in this Docker VM: perf reports “No permission to enable cycles event.” No host security settings were relaxed. Raw evidence is in results/risk-profile/perf.txt. Cycles, instructions, cache misses and branch misses are therefore unmeasured.

The existing path has no locks, no string keys, and no extra timestamp acquisition: account/order IDs are integers and event timestamps are supplied by the caller. Both benchmark paths use identical timing instrumentation.

Benchmark-only generated headers incrementally enable checks. They are never used by production targets and intermediate stages intentionally lack complete risk semantics. All retain account lookup/lifecycle scaffolding; this isolates additions within that scaffold, not pure instruction-level cost. Five repetitions per stage use the same one-million-order workload and outcome hashes. The results are medians; nonmonotonic rows reflect compiler/allocator/layout effects and timing noise, so stage differences must not be summed as causal cycle counts.

| Configuration | Crossing orders/s | Change vs bare | Resting orders/s | Change vs bare |
| --- | ---: | ---: | ---: | ---: |
| Bare matching adapter | 8,615,141 | 0.0% | 3,185,276 | 0.0% |
| Quantity/price + account lookup | 6,964,718 | -19.2% | 3,119,003 | -2.1% |
| Add notional | 8,404,659 | -2.4% | 3,087,484 | -3.1% |
| Add duplicate-ID set | 5,613,775 | -34.8% | 1,849,682 | -41.9% |
| Add position checks | 5,106,435 | -40.7% | 1,827,008 | -42.6% |
| Add rate limiter | 4,592,185 | -46.7% | 1,656,108 | -48.0% |
| Full reservations | 4,564,281 | -47.0% | 1,728,667 | -45.7% |
| Rate-check experiment (not retained) | 5,166,568 | -40.0% | 1,613,501 | -49.3% |

## Retained optimization
The accepted-ID set duplicated the retained order registry exactly, including terminal IDs. RiskGateway now uses that order registry for duplicate checking and still rolls back its insertion when admission allocation fails. PreTradeRiskEngine receives the duplicate result from its owner. The redundant set and its allocations are removed; rejection precedence and session-long duplicate semantics are preserved.

Separate seven-repeat paired trials compare the exact frozen header with the optimized header:
- crossing: 4,562,857 to 5,833,677 orders/s; improvement 27.9%.
- resting: 1,697,081 to 2,958,350 orders/s; improvement 74.3%.

Allocation instrumentation is a separate untimed diagnostic run; it counts global C++ new/new[] calls, allocated bytes, and actual bucket-count changes around registry insertion. It does not count every libc allocation. The counter files also count account map lookups. Removing the duplicate set removes one node allocation per accepted order plus its rehashes. Account lookup, admission deque allocation and reservation work remain.

All 30 risk cases/325 assertions and the Phase 1 suite pass in Debug, Release, ASan, UBSan, and Clang. Logs are in results/risk-profile/tests. The rate-check experiment is not retained because its results were inconsistent between workloads.

Reproduce in the diagnostic Docker environment:
    python3 scripts/profile_risk.py
    python3 scripts/profile_optimized.py
    python3 scripts/profile_counters.py
The first script reads the frozen header with git show and generates isolated build/profile variants. The optimized script reads the current performance endpoint header; use risk-controls-perf-verified to reproduce that endpoint.
