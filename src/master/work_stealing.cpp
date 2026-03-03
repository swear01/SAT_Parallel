#include "master/work_stealing.h"

namespace sat_parallel {

void WorkStealingManager::load_cubes(std::vector<Cube> cubes) {
    std::lock_guard<std::mutex> lk(mu_);
    while (!pending_.empty()) pending_.pop();
    in_progress_.clear();
    completed_.clear();
    sat_found_ = false;
    for (auto& c : cubes) pending_.push(std::move(c));
}

std::optional<Cube> WorkStealingManager::assign_next(int /*worker_id*/) {
    std::lock_guard<std::mutex> lk(mu_);
    if (pending_.empty() || sat_found_) return std::nullopt;
    Cube c = std::move(pending_.front());
    pending_.pop();
    in_progress_.insert(c.cube_id);
    return c;
}

void WorkStealingManager::report_done(int /*worker_id*/, int cube_id,
                                       CubeResult result) {
    std::lock_guard<std::mutex> lk(mu_);
    in_progress_.erase(cube_id);
    completed_.insert(cube_id);
    if (result == CubeResult::SAT) sat_found_ = true;
}

std::optional<Cube> WorkStealingManager::steal(int worker_id) {
    return assign_next(worker_id);
}

int WorkStealingManager::remaining_cubes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(pending_.size() + in_progress_.size());
}

bool WorkStealingManager::all_done() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pending_.empty() && in_progress_.empty();
}

bool WorkStealingManager::found_sat() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sat_found_;
}

}  // namespace sat_parallel
