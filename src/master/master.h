#pragma once

#include "comm/broadcast.h"
#include "comm/delta_patch.h"
#include "comm/gpu_channel.h"
#include "comm/mpsc_queue.h"
#include "core/aggregation.h"
#include "core/centrality.h"
#include "core/dsrg.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace sat_parallel {

struct MasterConfig {
    DSRGConfig         dsrg;
    CentralityConfig   centrality;
    AggregationConfig  aggregation;

    int   centrality_interval      = 5000;   // conflicts between centrality runs
    int   gc_interval              = 10000;
    int   broadcast_interval_ms    = 1000;
    float gpu_hotzone_boost        = 2.0f;
    int   learnt_clause_lbd_filter = 3;
    int   top_k_clauses            = 50;
};

MasterConfig load_master_config(const std::string& yaml_path);

class Master {
public:
    explicit Master(MasterConfig config,
                    MPSCQueue<DeltaPatch>& patch_queue,
                    BroadcastChannel& broadcast_channel,
                    GPUChannel& gpu_channel);

    // Main entry point: process one batch of pending work.
    // Call from the master thread's event loop.
    // Returns the total number of delta patches + GPU reports processed.
    size_t tick();

    // Force a centrality compute + broadcast cycle (for testing).
    void force_centrality_and_broadcast();

    // Accessors for testing / integration.
    const DSRG& graph() const { return graph_; }
    DSRG& graph() { return graph_; }
    uint64_t total_conflicts_seen() const { return total_conflicts_; }

    const std::unordered_map<uint32_t, float>& latest_var_scores() const {
        return var_scores_;
    }

private:
    void apply_delta_patch(DeltaPatch& patch);
    void apply_gpu_report(GPUReport& report);
    void run_centrality_and_broadcast();
    void run_gc();
    void push_clauses_to_gpu();

    bool should_run_centrality() const;
    bool should_run_gc() const;
    bool should_broadcast() const;

    MasterConfig config_;
    DSRG graph_;

    MPSCQueue<DeltaPatch>& patch_queue_;
    BroadcastChannel& broadcast_channel_;
    GPUChannel& gpu_channel_;

    uint64_t total_conflicts_          = 0;
    uint64_t last_centrality_conflict_ = 0;
    uint64_t last_gc_conflict_         = 0;

    using Clock = std::chrono::steady_clock;
    Clock::time_point last_broadcast_time_ = Clock::now();
    Clock::time_point last_gpu_push_time_  = Clock::now();

    std::unordered_map<uint32_t, float> clause_centrality_;
    std::unordered_map<uint32_t, float> var_scores_;

    // Accumulate high-quality clauses for GPU push.
    std::vector<GPUClausePush::Clause> gpu_clause_buffer_;
};

}  // namespace sat_parallel
