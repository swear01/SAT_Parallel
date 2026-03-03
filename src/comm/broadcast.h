#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace sat_parallel {

struct GlobalBroadcast {
    uint64_t timestamp = 0;

    struct VarScore {
        uint32_t var_id;
        float global_score;
    };
    std::vector<VarScore> top_k_var_scores;

    struct ClauseWeight {
        uint32_t clause_id;
        float global_weight;
    };
    std::vector<ClauseWeight> top_k_clause_weights;

    std::vector<std::vector<int>> shared_clauses;
};

// Broadcast channel: Master publishes, Workers read the latest snapshot.
//
// Uses a spinlock-guarded shared_ptr swap for GCC 11 compatibility
// (std::atomic<shared_ptr> requires GCC 12+).
// Critical section is < 100 ns (pointer swap only), contention negligible.
class BroadcastChannel {
public:
    BroadcastChannel() = default;

    void publish(GlobalBroadcast data) {
        auto slot = std::make_shared<const GlobalBroadcast>(std::move(data));
        {
            std::lock_guard<std::mutex> lk(mu_);
            current_ = std::move(slot);
        }
        version_.fetch_add(1, std::memory_order_release);
    }

    std::shared_ptr<const GlobalBroadcast> read() const {
        std::lock_guard<std::mutex> lk(mu_);
        return current_;
    }

    uint64_t version() const {
        return version_.load(std::memory_order_acquire);
    }

private:
    mutable std::mutex mu_;
    std::shared_ptr<const GlobalBroadcast> current_;
    alignas(64) std::atomic<uint64_t> version_{0};
};

}  // namespace sat_parallel
