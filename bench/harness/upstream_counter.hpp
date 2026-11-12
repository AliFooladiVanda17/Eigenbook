#pragma once

// Counts allocations that reach the std::pmr default resource (single-threaded benchmark use).
//
// Installed as the default resource by eigenbook_latency, so the map ladder's pool resource takes
// it as upstream: every chunk the pool fetches during a measured run is counted and reported next
// to the latencies. It forwards to new_delete_resource(); the only cost is one extra virtual call
// on the (rare) upstream path.

#include <cstddef>
#include <cstdint>
#include <memory_resource>

namespace eigenbook::bench {

class UpstreamCounter final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::uint64_t allocations() const noexcept { return count; }

private:
    void* do_allocate(const std::size_t bytes, const std::size_t alignment) override {
        ++count;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, const std::size_t bytes, const std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::uint64_t count{0};
};

} // namespace eigenbook::bench
