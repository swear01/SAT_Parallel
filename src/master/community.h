#pragma once

#include "core/dsrg.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

namespace sat_parallel {

struct CommunityConfig {
    std::string algorithm   = "louvain";  // "louvain" | "label_propagation"
    int  max_iterations     = 10;
    float min_modularity_gain = 1e-4f;
};

inline CommunityConfig load_community_config(const std::string& yaml_path) {
    CommunityConfig cfg;
    YAML::Node root = YAML::LoadFile(yaml_path);
    if (auto p = root["partitioning"]) {
        if (p["algorithm"]) cfg.algorithm = p["algorithm"].as<std::string>();
    }
    return cfg;
}

// Run Louvain community detection on the DSRG.
// Returns a map: clause_id -> community_id.
// Also writes community_id into each GraphNode (via non-const DSRG ref).
std::unordered_map<uint32_t, int>
detect_communities_louvain(DSRG& graph, const CommunityConfig& config);

// Label Propagation alternative (simpler, faster, lower quality).
std::unordered_map<uint32_t, int>
detect_communities_label_propagation(DSRG& graph, const CommunityConfig& config);

// Dispatch based on config.algorithm.
std::unordered_map<uint32_t, int>
detect_communities(DSRG& graph, const CommunityConfig& config);

}  // namespace sat_parallel
