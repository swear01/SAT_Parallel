#include "master/community.h"

#include <algorithm>
#include <numeric>
#include <random>
#include <unordered_set>
#include <vector>

namespace sat_parallel {

namespace {

struct LouvainState {
    std::vector<uint32_t> node_ids;
    std::unordered_map<uint32_t, int> node_to_comm;

    // Sum of edge weights incident to community c.
    std::unordered_map<int, double> sigma_tot;
    // Sum of edge weights internal to community c.
    std::unordered_map<int, double> sigma_in;
    // Sum of all edge weights (each edge counted once).
    double m = 0.0;
};

double modularity_gain(const LouvainState& st, uint32_t /*node*/,
                       int target_comm, double ki, double ki_in) {
    if (st.m == 0.0) return 0.0;
    double st_tot = st.sigma_tot.count(target_comm)
                    ? st.sigma_tot.at(target_comm) : 0.0;
    return ki_in - st_tot * ki / (2.0 * st.m);
}

}  // namespace

std::unordered_map<uint32_t, int>
detect_communities_louvain(DSRG& graph, const CommunityConfig& config) {
    LouvainState st;

    graph.for_each_node([&](const GraphNode& n) {
        st.node_ids.push_back(n.clause_id);
    });
    if (st.node_ids.empty()) return {};

    // Each node starts in its own community.
    for (size_t i = 0; i < st.node_ids.size(); ++i) {
        st.node_to_comm[st.node_ids[i]] = static_cast<int>(i);
    }

    // Compute weighted degree for each node and total edge weight.
    std::unordered_map<uint32_t, double> weighted_deg;
    graph.for_each_edge([&](const GraphEdge& e) {
        st.m += e.weight;
        weighted_deg[e.source] += e.weight;
        weighted_deg[e.target] += e.weight;
    });

    // Initialize sigma_tot per community (= weighted degree since each node is alone).
    for (auto id : st.node_ids) {
        int c = st.node_to_comm[id];
        st.sigma_tot[c] = weighted_deg[id];
        st.sigma_in[c] = 0.0;
    }

    if (st.m == 0.0) {
        std::unordered_map<uint32_t, int> result;
        for (auto id : st.node_ids) {
            result[id] = st.node_to_comm[id];
            graph.set_community_id(id, st.node_to_comm[id]);
        }
        return result;
    }

    std::mt19937 rng(42);
    bool improved = true;
    for (int iter = 0; iter < config.max_iterations && improved; ++iter) {
        improved = false;

        // Shuffle for randomized traversal order.
        std::shuffle(st.node_ids.begin(), st.node_ids.end(), rng);

        for (auto node : st.node_ids) {
            int old_comm = st.node_to_comm[node];
            double ki = weighted_deg[node];

            // Compute weights to neighboring communities.
            std::unordered_map<int, double> comm_weights;
            for (auto nbr : graph.get_neighbors(node)) {
                int nbr_comm = st.node_to_comm[nbr];
                const auto* e = graph.get_edge(node, nbr);
                if (e) comm_weights[nbr_comm] += e->weight;
            }

            // Remove node from its current community.
            double ki_old = comm_weights.count(old_comm) ? comm_weights[old_comm] : 0.0;
            st.sigma_tot[old_comm] -= ki;
            st.sigma_in[old_comm] -= ki_old;

            // Find best community.
            int best_comm = old_comm;
            double best_gain = 0.0;

            for (const auto& [comm, w_in] : comm_weights) {
                double gain = modularity_gain(st, node, comm, ki, w_in);
                if (gain > best_gain) {
                    best_gain = gain;
                    best_comm = comm;
                }
            }
            // Also consider staying (gain = 0 is the baseline).

            // Move node to best community.
            double ki_new = comm_weights.count(best_comm) ? comm_weights[best_comm] : 0.0;
            st.node_to_comm[node] = best_comm;
            st.sigma_tot[best_comm] += ki;
            st.sigma_in[best_comm] += ki_new;

            if (best_comm != old_comm && best_gain > config.min_modularity_gain) {
                improved = true;
            }
        }
    }

    // Compact community IDs to [0, K).
    std::unordered_map<int, int> comm_remap;
    int next_id = 0;
    for (auto id : st.node_ids) {
        int c = st.node_to_comm[id];
        if (comm_remap.find(c) == comm_remap.end()) {
            comm_remap[c] = next_id++;
        }
    }

    std::unordered_map<uint32_t, int> result;
    for (auto id : st.node_ids) {
        int compact = comm_remap[st.node_to_comm[id]];
        result[id] = compact;
        graph.set_community_id(id, compact);
    }
    return result;
}

std::unordered_map<uint32_t, int>
detect_communities_label_propagation(DSRG& graph, const CommunityConfig& config) {
    std::vector<uint32_t> node_ids;
    graph.for_each_node([&](const GraphNode& n) {
        node_ids.push_back(n.clause_id);
    });
    if (node_ids.empty()) return {};

    std::unordered_map<uint32_t, int> label;
    for (size_t i = 0; i < node_ids.size(); ++i) {
        label[node_ids[i]] = static_cast<int>(i);
    }

    std::mt19937 rng(42);
    for (int iter = 0; iter < config.max_iterations; ++iter) {
        bool changed = false;
        std::shuffle(node_ids.begin(), node_ids.end(), rng);

        for (auto node : node_ids) {
            std::unordered_map<int, double> label_weight;
            for (auto nbr : graph.get_neighbors(node)) {
                const auto* e = graph.get_edge(node, nbr);
                double w = e ? e->weight : 1.0;
                label_weight[label[nbr]] += w;
            }
            if (label_weight.empty()) continue;

            int best_label = label[node];
            double best_w = -1.0;
            for (const auto& [l, w] : label_weight) {
                if (w > best_w) { best_w = w; best_label = l; }
            }
            if (best_label != label[node]) {
                label[node] = best_label;
                changed = true;
            }
        }
        if (!changed) break;
    }

    // Compact IDs.
    std::unordered_map<int, int> remap;
    int next_id = 0;
    for (auto id : node_ids) {
        if (remap.find(label[id]) == remap.end()) {
            remap[label[id]] = next_id++;
        }
    }
    std::unordered_map<uint32_t, int> result;
    for (auto id : node_ids) {
        int compact = remap[label[id]];
        result[id] = compact;
        graph.set_community_id(id, compact);
    }
    return result;
}

std::unordered_map<uint32_t, int>
detect_communities(DSRG& graph, const CommunityConfig& config) {
    if (config.algorithm == "label_propagation")
        return detect_communities_label_propagation(graph, config);
    return detect_communities_louvain(graph, config);
}

}  // namespace sat_parallel
