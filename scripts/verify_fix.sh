#!/usr/bin/env bash
set -euo pipefail
cd /workspace
mkdir -p results/fix
modes=("$@")
if [ "${#modes[@]}" -eq 0 ]; then modes=(Debug Release ASan UBSan ClangDebug); fi
for mode in "${modes[@]}"; do
  flags=(); build_type=$mode
  case "$mode" in
    ASan) build_type=Debug; flags+=("-DCMAKE_CXX_FLAGS=-fsanitize=address -fno-omit-frame-pointer");;
    UBSan) build_type=Debug; flags+=("-DCMAKE_CXX_FLAGS=-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer");;
    ClangDebug) build_type=Debug; flags+=("-DCMAKE_CXX_COMPILER=clang++");;
  esac
  cmake -S . -B "build/fix-$mode" -DCMAKE_BUILD_TYPE="$build_type" -DBUILD_FIX_GATEWAY=ON "${flags[@]}" > "results/fix/$mode-configure.txt" 2>&1
  cmake --build "build/fix-$mode" -j4 > "results/fix/$mode-build.txt" 2>&1
  ctest --test-dir "build/fix-$mode" -V --output-on-failure > "results/fix/$mode-tests.txt" 2>&1
  echo "Verified FIX $mode"
done
