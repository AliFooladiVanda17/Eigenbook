#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace eigenbook {

// Two-level occupancy bitset for a dense price ladder.
//
// Level 0 has one bit per tick; level 1 has one bit per non-zero level-0 word. Neighbouring set
// bits are found with a single count-zero instruction, and the summary keeps jumps over long
// empty stretches to one bit scan per 4096 ticks.
class LevelBitmap {
public:
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    explicit LevelBitmap(const std::size_t iBits)
        : bits(iBits), words((iBits + 63) / 64, 0), summary((words.size() + 63) / 64, 0) {}

    [[nodiscard]] std::size_t size() const noexcept { return bits; }

    [[nodiscard]] bool test(const std::size_t i) const noexcept {
        assert(i < bits);
        return (words[i >> 6] & bit(i)) != 0;
    }

    void set(const std::size_t i) noexcept {
        assert(i < bits);
        words[i >> 6] |= bit(i);
        summary[i >> 12] |= bit(i >> 6);
    }

    void reset(const std::size_t i) noexcept {
        assert(i < bits);
        std::uint64_t& word = words[i >> 6];
        word &= ~bit(i);
        if (word == 0) {
            summary[i >> 12] &= ~bit(i >> 6);
        }
    }

    void clear() noexcept {
        std::fill(words.begin(), words.end(), 0);
        std::fill(summary.begin(), summary.end(), 0);
    }

    // Smallest set index >= from, or npos.
    [[nodiscard]] std::size_t findNext(const std::size_t from) const noexcept {
        if (from >= bits) {
            return npos;
        }
        std::size_t w = from >> 6;
        if (const std::uint64_t word = words[w] & (~std::uint64_t{0} << (from & 63))) {
            return (w << 6) | static_cast<std::size_t>(std::countr_zero(word));
        }
        if (++w >= words.size()) {
            return npos;
        }
        std::size_t s = w >> 6;
        std::uint64_t sword = summary[s] & (~std::uint64_t{0} << (w & 63));
        while (sword == 0) {
            if (++s >= summary.size()) {
                return npos;
            }
            sword = summary[s];
        }
        w = (s << 6) | static_cast<std::size_t>(std::countr_zero(sword));
        return (w << 6) | static_cast<std::size_t>(std::countr_zero(words[w]));
    }

    // Largest set index <= from (clamped to the last bit), or npos.
    [[nodiscard]] std::size_t findPrev(std::size_t from) const noexcept {
        if (bits == 0) {
            return npos;
        }
        if (from >= bits) {
            from = bits - 1;
        }
        std::size_t w = from >> 6;
        if (const std::uint64_t word = words[w] & (~std::uint64_t{0} >> (63 - (from & 63)))) {
            return (w << 6) | highest(word);
        }
        if (w-- == 0) {
            return npos;
        }
        std::size_t s = w >> 6;
        std::uint64_t sword = summary[s] & (~std::uint64_t{0} >> (63 - (w & 63)));
        while (sword == 0) {
            if (s-- == 0) {
                return npos;
            }
            sword = summary[s];
        }
        w = (s << 6) | highest(sword);
        return (w << 6) | highest(words[w]);
    }

    [[nodiscard]] std::size_t findFirst() const noexcept { return findNext(0); }

    [[nodiscard]] std::size_t findLast() const noexcept { return findPrev(npos); }

private:
    [[nodiscard]] static constexpr std::uint64_t bit(const std::size_t i) noexcept {
        return std::uint64_t{1} << (i & 63);
    }

    [[nodiscard]] static constexpr std::size_t highest(const std::uint64_t word) noexcept {
        return 63 - static_cast<std::size_t>(std::countl_zero(word));
    }

    std::size_t bits;
    std::vector<std::uint64_t> words;
    std::vector<std::uint64_t> summary;
};

} // namespace eigenbook
