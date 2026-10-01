# Phase 2D: QuickFIX 4.4 gateway

The gateway adapts FIX requests to the existing canonical NewOrder, CancelOrder and ReplaceOrder commands. The matching engine and business WAL/snapshot formats are unchanged. Read-only order-event observers were added to the existing risk wrapper to collect acceptance, fill, cancel and replace events; they perform no network I/O.

## Supported protocol

The first version accepts FIX 4.4 NewOrderSingle (D), OrderCancelRequest (F), and OrderCancelReplaceRequest (G). It emits ExecutionReport (8) and the standard OrderCancelReject (9) for failed cancel/replace requests. Overload or a halted worker produces BusinessMessageReject (j), application unavailable.

Only explicit GTC limit orders are supported: OrdType=2 and TimeInForce=1. IOC, market orders and other time-in-force values are explicitly rejected without changing business state. Emulating IOC as an independently committed New followed by Cancel would leave a resting order after a crash between those commands; this adapter does not claim atomic IOC support.

Fields:
- ClOrdID: one to 64 alphanumeric, underscore or hyphen characters, unique per client for the lifetime of this dataset. Include a trading-date prefix if necessary.
- OrigClOrdID: the current accepted ClOrdID in the same client's order chain.
- Symbol: a configured string mapped to an integer symbol ID.
- Account: an exact configured account string authorized for that FIX session's TargetCompID.
- Side: buy or sell only.
- OrderQty: positive integral quantity; decimal suffixes consisting only of zeros are accepted.
- Price: parsed as an exact decimal string into integer ticks. Off-tick prices, negatives and overflow are rejected. No floating-point parsing reaches the engine.
- Replace quantity: the new total quantity, including existing fills.
- Cancel quantity: must equal the current total quantity.

External order IDs and symbol strings stay in the adapter. New orders receive a deterministic numeric ID from the durable adapter-request ordinal. Report quantities describe each event, not just the final state after all fills. LastPx uses the actual execution price. AvgPx is computed with widened integer arithmetic and emitted with six extra fractional digits beyond the instrument scale, truncating excess precision.

Accepted replace/cancel requests advance the ClOrdID chain. Requests using stale OrigClOrdID are rejected. A repeated ordinary ClOrdID is rejected. A matching PossDup request reuses its original responses and ExecIDs without another business command; a changed payload under the same ID is rejected.

## Threading and acknowledgment

QuickFIX owns Logon, Logout, Heartbeat, wire validation and FIX sequence handling. Its session callbacks only decode and enqueue requests. One worker owns the adapter, risk engine, business WAL and Liquibook.

The inbound queue is bounded to 4,096 requests and enqueue is nonblocking. Blocking a QuickFIX callback while a worker waits to send on the same session can deadlock; saturation therefore rejects admission. Concurrent sessions are ordered by enqueue order and recorded in the adapter journal.

The worker:
1. Synchronizes the adapter intent.
2. Calls SegmentedExchange::execute with EveryCommand durability.
3. Requires a durable business receipt before exposing successful execution reports.
4. Submits reports to QuickFIX and records completion in the adapter journal.

Reports to an offline counterparty remain pending. Other sessions can continue receiving reports. Pending reports preserve their original per-client order; crash recovery may repeat reports already submitted before the crash. Any journal, engine or delivery exception halts further business mutation. The executable detects that state and exits; restart reconciles the persisted intent.

## Persistence responsibilities

Three filesets serve different purposes:
- QuickFIX FileStore: FIX message sequence numbers and outbound messages for session resend.
- Existing segmented business WAL/snapshots: matching, positions, risk reservations and canonical business state.
- Adapter journal: external ID chains, deterministic request times, duplicate handling and reconstructible pending reports.

The adapter journal is a separate checksummed append-only format, bound to the business WAL stream identity and exact adapter configuration. Mixing journals from different streams fails closed even if their numeric business states happen to match. Each intent is fsynced before calling the business engine. On restart it rebuilds adapter metadata using the same canonical commands against a temporary recovery engine, checks agreement with recovered business state, and completes an intent that survived before its business command committed. There is only one live execution path. Reconstruction is recovery work, not an alternative order-entry implementation.

Stable ExecIDs derive from the request ordinal and event index. A crash after sending but before recording delivery can resend the same report with the same ExecID. Clients must deduplicate. This is at-least-once application reporting, not a claim of end-to-end exactly-once delivery.

A delivery marker means QuickFIX accepted the report for its session, not that the remote application processed it. Tests cover process crashes; power-loss durability of QuickFIX's packaged FileStore is not certified here. Session stores, the adapter journal and business WAL must be retained together. Resetting one independently requires explicit reconciliation.

Adapter reconstruction currently reads its full journal, capped at 512 MiB; external ID and request caches are retained. This is a deliberate first-version limit. The previously measured direct snapshot recovery benefit applies to the business WAL, not to total gateway startup. A future adapter checkpoint/outbox retention design would be needed before claiming fast end-to-end gateway restart at very large history sizes.

## Build and run

    docker build -f Dockerfile.quickfix -t liquibook-quickfix:ubuntu24.04 .
    docker run --rm -v "$PWD:/workspace" liquibook-quickfix:ubuntu24.04       bash scripts/verify_fix.sh

BUILD_FIX_GATEWAY defaults to OFF, so existing core builds do not require QuickFIX. The gateway uses the Ubuntu 24.04 QuickFIX package (1.15.1 packaging) and the FIX44 dictionary pinned to upstream commit 6908dc8c1084eeb7af7f322d35216e9965684489. The package's pkg-config version string is older than its package version; see recorded environment evidence.

The standalone executable takes a QuickFIX settings file and a gateway-state directory:

    mkdir -p run/fix-sessions run/gateway
    build/fix-Release/fix_gateway examples/fix/acceptor.cfg run/gateway

Run this inside the build container, where /opt/quickfix/FIX44.xml is installed. The example binds TCP localhost port 9876 and configures C1/account 1 and C2/account 2. ESZ6 and TEST use a 0.01 tick. Demo risk limits and mappings are explicit in extensions/fix/demo_settings.h. This is a local integration example, with no TLS, user authentication beyond configured FIX session identity/account mapping, market data, or production deployment claim.

## Verification and measurements

The integration suite uses actual TCP localhost connections between QuickFIX initiators and an acceptor, with FIX44 dictionary validation and file-backed session stores. It also tests adapter restart/outbox behavior and actual SIGKILL at intent, business-commit and report-send boundaries.

Tests cover acceptance, complete and partial fills, cancel/replace chains, decimal validation, duplicate IDs, unsupported IOC/market orders, risk/position/rate limits, client/account separation, reconnects, sequence-gap resend handling, concurrent clients, and exact equality with direct canonical-command execution.

Engine measurements remain separate from protocol measurements:

    docker run --rm -v "$PWD:/workspace" liquibook-quickfix:ubuntu24.04       python3 scripts/benchmark_fix.py

The engine comparison uses the frozen snapshot-recovery-optimized risk header and the current header, the unchanged million-order seeded benchmark, three paired trials with alternating version order, and exact workload/result hash checks. The current engine measurement has no observer attached.

The loopback benchmark uses three trials of 5,000 accepted requests after 100 warm-up requests. It is a closed-loop client with one outstanding request, measuring TCP send through receipt of the acceptance ExecutionReport. The path includes QuickFIX, a single-worker queue, risk, adapter-journal fsync, per-command business-WAL fsync, matching and report serialization. It is not a maximum-throughput saturation test. Session setup and final teardown are excluded; protocol delivery-marker synchronization may overlap receipt and the following request. The p99.9 estimate has only five tail observations per trial and should not be overinterpreted.

All files reside in the container's local storage. These timings are machine/workload specific. QuickFIX itself is the system package; sanitizers instrument this project's code, not a rebuilt QuickFIX library.

## Verified endpoint and measured results

Tag: quickfix-gateway-verified. Branch: project/quickfix-gateway.

All 14 CTest entries pass under GCC Debug/Release/ASan/UBSan and Clang Debug. The new suite contains 40 cases and 88 assertions: 35 TCP integration cases and five adapter persistence/idempotency cases. Actual SIGKILL tests cover the durable-intent, business-commit and report-send boundaries. A standalone server startup/SIGTERM smoke test also passes. Matching-engine sources under src remain unchanged from reproduction-verified.

Median of three closed-loop, 5,000-request loopback trials:

| Requests/second | p50 ms | p95 ms | p99 ms | p99.9 ms |
| ---: | ---: | ---: | ---: | ---: |
| 1,044.1 | 0.928 | 1.244 | 1.588 | 3.609 |

The first three-pair engine comparison showed a 10.1% lower risk-crossing median, but individual trials and the unchanged bare control varied substantially. A follow-up used nine adjacent frozen/current pairs per workload and pinned both processes to guest CPU 0. Its results are below; raw timings and interquartile ranges are preserved. Guest affinity does not control the host scheduler, thermal state or memory-allocation noise.

| Engine path | Frozen orders/second | Current orders/second | Median change |
| --- | ---: | ---: | ---: |
| bare-crossing | 8,526,040 | 9,169,037 | +7.54% |
| bare-resting | 3,669,977 | 3,766,180 | +2.62% |
| risk-crossing | 7,164,382 | 7,430,929 | +3.72% |
| risk-resting | 3,253,029 | 3,313,084 | +1.85% |

The follow-up did not reproduce the initial slowdown, but its broad overlapping ranges do not establish zero observer cost or an engine speedup. No such performance claim is made. All workload and final-result hashes match the frozen baseline. The million-order engine tests and the 5,000-request durable loopback workload are separate measurements; their throughput ratio is not a measurement of QuickFIX parsing cost.

Evidence: results/fix contains matrix logs, raw and summarized engine/loopback measurements, the follow-up, runtime/package versions, the dictionary checksum, and the standalone smoke result.

To repeat the controlled follow-up after benchmark_fix.py:

    docker run --rm -v "$PWD:/workspace" liquibook-quickfix:ubuntu24.04 \
      python3 scripts/check_fix_observer.py

## Protocol references

- [QuickFIX C++ configuration](https://quickfixengine.org/c/documentation/getting-started/configuration.html)
- [QuickFIX C++ receiving messages](https://quickfixengine.org/c/documentation/working-with-messages/receiving-messages.html)
- [Pinned upstream FIX44 dictionary](https://github.com/quickfix/quickfix/blob/6908dc8c1084eeb7af7f322d35216e9965684489/spec/FIX44.xml)
