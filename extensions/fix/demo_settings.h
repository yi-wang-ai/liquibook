#pragma once
#include "adapter.h"
namespace exchange { namespace fix {
inline Settings demo_settings(){
 Settings s;RiskLimits l;l.max_long_position=2000;l.max_short_position=2000;l.max_order_qty=1000;l.max_order_notional=10000000;
 s.risk={{1,{{1,l,0},{2,l,0}}},{2,{{1,l,0},{2,l,0}}}};
 s.symbols={{"ESZ6",{1,2}},{"TEST",{2,2}}};s.clients={{"C1",1},{"C2",2}};return s;
}
} }
