#pragma once

#include "comm/broadcast.h"
#include "comm/delta_patch.h"
#include "comm/gpu_channel.h"
#include "comm/mpsc_queue.h"
#include "master/community.h"
#include "master/master.h"
#include "master/partitioner.h"
#include "master/work_stealing.h"

#include <memory>
#include <string>
#include <yaml-cpp/yaml.h>

namespace sat_parallel {

// Abstract interface that a Painless-style sharing strategy must implement.
// The DSRG-based Master plugs into this interface.
class SharingStrategy {
public:
    virtual ~SharingStrategy() = default;

    // Called by the framework once per sharing round.
    virtual void do_sharing() = 0;

    // Called when a new clause is learned by a worker.
    virtual void on_clause_learned(uint32_t worker_id,
                                   uint32_t clause_id,
                                   const int* literals,
                                   int length,
                                   int lbd) = 0;
};

// Concrete strategy that bridges DSRG Master with the Painless framework.
struct DSRGSharingConfig {
    MasterConfig   master;
    CommunityConfig community;
    PartitionConfig partition;
    int num_workers = 4;
};

inline DSRGSharingConfig load_dsrg_sharing_config(const std::string& yaml_path) {
    DSRGSharingConfig cfg;
    cfg.master    = load_master_config(yaml_path);
    cfg.community = load_community_config(yaml_path);
    cfg.partition = load_partition_config(yaml_path);
    return cfg;
}

class DSRGSharingStrategy : public SharingStrategy {
public:
    explicit DSRGSharingStrategy(DSRGSharingConfig config)
        : config_(std::move(config)),
          master_(config_.master, patch_queue_, broadcast_, gpu_channel_),
          work_mgr_(config_.num_workers) {}

    void do_sharing() override {
        master_.tick();
    }

    void on_clause_learned(uint32_t worker_id, uint32_t clause_id,
                           const int* literals, int length, int lbd) override {
        if (lbd > master_.graph().config().lbd_entry_threshold) return;

        DeltaPatch patch;
        patch.worker_id = worker_id;
        patch.conflict_count = master_.total_conflicts_seen() + 1;

        DeltaPatch::LearnedClause cl;
        cl.clause_id = clause_id;
        cl.literals.assign(literals, literals + length);
        cl.lbd = lbd;
        patch.new_clauses.push_back(std::move(cl));

        patch_queue_.push(std::move(patch));
    }

    // Run community detection and partitioning. Returns cubes for distribution.
    std::vector<Cube> partition_and_generate_cubes() {
        auto communities = detect_communities(master_.graph(), config_.community);
        auto cut_vars = find_cut_variables(
            master_.graph(), communities, config_.partition.max_cut_variables);
        auto cubes = generate_cubes(cut_vars);
        work_mgr_.load_cubes(cubes);
        return cubes;
    }

    // Accessors.
    Master& master() { return master_; }
    const Master& master() const { return master_; }
    BroadcastChannel& broadcast() { return broadcast_; }
    GPUChannel& gpu_channel() { return gpu_channel_; }
    WorkStealingManager& work_manager() { return work_mgr_; }

private:
    DSRGSharingConfig config_;
    MPSCQueue<DeltaPatch> patch_queue_;
    BroadcastChannel broadcast_;
    GPUChannel gpu_channel_;
    Master master_;
    WorkStealingManager work_mgr_;
};

}  // namespace sat_parallel
