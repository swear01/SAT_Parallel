#pragma once

#include "comm/gpu_channel.h"
#include "gpu/gpu_types.h"
#include "gpu/walksat_kernel.cuh"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace sat_parallel {

class GPUProber {
public:
    GPUProber(GPUProberConfig config, GPUChannel& channel);
    ~GPUProber();

    GPUProber(const GPUProber&) = delete;
    GPUProber& operator=(const GPUProber&) = delete;

    void load_clauses(const std::vector<std::vector<int>>& clauses,
                      const std::vector<uint32_t>& clause_ids,
                      int num_vars);

    void start();
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }
    uint64_t total_steps() const { return total_steps_.load(std::memory_order_relaxed); }

private:
    void run_loop();
    void run_one_batch();
    void collect_and_report();
    void check_new_clauses();
    void alloc_device_memory();
    void free_device_memory();
    void upload_clause_db();
    void compute_launch_config();

    GPUProberConfig config_;
    GPUChannel& channel_;

    std::vector<int> h_clause_offsets_;
    std::vector<int> h_literals_;
    std::vector<uint32_t> h_clause_ids_;
    int num_clauses_ = 0;
    int num_vars_    = 0;

    int*       d_clause_offsets_    = nullptr;
    int*       d_literals_          = nullptr;
    bool*      d_assignments_       = nullptr;
    int*       d_unsat_counts_      = nullptr;
    int*       d_best_unsat_        = nullptr;
    bool*      d_best_assignments_  = nullptr;
    int*       d_clause_unsat_freq_ = nullptr;
    uint64_t*  d_edge_keys_        = nullptr;
    int*       d_edge_counts_       = nullptr;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<uint64_t> total_steps_{0};

    int grid_size_     = 0;
    int block_size_    = 0;
    int total_threads_ = 0;
};

}  // namespace sat_parallel
