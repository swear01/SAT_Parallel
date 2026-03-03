#pragma once

#include "core/dsrg_types.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace sat_parallel {

class DSRG {
public:
    explicit DSRG(DSRGConfig config);

    // --- Node operations ---

    // Returns false if clause_id already exists or LBD filter rejects it.
    bool add_node(uint32_t clause_id, std::span<const int> literals,
                  int lbd, bool is_original, uint64_t birth_conflict);

    // Returns false if clause_id not found. Removes all incident edges and
    // cleans up clause-variable indices.
    bool remove_node(uint32_t clause_id);

    const GraphNode* get_node(uint32_t clause_id) const;
    bool has_node(uint32_t clause_id) const;
    size_t node_count() const;

    // --- Edge operations ---

    // Track a co-conflict event between two clauses.
    // If the edge already exists: increments co_conflict_count and updates
    // weight via EMA.  If no edge yet: increments the pending counter and
    // creates the edge once edge_creation_threshold is reached.
    void record_co_conflict(uint32_t clause_a, uint32_t clause_b);

    bool remove_edge(uint32_t a, uint32_t b);
    const GraphEdge* get_edge(uint32_t a, uint32_t b) const;
    bool has_edge(uint32_t a, uint32_t b) const;
    size_t edge_count() const;
    const std::vector<uint32_t>& get_neighbors(uint32_t clause_id) const;

    // --- Weight operations ---

    // Full-pass decay: nodes *= gamma, edges *= beta.
    void decay_all_weights();

    void boost_node(uint32_t clause_id, float delta);

    // --- Clause-Variable index ---

    const std::vector<uint32_t>& get_clause_vars(uint32_t clause_id) const;
    const std::vector<uint32_t>& get_var_clauses(uint32_t var_id) const;

    // --- GC operations (implemented in dsrg_gc.cpp) ---

    // Evict nodes satisfying all three conditions:
    //   weight < threshold, lbd > threshold, age > min
    // Original clauses are never evicted.  Returns eviction count.
    size_t evict_stale_nodes(uint64_t current_conflict);

    // Remove edges with weight < edge_removal_threshold.  Returns count.
    size_t prune_weak_edges();

    // Subsumption: merge subsumed into subsuming.
    void merge_subsumption(uint32_t subsuming_id, uint32_t subsumed_id);

    // Variable elimination: remove affected clauses, add new resolvents.
    void handle_variable_elimination(
        uint32_t eliminated_var,
        std::span<const uint32_t> removed_clause_ids,
        std::span<const GraphNode> new_resolvents,
        const std::vector<std::vector<int>>& resolvent_literals);

    // --- Iteration helpers ---

    template <typename Fn>
    void for_each_node(Fn&& fn) const {
        for (const auto& [id, node] : nodes_) {
            fn(node);
        }
    }

    template <typename Fn>
    void for_each_edge(Fn&& fn) const {
        for (const auto& [key, edge] : edges_) {
            fn(edge);
        }
    }

    const DSRGConfig& config() const { return config_; }

private:
    static uint64_t edge_key(uint32_t a, uint32_t b) {
        auto lo = std::min(a, b);
        auto hi = std::max(a, b);
        return (static_cast<uint64_t>(lo) << 32) | hi;
    }

    void remove_from_adj(uint32_t node, uint32_t neighbor);
    void remove_all_edges_of(uint32_t clause_id);
    void remove_clause_var_index(uint32_t clause_id);
    bool create_edge(uint32_t a, uint32_t b, int initial_co_conflict);

    DSRGConfig config_;

    std::unordered_map<uint32_t, GraphNode> nodes_;
    std::unordered_map<uint64_t, GraphEdge> edges_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> adj_;
    std::unordered_map<uint64_t, int> pending_conflicts_;

    std::unordered_map<uint32_t, std::vector<uint32_t>> clause_vars_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> var_clauses_;

    static inline const std::vector<uint32_t> empty_vec_{};
};

}  // namespace sat_parallel
