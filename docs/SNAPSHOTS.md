# Phase 2C: snapshots and WAL-tail recovery

The project/snapshots branch starts at wal-recovery-verified. The matching engine remains unchanged. The exchange_durable CMake library provides snapshot serialization and is required when linking Exchange.

## API and boundary

Call Exchange::save_snapshot(path) between commands on the single writer thread. The method flushes the WAL, captures the current global sequence, and writes a complete snapshot. Recover with the optional final constructor argument:

    Exchange recovered(wal_path, config, Durability::EveryCommand, 100, snapshot_path);

The snapshot contains the immutable configuration, sequence, logical-clock floor, account positions and reservations, rate-admission history, all retained order IDs and lifecycle quantities, bid/ask FIFO order, market price, and callback count. The order book is reconstructed using captured open quantities in FIFO order with callbacks detached; original total quantities and fill history are retained.

Serialization uses explicit little-endian fields, a versioned type-256 frame, and CRC32. No pointers or native object layouts are persisted. Restore checks lifecycle consistency, account limits, reservation sums, rate timestamps, unique IDs, live-book membership, and canonical re-encoding.

A snapshot records its WAL prefix byte offset and a noncryptographic 64-bit prefix hash. Recovery requires matching configuration, sequence boundary and prefix hash. This detects accidental use with a different history; it is not authentication against malicious file modification.

## Publication and recovery

Publication writes a same-directory temporary file, synchronizes it, atomically renames it to the requested path, and synchronizes the parent directory. An existing published snapshot remains available until rename. An orphan temporary file is ignored on recovery and reused by the next save. WAL inode aliases are rejected before writing or truncating the destination/temporary file. Use application-owned directories and serialize snapshot calls with commands.

Recovery first validates the entire WAL, loads the snapshot, then applies commands after the captured sequence. It still reads and checksums the WAL prefix and verifies the clock floor. It avoids executing prefix commands, but does not eliminate prefix I/O. Log rotation, prefix pruning, background snapshotting and incremental snapshots are not implemented. WAL and snapshot files each have a 512 MiB limit.

An invalid requested snapshot fails closed. The caller may explicitly choose full WAL recovery instead. A complete command may survive without a prior client acknowledgment, as documented in WAL_RECOVERY.md.

## Verification

Fourteen focused cases cover empty snapshots, multi-symbol state and tail replay, partial fills, cancellation and replacement, subsequent FIFO matching, terminal duplicate IDs, rate history, corrupt/truncated snapshots, wrong WAL history, snapshots ahead of WAL, orphan temporary files, WAL aliases, incomplete WAL tails, and refusal to save without WAL.

Actual fork/SIGKILL tests interrupt snapshot publication after writing, after fsync, and after rename. Recovery from the old or new published snapshot plus the WAL must exactly match full replay. These are process-crash tests, not power-loss simulation.

The complete matrix runs GCC Debug/Release/ASan/UBSan and Clang Debug, including all previous tests:

    docker run --rm -v "$PWD:/workspace" liquibook-diagnostics:ubuntu24.04 bash scripts/verify_durability.sh snapshots

## Recovery benchmark

The Release benchmark creates 1,000 resting orders, applies 50,000 replacements, saves a snapshot at sequence 51,000, then applies 1,000 more replacements. Deterministic timestamps advance by one second per command, keeping rate history bounded. The final state contains 1,000 live orders; this intentionally measures a history-heavy workload. A workload retaining millions of terminal order IDs would produce a larger snapshot.

Seven trials alternate full-replay and snapshot-recovery order. Every trial compares complete canonical state. Timing covers construction, file reading/checksums, restoration/replay, and recovery fsync; it excludes the subsequent comparison and destructor. Files reside in the container's /tmp filesystem, and repeated runs mostly measure warm-cache behavior. Snapshot creation time includes WAL flush, prefix scan, serialization, file fsync, rename and directory fsync. This is a local Docker VM measurement, not a general storage guarantee.

    docker run --rm -v "$PWD:/workspace" liquibook-diagnostics:ubuntu24.04 build/snapshots-Release/snapshot_benchmark

Raw results and measured medians are stored in results/snapshots.

### Local result

| Metric | Measured value |
| --- | ---: |
| Median full replay | 24.848 ms |
| Median snapshot plus tail | 24.501 ms |
| Ratio of median recovery times | 1.014 |
| Snapshot creation, single observation | 32.774 ms |
| WAL size | 3,588,104 bytes |
| Snapshot size | 58,232 bytes |

This is only a 1.4% median difference and is not evidence of a meaningful recovery speedup. The implementation still scans and checksums the full WAL, verifies the prefix hash and clock, and reconstructs the live book. Those remaining steps are plausible constraints, not separately profiled causal measurements. The snapshot correctness goal is met; the recovery performance goal remains limited by this design. A future benchmark should evaluate indexed/segmented WAL recovery with a verified snapshot anchor before claiming a material improvement. No log pruning was added to improve this result artificially.
