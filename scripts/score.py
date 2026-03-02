#!/usr/bin/env python3
"""Compute scoring metrics and generate comparison plots from benchmark results.

Usage:
    python scripts/score.py results/cadical.csv results/painless.csv --timeout 5000
"""

import argparse
import csv
import logging
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s", datefmt="%H:%M:%S")
log = logging.getLogger(__name__)

PROJECT_ROOT = Path(__file__).resolve().parent.parent


def load_results(csv_path):
    rows, meta = [], {}
    with open(csv_path) as f:
        lines = []
        for line in f:
            if line.startswith("#"):
                parts = line.lstrip("# ").strip().split("=", 1)
                if len(parts) == 2:
                    meta[parts[0].strip()] = parts[1].strip()
            else:
                lines.append(line)
        for row in csv.DictReader(lines):
            row["wall_time"] = float(row["wall_time"])
            rows.append(row)
    return rows, meta


def compute_metrics(rows, timeout):
    solved_sat = [r for r in rows if r["result"] == "SAT"]
    solved_unsat = [r for r in rows if r["result"] == "UNSAT"]
    solved = solved_sat + solved_unsat
    unsolved = [r for r in rows if r["result"] not in ("SAT", "UNSAT")]
    solved_times = sorted(r["wall_time"] for r in solved)
    par2_sum = sum(r["wall_time"] for r in solved) + len(unsolved) * 2 * timeout
    par2 = par2_sum / len(rows) if rows else 0
    wrong = [r for r in solved if r.get("expected") in ("sat", "unsat") and r["result"].lower() != r["expected"]]
    return {
        "total": len(rows), "solved": len(solved), "sat": len(solved_sat),
        "unsat": len(solved_unsat),
        "timeout": len([r for r in rows if r["result"] == "TIMEOUT"]),
        "error": len([r for r in rows if r["result"] in ("ERROR", "UNKNOWN")]),
        "par2": round(par2, 2),
        "mean_time": round(float(np.mean(solved_times)), 2) if solved_times else 0,
        "median_time": round(float(np.median(solved_times)), 2) if solved_times else 0,
        "solved_times": solved_times, "wrong": len(wrong),
    }


def print_summary(label, m):
    print()
    print("=" * 55)
    print(f"  {label}")
    print("=" * 55)
    print(f"  Total instances     : {m["total"]}")
    print(f"  Solved              : {m["solved"]}  (SAT={m["sat"]}, UNSAT={m["unsat"]})")
    print(f"  Timeout             : {m["timeout"]}")
    print(f"  Error/Unknown       : {m["error"]}")
    if m["wrong"] > 0:
        print(f"  ** WRONG ANSWERS ** : {m["wrong"]}")
    print(f"  PAR-2               : {m["par2"]}")
    print(f"  Mean  solve time    : {m["mean_time"]}s")
    print(f"  Median solve time   : {m["median_time"]}s")
    print("=" * 55)


def print_comparison_table(all_data, timeout):
    print()
    print("=" * 80)
    print(f"  COMPARISON TABLE  (timeout={timeout}s)")
    print("=" * 80)
    print(f"{"Solver":<30} {"Solved":>7} {"SAT":>5} {"UNSAT":>6} {"PAR-2":>10} {"Median":>8}")
    print("-" * 80)
    for label, m in sorted(all_data.items(), key=lambda x: x[1]["par2"]):
        print(f"{label:<30} {m["solved"]:>7} {m["sat"]:>5} {m["unsat"]:>6} {m["par2"]:>10.2f} {m["median_time"]:>7.1f}s")
    print("=" * 80)


def plot_cactus(all_data, timeout, output_path):
    fig, ax = plt.subplots(figsize=(10, 7))
    colors = plt.cm.tab10.colors
    for i, (label, m) in enumerate(sorted(all_data.items(), key=lambda x: -x[1]["solved"])):
        times = m["solved_times"]
        if not times:
            continue
        x = np.arange(1, len(times) + 1)
        ax.plot(x, times, label=f"{label} ({len(times)} solved)", color=colors[i % len(colors)], linewidth=1.5)
    ax.set_xlabel("Number of instances solved", fontsize=12)
    ax.set_ylabel("Runtime (seconds)", fontsize=12)
    ax.set_title("Cactus Plot", fontsize=14)
    ax.set_yscale("log")
    ax.set_ylim(0.1, timeout * 2)
    ax.axhline(y=timeout, color="gray", linestyle="--", alpha=0.5, label=f"timeout={timeout}s")
    ax.legend(loc="upper left", fontsize=9)
    ax.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output_path, dpi=150)
    log.info("Cactus plot saved: %s", output_path)
    plt.close()


def save_summary_csv(all_data, output_path):
    fields = ["solver", "solved", "sat", "unsat", "timeout_count", "error", "wrong", "par2", "mean_time", "median_time"]
    with open(output_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for label, m in sorted(all_data.items(), key=lambda x: x[1]["par2"]):
            w.writerow({"solver": label, "solved": m["solved"], "sat": m["sat"], "unsat": m["unsat"],
                         "timeout_count": m["timeout"], "error": m["error"], "wrong": m["wrong"],
                         "par2": m["par2"], "mean_time": m["mean_time"], "median_time": m["median_time"]})
    log.info("Summary CSV saved: %s", output_path)


def main():
    parser = argparse.ArgumentParser(description="Score and compare SAT solver benchmark results")
    parser.add_argument("results", nargs="+", help="Result CSV file(s) from run_benchmark.py")
    parser.add_argument("--timeout", type=int, default=5000, help="Timeout used in benchmark (default: 5000)")
    parser.add_argument("--output", type=str, default=None, help="Output directory for plots")
    parser.add_argument("--labels", type=str, nargs="*", default=None, help="Custom labels")
    args = parser.parse_args()

    out_dir = Path(args.output) if args.output else PROJECT_ROOT / "results" / "plots"
    out_dir.mkdir(parents=True, exist_ok=True)

    all_data = {}
    for i, csv_path in enumerate(args.results):
        rows, meta = load_results(csv_path)
        if not rows:
            log.warning("No results in %s", csv_path)
            continue
        if args.labels and i < len(args.labels):
            label = args.labels[i]
        else:
            label = Path(csv_path).stem
            parts = label.rsplit("_", 2)
            label = "_".join(parts[:-2]) if len(parts) >= 3 else parts[0]
        metrics = compute_metrics(rows, args.timeout)
        all_data[label] = metrics
        print_summary(label, metrics)

    if len(all_data) > 1:
        print_comparison_table(all_data, args.timeout)

    plot_cactus(all_data, args.timeout, out_dir / "cactus.png")
    save_summary_csv(all_data, out_dir / "summary.csv")


if __name__ == "__main__":
    main()
