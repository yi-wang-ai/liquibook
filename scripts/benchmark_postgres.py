import json
import pathlib
import statistics
import subprocess

root = pathlib.Path("/workspace")
out = root / "results/postgres"
out.mkdir(parents=True, exist_ok=True)
connection = "dbname=audit host=/var/run/postgresql connect_timeout=1"
subprocess.run(["pg_ctlcluster", "16", "main", "start"], check=False,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
subprocess.run(["runuser", "-u", "postgres", "--", "createuser", "root", "--superuser"],
               check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
subprocess.run(["runuser", "-u", "postgres", "--", "createdb", "-O", "root", "audit"],
               check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

rows = []
for repeat in range(5):
    variants = [
        ("engine", [str(root / "build/postgres-Release/fix_benchmark"), "5000"]),
        ("postgres", [str(root / "build/postgres-Release/postgres_benchmark"), "5000", connection]),
    ]
    if repeat % 2:
        variants.reverse()
    for variant, command in variants:
        print(f"{variant} repeat {repeat + 1}", flush=True)
        row = json.loads(subprocess.check_output(command, text=True))
        row.update(configuration=variant, repeat=repeat)
        rows.append(row)

(out / "benchmark-raw.json").write_text(json.dumps(rows, indent=2) + "\n")
keys = ["requests_s", "p50_ns", "p95_ns", "p99_ns", "p99_9_ns"]
summary = {
    variant: {key: statistics.median(row[key] for row in rows if row["configuration"] == variant)
              for key in keys}
    for variant in ["engine", "postgres"]
}
audit_rows = [row for row in rows if row["configuration"] == "postgres"]
for key in ["audit_events_s", "average_batch", "max_queue_depth", "max_commit_us", "drain_ms"]:
    summary["postgres"][key] = statistics.median(row[key] for row in audit_rows)
for key in keys:
    paired_changes = []
    for repeat in range(5):
        engine = next(row[key] for row in rows if row["configuration"] == "engine" and row["repeat"] == repeat)
        postgres = next(row[key] for row in rows if row["configuration"] == "postgres" and row["repeat"] == repeat)
        paired_changes.append((postgres / engine - 1) * 100)
    summary[f"paired_{key}_change_percent"] = statistics.median(paired_changes)
(out / "benchmark-summary.json").write_text(json.dumps(summary, indent=2) + "\n")

print("10-second PostgreSQL outage", flush=True)
outage = json.loads(subprocess.check_output(
    [str(root / "build/postgres-Release/audit_outage_benchmark"), "10", connection], text=True
))
(out / "outage-benchmark.json").write_text(json.dumps(outage, indent=2) + "\n")
print(json.dumps({"paired": summary, "outage": outage}, indent=2))
