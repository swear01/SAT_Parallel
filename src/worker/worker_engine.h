#pragma once

#include "comm/broadcast.h"
#include "comm/delta_patch.h"
#include "comm/mpsc_queue.h"
#include "master/partitioner.h"
#include "worker/local_weights.h"
#include "worker/patch_builder.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace sat_parallel {

struct WorkerConfig {
    LocalWeightsConfig  weights;
    PatchBuilderConfig  patch;
};

inline WorkerConfig load_worker_config(const std::string& yaml_path) {
    WorkerConfig cfg;
    cfg.weights = load_local_weights_config(yaml_path);
    cfg.patch   = load_patch_builder_config(yaml_path);
    return cfg;
}

// Abstract interface for the underlying SAT solver (CaDiCaL via Painless).
class SolverInterface {
public:
    virtual ~SolverInterface() = default;

    // Set assumption literals for cube-based solving.
    virtual void set_assumptions(const std::vector<int>& assumptions) = 0;

    // Retrieve current VSIDS scores for the top-N variables.
    virtual std::unordered_map<uint32_t, float> get_vsids_scores() const = 0;

    // Bump the priority of a variable in the solver's decision heuristic.
    virtual void bump_variable(uint32_t var_id, float score) = 0;
};

// Worker engine: bridges the SAT solver with the DSRG communication layer.
// One instance per worker thread.
class WorkerEngine {
public:
    WorkerEngine(uint32_t worker_id, WorkerConfig config,
                 MPSCQueue<DeltaPatch>& patch_queue,
                 const BroadcastChannel& broadcast)
        : worker_id_(worker_id),
          config_(config),
          local_weights_(config.weights),
          patch_builder_(worker_id, config.patch, patch_queue),
          broadcast_(broadcast) {}

    // --- Solver event callbacks ---

    void on_conflict(uint64_t conflict_count) {
        patch_builder_.on_conflict(conflict_count);
        maybe_poll_broadcast();
    }

    void on_clause_learned(uint32_t clause_id,
                           const int* literals, int length, int lbd) {
        patch_builder_.on_clause_learned(clause_id, literals, length, lbd);
    }

    void on_co_conflict(uint32_t clause_a, uint32_t clause_b) {
        patch_builder_.on_co_conflict(clause_a, clause_b);
    }

    void on_variable_bump(uint32_t var_id) {
        patch_builder_.on_variable_bump(var_id);
    }

    // --- Broadcast polling ---

    // Check for new broadcast from Master and merge into local weights.
    // Called periodically (internally throttled).
    void poll_broadcast() {
        uint64_t ver = broadcast_.version();
        if (ver <= last_broadcast_version_) return;
        last_broadcast_version_ = ver;

        auto snap = broadcast_.read();
        if (!snap) return;

        local_weights_.merge_broadcast(snap->top_k_var_scores);
    }

    // --- Cube assignment ---

    void set_cube(const Cube& cube) {
        current_cube_ = cube;
        cube_assumptions_.clear();
        for (const auto& [var_id, phase] : cube.assumptions) {
            int lit = phase ? static_cast<int>(var_id) : -static_cast<int>(var_id);
            cube_assumptions_.push_back(lit);
        }
    }

    bool has_cube() const { return current_cube_.has_value(); }
    const Cube& current_cube() const { return *current_cube_; }
    const std::vector<int>& cube_assumptions() const { return cube_assumptions_; }

    // Apply cube assumptions to the underlying solver.
    void apply_cube_to_solver(SolverInterface& solver) {
        if (!cube_assumptions_.empty()) {
            solver.set_assumptions(cube_assumptions_);
        }
    }

    // Apply graph-derived scores to solver's decision heuristic.
    void apply_scores_to_solver(SolverInterface& solver) {
        auto vsids = solver.get_vsids_scores();
        auto top = local_weights_.top_k(vsids, 100);
        for (const auto& [vid, score] : top) {
            solver.bump_variable(vid, score);
        }
    }

    // --- Lifecycle ---

    void flush() { patch_builder_.flush(); }

    // Accessors.
    uint32_t worker_id() const { return worker_id_; }
    const LocalWeights& local_weights() const { return local_weights_; }
    LocalWeights& local_weights() { return local_weights_; }
    PatchBuilder& patch_builder() { return patch_builder_; }
    uint64_t patches_sent() const { return patch_builder_.patches_sent(); }

private:
    static constexpr int BROADCAST_POLL_INTERVAL = 1000;

    void maybe_poll_broadcast() {
        if (++poll_counter_ >= BROADCAST_POLL_INTERVAL) {
            poll_broadcast();
            poll_counter_ = 0;
        }
    }

    uint32_t worker_id_;
    WorkerConfig config_;
    LocalWeights local_weights_;
    PatchBuilder patch_builder_;
    const BroadcastChannel& broadcast_;

    uint64_t last_broadcast_version_ = 0;
    int poll_counter_ = 0;

    std::optional<Cube> current_cube_;
    std::vector<int> cube_assumptions_;
};

}  // namespace sat_parallel
