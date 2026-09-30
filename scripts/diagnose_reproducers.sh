#!/usr/bin/env bash
set -euo pipefail
phase=${1:-baseline}
src="/tmp/repro-$phase"
out="/workspace/diagnostics/$phase/reproducers"
mkdir -p "$src" "$out"
git -C /workspace archive reproduction-baseline | tar -x -C "$src"
if [ "$phase" = repaired ]; then
  git -C "$src" apply /workspace/diagnostics/repair.patch
fi
for compiler in g++ clang++; do
  for mode in Debug Release ASan UBSan; do
    flags=(-g -O0)
    case $mode in
      Release) flags=(-O3 -DNDEBUG);;
      ASan) flags+=(-fsanitize=address -fno-omit-frame-pointer);;
      UBSan) flags+=(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer);;
    esac
    for program in trade_price stop_lifecycle; do
      binary="/tmp/$phase-$compiler-$mode-$program"
      "$compiler" -std=c++11 "${flags[@]}" -I"$src/src" "/workspace/test/regression/$program.cpp" "$src/src/simple/simple_order.cpp" -o "$binary"
      cases=(price)
      [ "$program" = trade_price ] || cases=(accept cancel trigger)
      for action in "${cases[@]}"; do
        sides=(buy)
        [ "$program" = trade_price ] || sides=(buy sell)
        for side in "${sides[@]}"; do
          log="$out/$compiler-$mode-$program-$action-$side"
          set +e
          "$binary" "$action" "$side" > "$log.txt" 2>&1
          status=$?
          set -e
          printf '%s\n' "$status" > "$log.exit"
        done
      done
    done
  done
done
