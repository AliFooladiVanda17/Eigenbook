//
// Created by afouladi on 5/12/2026.
//

#ifndef HPC_QUEUE_SRC_BOOK_PRICELEVEL_H
#define HPC_QUEUE_SRC_BOOK_PRICELEVEL_H

#include <deque>
#include <tradingengine/order.h>

class PriceLevel {
public:
  using OrderList = std::deque<Order>;

  void addOrder(const Order& order);
  void addOrder(Order&& order);

  void removeFrontOrder();

  Order& frontOrder();
  const Order& frontOrder() const;

  bool empty() const noexcept;

  std::size_t orderCount() const noexcept;

  unsigned long quantity() const noexcept;

private:
  OrderList orders;
  unsigned long totalQuantity {0};
};

#endif // HPC_QUEUE_SRC_BOOK_PRICELEVEL_H
