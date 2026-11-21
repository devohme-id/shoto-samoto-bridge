#include "../firmware/safety_core.hpp"
#include <cassert>
#include <iostream>
int main(){
    Pack a; a.complete=a.identity_verified=a.limits_verified=a.allow_charge=a.allow_discharge=true; a.critical=false;
    a.sampled_ms=1000;a.soc=80;a.capacity_ah=100;a.bus_v=52;a.bus_a=2;a.ccl=15;a.dcl=20;a.cvl=54;a.dvl=48;
    Pack b=a;b.soc=60;b.capacity_ah=50;b.bus_a=3;b.ccl=10;
    Policy p; p.approved=true;p.max_delta_v=.3;p.charge_cap=12;p.discharge_cap=18;
    auto r=aggregate(a,b,2000,p);assert(r.valid&&r.charge&&r.discharge&&r.ccl==10&&r.dcl==18&&r.bus_a==5&&std::fabs(r.soc-73.3333)<.01);
    assert(!aggregate(a,b,7000,p).valid); b.critical=true;assert(!aggregate(a,b,2000,p).valid);b.critical=false;
    b.allow_charge=false;r=aggregate(a,b,2000,p);assert(r.valid&&!r.charge&&r.ccl==0&&r.discharge);
    b.identity_verified=false;assert(!aggregate(a,b,2000,p).valid);b.identity_verified=true;
    b.bus_v=53;assert(!aggregate(a,b,2000,p).valid);b.bus_v=52;
    p.approved=false;assert(!aggregate(a,b,2000,p).valid);
    std::cout<<"7 host safety scenarios PASS\n";
}
