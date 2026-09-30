set pagination off
set print pretty on
set breakpoint pending on
break ut_stop_orders.cpp:61
commands
silent
bt full
continue
end
break ut_stop_orders.cpp:62
commands
silent
bt full
continue
end
break ut_stop_orders.cpp:65
commands
silent
bt full
continue
end
break ut_stop_orders.cpp:66
commands
silent
bt full
continue
end
run --run_test=TestStopOrdersOffMarketNoTrade --log_level=all --color_output=no
