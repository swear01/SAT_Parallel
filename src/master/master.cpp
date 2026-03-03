#include "master/master.h"

#include <yaml-cpp/yaml.h>

namespace sat_parallel {

MasterConfig load_master_config(const std::string& yaml_path) {
    MasterConfig cfg;
    cfg.dsrg        = load_dsrg_config(yaml_path);
    cfg.centrality  = load_centrality_config(yaml_path);
    cfg.aggregation = load_aggregation_config(yaml_path);

    YAML::Node root = YAML::LoadFile(yaml_path);
    if (auto c = root["centrality"]) {
        if (c["centrality_interval"])
            cfg.centrality_interval = c["centrality_interval"].as<int>();
    }
    if (auto gc = root["gc"]) {
        if (gc["gc_interval"])
            cfg.gc_interval = gc["gc_interval"].as<int>();
    }
    if (auto comm = root["communication"]) {
        if (comm["broadcast_interval_ms"])
            cfg.broadcast_interval_ms = comm["broadcast_interval_ms"].as<int>();
        if (comm["top_k_clauses"])
            cfg.top_k_clauses = comm["top_k_clauses"].as<int>();
    }
    if (auto g = root["graph"]) {
        if (g["gpu_hotzone_boost"])
            cfg.gpu_hotzone_boost = g["gpu_hotzone_boost"].as<float>();
    }
    if (auto gpu = root["gpu"]) {
        if (gpu["learnt_clause_lbd_filter"])
            cfg.learnt_clause_lbd_filter = gpu["learnt_clause_lbd_filter"].as<int>();
    }
    return cfg;
}

Master::Master(MasterConfig config,
               MPSCQueue<DeltaPatch>& patch_queue,
               BroadcastChannel& broadcast_channel,
               GPUChannel& gpu_channel)
    : config_(std::move(config)),
      graph_(config_.dsrg),
      patch_queue_(patch_queue),
      broadcast_channel_(broadcast_channel),
      gpu_channel_(gpu_channel) {}

size_t Master::tick() {
    size_t processed = 0;

    processed += patch_queue_.drain([this](DeltaPatch patch) {
        apply_delta_patch(patch);
    });

    processed += gpu_channel_.drain_reports([this](GPUReport report) {
        apply_gpu_report(report);
    });

    if (should_run_centrality()) {
        run_centrality_and_broadcast();
        last_centrality_conflict_ = total_conflicts_;
    }

    if (should_run_gc()) {
        run_gc();
        last_gc_conflict_ = total_conflicts_;
    }

    if (should_broadcast()) {
        run_centrality_and_broadcast();
        last_broadcast_time_ = Clock::now();
    }

    push_clauses_to_gpu();

    return processed;
}

void Master::force_centrality_and_broadcast() {
    run_centrality_and_broadcast();
}

void Master::apply_delta_patch(DeltaPatch& patch) {
    total_conflicts_ = std::max(total_conflicts_, patch.conflict_count);

    for (const auto& cl : patch.new_clauses) {
        graph_.add_node(cl.clause_id, cl.literals, cl.lbd,
                        false, patch.conflict_count);

        clause_literals_[cl.clause_id] = cl.literals;

        if (cl.lbd <= config_.learnt_clause_lbd_filter) {
            gpu_clause_buffer_.push_back({cl.clause_id, cl.literals, cl.lbd});
        }
    }

    for (const auto& cp : patch.conflict_pairs) {
        for (int i = 0; i < cp.delta_count; ++i) {
            graph_.record_co_conflict(cp.clause_a, cp.clause_b);
        }
    }

    for (const auto& hv : patch.hot_variables) {
        for (auto cid : graph_.get_var_clauses(hv.var_id)) {
            graph_.boost_node(cid, static_cast<float>(hv.frequency));
        }
    }
}

void Master::apply_gpu_report(GPUReport& report) {
    for (const auto& hz : report.hotzone) {
        graph_.boost_node(hz.clause_id, config_.gpu_hotzone_boost *
                          static_cast<float>(hz.frequency));
    }
}

void Master::run_centrality_and_broadcast() {
    if (graph_.node_count() == 0) return;

    graph_.decay_all_weights();

    clause_centrality_ = compute_centrality(graph_, config_.centrality);

    var_scores_ = aggregate_to_variables(
        graph_, clause_centrality_, config_.aggregation);

    auto top_vars = select_top_k(var_scores_, config_.aggregation.top_k);
    auto top_clauses = select_top_k(clause_centrality_, config_.top_k_clauses);

    GlobalBroadcast bcast;
    bcast.timestamp = total_conflicts_;

    for (const auto& [vid, score] : top_vars) {
        bcast.top_k_var_scores.push_back({vid, score});
    }

    for (const auto& [cid, weight] : top_clauses) {
        bcast.top_k_clause_weights.push_back({cid, weight});
        auto it = clause_literals_.find(cid);
        if (it != clause_literals_.end()) {
            bcast.shared_clauses.push_back(it->second);
        }
    }

    broadcast_channel_.publish(std::move(bcast));
    last_broadcast_time_ = Clock::now();
}

void Master::run_gc() {
    graph_.evict_stale_nodes(total_conflicts_);
    graph_.prune_weak_edges();
}

void Master::push_clauses_to_gpu() {
    auto now = Clock::now();
    auto elapsed = std::chrono::duration<double>(now - last_gpu_push_time_).count();
    if (elapsed < 5.0 || gpu_clause_buffer_.empty()) return;

    GPUClausePush push;
    push.clauses = std::move(gpu_clause_buffer_);
    gpu_clause_buffer_.clear();
    gpu_channel_.push_clauses(std::move(push));
    last_gpu_push_time_ = now;
}

bool Master::should_run_centrality() const {
    return total_conflicts_ - last_centrality_conflict_ >=
           static_cast<uint64_t>(config_.centrality_interval);
}

bool Master::should_run_gc() const {
    return total_conflicts_ - last_gc_conflict_ >=
           static_cast<uint64_t>(config_.gc_interval);
}

bool Master::should_broadcast() const {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - last_broadcast_time_).count();
    return elapsed >= config_.broadcast_interval_ms;
}

}  // namespace sat_parallel
