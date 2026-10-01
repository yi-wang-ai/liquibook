#!/usr/bin/env bash
set -euo pipefail
cd /workspace
out=results/risk-controls
mkdir -p "$out"
for mode in Debug Release ASan UBSan ClangDebug; do
  flags=(); build_type=$mode
  case "$mode" in
    ASan) build_type=Debug; flags+=("-DCMAKE_CXX_FLAGS=-fsanitize=address -fno-omit-frame-pointer");;
    UBSan) build_type=Debug; flags+=("-DCMAKE_CXX_FLAGS=-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer");;
    ClangDebug) build_type=Debug; flags+=("-DCMAKE_CXX_COMPILER=clang++");;
  esac
  cmake -S . -B "build/risk-$mode" -DCMAKE_BUILD_TYPE="$build_type" "${flags[@]}" > "$out/$mode-configure.txt" 2>&1
  cmake --build "build/risk-$mode" -j4 > "$out/$mode-build.txt" 2>&1
  ctest --test-dir "build/risk-$mode" --output-on-failure -V > "$out/$mode-tests.txt" 2>&1
  echo "Verified $mode"
done
