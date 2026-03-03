#!/usr/bin/env python3
"""Run a SAT solver on a benchmark set and record results.

Presets:
    python scripts/run_benchmark.py --preset test  --solver deps/cadical/build/cadical
    python scripts/run_benchmark.py --preset full  --solver deps/painless/painless --solver-cpus 32

Custom:
    python scripts/run_benchmark.py --solver deps/cadical/build/cadical \\
        --benchmarks benchmarks/instances.csv --timeout 1000 --limit 20
"""

import argparse
import csv
import logging
import os
import platform
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import datetime
from pathlib import Path

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger(__name__)

PROJECT_ROOT = Path(__file__).resolve().parent.parent
RESULTS_DIR = PROJECT_ROOT / "results"

EXIT_SAT = 10
EXIT_UNSAT = 20


def detect_solver_type(solver_path):
    """Detect whether solver is painless or cadical based on binary name."""
    name = Path(solver_path).name.lower()
    if "painless" in name:
        return "painless"
    if "cadical" in name:
        return "cadical"
    return "generic"


def build_command(solver_path, cnf_path, timeout, solver_cpus, solver_type):
    """Build the solver command with appropriate arguments."""
    cmd = [str(solver_path)]

    if solver_type == "painless":
        cmd.extend([f"-c={solver_cpus}", f"-t={timeout}"])
    elif solver_type == "cadical":
        cmd.extend(["-t", str(timeout)])

    cmd.append(str(cnf_path))
    return cmd


def run_single(solver_path, cnf_path, timeout, solver_cpus, solver_type):
    """Run solver on a single instance. Returns dict with results."""
    cnf = Path(cnf_path)
    if not cnf.exists():
        return {
            "instance": cnf.name,
            "cnf_path": str(cnf_path),
            "result": "ERROR",
            "wall_time": 0.0,
            "exit_code": -1,
            "error": f"File not found: {cnf_path}",
        }

    cmd = build_command(solver_path, cnf_path, timeout, solver_cpus, solver_type)

    wall_limit = timeout + 60

    start = time.monotonic()
    try:
        proc = subprocess.run(
            cmd,
            timeout=wall_limit,
            capture_output=True,
        )
        elapsed = time.monotonic() - start
        exit_code = proc.returncode

        if exit_code == EXIT_SAT:
            result = "SAT"
        elif exit_code == EXIT_UNSAT:
            result = "UNSAT"
        elif elapsed >= timeout - 1:
            result = "TIMEOUT"
        else:
            result = "UNKNOWN"

        return {
            "instance": cnf.name,
            "cnf_path": str(cnf_path),
            "result": result,
            "wall_time": round(elapsed, 3),
            "exit_code": exit_code,
            "error": "",
        }

    except subprocess.TimeoutExpired:
        elapsed = time.monotonic() - start
        return {
            "instance": cnf.name,
            "cnf_path": str(cnf_path),
            "result": "TIMEOUT",
            "wall_time": round(elapsed, 3),
            "exit_code": -1,
            "error": f"Wall clock limit {wall_limit}s exceeded",
        }
    except Exception as e:
        elapsed = time.monotonic() - start
        return {
            "instance": cnf.name,
            "cnf_path": str(cnf_path),
            "result": "ERROR",
            "wall_time": round(elapsed, 3),
            "exit_code": -1,
            "error": str(e),
        }


def _worker(args):
    """Wrapper for ProcessPoolExecutor."""
    return run_single(*args)


def load_instances(csv_path):
    """Load instance list from instances.csv."""
    instances = []
    with open(csv_path) as f:
        for row in csv.DictReader(f):
            cnf_path = PROJECT_ROOT / row["cnf_path"]
            instances.append({
                "cnf_path": str(cnf_path),
                "expected": row.get("expected", "unknown"),
                "hash": row.get("hash", ""),
            })
    return instances


PRESETS = {
    "test": {"timeout": 5, "limit": 10, "desc": "Quick smoke test (5s, 10 instances)"},
    "full": {"timeout": 1000, "limit": None, "desc": "Full run (1000s, all instances)"},
}


def main():
    parser = argparse.ArgumentParser(description="Run SAT solver benchmark suite")
    parser.add_argument("--solver", required=True, help="Path to solver binary")
    parser.add_argument("--preset", choices=list(PRESETS.keys()), default=None,
                        help="Preset config: 'test' (5s, 10 cases) or 'full' (1000s, all)")
    parser.add_argument("--benchmarks", type=str, default="benchmarks/sc2023/instances.csv",
                        help="Path to instances.csv (default: benchmarks/sc2023/instances.csv)")
    parser.add_argument("--timeout", type=int, default=None, help="Per-instance timeout in seconds")
    parser.add_argument("--limit", type=int, default=None, help="Max number of instances to run")
    parser.add_argument("--solver-cpus", type=int, default=0,
                        help="CPUs for the solver (Painless -c flag, 0=auto)")
    parser.add_argument("--parallel", type=int, default=1,
                        help="Number of instances to run in parallel (default: 1)")
    parser.add_argument("--tag", type=str, default=None, help="Tag for output filename")
    parser.add_argument("--output-dir", type=str, default=None, help="Output directory (default: results/)")
    args = parser.parse_args()

    if args.preset:
        p = PRESETS[args.preset]
        log.info("Using preset '%s': %s", args.preset, p["desc"])
        if args.timeout is None:
            args.timeout = p["timeout"]
        if args.limit is None:
            args.limit = p["limit"]
    if args.timeout is None:
        args.timeout = 1000

    if args.solver_cpus == 0:
        import multiprocessing
        args.solver_cpus = multiprocessing.cpu_count()

    solver_path = Path(args.solver).resolve()
    if not solver_path.exists():
        log.error("Solver not found: %s", solver_path)
        sys.exit(1)

    solver_type = detect_solver_type(solver_path)
    log.info("Solver: %s (type: %s)", solver_path.name, solver_type)

    benchmarks_path = args.benchmarks
    if not Path(benchmarks_path).is_absolute():
        benchmarks_path = str(PROJECT_ROOT / benchmarks_path)

    instances = load_instances(benchmarks_path)
    if args.limit:
        instances = instances[:args.limit]
    log.info("Loaded %d instances from %s", len(instances), benchmarks_path)

    tag = args.tag or solver_path.stem
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = Path(args.output_dir) if args.output_dir else RESULTS_DIR
    out_dir.mkdir(parents=True, exist_ok=True)
    out_csv = out_dir / f"{tag}_{timestamp}.csv"

    meta = {
        "solver": str(solver_path),
        "solver_type": solver_type,
        "timeout": args.timeout,
        "solver_cpus": args.solver_cpus,
        "parallel": args.parallel,
        "timestamp": timestamp,
        "hostname": platform.node(),
    }
    log.info("Config: timeout=%ds, solver_cpus=%d, parallel=%d", args.timeout, args.solver_cpus, args.parallel)

    tasks = [
        (str(solver_path), inst["cnf_path"], args.timeout, args.solver_cpus, solver_type)
        for inst in instances
    ]

    results = []
    fieldnames = ["instance", "cnf_path", "result", "wall_time", "exit_code", "expected", "error"]

    with open(out_csv, "w", newline="") as f:
        f.write(f"# solver={meta['solver']}\n")
        f.write(f"# timeout={meta['timeout']}\n")
        f.write(f"# solver_cpus={meta['solver_cpus']}\n")
        f.write(f"# hostname={meta['hostname']}\n")
        f.write(f"# timestamp={meta['timestamp']}\n")
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()

        completed = 0
        total = len(tasks)
        sat_count, unsat_count, timeout_count, error_count = 0, 0, 0, 0

        if args.parallel <= 1:
            for i, task in enumerate(tasks):
                result = run_single(*task)
                result["expected"] = instances[i]["expected"]
                results.append(result)
                writer.writerow(result)
                f.flush()
                completed += 1

                if result["result"] == "SAT":
                    sat_count += 1
                elif result["result"] == "UNSAT":
                    unsat_count += 1
                elif result["result"] == "TIMEOUT":
                    timeout_count += 1
                else:
                    error_count += 1

                log.info(
                    "[%d/%d] %-40s %7s %8.1fs  (SAT=%d UNSAT=%d TO=%d ERR=%d)",
                    completed, total, result["instance"][:40],
                    result["result"], result["wall_time"],
                    sat_count, unsat_count, timeout_count, error_count,
                )
        else:
            with ProcessPoolExecutor(max_workers=args.parallel) as pool:
                future_to_idx = {
                    pool.submit(run_single, *task): i
                    for i, task in enumerate(tasks)
                }
                for future in as_completed(future_to_idx):
                    idx = future_to_idx[future]
                    result = future.result()
                    result["expected"] = instances[idx]["expected"]
                    results.append(result)
                    writer.writerow(result)
                    f.flush()
                    completed += 1

                    if result["result"] == "SAT":
                        sat_count += 1
                    elif result["result"] == "UNSAT":
                        unsat_count += 1
                    elif result["result"] == "TIMEOUT":
                        timeout_count += 1
                    else:
                        error_count += 1

                    log.info(
                        "[%d/%d] %-40s %7s %8.1fs  (SAT=%d UNSAT=%d TO=%d ERR=%d)",
                        completed, total, result["instance"][:40],
                        result["result"], result["wall_time"],
                        sat_count, unsat_count, timeout_count, error_count,
                    )

    solved = sat_count + unsat_count
    log.info("=" * 60)
    log.info("Results: %d/%d solved (SAT=%d, UNSAT=%d, TIMEOUT=%d, ERROR=%d)",
             solved, total, sat_count, unsat_count, timeout_count, error_count)
    log.info("Output: %s", out_csv)


if __name__ == "__main__":
    main()
