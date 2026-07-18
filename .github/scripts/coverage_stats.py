#!/usr/bin/env python3
import json
import statistics
import sys


def main() -> int:
    with open(sys.argv[1], encoding="utf-8") as f:
        summary = json.load(f)

    files = [e for e in summary["files"] if e.get("line_total", 0) > 0]
    if not files:
        print("no coverage data", file=sys.stderr)
        return 1

    worst = min(files, key=lambda e: e["line_percent"])
    best = max(files, key=lambda e: e["line_percent"])
    median = statistics.median(e["line_percent"] for e in files)

    print(
        "overall\tmedian\tmin_pct\tmin_file\tmax_pct\tmax_file\tfiles\tcovered\ttotal"
    )
    print(
        "\t".join(
            str(v)
            for v in (
                summary["line_percent"],
                median,
                worst["line_percent"],
                worst["filename"],
                best["line_percent"],
                best["filename"],
                len(files),
                summary["line_covered"],
                summary["line_total"],
            )
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
