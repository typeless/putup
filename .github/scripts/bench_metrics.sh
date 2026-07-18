#!/bin/sh
# usage: bench_metrics.sh <putup> <config-dir> <source-dir> <build-dir>
set -eu
PUTUP=$1
CDIR=$2
SDIR=$3
BDIR=$4

PERF=${PERF:-perf}
REPEAT=${BENCH_REPEAT:-3}

metric() {
    awk -v ev="$1" -v div="$2" -v fmt="$3" '
        index($0, ev) {
            gsub(",", "", $1)
            if ($1 ~ /^[0-9.]+$/) printf fmt, $1 / div
            else printf "n/a"
            exit
        }' "$pf"
}

run_row() {
    name=$1
    shift
    tf=$(mktemp)
    pf=$(mktemp)
    /usr/bin/time -v "$@" >/dev/null 2>"$tf"
    rss_mb=$(awk '/Maximum resident set size/{printf "%.1f", $NF/1024}' "$tf")
    "$PERF" stat -r "$REPEAT" -e task-clock:u,page-faults:u,cycles:u,instructions:u -o "$pf" -- "$@" >/dev/null 2>&1
    printf '| %s | %s | %s | %s | %s | %s | %s MB |\n' \
        "$name" \
        "$(metric instructions:u 1e6 '%.0f M')" \
        "$(metric cycles:u 1e6 '%.0f M')" \
        "$(metric task-clock 1000 '%.2f s')" \
        "$(metric page-faults 1000 '%.1f k')" \
        "$(metric 'time elapsed' 1 '%.3f s')" \
        "$rss_mb"
    rm -f "$tf" "$pf"
}

echo "### Performance (gcc example, Linux)"
echo
echo "| Workload | Instructions | Cycles | CPU time | Page faults | Wall | Peak RSS |"
echo "|---|---|---|---|---|---|---|"
run_row "parse" "$PUTUP" parse -C "$CDIR" -S "$SDIR" -B "$BDIR"
run_row "dry-run (graph load + schedule)" "$PUTUP" -n -C "$CDIR" -S "$SDIR" -B "$BDIR" -j"$(nproc)"
echo
echo "Page faults and peak RSS are the stable memory signals; CPU time is the stable compute signal on shared runners. Instructions and cycles read n/a on GitHub-hosted runners (virtualized, no PMU) but populate on machines with hardware counters. perf stat -r ${REPEAT}, user-space only."
