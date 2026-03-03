#pragma once

#include "comm/delta_patch.h"
#include "comm/mpsc_queue.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace sat_parallel {

struct PatchBuilderConfig {
    int   conflict_interval = 10000;  // conflicts between forced patches
    int   lbd_trigger       = 2;      // immediate trigger on very good clause
    int   lbd_entry         = 3;      // LBD threshold for including in patch
    size_t budget_bytes     = 4096;   // max patch size
};

inline PatchBuilderConfig load_patch_builder_config(const std::string& yaml_path) {
    PatchBuilderConfig cfg;
    YAML::Node root = YAML::LoadFile(yaml_path);
    if (auto comm = root["communication"]) {
        if (comm["delta_patch_conflict_interval"])
            cfg.conflict_interval = comm["delta_patch_conflict_interval"].as<int>();
        if (comm["delta_patch_lbd_trigger"])
            cfg.lbd_trigger = comm["delta_patch_lbd_trigger"].as<int>();
    }
    if (auto g = root["graph"]) {
        if (g["lbd_entry_threshold"])
            cfg.lbd_entry = g["lbd_entry_threshold"].as<int>();
    }
    return cfg;
}

// Accumulates solver events and emits DeltaPatch when trigger fires.
// One instance per worker thread. Not thread-safe (single-writer by design).
class PatchBuilder {
public:
    PatchBuilder(uint32_t worker_id, PatchBuilderConfig config,
                 MPSCQueue<DeltaPatch>& out_queue)
        : worker_id_(worker_id), config_(config), queue_(out_queue) {}

    // Called by the solver on every conflict.
    // If a trigger fires, flushes the accumulated patch.
    void on_conflict(uint64_t conflict_count) {
        conflicts_since_last_++;
        conflict_count_ = conflict_count;

        if (conflicts_since_last_ >= static_cast<uint64_t>(config_.conflict_interval)) {
            flush();
        }
    }

    // Called when a new clause is learned.
    // Clauses with LBD <= lbd_trigger cause an immediate flush.
    void on_clause_learned(uint32_t clause_id,
                           const int* literals, int length, int lbd) {
        if (lbd <= config_.lbd_entry) {
            DeltaPatch::LearnedClause cl;
            cl.clause_id = clause_id;
            cl.literals.assign(literals, literals + length);
            cl.lbd = lbd;
            pending_.new_clauses.push_back(std::move(cl));
        }

        if (lbd <= config_.lbd_trigger && !pending_.new_clauses.empty()) {
            flush();
        }
    }

    // Called when two clauses participate in the same conflict analysis.
    void on_co_conflict(uint32_t clause_a, uint32_t clause_b) {
        auto key = co_conflict_key(clause_a, clause_b);
        co_conflicts_[key]++;
    }

    // Called when a variable is bumped (VSIDS event).
    void on_variable_bump(uint32_t var_id) {
        hot_vars_[var_id]++;
    }

    // Force-flush whatever is accumulated (e.g. at solver shutdown).
    void flush() {
        if (pending_.new_clauses.empty() &&
            co_conflicts_.empty() &&
            hot_vars_.empty()) {
            conflicts_since_last_ = 0;
            return;
        }

        pending_.worker_id = worker_id_;
        pending_.conflict_count = conflict_count_;

        for (const auto& [key, count] : co_conflicts_) {
            auto [a, b] = decode_key(key);
            pending_.conflict_pairs.push_back({a, b, count});
        }

        // Top hot variables (limit to keep under budget).
        std::vector<std::pair<uint32_t, int>> sorted_vars(
            hot_vars_.begin(), hot_vars_.end());
        std::sort(sorted_vars.begin(), sorted_vars.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        constexpr int MAX_HOT_VARS = 50;
        int n = std::min(MAX_HOT_VARS, static_cast<int>(sorted_vars.size()));
        for (int i = 0; i < n; ++i) {
            pending_.hot_variables.push_back(
                {sorted_vars[static_cast<size_t>(i)].first,
                 sorted_vars[static_cast<size_t>(i)].second});
        }

        // Enforce budget: trim clauses if exceeding.
        while (pending_.estimated_size_bytes() > config_.budget_bytes &&
               pending_.new_clauses.size() > 1) {
            pending_.new_clauses.pop_back();
        }

        queue_.push(std::move(pending_));
        pending_ = DeltaPatch{};
        co_conflicts_.clear();
        hot_vars_.clear();
        conflicts_since_last_ = 0;
        patches_sent_++;
    }

    uint64_t patches_sent() const { return patches_sent_; }
    const PatchBuilderConfig& config() const { return config_; }

private:
    static uint64_t co_conflict_key(uint32_t a, uint32_t b) {
        auto lo = std::min(a, b), hi = std::max(a, b);
        return (static_cast<uint64_t>(lo) << 32) | hi;
    }
    static std::pair<uint32_t, uint32_t> decode_key(uint64_t key) {
        return {static_cast<uint32_t>(key >> 32),
                static_cast<uint32_t>(key & 0xFFFFFFFF)};
    }

    uint32_t worker_id_;
    PatchBuilderConfig config_;
    MPSCQueue<DeltaPatch>& queue_;

    DeltaPatch pending_{};
    std::unordered_map<uint64_t, int> co_conflicts_;
    std::unordered_map<uint32_t, int> hot_vars_;
    uint64_t conflict_count_ = 0;
    uint64_t conflicts_since_last_ = 0;
    uint64_t patches_sent_ = 0;
};

}  // namespace sat_parallel
