#include "core/dsrg.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace sat_parallel;
using Clock = std::chrono::high_resolution_clock;

static double elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

static void print_row(const char* name, size_t ops, double ms) {
    double us_per_op = (ms * 1000.0) / static_cast<double>(ops);
    std::printf("  %-35s  %8zu ops  %10.2f ms  %8.2f us/op\n",
                name, ops, ms, us_per_op);
}

int main() {
    constexpr size_t NUM_NODES = 100'000;
    constexpr size_t NUM_EDGES = 500'000;
    constexpr size_t NUM_QUERIES = 10'000;

    DSRGConfig cfg;
    cfg.edge_creation_threshold = 1;  // immediate edge creation for bench
    DSRG g(cfg);

    std::mt19937 rng(42);

    // --- Add 100K nodes ---
    std::vector<uint32_t> node_ids(NUM_NODES);
    {
        auto t0 = Clock::now();
        for (size_t i = 0; i < NUM_NODES; ++i) {
            uint32_t cid = static_cast<uint32_t>(i + 1);
            node_ids[i] = cid;

            int num_lits = 3 + static_cast<int>(rng() % 8);
            std::vector<int> lits(num_lits);
            for (int j = 0; j < num_lits; ++j) {
                int var = 1 + static_cast<int>(rng() % 10000);
                lits[j] = (rng() % 2 == 0) ? var : -var;
            }
            g.add_node(cid, lits, 2, (i < 1000), 0);
        }
        auto t1 = Clock::now();
        print_row("Add 100K nodes", NUM_NODES, elapsed_ms(t0, t1));
    }

    // --- Create 500K edges via record_co_conflict ---
    {
        std::uniform_int_distribution<size_t> dist(0, NUM_NODES - 1);
        size_t created = 0;
        auto t0 = Clock::now();
        while (created < NUM_EDGES) {
            uint32_t a = node_ids[dist(rng)];
            uint32_t b = node_ids[dist(rng)];
            if (a != b && !g.has_edge(a, b)) {
                g.record_co_conflict(a, b);
                created++;
            }
        }
        auto t1 = Clock::now();
        print_row("Create 500K edges", NUM_EDGES, elapsed_ms(t0, t1));
        std::printf("    (actual edge_count = %zu)\n", g.edge_count());
    }

    // --- Random node queries ---
    {
        std::uniform_int_distribution<size_t> dist(0, NUM_NODES - 1);
        auto t0 = Clock::now();
        volatile const GraphNode* sink = nullptr;
        for (size_t i = 0; i < NUM_QUERIES; ++i) {
            sink = g.get_node(node_ids[dist(rng)]);
        }
        auto t1 = Clock::now();
        (void)sink;
        print_row("Query 10K nodes", NUM_QUERIES, elapsed_ms(t0, t1));
    }

    // --- Random edge queries ---
    {
        std::uniform_int_distribution<size_t> dist(0, NUM_NODES - 1);
        auto t0 = Clock::now();
        volatile const GraphEdge* sink = nullptr;
        for (size_t i = 0; i < NUM_QUERIES; ++i) {
            uint32_t a = node_ids[dist(rng)];
            uint32_t b = node_ids[dist(rng)];
            sink = g.get_edge(a, b);
        }
        auto t1 = Clock::now();
        (void)sink;
        print_row("Query 10K edges", NUM_QUERIES, elapsed_ms(t0, t1));
    }

    // --- Full decay pass ---
    {
        auto t0 = Clock::now();
        g.decay_all_weights();
        auto t1 = Clock::now();
        print_row("decay_all_weights (full graph)", 1, elapsed_ms(t0, t1));
    }

    // --- GC: evict + prune ---
    {
        // Decay many times to make some nodes/edges weak.
        for (int i = 0; i < 100; ++i) g.decay_all_weights();

        auto t0 = Clock::now();
        size_t evicted = g.evict_stale_nodes(1'000'000);
        size_t pruned  = g.prune_weak_edges();
        auto t1 = Clock::now();
        std::printf("    (evicted %zu nodes, pruned %zu edges)\n", evicted, pruned);
        print_row("GC pass (evict + prune)", 1, elapsed_ms(t0, t1));
    }

    // --- Remove 10K nodes ---
    {
        size_t to_remove = std::min(NUM_QUERIES, g.node_count());
        std::vector<uint32_t> targets;
        targets.reserve(to_remove);
        g.for_each_node([&](const GraphNode& n) {
            if (targets.size() < to_remove) targets.push_back(n.clause_id);
        });

        auto t0 = Clock::now();
        for (uint32_t id : targets) {
            g.remove_node(id);
        }
        auto t1 = Clock::now();
        print_row("Remove 10K nodes", targets.size(), elapsed_ms(t0, t1));
    }

    std::printf("\nFinal state: %zu nodes, %zu edges\n",
                g.node_count(), g.edge_count());

    return 0;
}
