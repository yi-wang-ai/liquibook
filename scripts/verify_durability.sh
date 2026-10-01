#!/usr/bin/env bash
set -euo pipefail
cd /workspace
phase=${1:-wal}
mkdir -p "results/$phase"
for mode in Debug Release ASan UBSan ClangDebug; do
  flags=(); build_type=$mode
  case "$mode" in
    ASan) build_type=Debug; flags+=("-DCMAKE_CXX_FLAGS=-fsanitize=address -fno-omit-frame-pointer");;
    UBSan) build_type=Debug; flags+=("-DCMAKE_CXX_FLAGS=-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer");;
    ClangDebug) build_type=Debug; flags+=("-DCMAKE_CXX_COMPILER=clang++");;
  esac
  cmake -S . -B "build/$phase-$mode" -DCMAKE_BUILD_TYPE="$build_type" "${flags[@]}" > "results/$phase/$mode-configure.txt" 2>&1
  cmake --build "build/$phase-$mode" -j4 > "results/$phase/$mode-build.txt" 2>&1
  ctest --test-dir "build/$phase-$mode" -V --output-on-failure > "results/$phase/$mode-tests.txt" 2>&1
  echo "Verified $phase $mode"
done
