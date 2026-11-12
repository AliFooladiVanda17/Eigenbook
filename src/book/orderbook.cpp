#include "eigenbook/book/order_book.hpp"

// Explicit instantiations of the two supported books (see the extern declarations in the header).
template class eigenbook::BasicOrderBook<eigenbook::MapPriceLadder>;
template class eigenbook::BasicOrderBook<eigenbook::ArrayPriceLadder>;
