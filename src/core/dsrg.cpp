#include "core/dsrg.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sat_parallel {

DSRG::DSRG(DSRGConfig config) : config_(config) {}

// ---------------------------------------------------------------------------
// Node operations
// ---------------------------------------------------------------------------

bool DSRG::add_node(uint32_t clause_id, std::span<const int> literals,
                    int lbd, bool is_original, uint64_t birth_conflict) {
    if (nodes_.contains(clause_id))
        return false;

    if (!is_original && lbd > config_.lbd_entry_threshold)
        return false;

    GraphNode node{};
    node.clause_id      = clause_id;
    node.weight         = 1.0f;
    node.lbd            = lbd;
    node.length         = static_cast<int>(literals.size());
    node.is_original    = is_original;
    node.birth_conflict = birth_conflict;
    node.community_id   = -1;

    nodes_.emplace(clause_id, node);
    adj_[clause_id];  // ensure adjacency entry exists

    // Build clause-variable bidirectional index.
    auto& vars = clause_vars_[clause_id];
    vars.reserve(literals.size());
    for (int lit : literals) {
        uint32_t var = static_cast<uint32_t>(std::abs(lit));
        vars.push_back(var);
        var_clauses_[var].push_back(clause_id);
    }

    return true;
}

bool DSRG::remove_node(uint32_t clause_id) {
    if (!nodes_.contains(clause_id))
        return false;

    remove_all_edges_of(clause_id);
    remove_clause_var_index(clause_id);
    adj_.erase(clause_id);
    nodes_.erase(clause_id);
    return true;
}

const GraphNode* DSRG::get_node(uint32_t clause_id) const {
    auto it = nodes_.find(clause_id);
    return it != nodes_.end() ? &it->second : nullptr;
}

bool DSRG::has_node(uint32_t clause_id) const {
    return nodes_.contains(clause_id);
}

size_t DSRG::node_count() const { return nodes_.size(); }

// ---------------------------------------------------------------------------
// Edge operations
// ---------------------------------------------------------------------------

void DSRG::record_co_conflict(uint32_t clause_a, uint32_t clause_b) {
    if (clause_a == clause_b) return;
    if (!has_node(clause_a) || !has_node(clause_b)) return;

    uint64_t key = edge_key(clause_a, clause_b);

    auto eit = edges_.find(key);
    if (eit != edges_.end()) {
        auto& edge = eit->second;
        edge.co_conflict_count++;
        float beta = config_.edge_weight_momentum;
        edge.weight = beta * edge.weight + (1.0f - beta) * 1.0f;
        return;
    }

    int& count = pending_conflicts_[key];
    count++;
    if (count >= config_.edge_creation_threshold) {
        create_edge(clause_a, clause_b, count);
        pending_conflicts_.erase(key);
    }
}

void DSRG::record_flip_induced_edge(uint32_t clause_a, uint32_t clause_b, float delta) {
    if (clause_a == clause_b) return;
    if (!has_node(clause_a) || !has_node(clause_b)) return;

    uint64_t key = edge_key(clause_a, clause_b);
    auto eit = edges_.find(key);
    if (eit != edges_.end()) {
        float beta = config_.edge_weight_momentum;
        eit->second.weight = beta * eit->second.weight + (1.0f - beta) * delta;
        return;
    }
    if (create_edge(clause_a, clause_b, 0)) {
        auto it = edges_.find(key);
        if (it != edges_.end())
            it->second.weight = delta;
    }
}

bool DSRG::remove_edge(uint32_t a, uint32_t b) {
    uint64_t key = edge_key(a, b);
    auto it = edges_.find(key);
    if (it == edges_.end()) return false;

    remove_from_adj(a, b);
    remove_from_adj(b, a);
    edges_.erase(it);
    return true;
}

const GraphEdge* DSRG::get_edge(uint32_t a, uint32_t b) const {
    auto it = edges_.find(edge_key(a, b));
    return it != edges_.end() ? &it->second : nullptr;
}

bool DSRG::has_edge(uint32_t a, uint32_t b) const {
    return edges_.contains(edge_key(a, b));
}

size_t DSRG::edge_count() const { return edges_.size(); }

const std::vector<uint32_t>& DSRG::get_neighbors(uint32_t clause_id) const {
    auto it = adj_.find(clause_id);
    return it != adj_.end() ? it->second : empty_vec_;
}

// ---------------------------------------------------------------------------
// Weight operations
// ---------------------------------------------------------------------------

void DSRG::decay_all_weights() {
    float gamma = config_.node_weight_decay;
    for (auto& [id, node] : nodes_) {
        node.weight *= gamma;
    }

    float beta = config_.edge_weight_momentum;
    for (auto& [key, edge] : edges_) {
        edge.weight *= beta;
    }
}

void DSRG::boost_node(uint32_t clause_id, float delta) {
    auto it = nodes_.find(clause_id);
    if (it != nodes_.end()) {
        it->second.weight += delta;
    }
}

// ---------------------------------------------------------------------------
// Clause-Variable index
// ---------------------------------------------------------------------------

const std::vector<uint32_t>& DSRG::get_clause_vars(uint32_t clause_id) const {
    auto it = clause_vars_.find(clause_id);
    return it != clause_vars_.end() ? it->second : empty_vec_;
}

const std::vector<uint32_t>& DSRG::get_var_clauses(uint32_t var_id) const {
    auto it = var_clauses_.find(var_id);
    return it != var_clauses_.end() ? it->second : empty_vec_;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void DSRG::remove_from_adj(uint32_t node, uint32_t neighbor) {
    auto it = adj_.find(node);
    if (it == adj_.end()) return;

    auto& vec = it->second;
    auto pos = std::find(vec.begin(), vec.end(), neighbor);
    if (pos != vec.end()) {
        *pos = vec.back();
        vec.pop_back();
    }
}

void DSRG::remove_all_edges_of(uint32_t clause_id) {
    auto adj_it = adj_.find(clause_id);
    if (adj_it == adj_.end()) return;

    // Copy neighbors since we modify adj_ during removal.
    auto neighbors = adj_it->second;
    for (uint32_t nb : neighbors) {
        uint64_t key = edge_key(clause_id, nb);
        edges_.erase(key);
        remove_from_adj(nb, clause_id);
    }
    adj_it->second.clear();
}

void DSRG::remove_clause_var_index(uint32_t clause_id) {
    auto cv_it = clause_vars_.find(clause_id);
    if (cv_it == clause_vars_.end()) return;

    for (uint32_t var : cv_it->second) {
        auto vc_it = var_clauses_.find(var);
        if (vc_it == var_clauses_.end()) continue;
        auto& vec = vc_it->second;
        auto pos = std::find(vec.begin(), vec.end(), clause_id);
        if (pos != vec.end()) {
            *pos = vec.back();
            vec.pop_back();
        }
        if (vec.empty()) {
            var_clauses_.erase(vc_it);
        }
    }
    clause_vars_.erase(cv_it);
}

bool DSRG::create_edge(uint32_t a, uint32_t b, int initial_co_conflict) {
    uint64_t key = edge_key(a, b);
    if (edges_.contains(key)) return false;

    // Lazy cleanup: both endpoints must still exist.
    if (!has_node(a) || !has_node(b)) return false;

    GraphEdge edge{};
    edge.source            = std::min(a, b);
    edge.target            = std::max(a, b);
    edge.weight            = 1.0f;
    edge.co_conflict_count = initial_co_conflict;

    edges_.emplace(key, edge);
    adj_[a].push_back(b);
    adj_[b].push_back(a);
    return true;
}

}  // namespace sat_parallel
