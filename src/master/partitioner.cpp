#include "master/partitioner.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace sat_parallel {

std::vector<uint32_t>
find_cut_variables(const DSRG& graph,
                   const std::unordered_map<uint32_t, int>& communities,
                   int max_cut_variables) {
    // Score each variable by total weight of cross-community edges involving it.
    std::unordered_map<uint32_t, float> var_cut_score;

    graph.for_each_edge([&](const GraphEdge& e) {
        auto it_s = communities.find(e.source);
        auto it_t = communities.find(e.target);
        if (it_s == communities.end() || it_t == communities.end()) return;
        if (it_s->second == it_t->second) return;

        // This is a cross-community edge. Find shared variables.
        const auto& vars_s = graph.get_clause_vars(e.source);
        const auto& vars_t = graph.get_clause_vars(e.target);

        std::unordered_set<uint32_t> vs(vars_s.begin(), vars_s.end());
        for (auto v : vars_t) {
            if (vs.count(v)) {
                var_cut_score[v] += e.weight;
            }
        }
    });

    // Sort by cut score descending.
    std::vector<std::pair<uint32_t, float>> scored(
        var_cut_score.begin(), var_cut_score.end());
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    int k = std::min(max_cut_variables, static_cast<int>(scored.size()));
    std::vector<uint32_t> result;
    result.reserve(static_cast<size_t>(k));
    for (int i = 0; i < k; ++i) {
        result.push_back(scored[static_cast<size_t>(i)].first);
    }
    return result;
}

std::vector<Cube>
generate_cubes(const std::vector<uint32_t>& cut_variables) {
    int k = static_cast<int>(cut_variables.size());
    if (k == 0) return {};

    int num_cubes = 1 << k;
    std::vector<Cube> cubes;
    cubes.reserve(static_cast<size_t>(num_cubes));

    for (int mask = 0; mask < num_cubes; ++mask) {
        Cube c;
        c.cube_id = mask;
        for (int bit = 0; bit < k; ++bit) {
            bool phase = (mask >> bit) & 1;
            c.assumptions.push_back({cut_variables[static_cast<size_t>(bit)], phase});
        }
        cubes.push_back(std::move(c));
    }
    return cubes;
}

std::vector<std::vector<int>>
assign_cubes_to_workers(int num_cubes, int num_workers) {
    std::vector<std::vector<int>> assignment(static_cast<size_t>(num_workers));
    for (int i = 0; i < num_cubes; ++i) {
        assignment[static_cast<size_t>(i % num_workers)].push_back(i);
    }
    return assignment;
}

}  // namespace sat_parallel
