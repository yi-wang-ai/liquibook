#!/usr/bin/env bash
set -euo pipefail
src=/tmp/liquibook-pristine
out=/workspace/diagnostics/pristine-mpc
mkdir -p "$src" "$out"
git -C /workspace archive 2427613b32f1667abae68a01df6af9ba8270f8e7 | tar -x -C "$src"
export MPC_ROOT=/mpc LIQUIBOOK_ROOT="$src" QUICKFAST_ROOT="$src/noQuickFAST"
export BOOST_ROOT=/usr BOOST_ROOT_LIB=/usr/lib/aarch64-linux-gnu BOOST_VERSION=1.83.0 BOOST_CFG= BOOST_STATIC_LIB_PREFIX=
cd "$src"
perl /mpc/mwc.pl -type make liquibook.mwc -features QuickFAST=0 > "$out/generation.txt" 2>&1
set +e
make -C test/unit -f Makefile.liquibook_unit_test CFG=Debug ut_listeners.o > "$out/build.txt" 2>&1
status=$?
set -e
printf '%s\n' "$status" > "$out/build.exit"
test "$status" -ne 0
