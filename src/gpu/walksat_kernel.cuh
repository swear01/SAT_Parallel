#pragma once

#include <cstdint>

namespace sat_parallel {

// WalkSAT kernel launch parameters.
struct WalkSATParams {
    // Clause DB (device pointers).
    const int* clause_offsets;   // [num_clauses + 1]
    const int* literals;         // packed literals
    int num_clauses;
    int num_vars;

    // Per-thread state (device pointers).
    bool* assignments;           // [num_threads * num_vars]
    int*  unsat_counts;          // [num_threads]
    int*  best_unsat_counts;     // [num_threads]
    bool* best_assignments;      // [num_threads * num_vars]

    // Hotzone accumulation (device pointers, shared across threads).
    int* clause_unsat_freq;      // [num_clauses] atomic counters

    // Parameters.
    int   max_flips;
    float noise_prob;
    unsigned long long seed;
};

// Launch the WalkSAT kernel.
// grid_size = number of independent random walks (blocks).
// block_size = threads per block (used for parallel clause evaluation).
void launch_walksat_kernel(const WalkSATParams& params,
                           int grid_size, int block_size);

}  // namespace sat_parallel
