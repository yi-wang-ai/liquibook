# Verified project endpoints

| Phase | Tag | Evidence |
| --- | --- | --- |
| Untouched matching baseline | reproduction-baseline | [Reproduction](REPRODUCTION.md) |
| Reproduction and defect fixes | reproduction-verified | [Failure analysis](FAILURE_ANALYSIS.md) |
| External pre-trade risk | risk-controls-verified | [Risk controls](RISK_CONTROLS.md) |
| Risk profiling and allocation optimization | risk-controls-perf-verified | [Performance](RISK_PERFORMANCE.md) |
| WAL and deterministic crash recovery | wal-recovery-verified | [WAL](WAL_RECOVERY.md) |
| Snapshots and tail recovery | snapshots-verified | [Snapshots](SNAPSHOTS.md) |
| Direct segmented recovery and safe retention | snapshot-recovery-optimized | [Segmented recovery](SEGMENTED_RECOVERY.md) |

Each later phase starts from the previous verified endpoint. No matching-engine code has changed since reproduction-verified. Frozen performance measurements belong to their stated workload, compiler, container and tag; they should not be mixed into an implied universal latency claim.

The current complete suite contains the upstream tests, seven independent matching regressions, 30 risk cases, 30 WAL cases, 14 snapshot cases, and 27 segmented-recovery cases. The original 157 registered upstream cases include a documented disabled-body placeholder; see the reproduction evidence rather than interpreting registration count as 157 independent checks.

Hardware perf counters were denied by the Docker VM. The risk performance report includes controlled ablations, allocation/lookup/rehash counters and paired timing measurements instead.

The durability contract distinguishes pending application from durable acknowledgment. Recovery compares complete deterministic state, including FIFO, lifecycle history, positions, reservations and rate windows. Tests include real process kills; power-loss behavior has not been simulated.

Current limits: single serialized writer, per-instrument account risk, retained terminal IDs, synchronous snapshots, and no automatic format migration. The segmented API supports safe retention and WAL histories larger than 512 MiB; the original single-file API keeps its documented limits. QuickFIX networking and PostgreSQL audit persistence remain future phases.
