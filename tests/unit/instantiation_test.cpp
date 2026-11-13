//
// Explicit instantiation of the supported books and their building blocks.
//
// The library is header-only, so a member that no test calls would otherwise never be compiled.
// This translation unit makes every non-template member of these classes compile on every
// compiler in CI. It contains no test cases.
//

#include <eigenbook/book/ladder/dense_ladder.hpp>
#include <eigenbook/book/ladder/hybrid_ladder.hpp>
#include <eigenbook/book/ladder/map_ladder.hpp>
#include <eigenbook/book/order_book.hpp>
#include <eigenbook/core/order.hpp>
#include <eigenbook/util/object_pool.hpp>

template class eigenbook::BasicOrderBook<eigenbook::MapPriceLadder>;
template class eigenbook::BasicOrderBook<eigenbook::MapPriceLadderM1>;
template class eigenbook::BasicOrderBook<eigenbook::MapPriceLadderM2>;
template class eigenbook::BasicOrderBook<eigenbook::ArrayPriceLadder>;
template class eigenbook::BasicOrderBook<eigenbook::HybridPriceLadder>;

template class eigenbook::BasicMapPriceLadder<eigenbook::OrderSide::BUY, eigenbook::MapNodesOnHeap>;
template class eigenbook::BasicMapPriceLadder<eigenbook::OrderSide::SELL, eigenbook::MapNodesOnHeap>;
template class eigenbook::BasicMapPriceLadder<eigenbook::OrderSide::BUY,
                                              eigenbook::MapNodesOnMonotonicBuffer>;
template class eigenbook::BasicMapPriceLadder<eigenbook::OrderSide::SELL,
                                              eigenbook::MapNodesOnMonotonicBuffer>;
template class eigenbook::BasicMapPriceLadder<eigenbook::OrderSide::BUY,
                                              eigenbook::MapNodesOnFixedBlocks>;
template class eigenbook::BasicMapPriceLadder<eigenbook::OrderSide::SELL,
                                              eigenbook::MapNodesOnFixedBlocks>;
template class eigenbook::ArrayPriceLadder<eigenbook::OrderSide::BUY>;
template class eigenbook::ArrayPriceLadder<eigenbook::OrderSide::SELL>;
template class eigenbook::HybridPriceLadder<eigenbook::OrderSide::BUY>;
template class eigenbook::HybridPriceLadder<eigenbook::OrderSide::SELL>;

template class eigenbook::ObjectPool<eigenbook::Order>;
