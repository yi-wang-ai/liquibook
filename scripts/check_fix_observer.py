# Follow-up to a noisy initial engine comparison. Requires benchmark_fix.py's baseline build.
import json
import pathlib
import statistics
import subprocess

out = pathlib.Path("/workspace/results/fix")
rows = []
for repeat in range(9):
    variants = [("frozen", "/workspace/build/fix-baseline/benchmark"),
                ("current", "/workspace/build/fix-Release/risk_benchmark")]
    if repeat % 2:
        variants.reverse()
    for workload in ["crossing", "resting"]:
        for mode in ["bare", "risk"]:
            for version, executable in variants:
                row = json.loads(subprocess.check_output(
                    ["taskset", "-c", "0", executable, mode, workload, "1000000", "20260930"], text=True
                ))
                row.update(version=version, repeat=repeat, guest_cpu=0)
                rows.append(row)
    print(f"Observer comparison {repeat + 1}/9", flush=True)
summary = {}
for workload in ["crossing", "resting"]:
    selected = [r for r in rows if r["workload"] == workload]
    assert len({r["workload_hash"] for r in selected}) == 1
    assert len({r["result_hash"] for r in selected}) == 1
    for mode in ["bare", "risk"]:
        pair = {}
        for version in ["frozen", "current"]:
            group = [r["throughput_orders_s"] for r in selected if r["mode"] == mode and r["version"] == version]
            quartiles = statistics.quantiles(group, n=4)
            pair[version] = {"median_orders_s": statistics.median(group),
                             "q1_orders_s": quartiles[0], "q3_orders_s": quartiles[2]}
        pair["change_percent"] = 100 * (pair["current"]["median_orders_s"] / pair["frozen"]["median_orders_s"] - 1)
        summary[f"{mode}-{workload}"] = pair
(out / "observer-followup-raw.json").write_text(json.dumps(rows, indent=2))
(out / "observer-followup-summary.json").write_text(json.dumps(summary, indent=2))
print(json.dumps(summary, indent=2))
