//
// Created by afouladi on 5/12/2026.
//

#include "eigenbook/book/order_book.hpp"

// Explicit instantiations of the two supported books (see the extern declarations in the header).
template class BasicOrderBook<MapPriceLadder>;
template class BasicOrderBook<ArrayPriceLadder>;
