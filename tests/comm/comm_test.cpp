#include "comm/mpsc_queue.h"
#include "comm/delta_patch.h"
#include "comm/broadcast.h"
#include "comm/gpu_channel.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <thread>
#include <vector>
using namespace sat_parallel;

TEST(MPSCQueueTest, PushPopSingle) {
    MPSCQueue<int> q;
    EXPECT_TRUE(q.empty());
    q.push(1); q.push(2); q.push(3);
    EXPECT_FALSE(q.empty());
    EXPECT_EQ(q.try_pop().value(), 1);
    EXPECT_EQ(q.try_pop().value(), 2);
    EXPECT_EQ(q.try_pop().value(), 3);
    EXPECT_FALSE(q.try_pop().has_value());
}

TEST(MPSCQueueTest, Drain) {
    MPSCQueue<int> q;
    for (int i = 0; i < 5; ++i) q.push(i);
    std::vector<int> out;
    size_t n = q.drain([&](int v) { out.push_back(v); });
    EXPECT_EQ(n, 5u);
    EXPECT_EQ(out, (std::vector<int>{0,1,2,3,4}));
}

TEST(MPSCQueueTest, MoveOnly) {
    MPSCQueue<std::unique_ptr<int>> q;
    q.push(std::make_unique<int>(42));
    auto v = q.try_pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(**v, 42);
}

TEST(MPSCQueueTest, DeltaPatchPayload) {
    MPSCQueue<DeltaPatch> q;
    DeltaPatch p;
    p.worker_id = 7; p.conflict_count = 12345;
    p.new_clauses.push_back({100, {1,-2,3}, 2});
    p.conflict_pairs.push_back({10, 20, 5});
    p.hot_variables.push_back({42, 10});
    q.push(std::move(p));
    auto r = q.try_pop();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->worker_id, 7u);
    EXPECT_EQ(r->new_clauses.size(), 1u);
    EXPECT_EQ(r->new_clauses[0].literals.size(), 3u);
}

TEST(MPSCQueueTest, MultiProducerCorrectness) {
    constexpr int NP = 4, IPP = 10000;
    MPSCQueue<int> q;
    std::atomic<int> ready{0};
    std::vector<std::thread> prods;
    for (int p = 0; p < NP; ++p) {
        prods.emplace_back([&q, &ready, p]() {
            ready.fetch_add(1);
            while (ready.load() < NP) {}
            for (int i = 0; i < IPP; ++i) q.push(p*IPP+i);
        });
    }
    for (auto& t : prods) t.join();
    std::vector<int> consumed;
    q.drain([&](int v) { consumed.push_back(v); });
    EXPECT_EQ(consumed.size(), static_cast<size_t>(NP*IPP));
    std::sort(consumed.begin(), consumed.end());
    std::vector<int> expected(NP*IPP);
    std::iota(expected.begin(), expected.end(), 0);
    EXPECT_EQ(consumed, expected);
}

TEST(MPSCQueueTest, ConcurrentPushPop) {
    constexpr int NP = 4, IPP = 5000;
    MPSCQueue<int> q;
    std::atomic<bool> done{false};
    std::vector<int> consumed;
    std::thread consumer([&]() {
        while (!done.load(std::memory_order_acquire) || !q.empty()) {
            q.drain([&](int v) { consumed.push_back(v); });
            std::this_thread::yield();
        }
        q.drain([&](int v) { consumed.push_back(v); });
    });
    std::vector<std::thread> prods;
    for (int p = 0; p < NP; ++p) {
        prods.emplace_back([&q, p]() {
            for (int i = 0; i < IPP; ++i) q.push(p*IPP+i);
        });
    }
    for (auto& t : prods) t.join();
    done.store(true, std::memory_order_release);
    consumer.join();
    EXPECT_EQ(consumed.size(), static_cast<size_t>(NP*IPP));
    std::sort(consumed.begin(), consumed.end());
    std::vector<int> expected(NP*IPP);
    std::iota(expected.begin(), expected.end(), 0);
    EXPECT_EQ(consumed, expected);
}

TEST(DeltaPatchTest, SizeEstimation) {
    DeltaPatch p;
    p.worker_id = 0; p.conflict_count = 0;
    p.new_clauses.push_back({1, {1,-2,3}, 2});
    p.conflict_pairs.push_back({10, 20, 1});
    p.hot_variables.push_back({5, 3});
    EXPECT_GT(p.estimated_size_bytes(), 0u);
    EXPECT_LT(p.estimated_size_bytes(), 4096u);
}

TEST(BroadcastTest, PublishAndRead) {
    BroadcastChannel ch;
    EXPECT_EQ(ch.version(), 0u);
    EXPECT_EQ(ch.read(), nullptr);
    GlobalBroadcast b;
    b.timestamp = 42;
    b.top_k_var_scores.push_back({1, 0.5f});
    ch.publish(std::move(b));
    EXPECT_EQ(ch.version(), 1u);
    auto snap = ch.read();
    ASSERT_NE(snap, nullptr);
    EXPECT_EQ(snap->timestamp, 42u);
    EXPECT_EQ(snap->top_k_var_scores.size(), 1u);
}

TEST(BroadcastTest, LatestWins) {
    BroadcastChannel ch;
    for (int i = 0; i < 5; ++i) {
        GlobalBroadcast b; b.timestamp = static_cast<uint64_t>(i);
        ch.publish(std::move(b));
    }
    EXPECT_EQ(ch.version(), 5u);
    EXPECT_EQ(ch.read()->timestamp, 4u);
}

TEST(BroadcastTest, ConcurrentReaders) {
    BroadcastChannel ch;
    GlobalBroadcast b; b.timestamp = 100;
    b.top_k_var_scores.push_back({1, 1.0f});
    ch.publish(std::move(b));
    constexpr int NR = 8;
    std::atomic<int> ok{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < NR; ++i) {
        readers.emplace_back([&]() {
            auto s = ch.read();
            if (s && s->timestamp == 100) ok.fetch_add(1);
        });
    }
    for (auto& t : readers) t.join();
    EXPECT_EQ(ok.load(), NR);
}

TEST(GPUChannelTest, SendReceiveReport) {
    GPUChannel ch;
    GPUReport rpt;
    rpt.hotzone.push_back({10, 5});
    rpt.best_assignment.push_back({1, true});
    rpt.unsat_count = 42;
    ch.send_report(std::move(rpt));
    auto r = ch.try_receive_report();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->hotzone[0].clause_id, 10u);
    EXPECT_EQ(r->unsat_count, 42);
}

TEST(GPUChannelTest, PushClauses) {
    GPUChannel ch;
    GPUClausePush push;
    push.clauses.push_back({1, {1,-2}, 2});
    ch.push_clauses(std::move(push));
    auto p = ch.consume_push();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->clauses.size(), 1u);
    EXPECT_EQ(ch.consume_push(), nullptr);
}

TEST(MPSCQueueTest, ThroughputBench) {
    constexpr int NP = 8, IPP = 100000;
    MPSCQueue<int> q;
    std::atomic<bool> done{false};
    std::atomic<int> total{0};
    std::thread consumer([&]() {
        int c = 0;
        while (!done.load(std::memory_order_acquire) || !q.empty()) {
            c += static_cast<int>(q.drain([](int){}));
        }
        c += static_cast<int>(q.drain([](int){}));
        total.store(c);
    });
    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> prods;
    for (int p = 0; p < NP; ++p) {
        prods.emplace_back([&q, p]() {
            for (int i = 0; i < IPP; ++i) q.push(p*IPP+i);
        });
    }
    for (auto& t : prods) t.join();
    done.store(true, std::memory_order_release);
    consumer.join();
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1-t0).count();
    EXPECT_EQ(total.load(), NP*IPP);
    double mops = static_cast<double>(NP*IPP)/ms/1000.0;
    std::printf("  MPSC throughput: %d items, %.1f ms, %.1f M ops/s\n",
                NP*IPP, ms, mops);
}
