#pragma once

#include <cstdint>
#include <vector>

namespace sat_parallel {

// Worker -> Master communication payload.
// Sent when either:
//   - conflict_count_since_last >= delta_patch_conflict_interval
//   - a learnt clause with lbd <= delta_patch_lbd_trigger is produced
// Budget: < 4KB per patch.
struct DeltaPatch {
    uint32_t worker_id;
    uint64_t conflict_count;

    struct LearnedClause {
        uint32_t clause_id;
        std::vector<int> literals;
        int lbd;
    };
    std::vector<LearnedClause> new_clauses;

    struct ConflictPair {
        uint32_t clause_a, clause_b;
        int delta_count;
    };
    std::vector<ConflictPair> conflict_pairs;

    struct HotVariable {
        uint32_t var_id;
        int frequency;
    };
    std::vector<HotVariable> hot_variables;

    // Approximate serialized size for budget enforcement.
    size_t estimated_size_bytes() const {
        size_t s = sizeof(worker_id) + sizeof(conflict_count);
        for (const auto& c : new_clauses)
            s += sizeof(c.clause_id) + sizeof(c.lbd) +
                 c.literals.size() * sizeof(int);
        s += conflict_pairs.size() * sizeof(ConflictPair);
        s += hot_variables.size() * sizeof(HotVariable);
        return s;
    }
};

}  // namespace sat_parallel
