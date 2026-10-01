# Phase 2B: WAL and deterministic recovery

Branch project/wal-recovery starts at risk-controls-perf-verified. Matching code in src is unchanged. The implementation is in extensions/durability/wal.h. Snapshots are a subsequent phase.

## Contract

Validation occurs before logging. Each accepted NewOrder, CancelOrder, or ReplaceOrder receives the next global sequence number. The command is appended before it is applied to Liquibook. Every-command mode calls fsync before applying, and returns acknowledged only after successful application. Any append, synchronization, or application failure halts the writer.

A Receipt distinguishes a successful risk/application result, applied sequence, durable sequence, and acknowledged status:
- NoWal: ephemeral application, no durable acknowledgment.
- Buffered: successful calls are pending, not acknowledged. flush persists the batch and returns its cumulative durable watermark.
- Group: every configured group boundary syncs all preceding records. Earlier receipts remain pending; the new watermark acknowledges the whole prefix.
- EveryCommand: each successful command is durable before its acknowledgment.

The destructor does not implicitly flush pending commands. A client must track flush/group watermarks. An unacknowledged but completely written command may survive and replay after a process crash. This is an intentional uncertain-outcome case; no network exactly-once protocol is claimed.

## Format and configuration

The first frame contains the immutable symbol/account configuration at sequence zero: account IDs, initial positions, all risk limits and price bands. Reopening requires an exact configuration match. Price and quantity are integer fields; no C++ object layout, pointer, floating-point value, or padding is written.

Each little-endian frame contains:
- Four-byte LWB1 magic.
- Two-byte version, currently 1.
- Two-byte type.
- Four-byte payload length.
- Eight-byte sequence.
- Payload.
- Four-byte standard IEEE CRC32 over header and payload.

Command payloads are exactly 45 bytes: symbol ID, order ID, account ID, side, signed price, signed quantity, and logical timestamp. The checksum has a known-vector test. All header values, sizes, sequence continuity, checksum and command semantics are checked on replay.

The first version uses one append-only wal-000001.log file, bounded to 512 MiB; replay reads it into memory. It does not rotate, compact, or prune logs. These are explicit implementation limits. Record/schema or matching-semantics changes require a versioned migration, not silent reuse of the same version.

## Recovery and exact state

Recovery begins from configured accounts and empty books, then validates and applies each complete command in sequence. Duplicate or missing sequences, unsupported versions/types, malformed lengths, complete-record checksum failures, configuration mismatch, and invalid replayed commands fail closed.

Only an incomplete final frame is discarded. The writer truncates to the last valid boundary and synchronizes the recovered prefix before accepting further work. It does not skip a corrupted complete record or search for another magic marker.

Deterministic state includes:
- Per-symbol open order prices, quantities and FIFO ordering, from which depth is derived.
- All retained order IDs, total/filled/remaining quantities and lifecycle states.
- Account positions, buy/sell reservations, rate-admission queues and last admission timestamps.
- Market price, callback event count, global sequence and logical-clock floor.

Tests compare the complete canonical serialization, not only its noncryptographic 64-bit FNV-style hash. Crash tests before in-memory application compare recovery with the valid log prefix: recovery may correctly include a logged command that had not yet reached the book or client.

Exchange::now continues a logical nanosecond clock from the recovered command-time floor. It uses elapsed steady-clock time in the current process and conservatively does not credit unknown downtime. Supplied command times must belong to this logical clock domain; do not pass a reset raw host clock after restart.

Symbols have separate books and per-instrument account risk. Order IDs are unique within a symbol/session. Sequences are global across symbols. Cross-instrument portfolio exposure is outside this layer.

## File and crash behavior

The WAL uses an exclusive nonblocking file lock, close-on-exec, regular-file checks, and no-follow open semantics. File creation synchronizes both the WAL and its parent directory. Writes handle short writes and interrupted writes. The lock is held through recovery and normal operation.

Thirty recovery tests include empty/new/multiple orders, partial fills, cancellation, replacement and repricing, positions/reservations, multiple symbols, rate history, terminal duplicate IDs, malformed records, large replay, pending versus durable acknowledgment, concurrent-writer rejection, and failure-induced halt.

The tests fork a child and send actual SIGKILL:
- After risk acceptance.
- Halfway through writing a record.
- After a complete WAL write.
- After fsync but before application.
- Inside fill, cancel and replace callbacks, after the book has mutated but before risk state is updated.
- After matching, including a partial fill.
- After the 53rd command of a longer sequence.

Process-kill tests do not simulate power loss, drive-cache failure, or a lost virtual-machine disk. fsync durability remains subject to the storage stack honoring its contract. Benchmark files are on the container's /tmp filesystem; results are not a storage-device certification.

## Validation and reproduction

    docker run --rm -v "$PWD:/workspace" liquibook-diagnostics:ubuntu24.04 bash scripts/verify_durability.sh wal
    docker run --rm -v "$PWD:/workspace" liquibook-diagnostics:ubuntu24.04 python3 scripts/benchmark_wal.py

The matrix includes GCC Debug/Release/ASan/UBSan and Clang Debug. It also runs all previous tests.

The benchmark uses identical 5,000-command crossing streams in each mode, three repetitions, alternating mode order. It verifies exact recovered state and identical hashes across modes. Submission throughput excludes the final explicit flush; durable throughput includes it. Durable-ack latency measures each command from submission to the observed durable watermark, including group waiting. Buffered commands wait for the final flush. NoWal has no durable acknowledgments; zeros in raw JSON represent unavailable durable metrics, not instantaneous durability.

## Measured results

| Mode | Submit orders/s | Submit p99 ns | Durable orders/s | Durable-ack p99 ns | Replay seconds |
| --- | ---: | ---: | ---: | ---: | ---: |
| none | 8,815,108 | 542 | N/A | N/A | 0.000000 |
| buffered | 750,610 | 2,084 | 680,704 | 7,259,041 | 0.003069 |
| group100 | 170,738 | 24,208 | 170,634 | 750,584 | 0.003129 |
| sync1 | 3,100 | 1,246,000 | 3,100 | 1,246,000 | 0.003137 |
