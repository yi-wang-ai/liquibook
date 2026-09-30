set pagination off
set print pretty on
set breakpoint pending on
break ut_stop_orders.cpp:101
commands
silent
bt full
continue
end
break ut_stop_orders.cpp:102
commands
silent
bt full
continue
end
run --run_test=TestStopMarketOrdersTradeWhenStopPriceReached --log_level=all --color_output=no
