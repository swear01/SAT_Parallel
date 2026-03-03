#include "core/dsrg.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace sat_parallel;

static DSRGConfig test_config() {
    DSRGConfig cfg;
    cfg.edge_creation_threshold   = 3;
    cfg.edge_removal_threshold    = 0.005f;
    cfg.edge_weight_momentum      = 0.9f;
    cfg.node_weight_decay         = 0.95f;
    cfg.lbd_entry_threshold       = 3;
    cfg.gpu_hotzone_boost         = 2.0f;
    cfg.gc_interval               = 10000;
    cfg.eviction_weight_threshold = 0.01f;
    cfg.eviction_lbd_threshold    = 4;
    cfg.min_age_before_eviction   = 5000;
    return cfg;
}

// ============================================================
// Node operations
// ============================================================

TEST(DSRGNodeTest, AddAndRetrieve) {
    DSRG g(test_config());
    std::vector<int> lits = {1, -2, 3};

    EXPECT_TRUE(g.add_node(10, lits, 2, false, 0));
    EXPECT_EQ(g.node_count(), 1u);

    const auto* node = g.get_node(10);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->clause_id, 10u);
    EXPECT_FLOAT_EQ(node->weight, 1.0f);
    EXPECT_EQ(node->lbd, 2);
    EXPECT_EQ(node->length, 3);
    EXPECT_FALSE(node->is_original);
}

TEST(DSRGNodeTest, DuplicateAddFails) {
    DSRG g(test_config());
    std::vector<int> lits = {1, 2};

    EXPECT_TRUE(g.add_node(1, lits, 2, true, 0));
    EXPECT_FALSE(g.add_node(1, lits, 2, true, 0));
    EXPECT_EQ(g.node_count(), 1u);
}

TEST(DSRGNodeTest, RemoveNode) {
    DSRG g(test_config());
    std::vector<int> lits = {1, 2};

    g.add_node(1, lits, 2, true, 0);
    EXPECT_TRUE(g.remove_node(1));
    EXPECT_EQ(g.node_count(), 0u);
    EXPECT_EQ(g.get_node(1), nullptr);
}

TEST(DSRGNodeTest, RemoveNonexistentFails) {
    DSRG g(test_config());
    EXPECT_FALSE(g.remove_node(999));
}

TEST(DSRGNodeTest, LBDFilterRejectsHighLBD) {
    DSRG g(test_config());
    std::vector<int> lits = {1, 2, 3, 4, 5};

    EXPECT_FALSE(g.add_node(1, lits, 5, false, 0));
    EXPECT_EQ(g.node_count(), 0u);
}

TEST(DSRGNodeTest, OriginalBypassesLBDFilter) {
    DSRG g(test_config());
    std::vector<int> lits = {1, 2, 3, 4, 5};

    EXPECT_TRUE(g.add_node(1, lits, 10, true, 0));
    EXPECT_EQ(g.node_count(), 1u);
}

// ============================================================
// Clause-Variable index
// ============================================================

TEST(DSRGClauseVarTest, IndexBuiltOnAdd) {
    DSRG g(test_config());
    std::vector<int> lits = {1, -2, 3};
    g.add_node(10, lits, 2, true, 0);

    auto& vars = g.get_clause_vars(10);
    ASSERT_EQ(vars.size(), 3u);
    EXPECT_EQ(vars[0], 1u);
    EXPECT_EQ(vars[1], 2u);
    EXPECT_EQ(vars[2], 3u);

    EXPECT_EQ(g.get_var_clauses(1).size(), 1u);
    EXPECT_EQ(g.get_var_clauses(2).size(), 1u);
    EXPECT_EQ(g.get_var_clauses(2)[0], 10u);
}

TEST(DSRGClauseVarTest, IndexCleanedOnRemove) {
    DSRG g(test_config());
    std::vector<int> lits = {1, -2};
    g.add_node(10, lits, 2, true, 0);
    g.remove_node(10);

    EXPECT_TRUE(g.get_clause_vars(10).empty());
    EXPECT_TRUE(g.get_var_clauses(1).empty());
    EXPECT_TRUE(g.get_var_clauses(2).empty());
}

// ============================================================
// Edge creation via co-conflict threshold
// ============================================================

TEST(DSRGEdgeTest, EdgeCreatedAfterThreshold) {
    DSRG g(test_config());
    std::vector<int> lits_a = {1, 2};
    std::vector<int> lits_b = {2, 3};
    g.add_node(1, lits_a, 2, true, 0);
    g.add_node(2, lits_b, 2, true, 0);

    g.record_co_conflict(1, 2);
    g.record_co_conflict(1, 2);
    EXPECT_FALSE(g.has_edge(1, 2));
    EXPECT_EQ(g.edge_count(), 0u);

    g.record_co_conflict(1, 2);
    EXPECT_TRUE(g.has_edge(1, 2));
    EXPECT_EQ(g.edge_count(), 1u);

    const auto* edge = g.get_edge(1, 2);
    ASSERT_NE(edge, nullptr);
    EXPECT_EQ(edge->co_conflict_count, 3);
}

TEST(DSRGEdgeTest, ExistingEdgeUpdatesOnConflict) {
    DSRG g(test_config());
    std::vector<int> lits_a = {1};
    std::vector<int> lits_b = {2};
    g.add_node(1, lits_a, 1, true, 0);
    g.add_node(2, lits_b, 1, true, 0);

    for (int i = 0; i < 3; ++i) g.record_co_conflict(1, 2);
    ASSERT_TRUE(g.has_edge(1, 2));

    float w_before = g.get_edge(1, 2)->weight;
    g.record_co_conflict(1, 2);
    float w_after = g.get_edge(1, 2)->weight;

    float expected = 0.9f * w_before + 0.1f;
    EXPECT_NEAR(w_after, expected, 1e-5f);
    EXPECT_EQ(g.get_edge(1, 2)->co_conflict_count, 4);
}

TEST(DSRGEdgeTest, SelfLoopIgnored) {
    DSRG g(test_config());
    std::vector<int> lits = {1};
    g.add_node(1, lits, 1, true, 0);

    g.record_co_conflict(1, 1);
    g.record_co_conflict(1, 1);
    g.record_co_conflict(1, 1);
    EXPECT_EQ(g.edge_count(), 0u);
}

TEST(DSRGEdgeTest, RemoveEdge) {
    DSRG g(test_config());
    std::vector<int> lits = {1};
    g.add_node(1, lits, 1, true, 0);
    g.add_node(2, lits, 1, true, 0);

    for (int i = 0; i < 3; ++i) g.record_co_conflict(1, 2);
    ASSERT_TRUE(g.has_edge(1, 2));

    EXPECT_TRUE(g.remove_edge(1, 2));
    EXPECT_FALSE(g.has_edge(1, 2));
    EXPECT_EQ(g.edge_count(), 0u);
    EXPECT_TRUE(g.get_neighbors(1).empty());
    EXPECT_TRUE(g.get_neighbors(2).empty());
}

TEST(DSRGEdgeTest, Neighbors) {
    DSRG g(test_config());
    std::vector<int> lits = {1};
    g.add_node(1, lits, 1, true, 0);
    g.add_node(2, lits, 1, true, 0);
    g.add_node(3, lits, 1, true, 0);

    for (int i = 0; i < 3; ++i) {
        g.record_co_conflict(1, 2);
        g.record_co_conflict(1, 3);
    }

    auto& nb = g.get_neighbors(1);
    EXPECT_EQ(nb.size(), 2u);
}

TEST(DSRGEdgeTest, RemoveNodeCleansEdges) {
    DSRG g(test_config());
    std::vector<int> lits = {1};
    g.add_node(1, lits, 1, true, 0);
    g.add_node(2, lits, 1, true, 0);
    g.add_node(3, lits, 1, true, 0);

    for (int i = 0; i < 3; ++i) {
        g.record_co_conflict(1, 2);
        g.record_co_conflict(1, 3);
    }
    EXPECT_EQ(g.edge_count(), 2u);

    g.remove_node(1);
    EXPECT_EQ(g.edge_count(), 0u);
    EXPECT_TRUE(g.get_neighbors(2).empty());
    EXPECT_TRUE(g.get_neighbors(3).empty());
}

// ============================================================
// Weight decay
// ============================================================

TEST(DSRGWeightTest, DecayAllWeights) {
    DSRG g(test_config());
    std::vector<int> lits = {1};
    g.add_node(1, lits, 1, true, 0);
    g.add_node(2, lits, 1, true, 0);

    for (int i = 0; i < 3; ++i) g.record_co_conflict(1, 2);

    g.decay_all_weights();

    EXPECT_NEAR(g.get_node(1)->weight, 0.95f, 1e-5f);
    EXPECT_NEAR(g.get_node(2)->weight, 0.95f, 1e-5f);

    const auto* edge = g.get_edge(1, 2);
    EXPECT_NEAR(edge->weight, 1.0f * 0.9f, 1e-5f);
}

TEST(DSRGWeightTest, MultipleDecays) {
    DSRG g(test_config());
    std::vector<int> lits = {1};
    g.add_node(1, lits, 1, true, 0);

    for (int i = 0; i < 10; ++i) g.decay_all_weights();

    float expected = std::pow(0.95f, 10);
    EXPECT_NEAR(g.get_node(1)->weight, expected, 1e-5f);
}

TEST(DSRGWeightTest, BoostNode) {
    DSRG g(test_config());
    std::vector<int> lits = {1};
    g.add_node(1, lits, 1, true, 0);

    g.boost_node(1, 2.0f);
    EXPECT_FLOAT_EQ(g.get_node(1)->weight, 3.0f);
}

// ============================================================
// GC: evict_stale_nodes
// ============================================================

TEST(DSRGGCTest, EvictStaleNodes) {
    DSRGConfig cfg = test_config();
    cfg.lbd_entry_threshold       = 6;   // allow all test nodes in
    cfg.eviction_weight_threshold = 0.1f;
    cfg.eviction_lbd_threshold    = 3;
    cfg.min_age_before_eviction   = 100;
    DSRG g(cfg);

    std::vector<int> lits = {1, 2};

    // Node 1: low weight, high lbd, old → should be evicted.
    ASSERT_TRUE(g.add_node(1, lits, 5, false, 0));
    const_cast<GraphNode*>(g.get_node(1))->weight = 0.001f;

    // Node 2: original → never evicted.
    ASSERT_TRUE(g.add_node(2, lits, 5, true, 0));
    const_cast<GraphNode*>(g.get_node(2))->weight = 0.001f;

    // Node 3: lbd=2, NOT > eviction_lbd_threshold 3 → not evicted.
    ASSERT_TRUE(g.add_node(3, lits, 2, false, 0));
    const_cast<GraphNode*>(g.get_node(3))->weight = 0.001f;

    // Node 4: too young (age = 200 - 150 = 50 < 100) → not evicted.
    ASSERT_TRUE(g.add_node(4, lits, 5, false, 150));
    const_cast<GraphNode*>(g.get_node(4))->weight = 0.001f;

    EXPECT_EQ(g.node_count(), 4u);
    size_t evicted = g.evict_stale_nodes(200);

    EXPECT_EQ(evicted, 1u);
    EXPECT_EQ(g.node_count(), 3u);
    EXPECT_EQ(g.get_node(1), nullptr);
    EXPECT_NE(g.get_node(2), nullptr);
    EXPECT_NE(g.get_node(3), nullptr);
    EXPECT_NE(g.get_node(4), nullptr);
}

// ============================================================
// GC: prune_weak_edges
// ============================================================

TEST(DSRGGCTest, PruneWeakEdges) {
    DSRGConfig cfg = test_config();
    cfg.edge_removal_threshold = 0.5f;
    DSRG g(cfg);

    std::vector<int> lits = {1};
    g.add_node(1, lits, 1, true, 0);
    g.add_node(2, lits, 1, true, 0);
    g.add_node(3, lits, 1, true, 0);

    for (int i = 0; i < 3; ++i) {
        g.record_co_conflict(1, 2);
        g.record_co_conflict(1, 3);
    }
    ASSERT_EQ(g.edge_count(), 2u);

    for (int i = 0; i < 50; ++i) g.decay_all_weights();

    size_t pruned = g.prune_weak_edges();
    EXPECT_EQ(pruned, 2u);
    EXPECT_EQ(g.edge_count(), 0u);
}

// ============================================================
// GC: merge_subsumption
// ============================================================

TEST(DSRGGCTest, MergeSubsumption) {
    DSRG g(test_config());
    std::vector<int> lits_a = {1, 2};
    std::vector<int> lits_b = {1, 2, 3};
    std::vector<int> lits_c = {3, 4};

    g.add_node(1, lits_a, 1, true, 0);
    g.add_node(2, lits_b, 2, false, 0);
    g.add_node(3, lits_c, 1, true, 0);

    for (int i = 0; i < 3; ++i) g.record_co_conflict(2, 3);
    ASSERT_TRUE(g.has_edge(2, 3));

    float w2 = g.get_node(2)->weight;
    float edge_w = g.get_edge(2, 3)->weight;

    g.merge_subsumption(1, 2);

    EXPECT_EQ(g.get_node(2), nullptr);
    EXPECT_EQ(g.node_count(), 2u);
    EXPECT_NEAR(g.get_node(1)->weight, 1.0f + w2, 1e-5f);

    EXPECT_FALSE(g.has_edge(2, 3));
    EXPECT_TRUE(g.has_edge(1, 3));
    EXPECT_NEAR(g.get_edge(1, 3)->weight, edge_w, 1e-5f);
}

// ============================================================
// GC: handle_variable_elimination
// ============================================================

TEST(DSRGGCTest, VariableElimination) {
    DSRG g(test_config());
    std::vector<int> lits1 = {1, 2};
    std::vector<int> lits2 = {-1, 3};

    g.add_node(10, lits1, 2, false, 0);
    g.add_node(20, lits2, 2, false, 0);

    GraphNode resolvent{};
    resolvent.clause_id      = 30;
    resolvent.lbd            = 2;
    resolvent.is_original    = false;
    resolvent.birth_conflict = 100;

    std::vector<uint32_t> removed = {10, 20};
    std::vector<GraphNode> resolvents = {resolvent};
    std::vector<std::vector<int>> res_lits = {{2, 3}};

    g.handle_variable_elimination(1, removed, resolvents, res_lits);

    EXPECT_EQ(g.get_node(10), nullptr);
    EXPECT_EQ(g.get_node(20), nullptr);
    EXPECT_NE(g.get_node(30), nullptr);
    EXPECT_EQ(g.node_count(), 1u);

    auto& vars30 = g.get_clause_vars(30);
    EXPECT_EQ(vars30.size(), 2u);
    EXPECT_TRUE(g.get_var_clauses(1).empty());
}

// ============================================================
// Pending conflicts lazy cleanup
// ============================================================

TEST(DSRGEdgeTest, PendingConflictLazyCleanup) {
    DSRG g(test_config());
    std::vector<int> lits = {1};
    g.add_node(1, lits, 1, true, 0);
    g.add_node(2, lits, 1, true, 0);

    g.record_co_conflict(1, 2);
    g.record_co_conflict(1, 2);

    g.remove_node(2);

    g.add_node(2, lits, 1, true, 0);
    g.record_co_conflict(1, 2);

    EXPECT_TRUE(g.has_edge(1, 2));
}

// ============================================================
// Config defaults
// ============================================================

TEST(DSRGConfigTest, DefaultValues) {
    DSRGConfig cfg;
    EXPECT_EQ(cfg.edge_creation_threshold, 3);
    EXPECT_FLOAT_EQ(cfg.edge_weight_momentum, 0.9f);
    EXPECT_FLOAT_EQ(cfg.node_weight_decay, 0.95f);
    EXPECT_EQ(cfg.lbd_entry_threshold, 3);
}
