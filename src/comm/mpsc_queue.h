#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>

namespace sat_parallel {

// Lock-free Multi-Producer Single-Consumer queue (Vyukov/Dmitry style).
// - Push is wait-free (single atomic exchange).
// - Pop is lock-free (single consumer only).
// - T must be movable.
template <typename T>
class MPSCQueue {
public:
    MPSCQueue() {
        auto* sentinel = new Node{};
        head_.store(sentinel, std::memory_order_relaxed);
        tail_.store(sentinel, std::memory_order_relaxed);
    }

    ~MPSCQueue() {
        while (try_pop()) {}
        delete head_.load(std::memory_order_relaxed);
    }

    MPSCQueue(const MPSCQueue&) = delete;
    MPSCQueue& operator=(const MPSCQueue&) = delete;

    // Thread-safe: may be called from multiple producer threads.
    void push(T value) {
        auto* node = new Node{std::move(value)};
        Node* prev = tail_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    // Single consumer only.  Returns nullopt if the queue is empty.
    std::optional<T> try_pop() {
        Node* head = head_.load(std::memory_order_relaxed);
        Node* next = head->next.load(std::memory_order_acquire);
        if (!next) return std::nullopt;

        head_.store(next, std::memory_order_relaxed);
        T value = std::move(next->value);
        delete head;
        return value;
    }

    // Single consumer: drain all available items into the provided callback.
    // Returns the number of items consumed.
    template <typename Fn>
    size_t drain(Fn&& fn) {
        size_t count = 0;
        while (auto item = try_pop()) {
            fn(std::move(*item));
            ++count;
        }
        return count;
    }

    // Approximate: may be stale immediately after return.
    bool empty() const {
        Node* head = head_.load(std::memory_order_relaxed);
        return head->next.load(std::memory_order_relaxed) == nullptr;
    }

private:
    struct Node {
        T value{};
        std::atomic<Node*> next{nullptr};
    };

    alignas(64) std::atomic<Node*> head_;
    alignas(64) std::atomic<Node*> tail_;
};

}  // namespace sat_parallel
