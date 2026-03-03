#include "core/centrality.h"
#include "core/aggregation.h"
#include <gtest/gtest.h>
#include <cmath>
#include <vector>
using namespace sat_parallel;

static DSRG make_triangle() {
    DSRGConfig cfg;
    cfg.edge_creation_threshold = 1;
    DSRG g(cfg);
    g.add_node(1, std::vector<int>{1, 2},  2, true, 0);
    g.add_node(2, std::vector<int>{2, 3},  2, true, 0);
    g.add_node(3, std::vector<int>{3, -1}, 2, true, 0);
    g.record_co_conflict(1, 2);
    g.record_co_conflict(2, 3);
    g.record_co_conflict(1, 3);
    return g;
}

static DSRG make_star() {
    DSRGConfig cfg;
    cfg.edge_creation_threshold = 1;
    DSRG g(cfg);
    g.add_node(1, std::vector<int>{1}, 1, true, 0);
    g.add_node(2, std::vector<int>{2}, 1, true, 0);
    g.add_node(3, std::vector<int>{3}, 1, true, 0);
    g.add_node(4, std::vector<int>{4}, 1, true, 0);
    g.record_co_conflict(1, 2);
    g.record_co_conflict(1, 3);
    g.record_co_conflict(1, 4);
    return g;
}

TEST(DegreeCentralityTest, TriangleAllEqual) {
    auto g = make_triangle();
    auto s = compute_degree_centrality(g);
    ASSERT_EQ(s.size(), 3u);
    EXPECT_NEAR(s[1], 2.0f, 1e-5f);
    EXPECT_NEAR(s[2], 2.0f, 1e-5f);
    EXPECT_NEAR(s[3], 2.0f, 1e-5f);
}

TEST(DegreeCentralityTest, StarCenterHighest) {
    auto g = make_star();
    auto s = compute_degree_centrality(g);
    EXPECT_NEAR(s[1], 3.0f, 1e-5f);
    EXPECT_NEAR(s[2], 1.0f, 1e-5f);
}

TEST(DegreeCentralityTest, IsolatedNodeZero) {
    DSRGConfig cfg; DSRG g(cfg);
    g.add_node(1, std::vector<int>{1}, 1, true, 0);
    auto s = compute_degree_centrality(g);
    EXPECT_NEAR(s[1], 0.0f, 1e-5f);
}

TEST(DegreeCentralityTest, EmptyGraph) {
    DSRGConfig cfg; DSRG g(cfg);
    EXPECT_TRUE(compute_degree_centrality(g).empty());
}

TEST(PageRankTest, TriangleUniform) {
    auto g = make_triangle();
    CentralityConfig c; c.pagerank_max_iter=100; c.pagerank_epsilon=1e-6f;
    auto s = compute_pagerank(g, c);
    EXPECT_NEAR(s[1], 1.0f/3.0f, 1e-3f);
    EXPECT_NEAR(s[2], 1.0f/3.0f, 1e-3f);
    EXPECT_NEAR(s[3], 1.0f/3.0f, 1e-3f);
}

TEST(PageRankTest, StarCenterHighest) {
    auto g = make_star();
    CentralityConfig c; c.pagerank_max_iter=100; c.pagerank_epsilon=1e-6f;
    auto s = compute_pagerank(g, c);
    EXPECT_GT(s[1], s[2]);
    EXPECT_NEAR(s[2], s[3], 1e-4f);
    EXPECT_NEAR(s[3], s[4], 1e-4f);
}

TEST(PageRankTest, SumsToOne) {
    auto g = make_star();
    CentralityConfig c; c.pagerank_max_iter=100; c.pagerank_epsilon=1e-6f;
    auto s = compute_pagerank(g, c);
    float t = 0; for (auto& [_,v] : s) t += v;
    EXPECT_NEAR(t, 1.0f, 1e-3f);
}

TEST(PageRankTest, EmptyGraph) {
    DSRGConfig dc; DSRG g(dc); CentralityConfig c;
    EXPECT_TRUE(compute_pagerank(g, c).empty());
}

TEST(CentralityDispatchTest, Degree) {
    auto g = make_triangle();
    CentralityConfig c; c.algorithm = "degree";
    EXPECT_NEAR(compute_centrality(g, c)[1], 2.0f, 1e-5f);
}

TEST(CentralityDispatchTest, PageRank) {
    auto g = make_triangle();
    CentralityConfig c; c.algorithm = "pagerank"; c.pagerank_max_iter=50; c.pagerank_epsilon=1e-6f;
    EXPECT_NEAR(compute_centrality(g, c)[1], 1.0f/3.0f, 1e-3f);
}

TEST(AggregationTest, WeightedSum) {
    auto g = make_triangle();
    auto cent = compute_degree_centrality(g);
    AggregationConfig a; a.method="weighted_sum"; a.alpha=1.0f;
    auto vs = aggregate_to_variables(g, cent, a);
    EXPECT_NEAR(vs[1], 4.0f, 1e-5f);
    EXPECT_NEAR(vs[2], 4.0f, 1e-5f);
    EXPECT_NEAR(vs[3], 4.0f, 1e-5f);
}

TEST(AggregationTest, WeightedSumAlpha) {
    auto g = make_triangle();
    auto cent = compute_degree_centrality(g);
    AggregationConfig a; a.method="weighted_sum"; a.alpha=0.5f;
    auto vs = aggregate_to_variables(g, cent, a);
    EXPECT_NEAR(vs[1], 2.0f, 1e-5f);
}

TEST(AggregationTest, SumMethod) {
    auto g = make_triangle();
    auto cent = compute_degree_centrality(g);
    AggregationConfig a; a.method="sum";
    auto vs = aggregate_to_variables(g, cent, a);
    EXPECT_NEAR(vs[1], 4.0f, 1e-5f);
}

TEST(AggregationTest, MaxMethod) {
    auto g = make_star();
    auto cent = compute_degree_centrality(g);
    AggregationConfig a; a.method="max";
    auto vs = aggregate_to_variables(g, cent, a);
    EXPECT_NEAR(vs[1], 3.0f, 1e-5f);
    EXPECT_NEAR(vs[2], 1.0f, 1e-5f);
}

TEST(AggregationTest, StarHighlightsCenterVar) {
    auto g = make_star();
    auto cent = compute_degree_centrality(g);
    AggregationConfig a; a.method="weighted_sum"; a.alpha=1.0f;
    auto vs = aggregate_to_variables(g, cent, a);
    EXPECT_GT(vs[1], vs[2]);
}

TEST(VSIDSMixTest, LambdaZero) {
    std::unordered_map<uint32_t, float> gs={{1,10.0f}}, vi={{1,1.0f}};
    auto r = mix_with_vsids(gs, vi, 0.0f);
    EXPECT_FLOAT_EQ(r[1], 1.0f);
}

TEST(VSIDSMixTest, LambdaOne) {
    std::unordered_map<uint32_t, float> gs={{1,10.0f}}, vi={{1,1.0f}};
    auto r = mix_with_vsids(gs, vi, 1.0f);
    EXPECT_FLOAT_EQ(r[1], 10.0f);
}

TEST(VSIDSMixTest, DefaultLambda) {
    std::unordered_map<uint32_t, float> gs={{1,10.0f}}, vi={{1,1.0f}};
    auto r = mix_with_vsids(gs, vi, 0.3f);
    EXPECT_NEAR(r[1], 3.7f, 1e-5f);
}

TEST(VSIDSMixTest, GraphOnlyVar) {
    std::unordered_map<uint32_t, float> gs={{1,10.0f},{99,5.0f}}, vi={{1,1.0f}};
    auto r = mix_with_vsids(gs, vi, 0.3f);
    EXPECT_NEAR(r[99], 1.5f, 1e-5f);
}

TEST(TopKTest, SelectsCorrectly) {
    std::unordered_map<uint32_t, float> s={{1,5.0f},{2,1.0f},{3,9.0f},{4,3.0f},{5,7.0f}};
    auto t = select_top_k(s, 3);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[0].first, 3u);
    EXPECT_EQ(t[1].first, 5u);
    EXPECT_EQ(t[2].first, 1u);
}

TEST(TopKTest, KLargerThanSize) {
    std::unordered_map<uint32_t, float> s={{1,1.0f},{2,2.0f}};
    EXPECT_EQ(select_top_k(s, 5).size(), 2u);
}

TEST(TopKTest, EmptyInput) {
    std::unordered_map<uint32_t, float> s;
    EXPECT_TRUE(select_top_k(s, 10).empty());
}

TEST(EndToEndTest, FullPipeline) {
    auto g = make_star();
    auto cent = compute_degree_centrality(g);
    AggregationConfig a; a.method="weighted_sum"; a.alpha=1.0f; a.lambda=0.3f;
    auto gs = aggregate_to_variables(g, cent, a);
    std::unordered_map<uint32_t, float> vi;
    for (auto& [v,_] : gs) vi[v] = 1.0f;
    auto mixed = mix_with_vsids(gs, vi, a.lambda);
    auto top2 = select_top_k(mixed, 2);
    ASSERT_EQ(top2.size(), 2u);
    EXPECT_EQ(top2[0].first, 1u);
}

TEST(CentralityConfigTest, Defaults) {
    CentralityConfig c;
    EXPECT_EQ(c.algorithm, "degree");
    EXPECT_FLOAT_EQ(c.pagerank_damping, 0.85f);
}

TEST(AggregationConfigTest, Defaults) {
    AggregationConfig a;
    EXPECT_EQ(a.method, "weighted_sum");
    EXPECT_FLOAT_EQ(a.lambda, 0.3f);
    EXPECT_EQ(a.top_k, 100);
}
