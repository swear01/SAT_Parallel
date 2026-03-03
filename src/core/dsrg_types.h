#pragma once

#include <cstdint>
#include <string>
#include <yaml-cpp/yaml.h>

namespace sat_parallel {

struct GraphNode {
    uint32_t clause_id;
    float    weight;
    int      lbd;
    int      length;
    bool     is_original;
    uint64_t birth_conflict;
    int      community_id;
};

struct GraphEdge {
    uint32_t source;
    uint32_t target;
    float    weight;
    int      co_conflict_count;
};

struct DSRGConfig {
    int   edge_creation_threshold = 3;
    float edge_removal_threshold  = 0.005f;
    float edge_weight_momentum    = 0.9f;
    float node_weight_decay       = 0.95f;
    int   lbd_entry_threshold     = 3;
    float gpu_hotzone_boost       = 2.0f;

    int   gc_interval                = 10000;
    float eviction_weight_threshold  = 0.01f;
    int   eviction_lbd_threshold     = 4;
    int   min_age_before_eviction    = 5000;
};

inline DSRGConfig load_dsrg_config(const std::string& yaml_path) {
    YAML::Node root = YAML::LoadFile(yaml_path);
    DSRGConfig cfg;

    if (auto g = root["graph"]) {
        if (g["edge_creation_threshold"]) cfg.edge_creation_threshold = g["edge_creation_threshold"].as<int>();
        if (g["edge_removal_threshold"])  cfg.edge_removal_threshold  = g["edge_removal_threshold"].as<float>();
        if (g["edge_weight_momentum"])    cfg.edge_weight_momentum    = g["edge_weight_momentum"].as<float>();
        if (g["node_weight_decay"])       cfg.node_weight_decay       = g["node_weight_decay"].as<float>();
        if (g["lbd_entry_threshold"])     cfg.lbd_entry_threshold     = g["lbd_entry_threshold"].as<int>();
        if (g["gpu_hotzone_boost"])       cfg.gpu_hotzone_boost       = g["gpu_hotzone_boost"].as<float>();
    }

    if (auto gc = root["gc"]) {
        if (gc["gc_interval"])                cfg.gc_interval                = gc["gc_interval"].as<int>();
        if (gc["eviction_weight_threshold"])  cfg.eviction_weight_threshold  = gc["eviction_weight_threshold"].as<float>();
        if (gc["eviction_lbd_threshold"])     cfg.eviction_lbd_threshold     = gc["eviction_lbd_threshold"].as<int>();
        if (gc["min_age_before_eviction"])    cfg.min_age_before_eviction    = gc["min_age_before_eviction"].as<int>();
    }

    return cfg;
}

}  // namespace sat_parallel
