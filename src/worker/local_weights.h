#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace sat_parallel {

struct LocalWeightsConfig {
    float retention_alpha = 0.7f;  // EMA retention for local weights
    float lambda          = 0.3f;  // GraphScore mixing weight with VSIDS
};

inline LocalWeightsConfig load_local_weights_config(const std::string& yaml_path) {
    LocalWeightsConfig cfg;
    YAML::Node root = YAML::LoadFile(yaml_path);
    if (auto comm = root["communication"]) {
        if (comm["local_weight_retention"])
            cfg.retention_alpha = comm["local_weight_retention"].as<float>();
    }
    if (auto d = root["decision"]) {
        if (d["lambda"]) cfg.lambda = d["lambda"].as<float>();
    }
    return cfg;
}

// Per-worker local variable weight cache.
// Maintains a local copy of variable scores and fuses with Master broadcasts.
class LocalWeights {
public:
    explicit LocalWeights(LocalWeightsConfig config = {})
        : config_(config) {}

    // Get the current local score for a variable.
    float get(uint32_t var_id) const {
        auto it = scores_.find(var_id);
        return it != scores_.end() ? it->second : 0.0f;
    }

    // Set a local score directly (e.g. from VSIDS).
    void set(uint32_t var_id, float score) {
        scores_[var_id] = score;
    }

    // Merge global broadcast scores into local weights using EMA:
    //   W_local(v) = alpha * W_local(v) + (1 - alpha) * W_global(v)
    void merge_global(const std::unordered_map<uint32_t, float>& global_scores) {
        float alpha = config_.retention_alpha;
        for (const auto& [var_id, global_score] : global_scores) {
            float local = get(var_id);
            scores_[var_id] = alpha * local + (1.0f - alpha) * global_score;
        }
    }

    // Merge global scores from a broadcast VarScore vector.
    template <typename VarScoreVec>
    void merge_broadcast(const VarScoreVec& var_scores) {
        float alpha = config_.retention_alpha;
        for (const auto& vs : var_scores) {
            float local = get(vs.var_id);
            scores_[vs.var_id] = alpha * local + (1.0f - alpha) * vs.global_score;
        }
    }

    // Mix local graph scores with external VSIDS scores:
    //   Final(v) = (1 - lambda) * vsids(v) + lambda * local(v)
    float mix_with_vsids(uint32_t var_id, float vsids_score) const {
        float graph = get(var_id);
        return (1.0f - config_.lambda) * vsids_score + config_.lambda * graph;
    }

    // Batch mix: returns merged scores for all variables in either map.
    std::unordered_map<uint32_t, float>
    mix_all_with_vsids(const std::unordered_map<uint32_t, float>& vsids_scores) const {
        std::unordered_map<uint32_t, float> result;
        for (const auto& [vid, vs] : vsids_scores) {
            result[vid] = mix_with_vsids(vid, vs);
        }
        for (const auto& [vid, gs] : scores_) {
            if (result.find(vid) == result.end()) {
                result[vid] = config_.lambda * gs;
            }
        }
        return result;
    }

    // Select top-K by final score (after VSIDS mixing).
    std::vector<std::pair<uint32_t, float>>
    top_k(const std::unordered_map<uint32_t, float>& vsids_scores, int k) const {
        auto merged = mix_all_with_vsids(vsids_scores);
        std::vector<std::pair<uint32_t, float>> sorted(merged.begin(), merged.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        if (static_cast<int>(sorted.size()) > k) sorted.resize(static_cast<size_t>(k));
        return sorted;
    }

    const std::unordered_map<uint32_t, float>& scores() const { return scores_; }
    size_t size() const { return scores_.size(); }
    const LocalWeightsConfig& config() const { return config_; }

private:
    LocalWeightsConfig config_;
    std::unordered_map<uint32_t, float> scores_;
};

}  // namespace sat_parallel
