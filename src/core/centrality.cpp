#include "core/centrality.h"

#include <cmath>

namespace sat_parallel {

std::unordered_map<uint32_t, float>
compute_degree_centrality(const DSRG& graph) {
    std::unordered_map<uint32_t, float> scores;
    scores.reserve(graph.node_count());

    graph.for_each_node([&](const GraphNode& node) {
        float sum = 0.0f;
        for (uint32_t nb : graph.get_neighbors(node.clause_id)) {
            const auto* edge = graph.get_edge(node.clause_id, nb);
            if (edge) sum += edge->weight;
        }
        scores[node.clause_id] = sum;
    });

    return scores;
}

std::unordered_map<uint32_t, float>
compute_pagerank(const DSRG& graph, const CentralityConfig& config) {
    const size_t n = graph.node_count();
    if (n == 0) return {};

    const float d   = config.pagerank_damping;
    const float eps = config.pagerank_epsilon;
    const int max_iter = config.pagerank_max_iter;
    const float base = (1.0f - d) / static_cast<float>(n);

    std::unordered_map<uint32_t, float> rank;
    std::unordered_map<uint32_t, float> out_weight;
    rank.reserve(n);
    out_weight.reserve(n);

    const float init = 1.0f / static_cast<float>(n);
    graph.for_each_node([&](const GraphNode& node) {
        rank[node.clause_id] = init;
        float ow = 0.0f;
        for (uint32_t nb : graph.get_neighbors(node.clause_id)) {
            const auto* edge = graph.get_edge(node.clause_id, nb);
            if (edge) ow += edge->weight;
        }
        out_weight[node.clause_id] = ow;
    });

    std::unordered_map<uint32_t, float> new_rank;
    new_rank.reserve(n);

    for (int iter = 0; iter < max_iter; ++iter) {
        float dangling_sum = 0.0f;
        for (const auto& [id, ow] : out_weight) {
            if (ow == 0.0f) dangling_sum += rank[id];
        }
        float dangling_contrib = d * dangling_sum / static_cast<float>(n);

        new_rank.clear();
        graph.for_each_node([&](const GraphNode& node) {
            new_rank[node.clause_id] = base + dangling_contrib;
        });

        graph.for_each_edge([&](const GraphEdge& edge) {
            float ow_s = out_weight[edge.source];
            float ow_t = out_weight[edge.target];

            if (ow_s > 0.0f) {
                new_rank[edge.target] += d * rank[edge.source] * (edge.weight / ow_s);
            }
            if (ow_t > 0.0f) {
                new_rank[edge.source] += d * rank[edge.target] * (edge.weight / ow_t);
            }
        });

        float max_diff = 0.0f;
        for (const auto& [id, nr] : new_rank) {
            max_diff = std::max(max_diff, std::abs(nr - rank[id]));
        }

        rank.swap(new_rank);
        if (max_diff < eps) break;
    }

    return rank;
}

std::unordered_map<uint32_t, float>
compute_centrality(const DSRG& graph, const CentralityConfig& config) {
    if (config.algorithm == "pagerank") {
        return compute_pagerank(graph, config);
    }
    return compute_degree_centrality(graph);
}

}  // namespace sat_parallel
