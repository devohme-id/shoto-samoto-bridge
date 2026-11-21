#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>
// Pure C++ design example: no hardware outputs; not a certified protection system.
struct Pack {
    bool complete=false, identity_verified=false, limits_verified=false;
    bool allow_charge=false, allow_discharge=false, critical=true;
    uint32_t sampled_ms=0;
    float soc=0, capacity_ah=0, bus_v=0, bus_a=0;
    float ccl=0, dcl=0, cvl=0, dvl=0;
};
struct Policy {
    bool approved=false; uint32_t freshness_ms=5000;
    float max_delta_v=0, charge_cap=0, discharge_cap=0;
};
struct Bank {
    bool valid=false, charge=false, discharge=false;
    float soc=0, capacity_ah=0, bus_a=0, ccl=0, dcl=0, cvl=0, dvl=0;
};
inline bool finite(float x){return std::isfinite(x);}
inline bool usable(const Pack& p,uint32_t now,const Policy& x){
    return p.complete && p.identity_verified && p.limits_verified && !p.critical &&
        uint32_t(now-p.sampled_ms)<=x.freshness_ms && finite(p.soc) && p.soc>=0 && p.soc<=100 &&
        finite(p.capacity_ah) && p.capacity_ah>0 && finite(p.bus_v) && p.bus_v>0 && finite(p.bus_a) &&
        finite(p.ccl) && p.ccl>=0 && finite(p.dcl) && p.dcl>=0 && finite(p.cvl) && finite(p.dvl) && p.dvl>0 && p.cvl>p.dvl;
}
inline Bank aggregate(const Pack& a,const Pack& b,uint32_t now,const Policy& p){
    Bank r{}; // no permission on boot, invalid data or unapproved policy
    if(!p.approved || !finite(p.max_delta_v) || p.max_delta_v<=0 ||
       !finite(p.charge_cap) || p.charge_cap<0 || !finite(p.discharge_cap) || p.discharge_cap<0 ||
       !usable(a,now,p)||!usable(b,now,p)||std::fabs(a.bus_v-b.bus_v)>p.max_delta_v) return r;
    r.cvl=std::min(a.cvl,b.cvl); r.dvl=std::max(a.dvl,b.dvl);
    if(r.cvl<=r.dvl) return Bank{};
    r.capacity_ah=a.capacity_ah+b.capacity_ah;
    r.soc=(a.soc*a.capacity_ah+b.soc*b.capacity_ah)/r.capacity_ah;
    r.bus_a=a.bus_a+b.bus_a;
    // Conservative single-pack bound, not summed limits, until load sharing/isolation are proven.
    r.ccl=(a.allow_charge&&b.allow_charge)?std::min({a.ccl,b.ccl,p.charge_cap}):0;
    r.dcl=(a.allow_discharge&&b.allow_discharge)?std::min({a.dcl,b.dcl,p.discharge_cap}):0;
    r.charge=r.ccl>0; r.discharge=r.dcl>0;
    r.valid=true; return r;
}
