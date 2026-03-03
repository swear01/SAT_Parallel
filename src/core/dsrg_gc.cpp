#include "core/dsrg.h"

#include <algorithm>
#include <cstdlib>

namespace sat_parallel {

// ---------------------------------------------------------------------------
// evict_stale_nodes
// ---------------------------------------------------------------------------

size_t DSRG::evict_stale_nodes(uint64_t current_conflict) {
    std::vector<uint32_t> to_remove;

    for (const auto& [id, node] : nodes_) {
        if (node.is_original) continue;

        bool weight_low = node.weight < config_.eviction_weight_threshold;
        bool lbd_high   = node.lbd > config_.eviction_lbd_threshold;
        bool old_enough = (current_conflict - node.birth_conflict) >
                          static_cast<uint64_t>(config_.min_age_before_eviction);

        if (weight_low && lbd_high && old_enough) {
            to_remove.push_back(id);
        }
    }

    for (uint32_t id : to_remove) {
        remove_node(id);
    }

    return to_remove.size();
}

// ---------------------------------------------------------------------------
// prune_weak_edges
// ---------------------------------------------------------------------------

size_t DSRG::prune_weak_edges() {
    std::vector<std::pair<uint32_t, uint32_t>> to_remove;

    for (const auto& [key, edge] : edges_) {
        if (edge.weight < config_.edge_removal_threshold) {
            to_remove.emplace_back(edge.source, edge.target);
        }
    }

    for (auto [a, b] : to_remove) {
        remove_edge(a, b);
    }

    return to_remove.size();
}

// ---------------------------------------------------------------------------
// merge_subsumption
// ---------------------------------------------------------------------------

void DSRG::merge_subsumption(uint32_t subsuming_id, uint32_t subsumed_id) {
    if (!has_node(subsuming_id) || !has_node(subsumed_id)) return;
    if (subsuming_id == subsumed_id) return;

    auto* subsuming = const_cast<GraphNode*>(get_node(subsuming_id));
    const auto* subsumed = get_node(subsumed_id);

    subsuming->weight += subsumed->weight;

    // Transfer all edges from subsumed to subsuming.
    auto neighbors = get_neighbors(subsumed_id);
    for (uint32_t nb : neighbors) {
        if (nb == subsuming_id) continue;

        const auto* old_edge = get_edge(subsumed_id, nb);
        if (!old_edge) continue;

        float transferred_weight = old_edge->weight;
        int transferred_count    = old_edge->co_conflict_count;

        auto* existing = const_cast<GraphEdge*>(get_edge(subsuming_id, nb));
        if (existing) {
            existing->weight += transferred_weight;
            existing->co_conflict_count += transferred_count;
        } else {
            create_edge(subsuming_id, nb, transferred_count);
            auto* new_edge = const_cast<GraphEdge*>(get_edge(subsuming_id, nb));
            if (new_edge) {
                new_edge->weight = transferred_weight;
            }
        }
    }

    remove_node(subsumed_id);
}

// ---------------------------------------------------------------------------
// handle_variable_elimination
// ---------------------------------------------------------------------------

void DSRG::handle_variable_elimination(
    uint32_t eliminated_var,
    std::span<const uint32_t> removed_clause_ids,
    std::span<const GraphNode> new_resolvents,
    const std::vector<std::vector<int>>& resolvent_literals) {

    // Remove var from var_clauses_ — the per-clause cleanup happens in
    // remove_node, but we also need to handle clauses that are NOT being
    // removed (they still exist but no longer reference this variable).
    var_clauses_.erase(eliminated_var);

    // For clauses that remain in the graph but contained the eliminated
    // variable, remove that variable from their clause_vars_ entry.
    // (Not needed here because removed_clause_ids lists ALL affected clauses.)

    for (uint32_t cid : removed_clause_ids) {
        remove_node(cid);
    }

    for (size_t i = 0; i < new_resolvents.size(); ++i) {
        const auto& nr = new_resolvents[i];
        const auto& lits = resolvent_literals[i];
        add_node(nr.clause_id, lits, nr.lbd, nr.is_original, nr.birth_conflict);
    }
}

}  // namespace sat_parallel
