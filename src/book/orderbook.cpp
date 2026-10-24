//
// Created by afouladi on 5/12/2026.
//

#include "tradingengine/orderbook.h"

// Explicit instantiations of the two supported books (see the extern declarations in the header).
template class BasicOrderBook<MapPriceLadder>;
template class BasicOrderBook<ArrayPriceLadder>;
