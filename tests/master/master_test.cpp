#include "master/master.h"
#include "master/community.h"
#include "master/partitioner.h"
#include "master/work_stealing.h"
#include "master/sharing_strategy.h"
#include <gtest/gtest.h>
#include <vector>
using namespace sat_parallel;

static DSRGConfig test_dsrg_config() {
    DSRGConfig c;
    c.edge_creation_threshold = 2;
    c.edge_removal_threshold  = 0.001f;
    c.edge_weight_momentum    = 0.9f;
    c.node_weight_decay       = 0.95f;
    c.lbd_entry_threshold     = 5;
    c.gpu_hotzone_boost       = 2.0f;
    c.gc_interval             = 100;
    c.eviction_weight_threshold = 0.01f;
    c.eviction_lbd_threshold  = 4;
    c.min_age_before_eviction = 50;
    return c;
}

static MasterConfig test_master_config() {
    MasterConfig mc;
    mc.dsrg = test_dsrg_config();
    mc.centrality.algorithm = "degree";
    mc.aggregation.method   = "weighted_sum";
    mc.aggregation.alpha    = 1.0f;
    mc.aggregation.lambda   = 0.3f;
    mc.aggregation.top_k    = 10;
    mc.centrality_interval  = 10;
    mc.gc_interval          = 100;
    mc.broadcast_interval_ms = 0;  // always broadcast for testing
    mc.gpu_hotzone_boost    = 2.0f;
    mc.learnt_clause_lbd_filter = 3;
    mc.top_k_clauses        = 5;
    return mc;
}

// ===== Master basic tick =====

TEST(MasterTest, TickProcessesDeltaPatch) {
    MPSCQueue<DeltaPatch> pq;
    BroadcastChannel bc;
    GPUChannel gc;
    Master master(test_master_config(), pq, bc, gc);

    DeltaPatch patch;
    patch.worker_id = 0;
    patch.conflict_count = 100;
    patch.new_clauses.push_back({1, {1, -2, 3}, 2});
    patch.new_clauses.push_back({2, {-1, 4}, 2});
    patch.conflict_pairs.push_back({1, 2, 3});
    pq.push(std::move(patch));

    size_t n = master.tick();
    EXPECT_EQ(n, 1u);
    EXPECT_TRUE(master.graph().has_node(1));
    EXPECT_TRUE(master.graph().has_node(2));
    EXPECT_EQ(master.total_conflicts_seen(), 100u);
}

TEST(MasterTest, TickProcessesGPUReport) {
    MPSCQueue<DeltaPatch> pq;
    BroadcastChannel bc;
    GPUChannel gc;
    Master master(test_master_config(), pq, bc, gc);

    // Add a node first.
    DeltaPatch patch;
    patch.worker_id = 0;
    patch.conflict_count = 10;
    patch.new_clauses.push_back({1, {1, -2}, 2});
    pq.push(std::move(patch));
    master.tick();

    float w_before = master.graph().get_node(1)->weight;

    GPUReport rpt;
    rpt.hotzone.push_back({1, 3});
    gc.send_report(std::move(rpt));
    master.tick();

    float w_after = master.graph().get_node(1)->weight;
    EXPECT_GT(w_after, w_before);
}

TEST(MasterTest, CentralityAndBroadcast) {
    MPSCQueue<DeltaPatch> pq;
    BroadcastChannel bc;
    GPUChannel gc;
    Master master(test_master_config(), pq, bc, gc);

    DeltaPatch patch;
    patch.worker_id = 0;
    patch.conflict_count = 50;
    patch.new_clauses.push_back({1, {1, -2, 3}, 2});
    patch.new_clauses.push_back({2, {-1, 4}, 2});
    patch.conflict_pairs.push_back({1, 2, 3});
    pq.push(std::move(patch));
    master.tick();

    master.force_centrality_and_broadcast();

    auto snap = bc.read();
    ASSERT_NE(snap, nullptr);
    EXPECT_GT(snap->top_k_var_scores.size(), 0u);
}

// ===== Community detection =====

TEST(CommunityTest, LouvainBasic) {
    DSRG graph(test_dsrg_config());

    // Create two clusters: {1,2,3} and {4,5,6} with strong intra edges.
    int lits1[] = {1, 2};
    int lits2[] = {2, 3};
    int lits3[] = {1, 3};
    int lits4[] = {10, 11};
    int lits5[] = {11, 12};
    int lits6[] = {10, 12};
    graph.add_node(1, lits1, 2, false, 0);
    graph.add_node(2, lits2, 2, false, 0);
    graph.add_node(3, lits3, 2, false, 0);
    graph.add_node(4, lits4, 2, false, 0);
    graph.add_node(5, lits5, 2, false, 0);
    graph.add_node(6, lits6, 2, false, 0);

    // Strong intra-cluster edges.
    for (int i = 0; i < 3; ++i) {
        graph.record_co_conflict(1, 2);
        graph.record_co_conflict(2, 3);
        graph.record_co_conflict(1, 3);
        graph.record_co_conflict(4, 5);
        graph.record_co_conflict(5, 6);
        graph.record_co_conflict(4, 6);
    }

    CommunityConfig cc;
    cc.algorithm = "louvain";
    auto comms = detect_communities_louvain(graph, cc);

    EXPECT_EQ(comms.size(), 6u);
    // Nodes in the same cluster should have the same community.
    EXPECT_EQ(comms[1], comms[2]);
    EXPECT_EQ(comms[2], comms[3]);
    EXPECT_EQ(comms[4], comms[5]);
    EXPECT_EQ(comms[5], comms[6]);
    // The two clusters should be in different communities.
    EXPECT_NE(comms[1], comms[4]);
}

TEST(CommunityTest, LabelPropBasic) {
    DSRG graph(test_dsrg_config());
    int lits1[] = {1}; int lits2[] = {2}; int lits3[] = {3};
    graph.add_node(1, lits1, 2, false, 0);
    graph.add_node(2, lits2, 2, false, 0);
    graph.add_node(3, lits3, 2, false, 0);

    for (int i = 0; i < 3; ++i) {
        graph.record_co_conflict(1, 2);
        graph.record_co_conflict(2, 3);
    }

    CommunityConfig cc;
    cc.algorithm = "label_propagation";
    auto comms = detect_communities_label_propagation(graph, cc);
    EXPECT_EQ(comms.size(), 3u);
}

TEST(CommunityTest, Dispatch) {
    DSRG graph(test_dsrg_config());
    int lits1[] = {1}; int lits2[] = {2};
    graph.add_node(1, lits1, 2, false, 0);
    graph.add_node(2, lits2, 2, false, 0);
    for (int i = 0; i < 3; ++i) graph.record_co_conflict(1, 2);

    CommunityConfig cc;
    cc.algorithm = "louvain";
    auto c1 = detect_communities(graph, cc);
    EXPECT_EQ(c1.size(), 2u);

    cc.algorithm = "label_propagation";
    auto c2 = detect_communities(graph, cc);
    EXPECT_EQ(c2.size(), 2u);
}

// ===== Partitioner =====

TEST(PartitionerTest, CutVariablesAndCubes) {
    DSRG graph(test_dsrg_config());

    // Two communities sharing variable 5.
    int lits1[] = {1, 5};
    int lits2[] = {2, 5};
    graph.add_node(10, lits1, 2, false, 0);
    graph.add_node(20, lits2, 2, false, 0);
    for (int i = 0; i < 3; ++i) graph.record_co_conflict(10, 20);

    std::unordered_map<uint32_t, int> comms = {{10, 0}, {20, 1}};
    auto cut_vars = find_cut_variables(graph, comms, 5);

    EXPECT_GE(cut_vars.size(), 1u);
    // Variable 5 should be a cut variable (shared between communities).
    bool has_5 = false;
    for (auto v : cut_vars) if (v == 5) has_5 = true;
    EXPECT_TRUE(has_5);

    auto cubes = generate_cubes(cut_vars);
    int k = static_cast<int>(cut_vars.size());
    EXPECT_EQ(static_cast<int>(cubes.size()), 1 << k);
}

TEST(PartitionerTest, AssignToWorkers) {
    auto assignment = assign_cubes_to_workers(8, 3);
    EXPECT_EQ(assignment.size(), 3u);
    int total = 0;
    for (const auto& a : assignment) total += static_cast<int>(a.size());
    EXPECT_EQ(total, 8);
}

TEST(PartitionerTest, EmptyCutVars) {
    auto cubes = generate_cubes({});
    EXPECT_TRUE(cubes.empty());
}

// ===== Work Stealing =====

TEST(WorkStealingTest, BasicFlow) {
    WorkStealingManager mgr(2);

    std::vector<Cube> cubes;
    for (int i = 0; i < 4; ++i) {
        Cube c; c.cube_id = i;
        c.assumptions.push_back({static_cast<uint32_t>(i + 1), true});
        cubes.push_back(c);
    }
    mgr.load_cubes(std::move(cubes));

    EXPECT_EQ(mgr.remaining_cubes(), 4);
    EXPECT_FALSE(mgr.all_done());

    auto c0 = mgr.assign_next(0);
    ASSERT_TRUE(c0.has_value());
    EXPECT_EQ(c0->cube_id, 0);

    auto c1 = mgr.assign_next(1);
    ASSERT_TRUE(c1.has_value());
    EXPECT_EQ(c1->cube_id, 1);

    mgr.report_done(0, 0, WorkStealingManager::CubeResult::UNSAT);

    // Worker 0 steals.
    auto stolen = mgr.steal(0);
    ASSERT_TRUE(stolen.has_value());
    EXPECT_EQ(stolen->cube_id, 2);

    mgr.report_done(1, 1, WorkStealingManager::CubeResult::UNSAT);
    mgr.report_done(0, 2, WorkStealingManager::CubeResult::UNSAT);

    auto last = mgr.assign_next(1);
    ASSERT_TRUE(last.has_value());
    mgr.report_done(1, last->cube_id, WorkStealingManager::CubeResult::UNSAT);

    EXPECT_TRUE(mgr.all_done());
    EXPECT_FALSE(mgr.found_sat());
}

TEST(WorkStealingTest, SATStopsAssignment) {
    WorkStealingManager mgr(2);
    std::vector<Cube> cubes;
    for (int i = 0; i < 4; ++i) {
        Cube c; c.cube_id = i; cubes.push_back(c);
    }
    mgr.load_cubes(std::move(cubes));

    auto c = mgr.assign_next(0);
    mgr.report_done(0, c->cube_id, WorkStealingManager::CubeResult::SAT);
    EXPECT_TRUE(mgr.found_sat());

    auto next = mgr.assign_next(1);
    EXPECT_FALSE(next.has_value());
}

// ===== SharingStrategy end-to-end =====

TEST(SharingStrategyTest, EndToEnd) {
    DSRGSharingConfig cfg;
    cfg.master = test_master_config();
    cfg.community.algorithm = "louvain";
    cfg.partition.max_cut_variables = 3;
    cfg.num_workers = 2;

    DSRGSharingStrategy strategy(cfg);

    int lits1[] = {1, 2, 3};
    strategy.on_clause_learned(0, 100, lits1, 3, 2);
    int lits2[] = {4, 5};
    strategy.on_clause_learned(1, 200, lits2, 2, 2);

    strategy.do_sharing();

    EXPECT_TRUE(strategy.master().graph().has_node(100));
    EXPECT_TRUE(strategy.master().graph().has_node(200));

    strategy.master().force_centrality_and_broadcast();
    auto snap = strategy.broadcast().read();
    ASSERT_NE(snap, nullptr);
}
