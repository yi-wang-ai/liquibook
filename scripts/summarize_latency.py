"""Summarize upstream latency output; nearest-rank percentiles, nanoseconds."""
import json
import math
import sys
from pathlib import Path

sections = {}
current = None
for line in Path(sys.argv[1]).read_text().splitlines():
    if line.startswith("testing order book"):
        current = line
        sections[current] = []
    elif current and line.strip().isdigit():
        sections[current].append(int(line))
summary = {}
for name, samples in sections.items():
    if not samples:
        raise SystemExit(f"No latency samples for {name}")
    samples.sort()
    summary[name] = {
        "samples": len(samples),
        "mean_ns": sum(samples) / len(samples),
        **{f"p{p:g}_ns": samples[math.ceil(p / 100 * len(samples)) - 1]
           for p in (50, 95, 99, 99.9)},
        "max_ns": samples[-1],
    }
if len(summary) != 3:
    raise SystemExit("Expected all three upstream latency configurations")
print(json.dumps(summary, indent=2))
