#include "comm/gpu_channel.h"
#include "gpu/gpu_prober.h"
#include "gpu/gpu_types.h"
#include "gpu/walksat_kernel.cuh"
#include <cuda_runtime.h>
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <vector>
using namespace sat_parallel;

static bool has_gpu() {
    int count = 0;
    cudaGetDeviceCount(&count);
    return count > 0;
}

// Simple SAT instance: (x1 OR x2) AND (NOT x1 OR x3) AND (x2 OR x3)
// 3 vars, 3 clauses.  SAT: e.g. x1=T, x2=T, x3=T
static void make_simple_instance(std::vector<std::vector<int>>& clauses,
                                 std::vector<uint32_t>& ids, int& nvars) {
    clauses = {{1, 2}, {-1, 3}, {2, 3}};
    ids = {100, 200, 300};
    nvars = 3;
}

TEST(GPUTypesTest, FlatClauseDB) {
    FlatClauseDB db;
    EXPECT_EQ(db.num_clauses, 0);
    EXPECT_EQ(db.total_literals(), 0);
}

TEST(GPUProberTest, LoadClauses) {
    if (!has_gpu()) { GTEST_SKIP() << "No GPU available"; }

    GPUChannel channel;
    GPUProberConfig cfg;
    cfg.max_flips_per_run = 1000;
    GPUProber prober(cfg, channel);

    std::vector<std::vector<int>> clauses;
    std::vector<uint32_t> ids;
    int nvars;
    make_simple_instance(clauses, ids, nvars);

    prober.load_clauses(clauses, ids, nvars);
    EXPECT_FALSE(prober.running());
}

TEST(GPUProberTest, RunAndReport) {
    if (!has_gpu()) { GTEST_SKIP() << "No GPU available"; }

    GPUChannel channel;
    GPUProberConfig cfg;
    cfg.max_flips_per_run = 5000;
    cfg.hotzone_top_k = 10;
    GPUProber prober(cfg, channel);

    std::vector<std::vector<int>> clauses;
    std::vector<uint32_t> ids;
    int nvars;
    make_simple_instance(clauses, ids, nvars);
    prober.load_clauses(clauses, ids, nvars);

    prober.start();
    EXPECT_TRUE(prober.running());

    // Wait for at least one report.
    GPUReport report;
    bool got_report = false;
    for (int attempt = 0; attempt < 50 && !got_report; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto r = channel.try_receive_report();
        if (r.has_value()) {
            report = std::move(*r);
            got_report = true;
        }
    }

    prober.stop();
    EXPECT_FALSE(prober.running());
    EXPECT_GT(prober.total_steps(), 0u);

    ASSERT_TRUE(got_report) << "No GPU report received within 5 seconds";

    // Simple SAT instance should be solved (unsat_count = 0).
    EXPECT_EQ(report.unsat_count, 0);
    // Phase hints should cover all 3 variables.
    EXPECT_EQ(static_cast<int>(report.best_assignment.size()), nvars);
}

TEST(GPUProberTest, DynamicClausePush) {
    if (!has_gpu()) { GTEST_SKIP() << "No GPU available"; }

    GPUChannel channel;
    GPUProberConfig cfg;
    cfg.max_flips_per_run = 2000;
    GPUProber prober(cfg, channel);

    std::vector<std::vector<int>> clauses = {{1, 2}, {-1, 3}};
    std::vector<uint32_t> ids = {10, 20};
    prober.load_clauses(clauses, ids, 3);

    // Push a new clause via the channel.
    GPUClausePush push;
    push.clauses.push_back({30, {2, 3}, 2});
    channel.push_clauses(std::move(push));

    prober.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    prober.stop();

    // At least some steps should have been taken.
    EXPECT_GT(prober.total_steps(), 0u);
}

TEST(GPUProberTest, HotzoneContent) {
    if (!has_gpu()) { GTEST_SKIP() << "No GPU available"; }

    GPUChannel channel;
    GPUProberConfig cfg;
    cfg.max_flips_per_run = 10000;
    cfg.hotzone_top_k = 5;
    GPUProber prober(cfg, channel);

    // Harder instance: more clauses.
    std::vector<std::vector<int>> clauses = {
        {1, 2}, {-1, 3}, {2, -3}, {-2, 3}, {1, -2, 3},
        {-1, -2}, {1, 3}, {-3, 2}
    };
    std::vector<uint32_t> ids = {1, 2, 3, 4, 5, 6, 7, 8};
    prober.load_clauses(clauses, ids, 3);

    prober.start();

    GPUReport report;
    bool got_report = false;
    for (int attempt = 0; attempt < 30 && !got_report; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto r = channel.try_receive_report();
        if (r.has_value()) {
            report = std::move(*r);
            got_report = true;
        }
    }

    prober.stop();
    ASSERT_TRUE(got_report);

    // Hotzone should have some entries (clauses that were frequently unsatisfied).
    if (report.unsat_count > 0) {
        EXPECT_GT(report.hotzone.size(), 0u);
        for (const auto& hz : report.hotzone) {
            EXPECT_GT(hz.frequency, 0);
        }
    }
}
