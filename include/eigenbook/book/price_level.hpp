#pragma once

#include <cassert>
#include <cstdint>
#include <eigenbook/core/order.hpp>
#include <eigenbook/core/types.hpp>

namespace eigenbook {

// All resting orders at one price, in arrival order (intrusive doubly linked list).
//
// Push, pop, erase, fill and reduce are O(1) and never allocate: the nodes belong to the book's
// pool. The level only keeps the links and the aggregate quantity consistent.
class PriceLevel {
public:
    PriceLevel() = default;

    explicit PriceLevel(const Price iPrice) noexcept : levelPrice(iPrice) {}

    // Orders point back at their level, so a level never changes address.
    PriceLevel(const PriceLevel&) = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;
    PriceLevel(PriceLevel&&) = delete;
    PriceLevel& operator=(PriceLevel&&) = delete;

    ~PriceLevel() = default;

    [[nodiscard]] Price price() const noexcept { return levelPrice; }

    // Sum of the remaining quantity of every order at this level.
    [[nodiscard]] Quantity quantity() const noexcept { return totalQuantity; }

    [[nodiscard]] std::uint32_t orderCount() const noexcept { return count; }

    [[nodiscard]] bool empty() const noexcept { return head == nullptr; }

    // Order with time priority, or nullptr when empty.
    [[nodiscard]] Order* front() noexcept { return head; }

    [[nodiscard]] const Order* front() const noexcept { return head; }

    [[nodiscard]] const Order* back() const noexcept { return tail; }

    // Level an order currently rests at, or nullptr.
    [[nodiscard]] static PriceLevel* ownerOf(Order& order) noexcept { return order.level; }

    void pushBack(Order& order) noexcept {
        assert(order.level == nullptr && order.price == levelPrice);
        order.level = this;
        order.prev = tail;
        order.next = nullptr;
        if (tail != nullptr) {
            tail->next = &order;
        }
        else {
            head = &order;
        }
        tail = &order;
        totalQuantity += order.quantity;
        ++count;
    }

    // Unlinks any order of this level.
    void erase(Order& order) noexcept {
        assert(order.level == this);
        if (order.prev != nullptr) {
            order.prev->next = order.next;
        }
        else {
            head = order.next;
        }
        if (order.next != nullptr) {
            order.next->prev = order.prev;
        }
        else {
            tail = order.prev;
        }
        totalQuantity -= order.quantity;
        --count;
        order.prev = order.next = nullptr;
        order.level = nullptr;
    }

    void popFront() noexcept {
        assert(head != nullptr);
        erase(*head);
    }

    // Partial fill of the front order. A fill of the whole quantity must use popFront().
    void fillFront(const Quantity traded) noexcept {
        assert(head != nullptr && traded < head->quantity);
        head->quantity -= traded;
        totalQuantity -= traded;
    }

    // Lowers an order's quantity in place; the order keeps its time priority.
    void reduce(Order& order, const Quantity newQuantity) noexcept {
        assert(order.level == this && newQuantity <= order.quantity && !newQuantity.isZero());
        totalQuantity -= order.quantity - newQuantity;
        order.quantity = newQuantity;
    }

    // Visits orders in time priority: cb(const Order&).
    template <typename Callback>
    void forEachOrder(Callback&& cb) const {
        for (const Order* it = head; it != nullptr; it = it->next) {
            cb(*it);
        }
    }

private:
    // The dense ladder allocates its levels as one array and assigns their prices afterwards.
    template <OrderSide>
    friend class ArrayPriceLadder;

    Price levelPrice{0};
    Quantity totalQuantity{0};
    Order* head{nullptr};
    Order* tail{nullptr};
    std::uint32_t count{0};
};

} // namespace eigenbook
