#!/usr/bin/env bash
set -euo pipefail
phase=${1:-baseline}
out="/workspace/diagnostics/$phase"
tests=(TestTradeCallbacks TestStopOrdersOffMarketNoTrade TestStopMarketOrdersTradeWhenStopPriceReached TestStopOrdersCancelBeforeTrigger)
for config in Debug Release ASan UBSan; do
  src="/tmp/mpc-$phase-$config"
  dir="$out/mpc-$config"
  mkdir -p "$src" "$dir"
  git -C /workspace archive reproduction-baseline | tar -x -C "$src"
  if [ "$phase" = repaired ]; then
    git -C "$src" apply /workspace/diagnostics/repair.patch
  fi
  export MPC_ROOT=/mpc LIQUIBOOK_ROOT="$src" QUICKFAST_ROOT="$src/noQuickFAST"
  export BOOST_ROOT=/usr BOOST_ROOT_LIB=/usr/lib/aarch64-linux-gnu BOOST_VERSION=1.83.0 BOOST_CFG= BOOST_STATIC_LIB_PREFIX=
  cd "$src"
  perl /mpc/mwc.pl -type make liquibook.mwc -features QuickFAST=0 > "$dir/generation.txt" 2>&1
  mode=$config
  flags=()
  case $config in
    ASan) mode=Debug; flags+=("CCFLAGS=-fsanitize=address -fno-omit-frame-pointer");;
    UBSan) mode=Debug; flags+=("CCFLAGS=-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer");;
  esac
  make CFG="$mode" "${flags[@]}" depend > "$dir/depend.txt" 2>&1
  make -j4 CFG="$mode" "${flags[@]}" > "$dir/build.txt" 2>&1
  for test in "${tests[@]}" full-suite; do
    options=()
    [ "$test" = full-suite ] || options+=("--run_test=$test" "--log_level=all")
    set +e
    "$src/bin/test/liquibook_unit_test" "${options[@]}" --report_level=detailed --color_output=no > "$dir/$test.txt" 2>&1
    status=$?
    set -e
    printf '%s\n' "$status" > "$dir/$test.exit"
  done
  echo "Completed isolated MPC $phase $config"
done
