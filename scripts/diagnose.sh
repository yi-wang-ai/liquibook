#!/usr/bin/env bash
set -euo pipefail
cd /workspace
phase=${1:-baseline}
out="/workspace/diagnostics/$phase"
mkdir -p "$out"
# Isolated source tree from the tag, independent of current working edits.
mkdir -p /tmp/lb-baseline
git archive reproduction-baseline | tar -x -C /tmp/lb-baseline
src=/tmp/lb-baseline
if [ "$phase" = repaired ]; then
  git -C "$src" apply /workspace/diagnostics/repair.patch
fi
tests=(TestTradeCallbacks TestStopOrdersOffMarketNoTrade TestStopMarketOrdersTradeWhenStopPriceReached TestStopOrdersCancelBeforeTrigger)
run_tests() {
  local binary=$1 dir=$2
  mkdir -p "$dir"
  for test in "${tests[@]}"; do
    set +e
    "$binary" --run_test="$test" --log_level=all --report_level=detailed --color_output=no > "$dir/$test.txt" 2>&1
    result=$?
    set -e
    printf '%s\n' "$result" > "$dir/$test.exit"
  done
  set +e
  "$binary" --report_level=detailed --color_output=no > "$dir/full-suite.txt" 2>&1
  result=$?
  set -e
  printf '%s\n' "$result" > "$dir/full-suite.exit"
}
for config in Debug Release ASan UBSan ClangDebug; do
  dir="$out/cmake-$config"
  mkdir -p "$dir"
  flags=()
  mode=$config
  case $config in
    ASan) mode=Debug; flags+=("-DCMAKE_CXX_FLAGS=-fsanitize=address -fno-omit-frame-pointer" "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address");;
    UBSan) mode=Debug; flags+=("-DCMAKE_CXX_FLAGS=-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=undefined");;
    ClangDebug) mode=Debug; flags+=("-DCMAKE_CXX_COMPILER=clang++");;
  esac
  cmake -S "$src" -B "/tmp/lb-cmake-$config" -DCMAKE_BUILD_TYPE="$mode" "${flags[@]}" > "$dir/configure.txt" 2>&1
  cmake --build "/tmp/lb-cmake-$config" --target liquibook_unit_test --parallel 4 --verbose > "$dir/build.txt" 2>&1
  run_tests "/tmp/lb-cmake-$config/liquibook_unit_test" "$dir"
  echo "Completed CMake $config"
done
if [ "$phase" = baseline ]; then
# Assertion failures are not crashes: capture stacks and locals immediately
# before each failing assertion in a Debug build.
for test in "${tests[@]}"; do
  case $test in
    TestTradeCallbacks) file=ut_listeners.cpp; lines="396";;
    TestStopOrdersOffMarketNoTrade) file=ut_stop_orders.cpp; lines="61 62 65 66";;
    TestStopMarketOrdersTradeWhenStopPriceReached) file=ut_stop_orders.cpp; lines="101 102";;
    TestStopOrdersCancelBeforeTrigger) file=ut_stop_orders.cpp; lines="139 140 143 144 146 147";;
  esac
  commands="$out/cmake-Debug/$test.gdb"
  printf 'set pagination off\nset print pretty on\nset breakpoint pending on\n' > "$commands"
  for line in $lines; do
    printf 'break %s:%s\ncommands\nsilent\nbt full\ncontinue\nend\n' "$file" "$line" >> "$commands"
  done
  printf 'run --run_test=%s --log_level=all --color_output=no\n' "$test" >> "$commands"
  gdb -q -batch -x "$commands" /tmp/lb-cmake-Debug/liquibook_unit_test > "$out/cmake-Debug/$test.stack.txt" 2>&1
done
fi
bash /workspace/scripts/diagnose_mpc.sh "$phase"
