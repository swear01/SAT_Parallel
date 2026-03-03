#include "gpu/gpu_prober.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cuda_runtime.h>
#include <numeric>
#include <vector>

#define CUDA_CHECK(call) do { \
    cudaError_t err = (call); \
    if (err != cudaSuccess) { \
        std::fprintf(stderr, "CUDA error at %s:%d: %s\n", \
                     __FILE__, __LINE__, cudaGetErrorString(err)); \
    } \
} while(0)

namespace sat_parallel {

GPUProber::GPUProber(GPUProberConfig config, GPUChannel& channel)
    : config_(config), channel_(channel) {}

GPUProber::~GPUProber() {
    stop();
    free_device_memory();
}

void GPUProber::load_clauses(const std::vector<std::vector<int>>& clauses,
                             const std::vector<uint32_t>& clause_ids,
                             int num_vars) {
    num_clauses_ = static_cast<int>(clauses.size());
    num_vars_ = num_vars;
    h_clause_ids_ = clause_ids;

    h_clause_offsets_.resize(static_cast<size_t>(num_clauses_ + 1));
    h_clause_offsets_[0] = 0;
    h_literals_.clear();

    for (int i = 0; i < num_clauses_; ++i) {
        for (int lit : clauses[static_cast<size_t>(i)]) {
            h_literals_.push_back(lit);
        }
        h_clause_offsets_[static_cast<size_t>(i + 1)] =
            static_cast<int>(h_literals_.size());
    }

    free_device_memory();
    alloc_device_memory();
    upload_clause_db();
}

void GPUProber::alloc_device_memory() {
    if (num_clauses_ == 0 || num_vars_ == 0) return;
    int total_lits = static_cast<int>(h_literals_.size());

    CUDA_CHECK(cudaMalloc(&d_clause_offsets_,
                          static_cast<size_t>(num_clauses_ + 1) * sizeof(int)));
    CUDA_CHECK(cudaMalloc(&d_literals_,
                          static_cast<size_t>(total_lits) * sizeof(int)));
    CUDA_CHECK(cudaMalloc(&d_assignments_,
                          static_cast<size_t>(total_threads_ * num_vars_) * sizeof(bool)));
    CUDA_CHECK(cudaMalloc(&d_unsat_counts_,
                          static_cast<size_t>(total_threads_) * sizeof(int)));
    CUDA_CHECK(cudaMalloc(&d_best_unsat_,
                          static_cast<size_t>(total_threads_) * sizeof(int)));
    CUDA_CHECK(cudaMalloc(&d_best_assignments_,
                          static_cast<size_t>(total_threads_ * num_vars_) * sizeof(bool)));
    CUDA_CHECK(cudaMalloc(&d_clause_unsat_freq_,
                          static_cast<size_t>(num_clauses_) * sizeof(int)));
}

void GPUProber::free_device_memory() {
    if (d_clause_offsets_)    { cudaFree(d_clause_offsets_);    d_clause_offsets_ = nullptr; }
    if (d_literals_)          { cudaFree(d_literals_);          d_literals_ = nullptr; }
    if (d_assignments_)       { cudaFree(d_assignments_);       d_assignments_ = nullptr; }
    if (d_unsat_counts_)      { cudaFree(d_unsat_counts_);      d_unsat_counts_ = nullptr; }
    if (d_best_unsat_)        { cudaFree(d_best_unsat_);        d_best_unsat_ = nullptr; }
    if (d_best_assignments_)  { cudaFree(d_best_assignments_);  d_best_assignments_ = nullptr; }
    if (d_clause_unsat_freq_) { cudaFree(d_clause_unsat_freq_); d_clause_unsat_freq_ = nullptr; }
}

void GPUProber::upload_clause_db() {
    int total_lits = static_cast<int>(h_literals_.size());
    CUDA_CHECK(cudaMemcpy(d_clause_offsets_, h_clause_offsets_.data(),
                          static_cast<size_t>(num_clauses_ + 1) * sizeof(int),
                          cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_literals_, h_literals_.data(),
                          static_cast<size_t>(total_lits) * sizeof(int),
                          cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemset(d_clause_unsat_freq_, 0,
                          static_cast<size_t>(num_clauses_) * sizeof(int)));
}

void GPUProber::start() {
    if (running_.load()) return;
    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this]() { run_loop(); });
}

void GPUProber::stop() {
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
}

void GPUProber::run_loop() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        check_new_clauses();
        if (num_clauses_ > 0 && num_vars_ > 0) {
            run_one_batch();
            collect_and_report();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void GPUProber::run_one_batch() {
    WalkSATParams params{};
    params.clause_offsets    = d_clause_offsets_;
    params.literals          = d_literals_;
    params.num_clauses       = num_clauses_;
    params.num_vars          = num_vars_;
    params.assignments       = d_assignments_;
    params.unsat_counts      = d_unsat_counts_;
    params.best_unsat_counts = d_best_unsat_;
    params.best_assignments  = d_best_assignments_;
    params.clause_unsat_freq = d_clause_unsat_freq_;
    params.max_flips         = config_.max_flips_per_run;
    params.noise_prob        = config_.noise_probability;

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    params.seed = static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());

    launch_walksat_kernel(params, grid_size_, block_size_);
    CUDA_CHECK(cudaDeviceSynchronize());

    total_steps_.fetch_add(
        static_cast<uint64_t>(total_threads_) * static_cast<uint64_t>(config_.max_flips_per_run),
        std::memory_order_relaxed);
}

void GPUProber::collect_and_report() {
    // Download hotzone frequencies.
    std::vector<int> h_freq(static_cast<size_t>(num_clauses_));
    CUDA_CHECK(cudaMemcpy(h_freq.data(), d_clause_unsat_freq_,
                          static_cast<size_t>(num_clauses_) * sizeof(int),
                          cudaMemcpyDeviceToHost));

    // Reset frequency counters for next batch.
    CUDA_CHECK(cudaMemset(d_clause_unsat_freq_, 0,
                          static_cast<size_t>(num_clauses_) * sizeof(int)));

    // Find top-K hotzone clauses.
    std::vector<std::pair<int, int>> indexed_freq;
    indexed_freq.reserve(static_cast<size_t>(num_clauses_));
    for (int i = 0; i < num_clauses_; ++i) {
        if (h_freq[static_cast<size_t>(i)] > 0) {
            indexed_freq.push_back({i, h_freq[static_cast<size_t>(i)]});
        }
    }
    std::sort(indexed_freq.begin(), indexed_freq.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    int top_k = std::min(config_.hotzone_top_k,
                         static_cast<int>(indexed_freq.size()));

    // Download best assignments and unsat counts.
    std::vector<int> h_best_unsat(static_cast<size_t>(total_threads_));
    CUDA_CHECK(cudaMemcpy(h_best_unsat.data(), d_best_unsat_,
                          static_cast<size_t>(total_threads_) * sizeof(int),
                          cudaMemcpyDeviceToHost));

    // Find the thread with the lowest unsat count.
    int best_thread = 0;
    for (int t = 1; t < total_threads_; ++t) {
        if (h_best_unsat[static_cast<size_t>(t)] <
            h_best_unsat[static_cast<size_t>(best_thread)]) {
            best_thread = t;
        }
    }

    // std::vector<bool> is bit-packed; use a raw buffer instead.
    std::vector<char> h_best_assign(static_cast<size_t>(num_vars_));
    CUDA_CHECK(cudaMemcpy(h_best_assign.data(),
                          d_best_assignments_ + static_cast<ptrdiff_t>(best_thread) * num_vars_,
                          static_cast<size_t>(num_vars_) * sizeof(bool),
                          cudaMemcpyDeviceToHost));

    // Build and send report.
    GPUReport report;
    report.unsat_count = h_best_unsat[static_cast<size_t>(best_thread)];

    for (int i = 0; i < top_k; ++i) {
        int clause_local_idx = indexed_freq[static_cast<size_t>(i)].first;
        int freq = indexed_freq[static_cast<size_t>(i)].second;
        uint32_t cid = h_clause_ids_[static_cast<size_t>(clause_local_idx)];
        report.hotzone.push_back({cid, freq});
    }

    for (int v = 0; v < num_vars_; ++v) {
        report.best_assignment.push_back(
            {static_cast<uint32_t>(v + 1), h_best_assign[static_cast<size_t>(v)] != 0});
    }

    channel_.send_report(std::move(report));
}

void GPUProber::check_new_clauses() {
    auto push = channel_.consume_push();
    if (!push || push->clauses.empty()) return;

    // Append new clauses to the host DB.
    for (const auto& cl : push->clauses) {
        h_clause_ids_.push_back(cl.clause_id);
        for (int lit : cl.literals) {
            h_literals_.push_back(lit);
        }
        h_clause_offsets_.push_back(static_cast<int>(h_literals_.size()));
        num_clauses_++;
    }

    // Reallocate and re-upload.
    free_device_memory();
    alloc_device_memory();
    upload_clause_db();
}

}  // namespace sat_parallel
