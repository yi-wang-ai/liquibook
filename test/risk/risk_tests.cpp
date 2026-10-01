#define BOOST_TEST_MODULE PreTradeRiskTests
#include <boost/test/unit_test.hpp>
#include "../../extensions/risk/risk_gateway.h"
using namespace exchange;

struct Fixture {
  RiskGateway gateway;
  RiskLimits limits;
  Fixture() {
    limits.max_order_qty = 1000;
    limits.max_order_notional = 100000;
    limits.max_long_position = 1000;
    limits.max_short_position = 1000;
    limits.min_price = 10;
    limits.max_price = 200;
    limits.max_orders_per_second = 100;
    gateway.add_account(1, limits);
    gateway.add_account(2, limits);
  }
  void submit(OrderId id, AccountId account, bool buy, int64_t qty, int64_t price = 100,
              Timestamp time = 0) {
    BOOST_REQUIRE(static_cast<bool>(gateway.submit({id, account, buy, qty, price}, time)));
  }
};
static void same_account(const AccountState& before, const AccountState& after) {
  BOOST_CHECK_EQUAL(before.position, after.position);
  BOOST_CHECK_EQUAL(before.reserved_buy, after.reserved_buy);
  BOOST_CHECK_EQUAL(before.reserved_sell, after.reserved_sell);
  BOOST_CHECK_EQUAL(before.last_accepted_time, after.last_accepted_time);
  BOOST_CHECK(before.admissions == after.admissions);
}
static void rejected(Fixture& f, OrderRequest request, RejectReason reason, Timestamp now = 0) {
  const auto book = f.gateway.book_snapshot();
  const auto a = f.gateway.account(1), b = f.gateway.account(2);
  BOOST_CHECK(f.gateway.submit(request, now).reason == reason);
  BOOST_CHECK_EQUAL(book, f.gateway.book_snapshot());
  same_account(a, f.gateway.account(1));
  same_account(b, f.gateway.account(2));
}

BOOST_FIXTURE_TEST_CASE(valid_order, Fixture) {
  submit(1, 1, true, 100);
  BOOST_CHECK_EQUAL(gateway.account(1).reserved_buy, 100);
  BOOST_CHECK_EQUAL(gateway.order(1).remaining, 100);
}
BOOST_FIXTURE_TEST_CASE(nonpositive_quantity, Fixture) {
  rejected(*this, {1,1,true,0,100}, RejectReason::InvalidQuantity);
  rejected(*this, {1,1,true,-1,100}, RejectReason::InvalidQuantity);
}
BOOST_FIXTURE_TEST_CASE(nonpositive_price, Fixture) {
  rejected(*this, {1,1,true,1,0}, RejectReason::InvalidPrice);
  rejected(*this, {1,1,true,1,-1}, RejectReason::InvalidPrice);
}
BOOST_FIXTURE_TEST_CASE(price_band_boundaries, Fixture) {
  rejected(*this, {1,1,true,1,9}, RejectReason::InvalidPrice);
  rejected(*this, {1,1,true,1,201}, RejectReason::InvalidPrice);
  submit(1,1,true,1,10);
  submit(2,2,false,1,200);
}
BOOST_FIXTURE_TEST_CASE(quantity_limit_boundary, Fixture) {
  rejected(*this, {1,1,true,1001,100}, RejectReason::OrderQuantityLimit);
  submit(1,1,true,1000);
}
BOOST_FIXTURE_TEST_CASE(notional_limit_boundary, Fixture) {
  rejected(*this, {1,1,true,501,200}, RejectReason::OrderNotionalLimit);
  submit(1,1,true,500,200);
}
BOOST_AUTO_TEST_CASE(notional_multiplication_overflow) {
  RiskGateway g; RiskLimits l;
  l.max_price = INT64_MAX; l.max_order_qty = 100;
  l.max_order_notional = UINT64_MAX;
  g.add_account(1,l);
  const auto before = g.book_snapshot();
  BOOST_CHECK(g.submit({1,1,true,3,INT64_MAX},0).reason == RejectReason::OrderNotionalLimit);
  BOOST_CHECK_EQUAL(before,g.book_snapshot());
  BOOST_CHECK(static_cast<bool>(g.submit({1,1,true,2,INT64_MAX},0)));
}
BOOST_FIXTURE_TEST_CASE(duplicate_active_id_global, Fixture) {
  submit(1,1,true,100);
  rejected(*this,{1,2,false,100,100},RejectReason::DuplicateOrderId);
}
BOOST_FIXTURE_TEST_CASE(duplicate_terminal_ids, Fixture) {
  submit(1,1,true,100);
  BOOST_REQUIRE(static_cast<bool>(gateway.cancel(1,1)));
  rejected(*this,{1,1,true,100,100},RejectReason::DuplicateOrderId);
  submit(2,1,true,100); submit(3,2,false,100);
  rejected(*this,{2,1,true,100,100},RejectReason::DuplicateOrderId);
}
BOOST_FIXTURE_TEST_CASE(unknown_account, Fixture) {
  rejected(*this,{1,99,true,1,100},RejectReason::UnknownAccount);
}
BOOST_AUTO_TEST_CASE(long_position_limit) {
  RiskGateway g; RiskLimits l; l.max_long_position=1000;
  g.add_account(1,l,800);
  BOOST_CHECK(g.submit({1,1,true,300,100},0).reason == RejectReason::PositionLimit);
  BOOST_CHECK(static_cast<bool>(g.submit({1,1,true,200,100},0)));
}
BOOST_AUTO_TEST_CASE(short_position_limit) {
  RiskGateway g; RiskLimits l; l.max_short_position=1000;
  g.add_account(1,l,-800);
  BOOST_CHECK(g.submit({1,1,false,300,100},0).reason == RejectReason::PositionLimit);
  BOOST_CHECK(static_cast<bool>(g.submit({1,1,false,200,100},0)));
}
BOOST_FIXTURE_TEST_CASE(multiple_same_account_orders, Fixture) {
  submit(1,1,true,600,90);
  rejected(*this,{2,1,true,401,90},RejectReason::PositionLimit);
  submit(2,1,true,400,90);
  BOOST_CHECK_EQUAL(gateway.account(1).reserved_buy,1000);
}
BOOST_FIXTURE_TEST_CASE(opposite_exposure_not_netted, Fixture) {
  submit(1,1,false,500,110); submit(2,1,true,1000,90);
  rejected(*this,{3,1,true,1,90},RejectReason::PositionLimit);
  BOOST_CHECK_EQUAL(gateway.account(1).reserved_sell,500);
  BOOST_CHECK_EQUAL(gateway.account(1).reserved_buy,1000);
}
BOOST_AUTO_TEST_CASE(sliding_rate_window_boundary) {
  RiskGateway g; RiskLimits l; l.max_orders_per_second=2;
  g.add_account(1,l);
  BOOST_REQUIRE(static_cast<bool>(g.submit({1,1,true,1,100},0)));
  BOOST_REQUIRE(static_cast<bool>(g.submit({2,1,true,1,100},500000000)));
  const auto before=g.book_snapshot();
  BOOST_CHECK(g.submit({3,1,true,1,100},999999999).reason == RejectReason::RateLimit);
  BOOST_CHECK_EQUAL(before,g.book_snapshot());
  BOOST_CHECK(static_cast<bool>(g.submit({3,1,true,1,100},1000000000)));
  BOOST_CHECK(g.submit({4,1,true,1,100},1000000000).reason == RejectReason::RateLimit);
}
BOOST_AUTO_TEST_CASE(rate_is_per_account) {
  RiskGateway g; RiskLimits l; l.max_orders_per_second=1;
  g.add_account(1,l); g.add_account(2,l);
  BOOST_CHECK(static_cast<bool>(g.submit({1,1,true,1,100},0)));
  BOOST_CHECK(static_cast<bool>(g.submit({2,2,true,1,100},0)));
}
BOOST_AUTO_TEST_CASE(rejection_consumes_neither_id_nor_rate) {
  RiskGateway g; RiskLimits l; l.max_orders_per_second=1; g.add_account(1,l);
  BOOST_CHECK(g.submit({1,1,true,0,100},0).reason == RejectReason::InvalidQuantity);
  BOOST_CHECK(static_cast<bool>(g.submit({1,1,true,1,100},0)));
}
BOOST_FIXTURE_TEST_CASE(validation_is_read_only, Fixture) {
  const auto before=gateway.book_snapshot(); const auto a=gateway.account(1);
  for(int i=0;i<3;++i) BOOST_CHECK(static_cast<bool>(gateway.validate({1,1,true,100,100},0)));
  BOOST_CHECK_EQUAL(before,gateway.book_snapshot()); same_account(a,gateway.account(1));
  submit(1,1,true,100);
}
BOOST_FIXTURE_TEST_CASE(partial_fill_updates_both_accounts, Fixture) {
  submit(1,1,true,100); submit(2,2,false,40);
  BOOST_CHECK_EQUAL(gateway.account(1).position,40);
  BOOST_CHECK_EQUAL(gateway.account(2).position,-40);
  BOOST_CHECK_EQUAL(gateway.account(1).reserved_buy,60);
  BOOST_CHECK_EQUAL(gateway.account(2).reserved_sell,0);
  BOOST_CHECK_EQUAL(gateway.order(1).filled,40);
  BOOST_CHECK_EQUAL(gateway.order(1).remaining,60);
}
BOOST_FIXTURE_TEST_CASE(partial_fill_cancel_releases_remainder, Fixture) {
  submit(1,1,true,100); submit(2,2,false,40);
  BOOST_REQUIRE(static_cast<bool>(gateway.cancel(1,1)));
  BOOST_CHECK_EQUAL(gateway.account(1).position,40);
  BOOST_CHECK_EQUAL(gateway.account(1).reserved_buy,0);
  BOOST_CHECK(gateway.order(1).state==OrderState::Cancelled);
}
BOOST_FIXTURE_TEST_CASE(partial_fill_replace_increases_exposure, Fixture) {
  submit(1,1,true,100); submit(2,2,false,40);
  BOOST_REQUIRE(static_cast<bool>(gateway.replace(1,1,150,100,0)));
  BOOST_CHECK_EQUAL(gateway.order(1).total,150);
  BOOST_CHECK_EQUAL(gateway.order(1).remaining,110);
  BOOST_CHECK_EQUAL(gateway.account(1).reserved_buy,110);
  submit(3,2,false,110);
  BOOST_CHECK_EQUAL(gateway.account(1).position,150);
  BOOST_CHECK_EQUAL(gateway.account(1).reserved_buy,0);
}
BOOST_FIXTURE_TEST_CASE(partial_fill_replace_decreases_exposure, Fixture) {
  submit(1,1,true,100); submit(2,2,false,40);
  BOOST_REQUIRE(static_cast<bool>(gateway.replace(1,1,60,100,0)));
  BOOST_CHECK_EQUAL(gateway.account(1).reserved_buy,20);
  BOOST_CHECK_EQUAL(gateway.order(1).remaining,20);
  BOOST_REQUIRE(static_cast<bool>(gateway.cancel(1,1)));
  BOOST_CHECK_EQUAL(gateway.account(1).position,40);
  BOOST_CHECK_EQUAL(gateway.account(1).reserved_buy,0);
}
BOOST_FIXTURE_TEST_CASE(reprice_can_fill_and_release_reservation, Fixture) {
  submit(1,1,true,100,90); submit(2,2,false,50,100);
  BOOST_REQUIRE(static_cast<bool>(gateway.replace(1,1,100,100,0)));
  BOOST_CHECK_EQUAL(gateway.account(1).position,50);
  BOOST_CHECK_EQUAL(gateway.account(1).reserved_buy,50);
}
BOOST_FIXTURE_TEST_CASE(rejected_replace_preserves_order_book_and_risk, Fixture) {
  submit(1,1,true,600,90); submit(2,1,true,400,90);
  const auto before=gateway.book_snapshot(); const auto a=gateway.account(1);
  BOOST_CHECK(gateway.replace(1,1,601,90,0).reason==RejectReason::PositionLimit);
  BOOST_CHECK_EQUAL(before,gateway.book_snapshot()); same_account(a,gateway.account(1));
  BOOST_CHECK_EQUAL(gateway.order(1).total,600);
}
BOOST_FIXTURE_TEST_CASE(replace_checks_all_scalar_limits, Fixture) {
  submit(1,1,true,100);
  const auto before=gateway.book_snapshot(); const auto a=gateway.account(1);
  BOOST_CHECK(gateway.replace(1,1,100,0,0).reason==RejectReason::InvalidPrice);
  BOOST_CHECK(gateway.replace(1,1,1001,100,0).reason==RejectReason::OrderQuantityLimit);
  BOOST_CHECK(gateway.replace(1,1,501,200,0).reason==RejectReason::OrderNotionalLimit);
  BOOST_CHECK_EQUAL(before,gateway.book_snapshot()); same_account(a,gateway.account(1));
}
BOOST_FIXTURE_TEST_CASE(replace_cannot_remove_already_filled_quantity, Fixture) {
  submit(1,1,true,100); submit(2,2,false,40);
  const auto before=gateway.book_snapshot(); const auto a=gateway.account(1);
  for(auto qty : {-1,0,39,40})
    BOOST_CHECK(gateway.replace(1,1,qty,100,0).reason==RejectReason::InvalidQuantity);
  BOOST_CHECK_EQUAL(before,gateway.book_snapshot()); same_account(a,gateway.account(1));
}
BOOST_AUTO_TEST_CASE(replace_counts_rate_but_cancel_remains_available) {
  RiskGateway g; RiskLimits l; l.max_orders_per_second=2; g.add_account(1,l);
  BOOST_REQUIRE(static_cast<bool>(g.submit({1,1,true,100,100},0)));
  BOOST_REQUIRE(static_cast<bool>(g.replace(1,1,101,100,0)));
  const auto before=g.book_snapshot(); const auto a=g.account(1);
  BOOST_CHECK(g.replace(1,1,102,100,0).reason==RejectReason::RateLimit);
  BOOST_CHECK_EQUAL(before,g.book_snapshot()); same_account(a,g.account(1));
  BOOST_CHECK(static_cast<bool>(g.cancel(1,1)));
  BOOST_CHECK_EQUAL(g.account(1).reserved_buy,0);
}
BOOST_FIXTURE_TEST_CASE(unknown_foreign_and_closed_order_commands, Fixture) {
  submit(1,1,true,100);
  const auto before=gateway.book_snapshot();
  BOOST_CHECK(gateway.cancel(2,1).reason==RejectReason::UnknownOrder);
  BOOST_CHECK(gateway.replace(2,1,100,100,0).reason==RejectReason::UnknownOrder);
  BOOST_CHECK(gateway.cancel(1,999).reason==RejectReason::UnknownOrder);
  BOOST_CHECK(gateway.replace(1,999,100,100,0).reason==RejectReason::UnknownOrder);
  BOOST_CHECK_EQUAL(before,gateway.book_snapshot());
  BOOST_REQUIRE(static_cast<bool>(gateway.cancel(1,1)));
  BOOST_CHECK(gateway.cancel(1,1).reason==RejectReason::OrderNotOpen);
  BOOST_CHECK(gateway.replace(1,1,100,100,0).reason==RejectReason::OrderNotOpen);
  submit(2,1,true,100); submit(3,2,false,100);
  BOOST_CHECK(gateway.cancel(1,2).reason==RejectReason::OrderNotOpen);
}
BOOST_AUTO_TEST_CASE(configuration_and_extreme_position_bounds) {
  RiskGateway g; RiskLimits l;
  l.max_long_position=INT64_MAX; l.max_short_position=INT64_MAX;
  g.add_account(1,l,INT64_MAX);
  BOOST_CHECK(g.submit({1,1,true,1,100},0).reason==RejectReason::PositionLimit);
  g.add_account(2,l,-INT64_MAX);
  BOOST_CHECK(g.submit({2,2,false,1,100},0).reason==RejectReason::PositionLimit);
  BOOST_CHECK_THROW(g.add_account(1,l),std::invalid_argument);
  l.max_order_qty=static_cast<Quantity>(INT32_MAX)+1;
  BOOST_CHECK_THROW(g.add_account(3,l),std::invalid_argument);
  l.max_order_qty=1; l.max_short_position=-1;
  BOOST_CHECK_THROW(g.add_account(3,l),std::invalid_argument);
  l.max_short_position=1; l.max_orders_per_second=0;
  BOOST_CHECK_THROW(g.add_account(3,l),std::invalid_argument);
}
BOOST_FIXTURE_TEST_CASE(backward_timestamp_is_rejected_without_mutation, Fixture) {
  submit(1,1,true,1,100,1000);
  rejected(*this,{2,1,true,1,100},RejectReason::InvalidTimestamp,999);
  submit(2,1,true,1,100,1000);
}
