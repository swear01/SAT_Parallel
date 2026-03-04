#pragma once

#include <cstdint>
#include <string>
#include <yaml-cpp/yaml.h>

namespace sat_parallel {

struct GPUProberConfig {
    int   report_interval_steps   = 100000;
    int   hotzone_top_k           = 200;
    float master_push_interval_s  = 5.0f;
    int   learnt_clause_lbd_filter = 3;
    int   num_walks                = 0;     // Parallel WalkSAT runs (0=auto from device)
    int   max_flips_per_run        = 100000;
    float noise_probability        = 0.57f; // WalkSAT noise parameter
};

inline GPUProberConfig load_gpu_prober_config(const std::string& yaml_path) {
    GPUProberConfig cfg;
    YAML::Node root = YAML::LoadFile(yaml_path);
    if (auto gpu = root["gpu"]) {
        if (gpu["report_interval_steps"])
            cfg.report_interval_steps = gpu["report_interval_steps"].as<int>();
        if (gpu["hotzone_top_k"])
            cfg.hotzone_top_k = gpu["hotzone_top_k"].as<int>();
        if (gpu["master_push_interval_s"])
            cfg.master_push_interval_s = gpu["master_push_interval_s"].as<float>();
        if (gpu["learnt_clause_lbd_filter"])
            cfg.learnt_clause_lbd_filter = gpu["learnt_clause_lbd_filter"].as<int>();
        if (gpu["num_walks"])
            cfg.num_walks = gpu["num_walks"].as<int>();
    }
    return cfg;
}

// Flat clause representation for GPU transfer.
// All clauses are packed into a single array with an offset index.
struct FlatClauseDB {
    int num_clauses = 0;
    int num_vars    = 0;

    // clause_offsets[i] .. clause_offsets[i+1] defines the literal range for clause i.
    // Size: num_clauses + 1.
    int* clause_offsets = nullptr;

    // Packed literals. Size: clause_offsets[num_clauses].
    int* literals = nullptr;

    // clause_ids[i] = original clause_id for clause i (for hotzone reporting).
    uint32_t* clause_ids = nullptr;

    int total_literals() const {
        return clause_offsets ? clause_offsets[num_clauses] : 0;
    }
};

}  // namespace sat_parallel
