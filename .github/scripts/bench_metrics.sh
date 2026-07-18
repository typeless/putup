#!/bin/sh
# usage: bench_metrics.sh <putup> <config-dir> <source-dir> <build-dir>
# Emits TSV; .github/scripts/compose_metrics.py renders it.
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

miss_rate() {
    awk -v ev="$1" '
        $0 ~ ev {
            for (i = 1; i <= NF; i++)
                if ($i == "rate:") { sub("%", "", $(i + 1)); print $(i + 1); exit }
        }' "$cg"
}

run_row() {
    name=$1
    shift
    tf=$(mktemp)
    pf=$(mktemp)
    /usr/bin/time -v "$@" >/dev/null 2>"$tf"
    rss_mb=$(awk '/Maximum resident set size/{printf "%.1f", $NF/1024}' "$tf")
    "$PERF" stat -r "$REPEAT" -e task-clock:u,page-faults:u,cycles:u,instructions:u -o "$pf" -- "$@" >/dev/null 2>&1

    d1="n/a"
    ll="n/a"
    if command -v valgrind >/dev/null; then
        cg=$(mktemp)
        cgout=$(mktemp)
        valgrind --tool=cachegrind --cache-sim=yes \
            --cachegrind-out-file="$cgout" --log-file="$cg" "$@" >/dev/null 2>&1 || true
        d1=$(miss_rate 'D1 +miss rate:')
        ll=$(miss_rate 'LL miss rate:')
        rm -f "$cg" "$cgout"
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" \
        "$(metric instructions:u 1e6 '%.0f')" \
        "$(metric task-clock 1000 '%.2f')" \
        "$(metric page-faults 1000 '%.1f')" \
        "${d1:-n/a}" \
        "${ll:-n/a}" \
        "$(metric 'time elapsed' 1 '%.3f')" \
        "$rss_mb"
    rm -f "$tf" "$pf"
}

printf 'workload\tinstructions_m\tcpu_s\tfaults_k\td1_pct\tll_pct\twall_s\trss_mb\n'
run_row "parse" "$PUTUP" parse -C "$CDIR" -S "$SDIR" -B "$BDIR"
run_row "dry-run" "$PUTUP" -n -C "$CDIR" -S "$SDIR" -B "$BDIR" -j"$(nproc)"
