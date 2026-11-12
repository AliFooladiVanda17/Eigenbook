//
// Created by afouladi on 5/12/2026.
//

#ifndef HPC_QUEUE_INCLUDE_TRADINGENGINE_UTIL_OBJECTPOOL_H
#define HPC_QUEUE_INCLUDE_TRADINGENGINE_UTIL_OBJECTPOOL_H

#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

// Single-threaded fixed-size object pool.
//
// Slots are carved out of large chunks and recycled through an intrusive free list, so create()
// and destroy() are O(1) and never touch the global allocator once the pool is warm (reserve() up
// front to keep the hot path allocation-free). Addresses are stable for the lifetime of the pool.
template <typename T>
class ObjectPool {
    static_assert(std::is_trivially_destructible_v<T>,
                  "objects still alive when the pool dies are released without a destructor call");

public:
    explicit ObjectPool(const std::size_t iChunkSize = 4096) : chunkSize(iChunkSize) {
        assert(chunkSize > 0);
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    ~ObjectPool() = default;

    // Ensures at least n objects can be created without allocating.
    void reserve(const std::size_t n) {
        while (capacity() < n) {
            addChunk();
        }
    }

    template <typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        if (freeList == nullptr) [[unlikely]] {
            addChunk();
        }
        Slot* slot = freeList;
        freeList = slot->next;
        ++live;
        return ::new (static_cast<void*>(slot->storage)) T(std::forward<Args>(args)...);
    }

    void destroy(T* object) noexcept {
        assert(object != nullptr && live > 0);
        object->~T();
        auto* slot = reinterpret_cast<Slot*>(object);
        slot->next = freeList;
        freeList = slot;
        --live;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return chunks.size() * chunkSize; }

    [[nodiscard]] std::size_t size() const noexcept { return live; }

private:
    union Slot {
        Slot* next;
        alignas(T) std::byte storage[sizeof(T)];
    };

    void addChunk() {
        auto chunk = std::make_unique_for_overwrite<Slot[]>(chunkSize);
        // Thread back-to-front so consecutive creates walk memory forward.
        for (std::size_t i = chunkSize; i-- > 0;) {
            chunk[i].next = freeList;
            freeList = &chunk[i];
        }
        chunks.push_back(std::move(chunk));
    }

    std::vector<std::unique_ptr<Slot[]>> chunks;
    Slot* freeList{nullptr};
    std::size_t chunkSize;
    std::size_t live{0};
};

#endif // HPC_QUEUE_INCLUDE_TRADINGENGINE_UTIL_OBJECTPOOL_H
