# Phase 1: four-failure diagnosis

Phase 1 reproduction is complete for the core library, upstream unit suite, manual-order example, and benchmark executables. QuickFAST publisher/subscriber remains outside the stated reproduction scope. No exchange-specific extensions have been added.

## Preserved reference

- Original upstream commit: 2427613b32f1667abae68a01df6af9ba8270f8e7.
- Immutable local tag: reproduction-baseline.
- Baseline commit: 5489e1d8b730d308bcdcb2f78d970c53f54fe278.
- The baseline includes CMake, recorded benchmarks, and the test-only int64_t signature repair; its matching engine and SimpleOrderBook are unchanged from upstream.
- Completely pristine upstream also fails to compile through MPC because OrderCbListener::on_replace declares int32_t instead of the interface's int64_t. Evidence: ../diagnostics/pristine-mpc/build.txt.
- Every runtime comparison uses the same signature repair on both build methods. The preserved tag was never moved.

## Build comparison and classification

“Fail” below means the same original assertions fail in both Debug and Release. These failures also reproduce under ASan and UBSan on both build methods, and in a Clang CMake Debug build.

| Test | Upstream MPC | CMake | Classification and root cause | Action |
| --- | --- | --- | --- | --- |
| TestTradeCallbacks | Fail | Fail | Defective upstream test: TradeListener documents unit price, but the test expects total cost. | Rename test storage to prices and expect 3250. No callback implementation change. |
| TestStopOrdersOffMarketNoTrade | Fail | Fail | Genuine Liquibook bug in the SimpleOrderBook adapter: cb_order_accept_stop is not handled, leaving held orders in os_new. | Handle stop acceptance using SimpleOrder::accept. |
| TestStopMarketOrdersTradeWhenStopPriceReached | Fail | Fail | Same adapter bug. The two initial acceptance assertions fail; the later triggering/fill checks pass. | Same acceptance fix; independent trigger regressions retain fill checks. |
| TestStopOrdersCancelBeforeTrigger | Fail | Fail | The adapter ignores both cb_order_accept_stop and cb_order_cancel_stop. The stop book removes the order, but the order state stays os_new. | Handle stop cancellation using SimpleOrder::cancel. |

No CMake regression or compiler/platform-specific cause was observed. The matching algorithm in src/book is unchanged. The source repair consists of two additional switch labels in src/simple/simple_order_book.h. All original assertions except the independently disproved price/cost expectation remain unchanged.

## Exact inputs and evidence

Each named test has its complete assertion output and exit status under ../diagnostics/baseline/<variant>/. Debug stack files end in .stack.txt and include bt full at each of the 13 failing assertion locations. These are assertion failures, not crashes, so the recorded stacks are breakpoint snapshots immediately before the assertions. Original input source is preserved in the tag.

- TestTradeCallbacks: resting sell, quantity 100, price 3250; incoming buy, quantity 800, limit 3250. Callback receives quantity 100 and price 3250. The old assertion expects 325000. The public TradeListener interface explicitly calls the argument price.
- TestStopOrdersOffMarketNoTrade: establish market price 55 by a trade; add market buy quantity 100 with stop 56 and market sell quantity 100 with stop 54. Both orders enter the stop maps without filling. Their states stay os_new instead of os_accepted. Four assertions fail.
- TestStopMarketOrdersTradeWhenStopPriceReached: market price 55; seed bid at 53 and ask at 57; add stopped market buy at 56 and sell at 54, each quantity 100. Two acceptance helper assertions fail because they inspect SimpleOrder state. Subsequent trigger and fill checks pass.
- TestStopOrdersCancelBeforeTrigger: market price 55; stop buy at 56 and stop sell at 54, each quantity 100. Acceptance helper checks, explicit accepted-state checks, and cancellation state checks fail: six assertions. The stop maps themselves empty correctly.

The shared helper add_and_verify checks both add's match result and the resulting order state. Its failures must not be interpreted as proof of incorrect matching.

## Independent reproductions before source changes

The standalone programs in test/regression use public APIs directly, without Boost or upstream helper functions.

- trade_price.cpp confirms the documented price contract and passes before and after repair.
- stop_lifecycle.cpp has accept, cancel, and trigger modes, on both buy and sell sides. Acceptance requires no fill, one held stop, and os_accepted. Cancellation requires no fill, no held stop, and os_cancelled. Trigger checks retain the initial acceptance requirement and verify a complete fill after crossing the stop price.
- On the baseline all six stop modes fail. On the repaired source all six pass.
- Both GCC and Clang were used in Debug, Release, ASan, and UBSan configurations: 56 runs before and 56 after.
- All failures were independently confirmed before the SimpleOrderBook repair was applied.

## Validation

The full 157-case upstream suite was run in nine configurations before and after repair:
- CMake: GCC Debug, Release, ASan, UBSan; Clang Debug.
- Original MPC-generated makefiles: GCC Debug, Release, ASan, UBSan.

Before: 153 passing cases, four failing cases, 13 failed assertions.
After: 157 passing cases, all 2671 assertions passing.
Each of the four tests was also run individually in all configurations.
No AddressSanitizer, LeakSanitizer, or UndefinedBehaviorSanitizer errors were reported by these runs.
The final CMake integration passes all nine CTest entries in Debug and Release: one upstream-suite entry, one order-entry example, and seven independent regressions.

Important coverage limitation: TestTwoAonBidTwoAonAsk is registered but its actual scenario is disabled by an upstream preprocessor branch. It prints a warning and executes zero assertions. Thus “157 passing” includes this existing placeholder; it does not demonstrate support for many-to-many AON matching. Its exact status is retained in the accounting rather than silently treated as substantive coverage.

../diagnostics/test-accounting.json lists every case's result for all 18 suite runs. scripts/summarize_diagnostics.py checks identical case sets, expected before/after failures, individual exit statuses, reproducer results, and sanitizer output.

## Configuration controls

Execution was on Ubuntu ARM64 in Docker on the Apple M4 Pro host. Exact compiler, Boost, GDB, CMake, image ID, and MPC commit are in ../diagnostics/.
MPC was pinned to 420e24790d349057e6463890ae993eb61eecc273.
CMake Release uses -O3 -DNDEBUG and C++11; original MPC Release uses -O with the compiler's default language standard. Both yield identical failure classifications.
MPC Debug uses -g; CMake Debug also includes debug symbols. ASan and UBSan were run separately, with frame pointers, and UBSan configured to halt on an error.

MPC uses common object filenames across configurations. An initial same-directory Release attempt reused Debug objects; it was rejected as evidence. The final MPC logs come from separate fresh source directories per configuration and include the actual compilation commands. MPC_ROOT and Boost paths are explicitly set for dependency generation.

GDB reports that it cannot disable ASLR inside Docker. Breakpoints resolved, stacks and locals were captured successfully; no global security setting was changed.

## Reproduce

Run from the repository. Clone the pinned MPC dependency into a sibling directory if needed:

    git clone https://github.com/DOCGroup/MPC.git ../liquibook-mpc
    git -C ../liquibook-mpc checkout 420e24790d349057e6463890ae993eb61eecc273
    docker build -t liquibook-repro:ubuntu24.04 .
    docker build -f Dockerfile.diagnostics -t liquibook-diagnostics:ubuntu24.04 .

Set paths and run each phase in a fresh container:

    repo_path="$PWD"
    mpc_path="$(cd ../liquibook-mpc && pwd)"
    docker run --rm --cap-add=SYS_PTRACE -v "$repo_path:/workspace" -v "$mpc_path:/mpc:ro" liquibook-diagnostics:ubuntu24.04 bash scripts/diagnose.sh baseline
    docker run --rm -v "$repo_path:/workspace" -v "$mpc_path:/mpc:ro" liquibook-diagnostics:ubuntu24.04 bash scripts/diagnose.sh repaired
    docker run --rm -v "$repo_path:/workspace" liquibook-diagnostics:ubuntu24.04 bash scripts/diagnose_reproducers.sh baseline
    docker run --rm -v "$repo_path:/workspace" liquibook-diagnostics:ubuntu24.04 bash scripts/diagnose_reproducers.sh repaired
    docker run --rm -v "$repo_path:/workspace" -v "$mpc_path:/mpc:ro" liquibook-diagnostics:ubuntu24.04 bash scripts/check_pristine_mpc.sh
    python3 scripts/summarize_diagnostics.py

Both phases extract reproduction-baseline; only the repaired phase applies the saved diagnostics/repair.patch. This avoids accidentally comparing two copies of the repaired working tree. Logs are written to deterministic phase/configuration paths and overwritten on rerun; archive them first if preserving a second environment's evidence.

Phase 2 remains pending: pre-trade risk controls, followed by WAL/snapshot. No performance improvement is claimed from this correctness repair; existing benchmark measurements remain attached to the unchanged baseline tag.
