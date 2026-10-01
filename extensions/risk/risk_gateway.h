#pragma once

#include <book/order.h>
#include <book/order_book.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace exchange {
using Quantity = liquibook::book::Quantity;
using Money = uint64_t;
using Position = int64_t;
using AccountId = uint64_t;
using OrderId = uint64_t;
using Timestamp = uint64_t; // Monotonic nanoseconds, injected at the boundary.

inline Timestamp steady_now() {
  return static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct RiskLimits {
  Quantity max_order_qty = 1000;
  Money max_order_notional = 1000000;
  Position max_long_position = 10000;
  Position max_short_position = 10000; // Positive magnitude.
  int64_t min_price = 1;
  int64_t max_price = 1000000;
  uint32_t max_orders_per_second = 1000;
};

struct OrderRequest {
  OrderId id;
  AccountId account;
  bool buy;
  int64_t quantity;
  int64_t price;
};

enum class RejectReason {
  None, DuplicateOrderId, InvalidQuantity, InvalidPrice, OrderQuantityLimit,
  OrderNotionalLimit, PositionLimit, RateLimit, UnknownAccount,
  UnknownOrder, OrderNotOpen, InvalidTimestamp
};
struct RiskResult {
  RejectReason reason;
  explicit operator bool() const { return reason == RejectReason::None; }
};

struct AccountState {
  Position position = 0;
  Quantity reserved_buy = 0;
  Quantity reserved_sell = 0;
  Timestamp last_accepted_time = 0;
  std::deque<Timestamp> admissions;
};

// No access to Liquibook: validation is read-only; reservation is explicit.
// One instance represents one instrument and one serial event stream.
class PreTradeRiskEngine {
  friend class RiskGateway;
  struct Account {
    RiskLimits limits;
    AccountState state;
  };
  std::unordered_map<AccountId, Account> accounts_;
  std::unordered_set<OrderId> accepted_ids_;
  static constexpr Timestamp window = 1000000000ULL;

  RiskResult check(const OrderRequest& order, Timestamp now,
                   Quantity old_remaining, Quantity new_remaining) const {
    auto it = accounts_.find(order.account);
    if (it == accounts_.end()) return {RejectReason::UnknownAccount};
    const auto& limits = it->second.limits;
    const auto& state = it->second.state;
    if (order.quantity <= 0) return {RejectReason::InvalidQuantity};
    if (static_cast<Quantity>(order.quantity) > limits.max_order_qty)
      return {RejectReason::OrderQuantityLimit};
    if (order.price <= 0 || order.price < limits.min_price || order.price > limits.max_price)
      return {RejectReason::InvalidPrice};
    // Division avoids overflow of price * quantity.
    if (static_cast<Money>(order.price) > limits.max_order_notional /
        static_cast<Quantity>(order.quantity)) return {RejectReason::OrderNotionalLimit};
    // Widen before arithmetic. Never net opposite pending orders.
    const __int128 buys = static_cast<__int128>(state.reserved_buy) +
        (order.buy ? static_cast<__int128>(new_remaining) - old_remaining : 0);
    const __int128 sells = static_cast<__int128>(state.reserved_sell) +
        (!order.buy ? static_cast<__int128>(new_remaining) - old_remaining : 0);
    if (static_cast<__int128>(state.position) + buys > limits.max_long_position ||
        static_cast<__int128>(state.position) - sells < -static_cast<__int128>(limits.max_short_position))
      return {RejectReason::PositionLimit};
    if (now < state.last_accepted_time) return {RejectReason::InvalidTimestamp};
    auto first = state.admissions.begin();
    if (now >= window)
      first = std::upper_bound(first, state.admissions.end(), now - window);
    if (static_cast<size_t>(state.admissions.end() - first) >= limits.max_orders_per_second)
      return {RejectReason::RateLimit};
    return {RejectReason::None};
  }

  void admit(AccountState& state, Timestamp now) {
    // Allocate before mutating reservations.
    state.admissions.push_back(now);
    while (!state.admissions.empty() && now - state.admissions.front() >= window)
      state.admissions.pop_front();
    state.last_accepted_time = now;
  }

public:
  void add_account(AccountId id, const RiskLimits& limits, Position position = 0) {
    // Upstream replace narrows open quantity to int. Bound the supported domain.
    if (!limits.max_order_qty || limits.max_order_qty > static_cast<Quantity>(INT32_MAX) ||
        limits.max_long_position < 0 || limits.max_short_position < 0 ||
        limits.min_price <= 0 || limits.max_price < limits.min_price ||
        !limits.max_order_notional || !limits.max_orders_per_second ||
        position > limits.max_long_position || position < -limits.max_short_position)
      throw std::invalid_argument("invalid risk limits or initial position");
    Account account;
    account.limits = limits;
    account.state.position = position;
    if (!accounts_.emplace(id, account).second)
      throw std::invalid_argument("duplicate account");
  }
  const AccountState& account(AccountId id) const { return accounts_.at(id).state; }
  RiskResult validate(const OrderRequest& order, Timestamp now) const {
    if (accepted_ids_.count(order.id)) return {RejectReason::DuplicateOrderId};
    return check(order, now, 0, order.quantity > 0 ? static_cast<Quantity>(order.quantity) : 0);
  }
private:
  RiskResult validate_replace(const OrderRequest& order, Quantity old_remaining,
                              Quantity new_remaining, Timestamp now) const {
    return check(order, now, old_remaining, new_remaining);
  }
  void reserve(const OrderRequest& order, Timestamp now) {
    auto& state = accounts_.at(order.account).state;
    // Roll back the ID if deque allocation fails. No book call has occurred yet.
    accepted_ids_.insert(order.id);
    try { admit(state, now); }
    catch (...) { accepted_ids_.erase(order.id); throw; }
    (order.buy ? state.reserved_buy : state.reserved_sell) += order.quantity;
  }
  void reserve_replace(AccountId id, bool buy, Quantity old_qty, Quantity new_qty, Timestamp now) {
    auto& state = accounts_.at(id).state;
    admit(state, now);
    auto& reserved = buy ? state.reserved_buy : state.reserved_sell;
    reserved = reserved - old_qty + new_qty;
  }
  void release(AccountId id, bool buy, Quantity quantity) {
    auto& state = accounts_.at(id).state;
    auto& reserved = buy ? state.reserved_buy : state.reserved_sell;
    if (quantity > reserved) throw std::logic_error("reservation underflow");
    reserved -= quantity;
  }
  void fill(AccountId id, bool buy, Quantity quantity) {
    auto& state = accounts_.at(id).state;
    release(id, buy, quantity);
    const __int128 next = static_cast<__int128>(state.position) +
        (buy ? static_cast<__int128>(quantity) : -static_cast<__int128>(quantity));
    if (next > INT64_MAX || next < INT64_MIN) throw std::logic_error("position overflow");
    state.position = static_cast<Position>(next);
  }
};

enum class OrderState { Open, Filled, Cancelled, Rejected };
struct OrderRecord : liquibook::book::Order {
  OrderId id;
  AccountId account;
  bool buy;
  Quantity total;
  Quantity remaining;
  Quantity filled = 0;
  liquibook::book::Price limit;
  OrderState state = OrderState::Open;
  explicit OrderRecord(const OrderRequest& r)
      : id(r.id), account(r.account), buy(r.buy), total(r.quantity),
        remaining(r.quantity), limit(r.price) {}
  bool is_buy() const override { return buy; }
  liquibook::book::Price price() const override { return limit; }
  Quantity order_qty() const override { return total; }
  void apply_fill(Quantity qty) {
    if (qty > remaining) throw std::logic_error("fill exceeds remaining quantity");
    remaining -= qty;
    filled += qty;
    if (!remaining) state = OrderState::Filled;
  }
};

class RiskGateway : private liquibook::book::OrderListener<OrderRecord*>,
                    private liquibook::book::Logger {
  using Book = liquibook::book::OrderBook<OrderRecord*>;
  PreTradeRiskEngine risk_;
  std::unordered_map<OrderId, std::unique_ptr<OrderRecord>> orders_;
  Book book_;
  uint64_t events_ = 0;
  bool engine_rejected_ = false;
  bool halted_ = false;
  // Liquibook catches listener exceptions. Its logger must latch those errors
  // so dispatch can halt instead of reporting a successful risk operation.
  void log_exception(const std::string&, const std::exception&) override {
    engine_rejected_ = true;
  }
  void log_message(const std::string&) override { engine_rejected_ = true; }
  void require_live() const {
    if (halted_) throw std::logic_error("gateway halted after engine exception");
  }
  template<class Action> void dispatch(Action action) {
    engine_rejected_ = false;
    try {
      action();
      if (engine_rejected_) throw std::logic_error("unexpected Liquibook rejection");
    } catch (...) {
      halted_ = true;
      throw;
    }
  }

  void on_accept(OrderRecord* const&) override { ++events_; }
  void on_reject(OrderRecord* const& order, const char*) override {
    risk_.release(order->account, order->buy, order->remaining);
    order->remaining = 0;
    order->state = OrderState::Rejected;
    engine_rejected_ = true;
    ++events_;
  }
  void on_fill(OrderRecord* const& order, OrderRecord* const& other,
               Quantity qty, liquibook::book::Price) override {
    risk_.fill(order->account, order->buy, qty);
    risk_.fill(other->account, other->buy, qty);
    order->apply_fill(qty);
    other->apply_fill(qty);
    ++events_;
  }
  void on_cancel(OrderRecord* const& order) override {
    risk_.release(order->account, order->buy, order->remaining);
    order->remaining = 0;
    order->state = OrderState::Cancelled;
    ++events_;
  }
  void on_cancel_reject(OrderRecord* const&, const char*) override {
    engine_rejected_ = true;
    ++events_;
  }
  void on_replace(OrderRecord* const& order, const int64_t& delta,
                  liquibook::book::Price price) override {
    order->total = static_cast<Quantity>(static_cast<int64_t>(order->total) + delta);
    order->remaining = order->total - order->filled;
    order->limit = price;
    ++events_;
  }
  void on_replace_reject(OrderRecord* const&, const char*) override {
    engine_rejected_ = true;
    ++events_;
  }

public:
  RiskGateway() { book_.set_order_listener(this); book_.set_logger(this); }
  RiskGateway(const RiskGateway&) = delete;
  RiskGateway& operator=(const RiskGateway&) = delete;
  void add_account(AccountId id, const RiskLimits& limits, Position position = 0) {
    risk_.add_account(id, limits, position);
  }
  const AccountState& account(AccountId id) const { return risk_.account(id); }
  const OrderRecord& order(OrderId id) const { return *orders_.at(id); }
  RiskResult validate(const OrderRequest& order, Timestamp now) const {
    return risk_.validate(order, now);
  }
  RiskResult submit(const OrderRequest& request, Timestamp now) {
    require_live();
    auto result = risk_.validate(request, now);
    if (!result) return result;
    std::unique_ptr<OrderRecord> stored(new OrderRecord(request));
    auto* ptr = stored.get();
    orders_.emplace(request.id, std::move(stored));
    try { risk_.reserve(request, now); }
    catch (...) { orders_.erase(request.id); throw; }
    dispatch([&] { book_.add(ptr); });
    return {RejectReason::None};
  }
  RiskResult cancel(AccountId account, OrderId id) {
    require_live();
    auto it = orders_.find(id);
    if (it == orders_.end() || it->second->account != account) return {RejectReason::UnknownOrder};
    if (it->second->state != OrderState::Open) return {RejectReason::OrderNotOpen};
    dispatch([&] { book_.cancel(it->second.get()); });
    return {RejectReason::None};
  }
  // quantity is the new TOTAL order quantity, including previous fills.
  // Use cancel for full removal; replacement must retain positive open quantity.
  RiskResult replace(AccountId account, OrderId id, int64_t quantity, int64_t price, Timestamp now) {
    require_live();
    auto it = orders_.find(id);
    if (it == orders_.end() || it->second->account != account) return {RejectReason::UnknownOrder};
    auto& old = *it->second;
    if (old.state != OrderState::Open) return {RejectReason::OrderNotOpen};
    if (quantity <= 0 || static_cast<Quantity>(quantity) <= old.filled)
      return {RejectReason::InvalidQuantity};
    const Quantity remaining = static_cast<Quantity>(quantity) - old.filled;
    OrderRequest request{id, account, old.buy, quantity, price};
    auto result = risk_.validate_replace(request, old.remaining, remaining, now);
    if (!result) return result;
    const auto old_remaining = old.remaining;
    risk_.reserve_replace(account, old.buy, old_remaining, remaining, now);
    dispatch([&] {
      book_.replace(&old, quantity - static_cast<int64_t>(old.total), static_cast<Quantity>(price));
    });
    return {RejectReason::None};
  }
  // Read-only value snapshot: callers cannot mutate live book pointers.
  std::string book_snapshot() const {
    std::ostringstream out;
    out << book_.market_price() << ':' << events_ << '|';
    for (const auto& row : book_.bids())
      out << 'B' << row.second.ptr()->id << ':' << row.first.price()
          << ':' << row.second.open_qty() << ';';
    for (const auto& row : book_.asks())
      out << 'S' << row.second.ptr()->id << ':' << row.first.price()
          << ':' << row.second.open_qty() << ';';
    return out.str();
  }
};
} // namespace exchange
