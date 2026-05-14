//
// Created by afouladi on 5/12/2026.
//

#include "tradingengine/order.h"

Quantity Order::subtract(Quantity decQuantity)
{
    if (decQuantity >= quantity) {
        quantity = Quantity{0};
        return Quantity{0};
    }

    quantity -= decQuantity;
    return quantity;
}
