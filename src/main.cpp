#include "comm/broadcast.h"
#include "comm/delta_patch.h"
#include "comm/gpu_channel.h"
#include "comm/mpsc_queue.h"
#include "master/master.h"
#include "solver/cadical_adapter.h"
#include "solver/cnf_parser.h"
#include "worker/worker_engine.h"

#include <cadical.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace sat_parallel;

static std::atomic<int>  global_result{0};   // 0=unknown, 10=SAT, 20=UNSAT
static std::atomic<int>  winner_id{-1};
static std::mutex        result_mu;
static std::condition_variable result_cv;

struct WorkerCtx {
    std::unique_ptr<CaDiCaL::Solver> solver;
    std::unique_ptr<WorkerEngine>    engine;
    std::unique_ptr<CaDiCaLAdapter>  adapter;
    std::thread                      thread;
};

struct Config {
    std::string cnf_path;
    int  num_workers  = 0;
    int  timeout_sec  = 0;
    int  max_lbd      = 5;
};

static Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cfg.num_workers = std::atoi(argv[++i]);
        } else if (std::strncmp(argv[i], "-c=", 3) == 0) {
            cfg.num_workers = std::atoi(argv[i] + 3);
        } else if (std::strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            cfg.timeout_sec = std::atoi(argv[++i]);
        } else if (std::strncmp(argv[i], "-t=", 3) == 0) {
            cfg.timeout_sec = std::atoi(argv[i] + 3);
        } else if (std::strcmp(argv[i], "--lbd") == 0 && i + 1 < argc) {
            cfg.max_lbd = std::atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            cfg.cnf_path = argv[i];
        }
    }
    if (cfg.cnf_path.empty()) {
        std::cerr << "Usage: sat_parallel [-c CORES] [-t TIMEOUT] [--lbd N] <input.cnf>\n";
        std::exit(1);
    }
    if (cfg.num_workers <= 0) {
        cfg.num_workers = static_cast<int>(std::thread::hardware_concurrency());
        if (cfg.num_workers < 2) cfg.num_workers = 2;
    }
    return cfg;
}

static void diversify_solver(CaDiCaL::Solver& s, int id) {
    s.set("seed", id);
    switch (id % 8) {
    case 0: break;
    case 1: s.set("phase", 0); break;
    case 2: s.set("phase", 1); break;
    case 3: s.set("restartint", 100 + id * 50); break;
    case 4: s.set("shuffle", 1); s.set("shufflerandom", 1); break;
    case 5: s.set("stabilize", 0); break;
    case 6: s.set("walk", 1); break;
    case 7: s.set("target", 2); break;
    }
}

static void worker_fn(int id, WorkerCtx& ctx, CaDiCaLAdapter& adapter,
                      const BroadcastChannel& bc) {
    uint64_t last_ver = 0;

    while (global_result.load(std::memory_order_relaxed) == 0) {
        int res = ctx.solver->solve();

        if (res == 10 || res == 20) {
            int expected = 0;
            if (global_result.compare_exchange_strong(expected, res)) {
                winner_id.store(id, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lk(result_mu);
                result_cv.notify_all();
            }
            break;
        }

        // Poll broadcast between rounds.
        uint64_t ver = bc.version();
        if (ver > last_ver) {
            last_ver = ver;
            auto snap = bc.read();
            if (snap) {
                adapter.apply_phase_hints(snap->top_k_var_scores, 100);
                if (!snap->shared_clauses.empty())
                    adapter.import_shared_clauses(snap->shared_clauses);
            }
        }

        if (global_result.load(std::memory_order_relaxed) != 0) break;
    }

    ctx.engine->flush();
}

int main(int argc, char** argv) {
    auto cfg = parse_args(argc, argv);

    std::cerr << "c SAT_Parallel DSRG Solver\n";
    std::cerr << "c Workers: " << cfg.num_workers << "\n";
    std::cerr << "c Timeout: " << (cfg.timeout_sec > 0 ? std::to_string(cfg.timeout_sec) + "s" : "none") << "\n";
    std::cerr << "c Input: " << cfg.cnf_path << "\n";

    // Parse CNF.
    auto t0 = std::chrono::steady_clock::now();
    CNFFormula formula = parse_dimacs(cfg.cnf_path);
    double parse_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    std::cerr << "c Parsed: " << formula.num_vars << " vars, "
              << formula.clauses.size() << " clauses (" << parse_ms << " ms)\n";

    // Communication channels (shared between Master and Workers).
    MPSCQueue<DeltaPatch> patch_queue;
    BroadcastChannel      broadcast;
    GPUChannel            gpu_channel;

    // Master.
    MasterConfig mcfg;
    mcfg.dsrg.lbd_entry_threshold = cfg.max_lbd;
    mcfg.centrality_interval      = 2000;
    mcfg.gc_interval              = 5000;
    mcfg.broadcast_interval_ms    = 500;
    mcfg.learnt_clause_lbd_filter = cfg.max_lbd;
    Master master(mcfg, patch_queue, broadcast, gpu_channel);

    // Worker config.
    WorkerConfig wcfg;
    wcfg.weights.retention_alpha = 0.7f;
    wcfg.weights.lambda          = 0.3f;
    wcfg.patch.conflict_interval = 5000;
    wcfg.patch.lbd_trigger       = cfg.max_lbd;
    wcfg.patch.lbd_entry         = cfg.max_lbd;
    wcfg.patch.budget_bytes      = 8192;

    // Create workers.
    std::vector<std::unique_ptr<WorkerCtx>> workers;
    workers.reserve(cfg.num_workers);

    for (int i = 0; i < cfg.num_workers; i++) {
        auto ctx = std::make_unique<WorkerCtx>();
        ctx->solver = std::make_unique<CaDiCaL::Solver>();
        diversify_solver(*ctx->solver, i);

        // Load formula.
        for (const auto& cl : formula.clauses) {
            for (int lit : cl) ctx->solver->add(lit);
            ctx->solver->add(0);
        }

        ctx->engine = std::make_unique<WorkerEngine>(
            static_cast<uint32_t>(i), wcfg, patch_queue, broadcast);
        ctx->adapter = std::make_unique<CaDiCaLAdapter>(
            *ctx->solver, *ctx->engine, cfg.max_lbd);

        workers.push_back(std::move(ctx));
    }

    std::cerr << "c Initialized " << cfg.num_workers << " solvers\n";

    // Master thread.
    std::thread master_thr([&]() {
        while (global_result.load(std::memory_order_relaxed) == 0) {
            master.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Worker threads.
    for (int i = 0; i < cfg.num_workers; i++) {
        auto& ctx = workers[i];
        ctx->thread = std::thread(worker_fn, i, std::ref(*ctx),
                                  std::ref(*ctx->adapter), std::cref(broadcast));
    }

    // Timeout watcher.
    std::thread timeout_thr;
    if (cfg.timeout_sec > 0) {
        timeout_thr = std::thread([&]() {
            std::unique_lock<std::mutex> lk(result_mu);
            if (!result_cv.wait_for(lk, std::chrono::seconds(cfg.timeout_sec),
                                    [] { return global_result.load() != 0; })) {
                // Timeout: signal all workers to stop.
                for (auto& w : workers)
                    w->adapter->request_stop();
                global_result.store(-1, std::memory_order_relaxed);
                result_cv.notify_all();
            }
        });
    }

    // Wait for any result.
    {
        std::unique_lock<std::mutex> lk(result_mu);
        result_cv.wait(lk, [] { return global_result.load() != 0; });
    }

    auto t_end = std::chrono::steady_clock::now();
    double solve_sec = std::chrono::duration<double>(t_end - t0).count();

    // Stop everything.
    for (auto& w : workers)
        w->adapter->request_stop();

    for (auto& w : workers)
        if (w->thread.joinable()) w->thread.join();

    // Wake master.
    global_result.store(global_result.load(), std::memory_order_seq_cst);
    if (master_thr.joinable()) master_thr.join();

    if (timeout_thr.joinable()) {
        { std::lock_guard<std::mutex> lk(result_mu); result_cv.notify_all(); }
        timeout_thr.join();
    }

    int res = global_result.load();
    int wid = winner_id.load();

    std::cerr << "c Solve time: " << solve_sec << "s\n";
    std::cerr << "c DSRG nodes: " << master.graph().node_count()
              << ", edges: " << master.graph().edge_count()
              << ", total conflicts: " << master.total_conflicts_seen() << "\n";

    if (res == 10) {
        std::cout << "s SATISFIABLE" << std::endl;
        if (wid >= 0 && wid < cfg.num_workers) {
            std::cout << "v";
            for (int v = 1; v <= formula.num_vars; v++) {
                std::cout << " " << workers[wid]->solver->val(v);
            }
            std::cout << " 0" << std::endl;
        }
        return 10;
    } else if (res == 20) {
        std::cout << "s UNSATISFIABLE" << std::endl;
        return 20;
    } else {
        std::cout << "s UNKNOWN" << std::endl;
        return 0;
    }
}
