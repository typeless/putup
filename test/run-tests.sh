#!/bin/sh
# Run the putup test suite: fast tests serially, then E2E tests sharded in parallel.
set -u

bin=${1:?usage: run-tests.sh <putup_test-binary> [shards]}
shards=${2:-${PUTUP_TEST_SHARDS:-}}
if [ -z "$shards" ]; then
    cores=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
    shards=$((cores * 2))
    [ "$shards" -gt 32 ] && shards=32
fi

"$bin" "~[e2e]" || exit 1

if [ "$shards" -le 1 ]; then
    exec "$bin" "[e2e]"
fi

logdir=$(mktemp -d)
trap 'rm -rf "$logdir"' EXIT

pids=
i=0
while [ "$i" -lt "$shards" ]; do
    "$bin" "[e2e]" --shard-count "$shards" --shard-index "$i" > "$logdir/$i.log" 2>&1 &
    pids="$pids $!"
    i=$((i + 1))
done

failed=0
i=0
for pid in $pids; do
    if ! wait "$pid"; then
        failed=1
        echo "=== e2e shard $i/$shards FAILED ==="
        cat "$logdir/$i.log"
    fi
    i=$((i + 1))
done

if [ "$failed" -eq 0 ]; then
    awk '/All tests passed/ { sub(/\(/, "", $4); a += $4; c += $(NF - 2) }
         END { printf "E2E: all tests passed (%d assertions in %d test cases, %d shards)\n", a, c, ARGC - 1 }' \
        "$logdir"/*.log
fi
exit "$failed"
