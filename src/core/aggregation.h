#pragma once

#include "core/dsrg.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace sat_parallel {

struct AggregationConfig {
    std::string method = "weighted_sum";  // "sum" | "max" | "weighted_sum"
    float alpha        = 1.0f;
    float lambda       = 0.3f;            // GraphScore mixing weight
    int   top_k        = 100;
};

inline AggregationConfig load_aggregation_config(const std::string& yaml_path) {
    YAML::Node root = YAML::LoadFile(yaml_path);
    AggregationConfig cfg;
    if (auto c = root["centrality"]) {
        if (c["aggregation_method"]) cfg.method = c["aggregation_method"].as<std::string>();
        if (c["aggregation_alpha"])  cfg.alpha  = c["aggregation_alpha"].as<float>();
    }
    if (auto d = root["decision"]) {
        if (d["lambda"]) cfg.lambda = d["lambda"].as<float>();
    }
    if (auto comm = root["communication"]) {
        if (comm["top_k_variables"]) cfg.top_k = comm["top_k_variables"].as<int>();
    }
    return cfg;
}

// Clause -> Variable aggregation.
//   weighted_sum: Score(v) = Sigma alpha * Centrality(c) * W_node(c)  for c in C_v
//   sum:          Score(v) = Sigma Centrality(c)                      for c in C_v
//   max:          Score(v) = max  Centrality(c)                       for c in C_v
std::unordered_map<uint32_t, float>
aggregate_to_variables(
    const DSRG& graph,
    const std::unordered_map<uint32_t, float>& clause_centrality,
    const AggregationConfig& config);

// Mix graph-based variable scores with VSIDS scores.
//   Final(v) = (1 - lambda) * VSIDS(v) + lambda * GraphScore(v)
std::unordered_map<uint32_t, float>
mix_with_vsids(
    const std::unordered_map<uint32_t, float>& graph_scores,
    const std::unordered_map<uint32_t, float>& vsids_scores,
    float lambda);

// Select top-K variables by score, returned sorted descending.
std::vector<std::pair<uint32_t, float>>
select_top_k(
    const std::unordered_map<uint32_t, float>& scores,
    int k);

}  // namespace sat_parallel
