#include "gpu/walksat_kernel.cuh"

#include <curand_kernel.h>

namespace sat_parallel {

__device__ int eval_clause(const int* clause_offsets, const int* literals,
                           int clause_idx, const bool* assignment) {
    int start = clause_offsets[clause_idx];
    int end   = clause_offsets[clause_idx + 1];
    for (int i = start; i < end; ++i) {
        int lit = literals[i];
        int var = (lit > 0) ? lit : -lit;
        bool val = assignment[var - 1];
        if ((lit > 0 && val) || (lit < 0 && !val)) return 1;  // satisfied
    }
    return 0;  // unsatisfied
}

__global__ void walksat_kernel(WalkSATParams p) {
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    int total_threads = gridDim.x * blockDim.x;
    if (tid >= total_threads) return;

    curandState rng;
    curand_init(p.seed + static_cast<unsigned long long>(tid), 0, 0, &rng);

    bool* my_assign = p.assignments + static_cast<ptrdiff_t>(tid) * p.num_vars;
    bool* my_best   = p.best_assignments + static_cast<ptrdiff_t>(tid) * p.num_vars;

    // Random initial assignment.
    for (int v = 0; v < p.num_vars; ++v) {
        my_assign[v] = (curand(&rng) & 1) != 0;
    }

    int best_unsat = p.num_clauses;

    for (int flip = 0; flip < p.max_flips; ++flip) {
        // Count unsatisfied clauses and pick a random one.
        int unsat_count = 0;
        int random_unsat_idx = -1;
        int seen_unsat = 0;

        for (int c = 0; c < p.num_clauses; ++c) {
            if (!eval_clause(p.clause_offsets, p.literals, c, my_assign)) {
                unsat_count++;
                // Reservoir sampling: pick uniformly at random.
                seen_unsat++;
                if (curand(&rng) % seen_unsat == 0) {
                    random_unsat_idx = c;
                }
                // Atomically increment hotzone frequency.
                atomicAdd(&p.clause_unsat_freq[c], 1);
            }
        }

        if (unsat_count == 0) {
            // SAT found! Record and stop.
            p.unsat_counts[tid] = 0;
            for (int v = 0; v < p.num_vars; ++v) my_best[v] = my_assign[v];
            p.best_unsat_counts[tid] = 0;
            return;
        }

        if (unsat_count < best_unsat) {
            best_unsat = unsat_count;
            for (int v = 0; v < p.num_vars; ++v) my_best[v] = my_assign[v];
        }

        // Pick a variable from the random unsatisfied clause to flip.
        int start = p.clause_offsets[random_unsat_idx];
        int end   = p.clause_offsets[random_unsat_idx + 1];
        int clause_len = end - start;

        int chosen_var;
        float r = curand_uniform(&rng);
        if (r < p.noise_prob) {
            // Random walk: pick a random variable from the clause.
            int pick = curand(&rng) % clause_len;
            int lit = p.literals[start + pick];
            chosen_var = (lit > 0) ? lit : -lit;
        } else {
            // Greedy: pick the variable that minimizes break count.
            int min_break = p.num_clauses + 1;
            chosen_var = 1;

            for (int i = start; i < end; ++i) {
                int lit = p.literals[i];
                int var = (lit > 0) ? lit : -lit;

                // Flip tentatively and count how many currently-sat clauses break.
                my_assign[var - 1] = !my_assign[var - 1];
                int break_count = 0;
                for (int c2 = 0; c2 < p.num_clauses; ++c2) {
                    if (!eval_clause(p.clause_offsets, p.literals, c2, my_assign)) {
                        break_count++;
                    }
                }
                my_assign[var - 1] = !my_assign[var - 1];  // undo

                if (break_count < min_break) {
                    min_break = break_count;
                    chosen_var = var;
                }
            }
        }

        my_assign[chosen_var - 1] = !my_assign[chosen_var - 1];
    }

    // Record final state.
    p.unsat_counts[tid] = best_unsat;
    p.best_unsat_counts[tid] = best_unsat;
}

void launch_walksat_kernel(const WalkSATParams& params,
                           int grid_size, int block_size) {
    walksat_kernel<<<grid_size, block_size>>>(params);
}

}  // namespace sat_parallel
