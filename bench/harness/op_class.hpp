#pragma once

// Operation classes: every measured operation is recorded under exactly one class, and classes are
// never mixed into one number.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace eigenbook::bench {

enum class OpClass : std::uint8_t {
    ADD_PASSIVE,           // add that does not trade
    ADD_AGGRESSIVE_1,      // add that trades with exactly 1 maker
    ADD_AGGRESSIVE_2_4,    // 2-4 fills
    ADD_AGGRESSIVE_5_PLUS, // 5 or more fills
    CANCEL_HIT,            // cancel of a resting order, queue position not controlled
    CANCEL_HIT_FRONT,      // ... at the front of its level
    CANCEL_HIT_MIDDLE,     // ... in the middle of its level
    CANCEL_HIT_BACK,       // ... at the back of its level
    CANCEL_MISS,           // cancel of an id that is not resting (UNKNOWN_ORDER_ID)
    MODIFY_IN_PLACE,       // same price, quantity not larger: keeps priority
    MODIFY_REENTER,        // price change or increase: re-enters (may trade)
    COUNT
};

inline constexpr std::size_t kOpClassCount = static_cast<std::size_t>(OpClass::COUNT);

[[nodiscard]] constexpr std::string_view toString(const OpClass c) noexcept {
    constexpr std::array<std::string_view, kOpClassCount> names{"add-passive",
                                                                "add-aggr-1",
                                                                "add-aggr-2..4",
                                                                "add-aggr-5+",
                                                                "cancel-hit",
                                                                "cancel-hit-front",
                                                                "cancel-hit-middle",
                                                                "cancel-hit-back",
                                                                "cancel-miss",
                                                                "modify-in-place",
                                                                "modify-reenter"};
    const auto i = static_cast<std::size_t>(c);
    return i < kOpClassCount ? names[i] : std::string_view{"?"};
}

[[nodiscard]] constexpr OpClass addClassFor(const std::uint32_t fills) noexcept {
    if (fills == 0) {
        return OpClass::ADD_PASSIVE;
    }
    if (fills == 1) {
        return OpClass::ADD_AGGRESSIVE_1;
    }
    return fills <= 4 ? OpClass::ADD_AGGRESSIVE_2_4 : OpClass::ADD_AGGRESSIVE_5_PLUS;
}

} // namespace eigenbook::bench
