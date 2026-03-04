#!/bin/bash
# Baseline vs DSRG+GPU Benchmark Experiment
# 循序執行，避免 baseline 與實驗組同時跑造成 GPU/CPU 搶佔
# Run with: nohup bash scripts/run_experiment.sh > results/experiment.log 2>&1 &
set -e

cd "$(dirname "$0")/.."
source env.sh 2>/dev/null || true

SOLVER=deps/painless/painless
TIMEOUT=60
CPUS=8
BENCHMARKS=benchmarks/sample100/instances.csv

mkdir -p results

echo "================================================================"
echo "Baseline vs DSRG+GPU Benchmark Experiment (sample100)"
echo "Started: $(date)"
echo "Solver: $SOLVER"
echo "Timeout: ${TIMEOUT}s, CPUs: $CPUS"
echo "Instances: $BENCHMARKS"
echo "================================================================"

echo ""
echo "=== Phase 1/2: Baseline (Painless HordeSat, shr-strat=1) ==="
echo "Start: $(date)"
python3 scripts/run_benchmark.py \
    --preset sample100 \
    --solver "$SOLVER" \
    --timeout "$TIMEOUT" \
    --solver-cpus "$CPUS" \
    --benchmarks "$BENCHMARKS" \
    --solver-args="-shr-strat=1" \
    --tag baseline-sample100
echo "Phase 1 done: $(date)"

echo ""
echo "=== Phase 2/2: DSRG + GPU (shr-strat=4 -shr-gpu) ==="
echo "Start: $(date)"
python3 scripts/run_benchmark.py \
    --preset sample100 \
    --solver "$SOLVER" \
    --timeout "$TIMEOUT" \
    --solver-cpus "$CPUS" \
    --benchmarks "$BENCHMARKS" \
    --solver-args="-shr-strat=4 -shr-gpu" \
    --tag gpu-sample100
echo "Phase 2 done: $(date)"

echo ""
echo "=== Comparison ==="
BASELINE_CSV=$(ls -t results/baseline-sample100_*.csv 2>/dev/null | head -1)
GPU_CSV=$(ls -t results/gpu-sample100_*.csv 2>/dev/null | head -1)

if [ -n "$BASELINE_CSV" ] && [ -n "$GPU_CSV" ]; then
    echo "Baseline: $BASELINE_CSV"
    echo "GPU:      $GPU_CSV"
    python3 scripts/score.py "$BASELINE_CSV" "$GPU_CSV" --timeout "$TIMEOUT" || true
else
    echo "ERROR: Could not find result CSVs for comparison"
fi

echo ""
echo "================================================================"
echo "Experiment completed: $(date)"
echo "================================================================"
