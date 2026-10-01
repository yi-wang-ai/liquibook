# Phase 2E: asynchronous PostgreSQL audit store

PostgreSQL is an audit and query projection. It is not part of the matching transaction and it is not the recovery authority. The segmented business WAL and snapshots remain the source of truth for orders, positions, reservations and sequence state.

## Data path

```text
FIX request
    |
    v
single-owner gateway worker
    |
    +--> adapter intent journal (fsync)
    +--> risk + Liquibook + business WAL (fsync)
    +--> execution reports
    |
    v
derive immutable audit events
    |
    v
in-memory enqueue
    |
    v
audit worker --> checksummed durable spool (fsync) --> PostgreSQL transaction
```

The matching worker only derives records and moves them into an in-memory queue. The audit worker performs every spool write, `fsync`, libpq call, retry and acknowledgment. No spool or database I/O holds the producer mutex.

The local spool closes the gap between an in-memory queue and PostgreSQL. Its event and acknowledgment frames use the same checksummed frame format as the other durable extensions. A binding record prevents a spool from being opened with a different gateway configuration. The file has one exclusive writer, rejects checksum or sequence corruption, and truncates only an incomplete final frame.

## Delivery semantics

Delivery is at least once from the spool and idempotent in PostgreSQL:

- Each derived record has a deterministic event ID based on the durable adapter-request ordinal, category and event index.
- A PostgreSQL batch commits in one transaction.
- Every table uses the event ID as its primary idempotency key and inserts with `ON CONFLICT DO NOTHING`.
- The spool records acknowledgments only after PostgreSQL commits and the acknowledgment frames are synchronized.
- A disconnect, transaction failure, process crash or database restart repeats the unacknowledged batch safely.
- Gateway recovery replays the adapter journal through the same derivation callback. Existing spool event IDs suppress local duplicates; independent duplicate delivery is suppressed by PostgreSQL.

There is no distributed transaction between matching and PostgreSQL. An accepted order does not wait for the database. During an outage, matching and durable FIX acknowledgments continue while the spool backlog grows. Database failure cannot roll back or override business state.

## Schema

`orders` is the current order projection. It contains the accepted external and internal identifiers, client, symbol, side, exact integer-tick price, original quantity, current status and creation coordinates.

`order_events` is the append-only lifecycle history for acceptance, fill, cancel and replace events. The durable request ordinal plus event index provides global replay order.

`trades` contains paired buy/sell fills with both order IDs, exact integer-tick price, quantity and event coordinates.

`risk_events` contains one decision per adapter request, including accepted, rejected and replayed decisions and the rejection reason.

Times are stored as integer nanoseconds. Prices never pass through floating point. Status changes in `orders` occur only when a new `order_events` row is inserted, so duplicate delivery cannot apply a projection update twice.

## Failure behavior

- PostgreSQL unavailable: reconnect every 100 ms; preserve events in the spool; continue matching.
- Connection lost during a batch: discard the connection and retry the whole transaction.
- One record fails in a batch: PostgreSQL rolls back the whole transaction; retry after the cause is removed.
- Process killed before spool persistence: recover the durable adapter request and derive the events again.
- Process killed after spool persistence: recover the unacknowledged spool records and retry them.
- Process killed after commit but before spool acknowledgment: repeat the records; primary keys make the repeat harmless.
- Incomplete spool tail: truncate to the last complete checksummed frame.
- Complete corrupt spool frame or mismatched binding: fail closed.

The spool is intentionally append only in this phase and has a 512 MiB safety limit. Production operation would require checkpoint and retention policy before indefinite use.

## Build and run

```bash
docker build -f Dockerfile.quickfix -t liquibook-quickfix:ubuntu24.04 .
docker build -f Dockerfile.postgres -t liquibook-postgres:ubuntu24.04 .
docker run --rm -v "$PWD:/workspace" liquibook-postgres:ubuntu24.04 \
  bash scripts/verify_postgres.sh
```

`BUILD_POSTGRES_AUDIT` defaults to `OFF` and enables the QuickFIX gateway when selected. The standalone server takes QuickFIX settings, a state root and a libpq connection string:

```bash
build/postgres-Release/audit_gateway examples/fix/acceptor.cfg run/exchange \
  "dbname=audit host=/var/run/postgresql connect_timeout=1"
```

The state root contains separate `gateway` and `audit` directories. Keep the gateway journal, business WAL/snapshots and audit spool together.

## Verification

The audit suite contains 21 cases and 199 assertions. It covers accepted and rejected orders, full and partial fills, cancel, replace, trades, multiple clients and symbols, global event order, restart replay, duplicate delivery from independent spools, terminated database connections, transaction rollback, a real PostgreSQL stop/start outage, ordered backlog drain, metrics, exclusive writers, binding mismatch, truncated and corrupt spool records, `SIGKILL` recovery and shutdown while the database is offline.

The existing 40-case QuickFIX suite remains unchanged and passes with the audit callback available but disabled. The complete CTest suite is run under GCC Debug, GCC Release, AddressSanitizer, UndefinedBehaviorSanitizer and Clang Debug.

The paired loopback benchmark compares the same closed-loop 5,000-order FIX workload with and without the asynchronous audit store. It records request throughput and latency, committed audit-event throughput, batch count, average batch size, maximum queue depth, maximum commit latency and final drain time. `audit_outage_benchmark` stops PostgreSQL for ten seconds, continues submitting FIX orders, restarts PostgreSQL and verifies the complete projection after backlog drain.

## Verified endpoint and measurements

Tag: `postgres-audit-verified`. Branch: `project/postgres-audit`.

All 15 CTest targets pass in the five configurations. The audit suite contributes 21 cases and 199 assertions. Matching-engine sources under `src` remain unchanged from `reproduction-verified`.

Median absolute results from five alternating Release pairs:

| Configuration | Requests/s | p50 ms | p95 ms | p99 ms | p99.9 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| FIX + durable engine | 863.3 | 1.126 | 1.559 | 2.956 | 5.771 |
| FIX + durable engine + async PostgreSQL | 922.1 | 1.023 | 1.507 | 2.846 | 6.077 |

The median within-pair changes were +1.88% throughput, +2.95% p50 latency, +2.99% p95 latency and +3.77% p99 latency. Individual pairs varied widely in both directions as the Docker VM slowed during later runs. The data establishes no speedup and no precise overhead estimate. It does show that the asynchronous path did not add a persistent latency-scale change to this fsync-dominated closed-loop workload. Raw trials are retained so the variability is visible.

PostgreSQL-side medians were 2,819 audit events/s, 14.46 records per batch, queue depth 54, maximum observed commit 10.87 ms and final drain 3.66 ms. These are workload and machine results, not capacity limits.

During the measured ten-second PostgreSQL outage, the gateway accepted 4,103 additional orders and reached business sequence 4,104. The audit backlog reached 12,309 events. PostgreSQL startup plus complete drain took 2.20 seconds at 5,594 recovered events/s, and all 4,104 order rows were verified. A separate preliminary run accepted 10,298 orders during the same interval; this variation reinforces that the container timings are not production capacity measurements. The reliability result is exact in both runs: matching continued, the backlog drained and final row counts agreed.

Evidence is in `results/postgres`: five-mode build/test logs, raw paired measurements, the benchmark summary, outage result and environment record.
