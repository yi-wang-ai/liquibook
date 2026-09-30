set pagination off
set print pretty on
set breakpoint pending on
break ut_stop_orders.cpp:139
commands
silent
bt full
continue
end
break ut_stop_orders.cpp:140
commands
silent
bt full
continue
end
break ut_stop_orders.cpp:143
commands
silent
bt full
continue
end
break ut_stop_orders.cpp:144
commands
silent
bt full
continue
end
break ut_stop_orders.cpp:146
commands
silent
bt full
continue
end
break ut_stop_orders.cpp:147
commands
silent
bt full
continue
end
run --run_test=TestStopOrdersCancelBeforeTrigger --log_level=all --color_output=no
