// Independent reproduction: no Boost helpers, only public Liquibook APIs.
#include <simple/simple_order_book.h>
#include <iostream>
#include <string>
using namespace liquibook;

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "accept";
  const bool buy = argc < 3 || std::string(argv[2]) == "buy";
  simple::SimpleOrder order(buy, 0, 100, buy ? 56 : 54);
  simple::SimpleOrder opposite(!buy, buy ? 57 : 53, 100);
  simple::SimpleOrderBook<> book;
  book.set_market_price(55);
  bool matched = book.add(&order);
  std::cout << "input: market=55, side=" << (buy ? "buy" : "sell")
            << ", quantity=100, limit=market, stop=" << order.stop_price()
            << "\naccepted: matched=" << matched << ", state=" << order.state()
            << ", stopped=" << book.stopBids().size() + book.stopAsks().size()
            << ", filled=" << order.filled_qty() << '\n';
  const bool accepted = !matched && order.state() == simple::os_accepted &&
      book.stopBids().size() + book.stopAsks().size() == 1 && order.filled_qty() == 0;
  if (mode == "accept") return accepted ? 0 : 1;
  if (mode == "cancel") {
    book.cancel(&order);
    std::cout << "cancelled: state=" << order.state()
              << ", stopped=" << book.stopBids().size() + book.stopAsks().size()
              << ", filled=" << order.filled_qty() << '\n';
    return order.state() == simple::os_cancelled &&
        book.stopBids().empty() && book.stopAsks().empty() &&
        order.filled_qty() == 0 ? 0 : 1;
  }
  if (mode == "trigger") {
    book.add(&opposite);
    book.set_market_price(buy ? 56 : 54);
    // An add processes pending stop orders after set_market_price.
    simple::SimpleOrder unrelated(buy, buy ? 1 : 100, 1);
    book.add(&unrelated);
    std::cout << "triggered: state=" << order.state()
              << ", filled=" << order.filled_qty() << '\n';
    return accepted && order.state() == simple::os_complete &&
        order.filled_qty() == 100 ? 0 : 1;
  }
  return 2;
}
