#pragma once

#include "comm/broadcast.h"
#include "worker/worker_engine.h"

#include <cadical.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

namespace sat_parallel {

// Bridges CaDiCaL (Painless-modified) with the DSRG WorkerEngine.
// Implements CaDiCaL::Learner for clause export/import and
// CaDiCaL::Terminator for cooperative shutdown.
class CaDiCaLAdapter : public SolverInterface,
                       public CaDiCaL::Learner,
                       public CaDiCaL::Terminator {
public:
    CaDiCaLAdapter(CaDiCaL::Solver& solver, WorkerEngine& engine, int max_export_lbd = 5)
        : solver_(solver), engine_(engine), max_export_lbd_(max_export_lbd) {
        solver_.connect_learner(this);
        solver_.connect_terminator(this);
    }

    ~CaDiCaLAdapter() {
        solver_.disconnect_learner();
        solver_.disconnect_terminator();
    }

    // ---- SolverInterface ----

    void set_assumptions(const std::vector<int>& assumptions) override {
        for (int lit : assumptions) {
            solver_.assume(lit);
        }
    }

    std::unordered_map<uint32_t, float> get_vsids_scores() const override {
        return {};
    }

    void bump_variable(uint32_t var_id, float /*score*/) override {
        solver_.phase(static_cast<int>(var_id));
    }

    // ---- CaDiCaL::Learner (Painless extension) ----

    bool learning(int size, int glue) override {
        if (size > 50 || glue > max_export_lbd_) return false;
        learning_buf_.clear();
        learning_glue_ = glue;
        return true;
    }

    void learn(int lit) override {
        if (lit == 0) {
            uint32_t cid = next_clause_id_.fetch_add(1, std::memory_order_relaxed);
            engine_.on_clause_learned(
                cid, learning_buf_.data(),
                static_cast<int>(learning_buf_.size()),
                learning_glue_);
            conflict_count_++;
            engine_.on_conflict(conflict_count_);
        } else {
            learning_buf_.push_back(lit);
        }
    }

    bool hasClauseToImport() override {
        std::lock_guard<std::mutex> lk(import_mu_);
        return !import_queue_.empty();
    }

    void getClauseToImport(std::vector<int>& clause, int& glue) override {
        std::lock_guard<std::mutex> lk(import_mu_);
        if (import_queue_.empty()) return;
        auto& front = import_queue_.front();
        clause = std::move(front.first);
        glue = front.second;
        import_queue_.pop();
    }

    // ---- CaDiCaL::Terminator ----

    bool terminate() override {
        return stop_requested_.load(std::memory_order_relaxed);
    }

    // ---- Control ----

    void request_stop() {
        stop_requested_.store(true, std::memory_order_relaxed);
    }

    void import_shared_clauses(const std::vector<std::vector<int>>& clauses, int default_lbd = 2) {
        std::lock_guard<std::mutex> lk(import_mu_);
        for (const auto& c : clauses) {
            import_queue_.push({c, default_lbd});
        }
    }

    void apply_phase_hints(const std::vector<GlobalBroadcast::VarScore>& var_scores, int top_n = 50) {
        int count = 0;
        for (const auto& vs : var_scores) {
            if (count >= top_n) break;
            solver_.phase(static_cast<int>(vs.var_id));
            count++;
        }
    }

    uint64_t conflict_count() const { return conflict_count_; }

private:
    CaDiCaL::Solver& solver_;
    WorkerEngine& engine_;
    int max_export_lbd_;

    std::atomic<bool> stop_requested_{false};
    static std::atomic<uint32_t> next_clause_id_;

    std::vector<int> learning_buf_;
    int learning_glue_ = 0;
    uint64_t conflict_count_ = 0;

    std::mutex import_mu_;
    std::queue<std::pair<std::vector<int>, int>> import_queue_;
};

inline std::atomic<uint32_t> CaDiCaLAdapter::next_clause_id_{1};

}  // namespace sat_parallel
