#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
run_dir="results/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$run_dir"
{
  date -u
  git rev-parse HEAD
  git status --short
  uname -a
  cat /etc/os-release
  lscpu
  free -h
  c++ --version
  cmake --version
  dpkg-query -W libboost-test-dev
} > "$run_dir/environment.txt"
cmake -S . -B build/linux-release -DCMAKE_BUILD_TYPE=Release 2>&1 | tee "$run_dir/configure.txt"
cmake --build build/linux-release --parallel 4 2>&1 | tee "$run_dir/build.txt"
set +e
ctest --test-dir build/linux-release --output-on-failure -V 2>&1 | tee "$run_dir/tests.txt"
test_status=${PIPESTATUS[0]}
set -e
printf '%s\n' "$test_status" > "$run_dir/test_exit_status.txt"
for script in examples/mt_order_entry/*.script; do
  timeout 10 build/linux-release/mt_order_entry "$script" > "$run_dir/$(basename "$script").txt" 2>&1
done
build/linux-release/pt_order_book 1 2>&1 | tee "$run_dir/throughput.txt"
build/linux-release/lt_order_book 100000 > "$run_dir/latency.txt" 2>&1
printf 'Results: %s\n' "$run_dir"
exit "$test_status"
