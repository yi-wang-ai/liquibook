# Snapshot-aware segmented WAL

Branch: project/snapshot-aware-wal, based on snapshots-verified.
Verified endpoint: snapshot-recovery-optimized.

## API and compatibility

The new SegmentedExchange API is in extensions/durability/segmented.h and links against exchange_durable:

    SegmentedExchange exchange("wal-directory", config, 100000,
                               Durability::Group, 100);
    auto receipt = exchange.execute(command);
    exchange.save_snapshot(); // Durable publication, verification, then retention.

The directory is owned by one serialized writer, protected by an exclusive LOCK file. The existing single-file Exchange API and its tests remain available; its files are not silently interpreted as segmented WAL. There is no automatic migration. The new format uses distinct frame types: segment header 512, snapshot 513, manifest 514. Command frames retain their explicit integer fields, sequence and CRC32.

MANIFEST holds a random 128-bit log identity, fixed segment capacity, and exact immutable account/symbol configuration. Each segment header binds its segment ID, capacity and starting sequence to that identity. Segment files are named wal- followed by a twenty-digit ID and .log. Capacity is configurable from one to one million commands per segment.

Rotation synchronizes the preceding segment, writes and synchronizes the new header to a temporary file, renames it, and synchronizes the directory before appending commands. Header publication therefore cannot expose a partially initialized active segment. Buffered and group modes can advance their durable watermark at rotation; callers must use the returned watermark. A failed append, sync, application, or snapshot operation halts that writer instance.

## Direct recovery

Each snapshot stores its sequence, log identity, next-command segment ID, byte offset, complete engine/risk state, version and checksum.

The fixed-size command format makes the offset independently checkable from sequence and segment capacity. When a snapshot falls exactly on a segment boundary, saving it first publishes the next empty segment. At an interior boundary it records the byte immediately after the last included command.

Recovery:
1. Read and validate MANIFEST.
2. Try snapshot generations newest first. Check snapshot CRC, identity, anchor arithmetic, anchor segment header and file size, then validate and restore complete state.
3. Seek directly to the saved offset. Read, checksum, decode and apply only subsequent command records.
4. Check contiguous segment IDs and global command sequences. Only an incomplete final record in the final segment is truncated.
5. Synchronize the recovered active segment and resume the logical clock from the recovered time floor.

Directory enumeration examines filenames, not prefix contents. No prefix hash or prefix command scan is required. The snapshot and matching log identity become the trusted recovery anchor. Historical corruption before the anchor is intentionally outside the active recovery path; use a separate archival audit when historical integrity is required. CRC32 and random identity binding detect accidental corruption/mixing, not adversarial tampering.

RecoveryStats reports actual bytes returned by file reads, decoded command count, selected snapshot sequence, snapshot selection/restore time, tail time, and total startup time. Bytes exclude directory metadata and filesystem/kernel read-ahead. Decoded records count commands, not manifest/header fields. Tail time includes segment checks and recovery fsync. Total includes manifest handling and directory enumeration; it is not just the sum of the two phase timers.

## Durable retention and fallback

save_snapshot writes a temporary generation, fsyncs it, atomically renames it, fsyncs the directory, reopens the published snapshot, reconstructs its state independently, and compares complete canonical state.

Only then can retention run:
- Keep the newest two valid snapshot generations.
- Remove older snapshot generations and synchronize the directory.
- Delete only complete segments strictly before the older retained snapshot's anchor segment.
- Synchronize the directory again.

With fewer than two valid snapshots, retain the entire WAL for full-replay fallback. The anchor segment is retained even when only its trailing bytes are needed. Partial-segment prefix compaction is not implemented.

A corrupt newest snapshot falls back to the previous valid generation and its retained WAL. If all snapshots are unusable, full replay is attempted only when the required prefix still exists. Otherwise recovery fails closed. Tail corruption is always fatal; it does not trigger snapshot fallback that could hide damaged commands.

Crash interruption during deletion may leave extra old files. It cannot delete either retained generation's required tail. Repeated saves complete retention. Temporary files are ignored by recovery and replaced on the next publication.

Full replay for auditing or paired benchmarks is explicitly requested with the final constructor flag:

    SegmentedExchange full("wal-directory", config, 100000,
                           Durability::Buffered, 100, true);

It fails if retention has removed the prefix. Benchmark generation calls save_snapshot(false) to preserve the complete history for this comparison.

## Verification

27 focused tests cover:
- Boundary/interior snapshots, empty tails and several tail segments.
- Exact full-state equality, FIFO behavior, fills, replacements, cancellation and multiple symbols.
- Retained fallback, corrupt snapshots, three-generation retirement and missing prefixes.
- Truncated first post-snapshot records, corrupt tails, segment gaps and truncated sealed segments.
- Skipped-prefix corruption, configuration/capacity mismatch, writer locking, group watermarks and rejected commands.
- Actual SIGKILL during snapshot write, file sync, rename, directory sync, readback verification, and before/during/after segment deletion.
- Actual SIGKILL after rotation and at risk/write/sync/matching boundaries.
- Logical-clock advancement after either recovery path and recovery counters remaining unchanged during later snapshot saves.

All existing tests run alongside these cases. Process-kill tests do not emulate power loss or prove that a storage device honors fsync.

    docker run --rm -v "$PWD:/workspace" liquibook-diagnostics:ubuntu24.04 bash scripts/verify_durability.sh segmented

## Benchmark method

The deterministic stream creates 1,000 resting orders and then replaces them repeatedly, alternating prices. Command times advance by one second, so rate history stays bounded. State size is deliberately stable while WAL history grows. Workloads with growing terminal-ID history would have larger snapshots and different recovery costs.

Cases are 100k commands at 90%, 1M at 90%, 10M at 90%, and 10M at 99%. Each uses 100k-command segments, one snapshot, no retirement, and three paired recovery trials with alternating full/snapshot order. Every recovered canonical state and hash must match, and decoded command counts must match the expected full or tail count.

Files reside in the Linux container's /tmp filesystem. Trials are mostly warm-cache; these results demonstrate reduced recovery work on this machine, not cold-storage or universal performance. Snapshot creation is observed once per generated case. Generation uses buffered mode with explicit flush and synchronized segment rotation; it is not a command-admission throughput benchmark.

    docker run --rm -v "$PWD:/workspace" liquibook-diagnostics:ubuntu24.04 python3 scripts/benchmark_segmented.py

Raw CSVs and summary JSON are in results/segmented.

## Measured results

All 13 CTest entries pass in GCC Debug, Release, ASan, UBSan and Clang Debug. The new suite contributes 27 cases and 583 assertions, including real process kills. Matching-engine sources remain identical to reproduction-verified.

Median times from three paired trials on the final build:

| Commands | Snapshot | Full recovery ms | Snapshot load ms | Tail ms | Total snapshot recovery ms | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 100,000 | 90% | 65.292 | 0.833 | 6.031 | 6.847 | 9.54 times |
| 1,000,000 | 90% | 500.278 | 0.722 | 47.377 | 48.201 | 10.38 times |
| 10,000,000 | 90% | 5319.557 | 0.831 | 516.821 | 517.758 | 10.27 times |
| 10,000,000 | 99% | 4926.826 | 0.759 | 46.963 | 47.718 | 103.25 times |


| Commands / snapshot | Full bytes read | Snapshot recovery bytes read | Full commands decoded | Tail commands decoded |
| --- | ---: | ---: | ---: | ---: |
| 100,000 / 90% | 6,900,184 | 748,404 | 100,000 | 10,000 |
| 1,000,000 / 90% | 69,000,688 | 6,958,404 | 1,000,000 | 100,000 |
| 10,000,000 / 90% | 690,005,728 | 69,058,908 | 10,000,000 | 1,000,000 |
| 10,000,000 / 99% | 690,005,728 | 6,958,404 | 10,000,000 | 100,000 |

The byte and decode counters establish the architectural change independently of timing noise: a 99% snapshot on the 10-million-command log reads about 6.96 MB instead of 690.01 MB and decodes 100,000 rather than 10,000,000 commands. Total recovery fell from about 4.93 seconds to 47.72 ms in this workload. The approximately 103-times timing ratio is local to this benchmark; it is not a universal guarantee.

Snapshot creation took about 6–15 ms across the four generated cases (one observation per case). The old single-file snapshot format remains available for compatibility, but the optimized endpoint uses SegmentedExchange and never scans prefix records during successful snapshot recovery.

## Remaining limits

Snapshots are synchronous; all operations use one writer thread. Terminal order IDs remain retained in engine state. Recovery buffers one segment at a time; each segment is bounded, so total WAL history can exceed the old 512 MiB single-file limit. Each snapshot remains limited to 512 MiB. Retention uses deletion, not archival export. There is no automatic format migration, malicious-file authentication, background corruption scrub, or cross-instrument portfolio risk.
