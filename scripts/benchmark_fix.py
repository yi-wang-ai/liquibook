import json
import pathlib
import statistics
import subprocess

root = pathlib.Path("/workspace")
out = root / "results/fix"
out.mkdir(parents=True, exist_ok=True)
baseline = root / "build/fix-baseline"
baseline.mkdir(parents=True, exist_ok=True)
header = subprocess.check_output(
    ["git", "show", "snapshot-recovery-optimized:extensions/risk/risk_gateway.h"], text=True
)
(baseline / "risk_gateway.h").write_text(header)
source = (root / "benchmarks/risk_benchmark.cpp").read_text().replace(
    '../extensions/risk/risk_gateway.h', 'risk_gateway.h'
)
(baseline / "benchmark.cpp").write_text(source)
subprocess.run(["g++", "-O3", "-DNDEBUG", "-std=c++11", "-I/workspace/src",
                str(baseline / "benchmark.cpp"), "-o", str(baseline / "benchmark")], check=True)
core = []
for repeat in range(3):
    variants = [("frozen", baseline / "benchmark"),
                ("current", root / "build/fix-Release/risk_benchmark")]
    if repeat % 2:
        variants.reverse()
    for version, executable in variants:
        for mode in ["bare", "risk"]:
            for workload in ["crossing", "resting"]:
                row = json.loads(subprocess.check_output(
                    [str(executable), mode, workload, "1000000", "20260930"], text=True
                ))
                row.update(version=version, repeat=repeat)
                core.append(row)
for workload in ["crossing", "resting"]:
    group = [r for r in core if r["workload"] == workload]
    assert len({r["workload_hash"] for r in group}) == 1
    assert len({r["result_hash"] for r in group}) == 1
(out / "engine-benchmark-raw.json").write_text(json.dumps(core, indent=2))
summary = {}
for version in ["frozen", "current"]:
    for mode in ["bare", "risk"]:
        for workload in ["crossing", "resting"]:
            group = [r for r in core if r["version"] == version and r["mode"] == mode and r["workload"] == workload]
            summary[f"{version}-{mode}-{workload}"] = {
                k: statistics.median(r[k] for r in group)
                for k in ["throughput_orders_s", "p50_ns", "p95_ns", "p99_ns", "p99_9_ns"]
            }
(out / "engine-benchmark-summary.json").write_text(json.dumps(summary, indent=2))
print("Engine comparisons verified", flush=True)
loopback = []
for repeat in range(3):
    print(f"Loopback repeat {repeat + 1}", flush=True)
    row = json.loads(subprocess.check_output(
        ["/workspace/build/fix-Release/fix_benchmark", "5000"], text=True
    ))
    row["repeat"] = repeat
    loopback.append(row)
(out / "loopback-raw.json").write_text(json.dumps(loopback, indent=2))
summary = {k: statistics.median(r[k] for r in loopback)
           for k in ["requests_s", "p50_ns", "p95_ns", "p99_ns", "p99_9_ns"]}
(out / "loopback-summary.json").write_text(json.dumps(summary, indent=2))
print(json.dumps(summary, indent=2))
