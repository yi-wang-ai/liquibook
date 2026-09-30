set pagination off
set print pretty on
set breakpoint pending on
break ut_listeners.cpp:396
commands
silent
bt full
continue
end
run --run_test=TestTradeCallbacks --log_level=all --color_output=no
