#!/usr/bin/env python3
"""Select a balanced sample of SAT and UNSAT instances for quick benchmarking.

Usage:
    python scripts/select_sample.py --sat 50 --unsat 50 --seed 42
"""

import argparse
import csv
import logging
import random
from pathlib import Path

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s", datefmt="%H:%M:%S")
log = logging.getLogger(__name__)

PROJECT_ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description="Select balanced SAT/UNSAT sample")
    parser.add_argument("--input", type=str, default="benchmarks/sc2023/instances.csv",
                        help="Input instances.csv")
    parser.add_argument("--output", type=str, default="benchmarks/sample100/instances.csv",
                        help="Output instances.csv")
    parser.add_argument("--sat", type=int, default=50, help="Number of SAT instances")
    parser.add_argument("--unsat", type=int, default=50, help="Number of UNSAT instances")
    parser.add_argument("--seed", type=int, default=42, help="Random seed")
    parser.add_argument("--check-exists", action="store_true",
                        help="Only include instances whose CNF files exist on disk")
    args = parser.parse_args()

    input_path = args.input
    if not Path(input_path).is_absolute():
        input_path = str(PROJECT_ROOT / input_path)

    sat_instances = []
    unsat_instances = []

    with open(input_path) as f:
        for row in csv.DictReader(f):
            expected = row.get("expected", "").strip().lower()
            if args.check_exists:
                cnf = PROJECT_ROOT / row["cnf_path"]
                if not cnf.exists():
                    continue
            if expected == "sat":
                sat_instances.append(row)
            elif expected == "unsat":
                unsat_instances.append(row)

    log.info("Available: %d SAT, %d UNSAT", len(sat_instances), len(unsat_instances))

    rng = random.Random(args.seed)
    rng.shuffle(sat_instances)
    rng.shuffle(unsat_instances)

    selected_sat = sat_instances[:args.sat]
    selected_unsat = unsat_instances[:args.unsat]
    selected = selected_sat + selected_unsat
    selected.sort(key=lambda r: r["filename"])

    output_path = args.output
    if not Path(output_path).is_absolute():
        output_path = str(PROJECT_ROOT / output_path)
    Path(output_path).parent.mkdir(parents=True, exist_ok=True)

    fieldnames = list(selected[0].keys()) if selected else ["hash", "filename", "cnf_path", "expected", "family", "author"]
    with open(output_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(selected)

    log.info("Selected: %d SAT + %d UNSAT = %d total",
             len(selected_sat), len(selected_unsat), len(selected))
    log.info("Output: %s", output_path)


if __name__ == "__main__":
    main()
