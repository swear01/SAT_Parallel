#pragma once

#include "comm/mpsc_queue.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace sat_parallel {

struct GPUReport {
    struct HotzoneEntry {
        uint32_t clause_id;
        int frequency;
    };
    std::vector<HotzoneEntry> hotzone;

    struct PhaseHint {
        uint32_t var_id;
        bool phase;
    };
    std::vector<PhaseHint> best_assignment;

    int unsat_count = 0;
};

struct EdgeHotzoneReport {
    struct EdgeEntry {
        uint32_t clause_a;
        uint32_t clause_b;
        int count;
    };
    std::vector<EdgeEntry> top_edges;
};

struct GPUClausePush {
    struct Clause {
        uint32_t clause_id;
        std::vector<int> literals;
        int lbd;
    };
    std::vector<Clause> clauses;
    int num_vars = 0;  // max variable id; 0 = derive from literals
};

// Communication channel between Master and GPU Prober.
// Phase 3: standard memory.  Phase 6: CUDA pinned memory.
class GPUChannel {
public:
    GPUChannel() = default;

    // --- GPU -> Master (lock-free MPSC) ---

    void send_report(GPUReport report) {
        reports_.push(std::move(report));
    }

    std::optional<GPUReport> try_receive_report() {
        return reports_.try_pop();
    }

    template <typename Fn>
    size_t drain_reports(Fn&& fn) {
        return reports_.drain(std::forward<Fn>(fn));
    }

    void send_edge_report(EdgeHotzoneReport report) {
        edge_reports_.push(std::move(report));
    }

    template <typename Fn>
    size_t drain_edge_reports(Fn&& fn) {
        return edge_reports_.drain(std::forward<Fn>(fn));
    }

    // --- Master -> GPU (single-slot latest-value, mutex for GCC 11) ---

    void push_clauses(GPUClausePush push) {
        auto slot = std::make_shared<const GPUClausePush>(std::move(push));
        std::lock_guard<std::mutex> lk(push_mu_);
        pending_push_ = std::move(slot);
    }

    std::shared_ptr<const GPUClausePush> consume_push() {
        std::lock_guard<std::mutex> lk(push_mu_);
        return std::exchange(pending_push_, nullptr);
    }

private:
    MPSCQueue<GPUReport> reports_;
    MPSCQueue<EdgeHotzoneReport> edge_reports_;
    std::mutex push_mu_;
    std::shared_ptr<const GPUClausePush> pending_push_;
};

}  // namespace sat_parallel
