import csv
import io
import json
import pathlib
import statistics
import subprocess

out = pathlib.Path("/workspace/results/segmented")
out.mkdir(parents=True, exist_ok=True)
summary = []
for count, percent in [(100000, 90), (1000000, 90), (10000000, 90), (10000000, 99)]:
    print(f"Running {count} commands, snapshot at {percent}%", flush=True)
    raw = subprocess.check_output(
        ["/workspace/build/segmented-Release/segmented_benchmark", str(count), str(percent)], text=True
    )
    (out / f"benchmark-{count}-{percent}.csv").write_text(raw)
    rows = list(csv.DictReader(io.StringIO(raw)))
    assert len({r["state_hash"] for r in rows}) == 1
    result = {"commands": count, "snapshot_percent": percent}
    for mode in ("full", "snapshot"):
        result[mode] = {
            key: statistics.median(float(r[key]) for r in rows if r["mode"] == mode)
            for key in ("snapshot_ms", "tail_ms", "total_ms", "bytes_read",
                        "records_decoded", "snapshot_create_ms")
        }
    result["speedup"] = result["full"]["total_ms"] / result["snapshot"]["total_ms"]
    summary.append(result)
    (out / "benchmark-summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)
