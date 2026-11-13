#pragma once

// Single-size block memory resource (map-ladder policy M2, and the hybrid ladder's overflow).
//
// One array of `capacity` equal blocks is allocated and threaded onto an intrusive free list at
// construction, which also touches (pre-faults) every page. After that, allocate() and
// deallocate() of a block are a free-list pop / push: O(1), no system call, no global allocator.
//
// Requests that do not fit a block (bigger, or more strictly aligned) go to the fallback resource
// and are counted; a std::pmr::map only makes such requests while it is being constructed (MSVC
// debug builds allocate a container proxy). If the free list is empty, allocate() throws
// std::bad_alloc, as the memory_resource contract requires; the ladders check their capacity
// before inserting, so that path is a bug, not a load condition.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>

namespace eigenbook {

class FixedBlockResource final : public std::pmr::memory_resource {
public:
    FixedBlockResource(const std::size_t iBlockBytes,
                       const std::size_t iCapacity,
                       std::pmr::memory_resource* iFallback = std::pmr::get_default_resource())
        : blockBytes(roundUp(std::max(iBlockBytes, sizeof(Block)), kAlignment))
        , blockCapacity(iCapacity)
        , storage(static_cast<std::byte*>(
              ::operator new(blockBytes * iCapacity, std::align_val_t{kAlignment})))
        , fallback(iFallback) {
        // Thread back to front so consecutive allocations walk memory forward; this also writes
        // every block once, so all pages are resident before the first allocation.
        for (std::size_t i = blockCapacity; i-- > 0;) {
            auto* block = reinterpret_cast<Block*>(storage + i * blockBytes);
            block->next = freeList;
            freeList = block;
        }
        freeCount = blockCapacity;
    }

    FixedBlockResource(const FixedBlockResource&) = delete;
    FixedBlockResource& operator=(const FixedBlockResource&) = delete;
    FixedBlockResource(FixedBlockResource&&) = delete;
    FixedBlockResource& operator=(FixedBlockResource&&) = delete;

    ~FixedBlockResource() override { ::operator delete(storage, std::align_val_t{kAlignment}); }

    [[nodiscard]] std::size_t blockSize() const noexcept { return blockBytes; }

    [[nodiscard]] std::size_t capacity() const noexcept { return blockCapacity; }

    [[nodiscard]] std::size_t available() const noexcept { return freeCount; }

    // Requests that did not fit a block and went to the fallback resource.
    [[nodiscard]] std::uint64_t fallbackAllocations() const noexcept { return fallbackCount; }

    [[nodiscard]] bool owns(const void* p) const noexcept {
        const auto* b = static_cast<const std::byte*>(p);
        return b >= storage && b < storage + blockBytes * blockCapacity;
    }

private:
    static constexpr std::size_t kAlignment = alignof(std::max_align_t);

    struct Block {
        Block* next;
    };

    [[nodiscard]] static constexpr std::size_t roundUp(const std::size_t n,
                                                       const std::size_t a) noexcept {
        return (n + a - 1) / a * a;
    }

    void* do_allocate(const std::size_t bytes, const std::size_t alignment) override {
        if (bytes > blockBytes || alignment > kAlignment) [[unlikely]] {
            ++fallbackCount;
            return fallback->allocate(bytes, alignment);
        }
        if (freeList == nullptr) [[unlikely]] {
            throw std::bad_alloc();
        }
        Block* block = freeList;
        freeList = block->next;
        --freeCount;
        return block;
    }

    void do_deallocate(void* p, const std::size_t bytes, const std::size_t alignment) override {
        if (!owns(p)) [[unlikely]] {
            fallback->deallocate(p, bytes, alignment);
            return;
        }
        auto* block = static_cast<Block*>(p);
        block->next = freeList;
        freeList = block;
        ++freeCount;
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t blockBytes;
    std::size_t blockCapacity;
    std::byte* storage;
    std::pmr::memory_resource* fallback;
    Block* freeList{nullptr};
    std::size_t freeCount{0};
    std::uint64_t fallbackCount{0};
};

} // namespace eigenbook
