#!/usr/bin/env python3
import argparse
import csv
import sys

PERF_NOTE = (
    "Deterministic signals: page faults, peak RSS, and the cachegrind D1/LL miss"
    " rates (simulated cache, exact across runs). CPU time is the stable compute"
    " signal on shared runners; instructions read n/a on GitHub-hosted runners"
    " (virtualized, no PMU)."
)


def read_tsv(path):
    if not path:
        return []
    try:
        with open(path, encoding="utf-8", newline="") as f:
            return list(csv.DictReader(f, delimiter="\t"))
    except OSError:
        return []


def num(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def annotate(cur, base, kind, unit=""):
    if cur is None:
        return "n/a"
    shown = f"{cur:g}{unit}"
    if base is None:
        return shown
    if kind == "pct_change":
        if base == 0:
            return shown
        change = (cur - base) / base * 100
        if abs(change) < 0.05:
            return shown
        return f"{shown} ({change:+.1f}%)"
    diff = cur - base
    if abs(diff) < 0.05:
        return shown
    suffix = "pp" if kind == "pp" else unit.strip()
    return f"{shown} ({diff:+.1f}{suffix})"


def perf_section(cur_rows, base_rows):
    base_by_name = {r.get("workload"): r for r in base_rows}
    lines = [
        "### Performance (gcc example, Linux)",
        "",
        "| Workload | Instructions | CPU time | Page faults | D1 miss | LL miss | Wall | Peak RSS |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for row in cur_rows:
        base = base_by_name.get(row["workload"], {})
        cells = [
            row["workload"],
            annotate(num(row["instructions_m"]), num(base.get("instructions_m")), "pct_change", " M"),
            f'{num(row["cpu_s"]):g} s' if num(row["cpu_s"]) is not None else "n/a",
            annotate(num(row["faults_k"]), num(base.get("faults_k")), "pct_change", " k"),
            annotate(num(row["d1_pct"]), num(base.get("d1_pct")), "pp", "%"),
            annotate(num(row["ll_pct"]), num(base.get("ll_pct")), "pp", "%"),
            f'{num(row["wall_s"]):g} s' if num(row["wall_s"]) is not None else "n/a",
            annotate(num(row["rss_mb"]), num(base.get("rss_mb")), "abs", " MB"),
        ]
        lines.append("| " + " | ".join(cells) + " |")
    lines += ["", PERF_NOTE]
    return lines


def coverage_section(cur, base):
    lines = [
        "### Test coverage (lines)",
        "",
        "| Overall | Median file | Min file | Max file |",
        "|---|---|---|---|",
    ]
    lines.append(
        "| {} | {} | {:.1f}% `{}` | {:.1f}% `{}` |".format(
            annotate(num(cur["overall"]), num(base.get("overall")), "pp", "%"),
            annotate(num(cur["median"]), num(base.get("median")), "pp", "%"),
            num(cur["min_pct"]),
            cur["min_file"],
            num(cur["max_pct"]),
            cur["max_file"],
        )
    )
    lines += [
        "",
        "{} files · {}/{} lines covered".format(
            cur["files"], cur["covered"], cur["total"]
        ),
    ]
    return lines


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--perf")
    ap.add_argument("--coverage")
    ap.add_argument("--baseline-perf")
    ap.add_argument("--baseline-coverage")
    ap.add_argument("--baseline-label")
    args = ap.parse_args()

    sections = []
    baseline_used = False

    perf = read_tsv(args.perf)
    if perf:
        base = read_tsv(args.baseline_perf)
        baseline_used |= bool(base)
        sections.append(perf_section(perf, base))

    cov = read_tsv(args.coverage)
    if cov:
        base_rows = read_tsv(args.baseline_coverage)
        base = base_rows[0] if base_rows else {}
        baseline_used |= bool(base)
        sections.append(coverage_section(cov[0], base))

    if not sections:
        print("no metrics inputs", file=sys.stderr)
        return 1

    out = []
    for i, section in enumerate(sections):
        if i:
            out.append("")
        out.extend(section)
    if baseline_used and args.baseline_label:
        out += ["", f"Deltas vs {args.baseline_label}."]
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
