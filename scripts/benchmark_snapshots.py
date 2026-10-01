import csv
import io
import json
import pathlib
import statistics
import subprocess

out = pathlib.Path("/workspace/results/snapshots")
out.mkdir(parents=True, exist_ok=True)
raw = subprocess.check_output(
    ["/workspace/build/snapshots-Release/snapshot_benchmark"], text=True
)
rows = list(csv.DictReader(io.StringIO(raw)))
summary = {
    key: statistics.median(float(row[key]) for row in rows)
    for key in rows[0] if key != "trial"
}
summary["recovery_speedup"] = summary["full_replay_ms"] / summary["snapshot_tail_ms"]
(out / "benchmark-raw.csv").write_text(raw)
(out / "benchmark-summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))
