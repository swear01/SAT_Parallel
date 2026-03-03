#include "worker/local_weights.h"
#include "worker/patch_builder.h"
#include "worker/worker_engine.h"
#include "comm/broadcast.h"
#include "comm/mpsc_queue.h"
#include "master/partitioner.h"
#include <gtest/gtest.h>
#include <cmath>
#include <vector>
using namespace sat_parallel;

// ===== LocalWeights =====

TEST(LocalWeightsTest, GetSetBasic) {
    LocalWeights lw;
    EXPECT_FLOAT_EQ(lw.get(1), 0.0f);
    lw.set(1, 5.0f);
    EXPECT_FLOAT_EQ(lw.get(1), 5.0f);
}

TEST(LocalWeightsTest, MergeGlobal) {
    LocalWeightsConfig cfg;
    cfg.retention_alpha = 0.7f;
    LocalWeights lw(cfg);

    lw.set(1, 10.0f);
    lw.set(2, 0.0f);

    std::unordered_map<uint32_t, float> global = {{1, 20.0f}, {2, 5.0f}, {3, 8.0f}};
    lw.merge_global(global);

    // var 1: 0.7 * 10 + 0.3 * 20 = 13.0
    EXPECT_NEAR(lw.get(1), 13.0f, 0.01f);
    // var 2: 0.7 * 0 + 0.3 * 5 = 1.5
    EXPECT_NEAR(lw.get(2), 1.5f, 0.01f);
    // var 3: 0.7 * 0 + 0.3 * 8 = 2.4
    EXPECT_NEAR(lw.get(3), 2.4f, 0.01f);
}

TEST(LocalWeightsTest, MergeBroadcast) {
    LocalWeightsConfig cfg;
    cfg.retention_alpha = 0.5f;
    LocalWeights lw(cfg);
    lw.set(1, 4.0f);

    struct VS { uint32_t var_id; float global_score; };
    std::vector<VS> bcast = {{1, 6.0f}, {2, 10.0f}};
    lw.merge_broadcast(bcast);

    EXPECT_NEAR(lw.get(1), 5.0f, 0.01f);  // 0.5*4 + 0.5*6
    EXPECT_NEAR(lw.get(2), 5.0f, 0.01f);  // 0.5*0 + 0.5*10
}

TEST(LocalWeightsTest, MixWithVSIDS) {
    LocalWeightsConfig cfg;
    cfg.lambda = 0.3f;
    LocalWeights lw(cfg);
    lw.set(1, 10.0f);

    // Final = 0.7 * 5.0 + 0.3 * 10.0 = 6.5
    float mixed = lw.mix_with_vsids(1, 5.0f);
    EXPECT_NEAR(mixed, 6.5f, 0.01f);
}

TEST(LocalWeightsTest, TopK) {
    LocalWeightsConfig cfg;
    cfg.lambda = 1.0f;  // pure graph scores
    LocalWeights lw(cfg);
    for (uint32_t i = 1; i <= 10; ++i) lw.set(i, static_cast<float>(i));

    std::unordered_map<uint32_t, float> vsids;  // empty, lambda=1 means pure graph
    auto top = lw.top_k(vsids, 3);
    EXPECT_EQ(top.size(), 3u);
    EXPECT_EQ(top[0].first, 10u);
    EXPECT_EQ(top[1].first, 9u);
    EXPECT_EQ(top[2].first, 8u);
}

// ===== PatchBuilder =====

TEST(PatchBuilderTest, ConflictIntervalTrigger) {
    MPSCQueue<DeltaPatch> queue;
    PatchBuilderConfig cfg;
    cfg.conflict_interval = 5;
    cfg.lbd_entry = 3;
    cfg.lbd_trigger = 1;  // set trigger lower so lbd=3 won't immediately flush

    PatchBuilder pb(0, cfg, queue);
    int lits[] = {1, -2};
    pb.on_clause_learned(100, lits, 2, 3);  // lbd=3 passes entry but no trigger

    // 4 conflicts: no flush yet
    for (int i = 1; i <= 4; ++i) pb.on_conflict(static_cast<uint64_t>(i));
    EXPECT_TRUE(queue.empty());

    // 5th conflict triggers flush
    pb.on_conflict(5);
    auto patch = queue.try_pop();
    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ(patch->worker_id, 0u);
    EXPECT_EQ(patch->new_clauses.size(), 1u);
    EXPECT_EQ(pb.patches_sent(), 1u);
}

TEST(PatchBuilderTest, LBDTrigger) {
    MPSCQueue<DeltaPatch> queue;
    PatchBuilderConfig cfg;
    cfg.conflict_interval = 100000;
    cfg.lbd_entry = 3;
    cfg.lbd_trigger = 2;

    PatchBuilder pb(1, cfg, queue);

    // LBD=3 clause: added to buffer but no trigger
    int lits1[] = {1, -2, 3};
    pb.on_clause_learned(10, lits1, 3, 3);
    EXPECT_TRUE(queue.empty());

    // LBD=2 clause: immediate trigger
    int lits2[] = {4, -5};
    pb.on_clause_learned(20, lits2, 2, 2);
    auto patch = queue.try_pop();
    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ(patch->new_clauses.size(), 2u);
}

TEST(PatchBuilderTest, LBDFilterRejectsHigh) {
    MPSCQueue<DeltaPatch> queue;
    PatchBuilderConfig cfg;
    cfg.lbd_entry = 3;
    cfg.conflict_interval = 2;

    PatchBuilder pb(0, cfg, queue);

    // LBD=5 > entry threshold, should not be included
    int lits[] = {1, -2, 3, 4, 5};
    pb.on_clause_learned(50, lits, 5, 5);
    pb.on_conflict(1);
    pb.on_conflict(2);

    // Should flush but with no clauses (only co_conflicts/hot_vars if any)
    EXPECT_TRUE(queue.empty());  // nothing to flush
}

TEST(PatchBuilderTest, CoConflictAndHotVars) {
    MPSCQueue<DeltaPatch> queue;
    PatchBuilderConfig cfg;
    cfg.conflict_interval = 100000;
    cfg.lbd_entry = 5;

    PatchBuilder pb(0, cfg, queue);

    pb.on_co_conflict(10, 20);
    pb.on_co_conflict(10, 20);
    pb.on_variable_bump(5);
    pb.on_variable_bump(5);
    pb.on_variable_bump(7);

    int lits[] = {1};
    pb.on_clause_learned(1, lits, 1, 1);
    pb.flush();

    auto patch = queue.try_pop();
    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ(patch->conflict_pairs.size(), 1u);
    EXPECT_EQ(patch->conflict_pairs[0].delta_count, 2);
    EXPECT_EQ(patch->hot_variables.size(), 2u);
}

TEST(PatchBuilderTest, BudgetEnforcement) {
    MPSCQueue<DeltaPatch> queue;
    PatchBuilderConfig cfg;
    cfg.budget_bytes = 100;  // very small budget
    cfg.lbd_entry = 10;
    cfg.conflict_interval = 100000;

    PatchBuilder pb(0, cfg, queue);

    // Add many large clauses
    for (int i = 0; i < 50; ++i) {
        std::vector<int> big_lits(20);
        for (int j = 0; j < 20; ++j) big_lits[static_cast<size_t>(j)] = j + 1;
        pb.on_clause_learned(static_cast<uint32_t>(i), big_lits.data(), 20, 3);
    }
    pb.flush();

    auto patch = queue.try_pop();
    ASSERT_TRUE(patch.has_value());
    EXPECT_LE(patch->estimated_size_bytes(), cfg.budget_bytes);
}

// ===== WorkerEngine =====

class MockSolver : public SolverInterface {
public:
    void set_assumptions(const std::vector<int>& a) override { assumptions = a; }
    std::unordered_map<uint32_t, float> get_vsids_scores() const override {
        return vsids;
    }
    void bump_variable(uint32_t v, float s) override { bumped[v] = s; }

    std::vector<int> assumptions;
    std::unordered_map<uint32_t, float> vsids;
    std::unordered_map<uint32_t, float> bumped;
};

TEST(WorkerEngineTest, ConflictAndPatchFlow) {
    MPSCQueue<DeltaPatch> queue;
    BroadcastChannel bc;
    WorkerConfig cfg;
    cfg.patch.conflict_interval = 5;
    cfg.patch.lbd_entry = 3;

    WorkerEngine engine(0, cfg, queue, bc);

    int lits[] = {1, -2};
    engine.on_clause_learned(100, lits, 2, 2);
    for (int i = 1; i <= 5; ++i) engine.on_conflict(static_cast<uint64_t>(i));

    auto patch = queue.try_pop();
    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ(patch->worker_id, 0u);
}

TEST(WorkerEngineTest, BroadcastMerge) {
    MPSCQueue<DeltaPatch> queue;
    BroadcastChannel bc;
    WorkerConfig cfg;
    cfg.weights.retention_alpha = 0.5f;

    WorkerEngine engine(0, cfg, queue, bc);
    engine.local_weights().set(1, 10.0f);

    GlobalBroadcast bcast;
    bcast.timestamp = 1;
    bcast.top_k_var_scores.push_back({1, 20.0f});
    bcast.top_k_var_scores.push_back({2, 8.0f});
    bc.publish(std::move(bcast));

    engine.poll_broadcast();

    EXPECT_NEAR(engine.local_weights().get(1), 15.0f, 0.01f);  // 0.5*10+0.5*20
    EXPECT_NEAR(engine.local_weights().get(2), 4.0f, 0.01f);   // 0.5*0+0.5*8
}

TEST(WorkerEngineTest, CubeAssignment) {
    MPSCQueue<DeltaPatch> queue;
    BroadcastChannel bc;
    WorkerEngine engine(0, {}, queue, bc);

    EXPECT_FALSE(engine.has_cube());

    Cube cube;
    cube.cube_id = 3;
    cube.assumptions = {{5, true}, {7, false}, {10, true}};
    engine.set_cube(cube);

    EXPECT_TRUE(engine.has_cube());
    EXPECT_EQ(engine.current_cube().cube_id, 3);

    const auto& assumptions = engine.cube_assumptions();
    EXPECT_EQ(assumptions.size(), 3u);
    EXPECT_EQ(assumptions[0], 5);    // var 5, phase true -> +5
    EXPECT_EQ(assumptions[1], -7);   // var 7, phase false -> -7
    EXPECT_EQ(assumptions[2], 10);   // var 10, phase true -> +10
}

TEST(WorkerEngineTest, ApplyCubeToSolver) {
    MPSCQueue<DeltaPatch> queue;
    BroadcastChannel bc;
    WorkerEngine engine(0, {}, queue, bc);

    Cube cube;
    cube.cube_id = 0;
    cube.assumptions = {{3, true}, {4, false}};
    engine.set_cube(cube);

    MockSolver solver;
    engine.apply_cube_to_solver(solver);
    EXPECT_EQ(solver.assumptions.size(), 2u);
    EXPECT_EQ(solver.assumptions[0], 3);
    EXPECT_EQ(solver.assumptions[1], -4);
}

TEST(WorkerEngineTest, ApplyScoresToSolver) {
    MPSCQueue<DeltaPatch> queue;
    BroadcastChannel bc;
    WorkerConfig cfg;
    cfg.weights.lambda = 0.5f;
    WorkerEngine engine(0, cfg, queue, bc);

    engine.local_weights().set(1, 10.0f);
    engine.local_weights().set(2, 20.0f);

    MockSolver solver;
    solver.vsids = {{1, 6.0f}, {2, 4.0f}};
    engine.apply_scores_to_solver(solver);

    EXPECT_FALSE(solver.bumped.empty());
    // var 2 should have higher final score: 0.5*4 + 0.5*20 = 12
    // var 1: 0.5*6 + 0.5*10 = 8
    EXPECT_GT(solver.bumped[2], solver.bumped[1]);
}

// ===== End-to-end: Worker <-> Master via channels =====

TEST(WorkerMasterE2E, PatchFlowThroughChannels) {
    MPSCQueue<DeltaPatch> queue;
    BroadcastChannel bc;

    WorkerConfig wcfg;
    wcfg.patch.conflict_interval = 3;
    wcfg.patch.lbd_entry = 5;
    wcfg.patch.lbd_trigger = 1;  // only lbd<=1 triggers immediate flush
    WorkerEngine worker(0, wcfg, queue, bc);

    // Accumulate events first, then add clause, then conflicts trigger flush.
    worker.on_co_conflict(100, 200);
    worker.on_variable_bump(5);
    int lits[] = {1, -2, 3};
    worker.on_clause_learned(100, lits, 3, 2);  // lbd=2 > trigger=1, no immediate flush
    for (int i = 1; i <= 3; ++i) worker.on_conflict(static_cast<uint64_t>(i));

    auto patch = queue.try_pop();
    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ(patch->worker_id, 0u);
    EXPECT_GE(patch->new_clauses.size(), 1u);
    EXPECT_EQ(patch->conflict_pairs.size(), 1u);
    EXPECT_EQ(patch->hot_variables.size(), 1u);

    // Master publishes broadcast
    GlobalBroadcast bcast;
    bcast.timestamp = 10;
    bcast.top_k_var_scores.push_back({5, 50.0f});
    bc.publish(std::move(bcast));

    worker.poll_broadcast();
    EXPECT_GT(worker.local_weights().get(5), 0.0f);
}
