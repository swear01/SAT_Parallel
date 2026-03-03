#pragma once

#include "core/dsrg.h"

#include <string>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

namespace sat_parallel {

struct CentralityConfig {
    std::string algorithm       = "degree";  // "degree" | "pagerank"
    float pagerank_damping      = 0.85f;
    int   pagerank_max_iter     = 20;
    float pagerank_epsilon      = 1e-4f;
};

inline CentralityConfig load_centrality_config(const std::string& yaml_path) {
    YAML::Node root = YAML::LoadFile(yaml_path);
    CentralityConfig cfg;
    if (auto c = root["centrality"]) {
        if (c["algorithm"])          cfg.algorithm          = c["algorithm"].as<std::string>();
        if (c["pagerank_damping"])   cfg.pagerank_damping   = c["pagerank_damping"].as<float>();
        if (c["pagerank_max_iter"])  cfg.pagerank_max_iter  = c["pagerank_max_iter"].as<int>();
        if (c["pagerank_epsilon"])   cfg.pagerank_epsilon   = c["pagerank_epsilon"].as<float>();
    }
    return cfg;
}

// Weighted Degree Centrality:
//   Centrality(c) = Σ W_edge(c, c_j) for all neighbors c_j
std::unordered_map<uint32_t, float>
compute_degree_centrality(const DSRG& graph);

// Approximate PageRank via Power Iteration.
std::unordered_map<uint32_t, float>
compute_pagerank(const DSRG& graph, const CentralityConfig& config);

// Dispatch based on config.algorithm.
std::unordered_map<uint32_t, float>
compute_centrality(const DSRG& graph, const CentralityConfig& config);

}  // namespace sat_parallel
