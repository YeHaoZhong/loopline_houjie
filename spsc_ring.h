#ifndef SPSC_RING_H
#define SPSC_RING_H

#include <atomic>
#include <cstdint>
#include <cassert>
#include <algorithm>

template<typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacity) {
        cap = nextPow2(std::max<size_t>(2, capacity));
        mask = cap - 1;
        buffer = new T[cap];
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
    }
    ~SpscRing() {
        delete[] buffer;
    }

    // 非阻塞 push：成功返回 true，满则返回 false （调用方可统计丢弃）
    bool try_push(const T& item) {
        uint64_t t = tail.load(std::memory_order_relaxed);
        uint64_t h = head.load(std::memory_order_acquire);
        if ((t - h) >= cap) return false; // full
        buffer[t & mask] = item;
        tail.store(t + 1, std::memory_order_release);
        return true;
    }

    // pop up to max_items, 返回实际 pop 的数量
    size_t pop_bulk(T* out, size_t max_items) {
        uint64_t h = head.load(std::memory_order_relaxed);
        uint64_t t = tail.load(std::memory_order_acquire);
        uint64_t avail = t - h;
        size_t to_pop = static_cast<size_t>(std::min<uint64_t>(avail, max_items));
        for (size_t i = 0; i < to_pop; ++i) {
            out[i] = buffer[(h + i) & mask];
        }
        if (to_pop) head.store(h + to_pop, std::memory_order_release);
        return to_pop;
    }

    // 方便监控：当前可用数量
    uint64_t size_approx() const {
        uint64_t h = head.load(std::memory_order_acquire);
        uint64_t t = tail.load(std::memory_order_acquire);
        return t - h;
    }

private:
    static size_t nextPow2(size_t v) { size_t p=1; while(p<v) p <<= 1; return p; }
    T* buffer;
    size_t cap;
    size_t mask;
    std::atomic<uint64_t> head;
    std::atomic<uint64_t> tail;
};


#endif // SPSC_RING_H
