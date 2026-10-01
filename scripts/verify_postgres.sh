#!/usr/bin/env bash
set -euo pipefail
cd /workspace
mkdir -p results/postgres
pg_ctlcluster 16 main start >/dev/null 2>&1 || true
runuser -u postgres -- createuser root --superuser 2>/dev/null || true
runuser -u postgres -- createdb -O root audit 2>/dev/null || true
export AUDIT_TEST_CONNINFO='dbname=audit host=/var/run/postgresql connect_timeout=1'
modes=("$@")
if [ "${#modes[@]}" -eq 0 ]; then modes=(Debug Release ASan UBSan ClangDebug); fi
for mode in "${modes[@]}"; do
  flags=(); build_type=$mode
  case "$mode" in
    ASan) build_type=Debug; flags+=("-DCMAKE_CXX_FLAGS=-fsanitize=address -fno-omit-frame-pointer");;
    UBSan) build_type=Debug; flags+=("-DCMAKE_CXX_FLAGS=-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer");;
    ClangDebug) build_type=Debug; flags+=("-DCMAKE_CXX_COMPILER=clang++");;
  esac
  pg_ctlcluster 16 main start >/dev/null 2>&1 || true
  cmake -S . -B "build/postgres-$mode" -DCMAKE_BUILD_TYPE="$build_type" -DBUILD_POSTGRES_AUDIT=ON "${flags[@]}" > "results/postgres/$mode-configure.txt" 2>&1
  cmake --build "build/postgres-$mode" -j4 > "results/postgres/$mode-build.txt" 2>&1
  ctest --test-dir "build/postgres-$mode" -V --output-on-failure > "results/postgres/$mode-tests.txt" 2>&1
  echo "Verified PostgreSQL $mode"
done
