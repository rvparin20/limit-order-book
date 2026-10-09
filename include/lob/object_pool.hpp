#pragma once

#include <cstddef>
#include <vector>

namespace lob {

// Fixed-capacity pool: every object is allocated once, up front.
// acquire()/release() are O(1) and never touch the heap, so the hot path
// has no malloc/free and no allocator-induced latency spikes.
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t capacity) : storage_(capacity) {
        free_.reserve(capacity);
        // Push in reverse so the first acquire() returns storage_[0]:
        // consecutive orders then sit next to each other in memory.
        for (std::size_t i = capacity; i-- > 0;) free_.push_back(&storage_[i]);
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    T* acquire() {
        if (free_.empty()) return nullptr;
        T* p = free_.back();
        free_.pop_back();
        return p;
    }

    void release(T* p) { free_.push_back(p); }

    std::size_t capacity() const { return storage_.size(); }
    std::size_t in_use() const { return storage_.size() - free_.size(); }

private:
    std::vector<T> storage_;
    std::vector<T*> free_;  // LIFO free list: recently freed (cache-warm) slots are reused first
};

}  // namespace lob
