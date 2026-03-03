#pragma once

#include "core/dsrg.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace sat_parallel {

struct PartitionConfig {
    int max_cut_variables   = 15;
    bool enable_work_stealing = true;
};

inline PartitionConfig load_partition_config(const std::string& yaml_path) {
    PartitionConfig cfg;
    YAML::Node root = YAML::LoadFile(yaml_path);
    if (auto p = root["partitioning"]) {
        if (p["max_cut_variables"])
            cfg.max_cut_variables = p["max_cut_variables"].as<int>();
        if (p["enable_work_stealing"])
            cfg.enable_work_stealing = p["enable_work_stealing"].as<bool>();
    }
    return cfg;
}

// A cube is a partial assignment: variable -> polarity (true/false).
struct Cube {
    int cube_id;
    std::vector<std::pair<uint32_t, bool>> assumptions;  // (var_id, phase)
};

// Find variables that appear in edges crossing community boundaries.
// Returns cut variables sorted by cross-community edge weight (descending).
// At most max_cut_variables are returned.
std::vector<uint32_t>
find_cut_variables(const DSRG& graph,
                   const std::unordered_map<uint32_t, int>& communities,
                   int max_cut_variables);

// Generate 2^k cubes from k cut variables (all Boolean combinations).
// If k > max_cut_variables, only the top max_cut_variables are used.
std::vector<Cube>
generate_cubes(const std::vector<uint32_t>& cut_variables);

// Assign cubes to workers in round-robin.
// Returns worker_id -> list of cube indices.
std::vector<std::vector<int>>
assign_cubes_to_workers(int num_cubes, int num_workers);

}  // namespace sat_parallel
