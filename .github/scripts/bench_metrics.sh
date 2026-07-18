#!/bin/sh
# usage: bench_metrics.sh <putup> <config-dir> <source-dir> <build-dir>
set -eu
PUTUP=$1
CDIR=$2
SDIR=$3
BDIR=$4

PERF=${PERF:-perf}
REPEAT=${BENCH_REPEAT:-3}

run_row() {
    name=$1
    shift
    tf=$(mktemp)
    pf=$(mktemp)
    /usr/bin/time -v "$@" >/dev/null 2>"$tf"
    rss_mb=$(awk '/Maximum resident set size/{printf "%.1f", $NF/1024}' "$tf")
    "$PERF" stat -r "$REPEAT" -e cycles:u,instructions:u -o "$pf" -- "$@" >/dev/null 2>&1
    ins=$(awk '/instructions:u/{gsub(",",""); printf "%.0f", $1/1e6}' "$pf")
    cyc=$(awk '/cycles:u/{gsub(",",""); printf "%.0f", $1/1e6}' "$pf")
    wall=$(awk '/time elapsed/{printf "%.3f", $1}' "$pf")
    rm -f "$tf" "$pf"
    printf '| %s | %s M | %s M | %s s | %s MB |\n' "$name" "$ins" "$cyc" "$wall" "$rss_mb"
}

echo "### Performance (gcc example, Linux)"
echo
echo "| Workload | Instructions | Cycles | Wall | Peak RSS |"
echo "|---|---|---|---|---|"
run_row "parse" "$PUTUP" parse -C "$CDIR" -S "$SDIR" -B "$BDIR"
run_row "dry-run (graph load + schedule)" "$PUTUP" -n -C "$CDIR" -S "$SDIR" -B "$BDIR" -j"$(nproc)"
echo
echo "Instructions and peak RSS are stable across runs; cycles and wall time are noisy on shared CI runners. perf stat -r ${REPEAT}, user-space counters only."
