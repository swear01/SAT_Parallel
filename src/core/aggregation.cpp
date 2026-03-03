#include "core/aggregation.h"

#include <algorithm>
#include <functional>

namespace sat_parallel {

std::unordered_map<uint32_t, float>
aggregate_to_variables(
    const DSRG& graph,
    const std::unordered_map<uint32_t, float>& clause_centrality,
    const AggregationConfig& config) {

    std::unordered_map<uint32_t, float> var_scores;

    graph.for_each_node([&](const GraphNode& node) {
        auto cit = clause_centrality.find(node.clause_id);
        if (cit == clause_centrality.end()) return;

        float centrality = cit->second;
        const auto& vars = graph.get_clause_vars(node.clause_id);

        for (uint32_t var : vars) {
            if (config.method == "weighted_sum") {
                var_scores[var] += config.alpha * centrality * node.weight;
            } else if (config.method == "max") {
                auto& cur = var_scores[var];
                cur = std::max(cur, centrality);
            } else {
                // "sum" fallback
                var_scores[var] += centrality;
            }
        }
    });

    return var_scores;
}

std::unordered_map<uint32_t, float>
mix_with_vsids(
    const std::unordered_map<uint32_t, float>& graph_scores,
    const std::unordered_map<uint32_t, float>& vsids_scores,
    float lambda) {

    std::unordered_map<uint32_t, float> result;
    result.reserve(std::max(graph_scores.size(), vsids_scores.size()));

    // Start with all VSIDS entries.
    for (const auto& [var, vs] : vsids_scores) {
        float gs = 0.0f;
        auto it = graph_scores.find(var);
        if (it != graph_scores.end()) gs = it->second;
        result[var] = (1.0f - lambda) * vs + lambda * gs;
    }

    // Add graph-only entries (variables with graph score but no VSIDS entry).
    for (const auto& [var, gs] : graph_scores) {
        if (!result.contains(var)) {
            result[var] = lambda * gs;
        }
    }

    return result;
}

std::vector<std::pair<uint32_t, float>>
select_top_k(
    const std::unordered_map<uint32_t, float>& scores,
    int k) {

    std::vector<std::pair<uint32_t, float>> all(scores.begin(), scores.end());

    size_t n = std::min(all.size(), static_cast<size_t>(k));
    std::partial_sort(all.begin(), all.begin() + static_cast<long>(n), all.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    all.resize(n);
    return all;
}

}  // namespace sat_parallel
