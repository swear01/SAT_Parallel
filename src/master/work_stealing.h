#pragma once

#include "master/partitioner.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <unordered_set>
#include <vector>

namespace sat_parallel {

// Manages cube assignment and work stealing among workers.
// Thread-safe: workers may call steal() concurrently with Master's operations.
class WorkStealingManager {
public:
    explicit WorkStealingManager(int num_workers)
        : num_workers_(num_workers) {}

    // Master loads a fresh set of cubes (from partitioner output).
    void load_cubes(std::vector<Cube> cubes);

    // Assign the next unassigned cube to a worker. Returns nullopt if none left.
    std::optional<Cube> assign_next(int worker_id);

    // Worker reports a cube as finished (SAT / UNSAT / TIMEOUT).
    enum class CubeResult { SAT, UNSAT, TIMEOUT };
    void report_done(int worker_id, int cube_id, CubeResult result);

    // A worker that finished its cubes steals from the pool.
    // Re-splits the largest remaining unfinished cube if possible,
    // otherwise returns a whole unassigned cube.
    std::optional<Cube> steal(int worker_id);

    // Status queries.
    int remaining_cubes() const;
    bool all_done() const;
    bool found_sat() const;

private:
    int num_workers_;
    mutable std::mutex mu_;

    std::queue<Cube> pending_;
    std::unordered_set<int> in_progress_;
    std::unordered_set<int> completed_;
    bool sat_found_ = false;
};

}  // namespace sat_parallel
