// Independent check of the documented TradeListener price contract.
#include <simple/simple_order.h>
#include <book/order_book.h>
#include <iostream>
using namespace liquibook;
using Book = book::OrderBook<simple::SimpleOrder*>;
struct Listener : book::TradeListener<Book> {
  book::Quantity qty = 0;
  book::Price price = 0;
  void on_trade(const Book*, book::Quantity q, book::Price p) override {
    qty = q; price = p;
  }
};
int main() {
  simple::SimpleOrder sell(false, 3250, 100), buy(true, 3250, 800);
  Listener listener;
  Book book;
  book.set_trade_listener(&listener);
  book.add(&sell);
  book.add(&buy);
  std::cout << "input: sell 100 at 3250; buy 800 at 3250\n"
            << "callback quantity=" << listener.qty << ", price=" << listener.price << '\n';
  return listener.qty == 100 && listener.price == 3250 ? 0 : 1;
}
