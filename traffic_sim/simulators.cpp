#include "simulators.h"
#include <cstdio>
#include <ctime>

// ── Base ──────────────────────────────────────────────────────────────────────
Sim::Sim(std::string id, std::string sn, Category c, Iface i, SigType s)
    : _rng(std::random_device{}()) {
    _s.id=std::move(id); _s.sname=std::move(sn);
    _s.cat=c; _s.iface=i; _s.sig=s;
    _s.changedMs = nowMs();
}
State Sim::getState() const { std::lock_guard<std::mutex> lk(_mx); return _s; }
void  Sim::queueEvt(const std::string& m) { _s.evtCount++; _pending.push_back(m); }
void  Sim::flushEvents() { for(auto&m:_pending) if(_cb) _cb(_s.id,m); _pending.clear(); }
float Sim::rF(float lo,float hi) { return std::uniform_real_distribution<float>(lo,hi)(_rng); }
int   Sim::rI(int lo,int hi)     { if(lo>=hi) return lo; return std::uniform_int_distribution<int>(lo,hi)(_rng); }
bool  Sim::rP(double p)          { return std::uniform_real_distribution<double>(0,1)(_rng)<p; }

bool Sim::injFaultTick(int64_t now) {
    if (!_injActive) return false;
    if (now >= _injEndMs) {
        _injActive = false;
        _s.fault = false; _s.faultType.clear(); _s.active = false;
        _s.summary.clear();
        queueEvt("FAULT CLEARED");
        return false;
    }
    return true;
}

void Sim::injectFault(const std::string& type) {
    std::lock_guard<std::mutex> lk(_mx);
    if (type == "CLEAR") {
        _injActive = false;
        _s.fault = false; _s.faultType.clear(); _s.active = false;
        queueEvt("FAULT CLEARED (manual)");
        return;
    }
    _injActive = true;
    _injEndMs  = nowMs() + 30000;
    _s.fault    = true;
    _s.faultType = type;
    bool hi = (type=="SHORT"||type=="STUCK"||type=="TRIP"||type=="OUTAGE"||type=="ACTIVE");
    _s.active  = hi;
    _s.digital = hi;
    _s.summary = "FLT:" + type.substr(0,6);
    queueEvt("FAULT INJECTED: " + type);
}

// ── PresenceSim ───────────────────────────────────────────────────────────────
PresenceSim::PresenceSim(std::string id,std::string sn,PresenceCfg c)
    : Sim(id,sn,Category::VEHICLE,Iface::DISCRETE,SigType::DIGITAL),_c(c) {
    _nextMs = nowMs()+rI(100,(int)(60000.0/c.arrPerMin));
    _s.summary="VACANT";
}
void PresenceSim::tick(int64_t now) {
    std::lock_guard<std::mutex> lk(_mx);
    if (!_faultKind.empty() && now>=_faultEndMs) {
        _faultKind.clear(); _s.fault=false; _s.faultType.clear();
        _s.digital=_on; _s.active=_on; _s.summary=_on?"PRESENT":"VACANT";
        queueEvt("FAULT CLEARED");
    }
    if (!_faultKind.empty()) return;
    if (rP(_c.faultP/100.0)) {
        float r=rF(0,1); _faultEndMs=now+rI(5000,30000); _s.fault=true;
        if      (r<0.4f) { _faultKind="OPEN";  _s.faultType="OPEN CKT";  _s.digital=false; _s.active=false; _s.summary="FLT:OPEN";  queueEvt("FAULT open circuit"); }
        else if (r<0.8f) { _faultKind="SHORT"; _s.faultType="SHORT CKT"; _s.digital=true;  _s.active=true;  _s.summary="FLT:SHORT"; queueEvt("FAULT short circuit (stuck on)"); }
        else             { _faultKind="STUCK"; _s.faultType="STUCK ON";  _s.digital=true;  _s.active=true;  _s.summary="FLT:STUCK"; queueEvt("FAULT stuck on"); }
        return;
    }
    if (now<_nextMs) return;
    _on=!_on; _s.digital=_on; _s.active=_on; _s.changedMs=now;
    if (_on) {
        _nextMs=now+rI(_c.presLo,_c.presHi); _s.summary="PRESENT"; queueEvt("DETECTED");
    } else {
        int g=(int)(60000.0/_c.arrPerMin);
        _nextMs=now+rI(std::max(200,g/3),g*3); _s.summary="VACANT"; queueEvt("DEPARTED");
    }
}

void PresenceSim::injectFault(const std::string& type) {
    std::lock_guard<std::mutex> lk(_mx);
    if (type == "CLEAR") {
        _faultKind.clear(); _s.fault=false; _s.faultType.clear();
        _s.digital=_on; _s.active=_on; _s.summary=_on?"PRESENT":"VACANT";
        queueEvt("FAULT CLEARED (manual)"); return;
    }
    _faultEndMs = nowMs() + 30000;
    _s.fault = true;
    if (type == "OPEN") {
        _faultKind="OPEN"; _s.faultType="OPEN CKT"; _s.digital=false; _s.active=false;
        _s.summary="FLT:OPEN"; queueEvt("FAULT INJECTED: open circuit");
    } else if (type == "SHORT") {
        _faultKind="SHORT"; _s.faultType="SHORT CKT"; _s.digital=true; _s.active=true;
        _s.summary="FLT:SHORT"; queueEvt("FAULT INJECTED: short circuit (stuck on)");
    } else if (type == "STUCK") {
        _faultKind="STUCK"; _s.faultType="STUCK ON"; _s.digital=true; _s.active=true;
        _s.summary="FLT:STUCK"; queueEvt("FAULT INJECTED: stuck on");
    }
}

// ── PulseSim ──────────────────────────────────────────────────────────────────
PulseSim::PulseSim(std::string id,std::string sn,PulseCfg c)
    : Sim(id,sn,Category::VEHICLE,Iface::DISCRETE,SigType::DIGITAL),_c(c) {
    _nextMs=nowMs()+rI(200,(int)(60000.0/c.arrPerMin)); _s.summary="IDLE";
}
void PulseSim::tick(int64_t now) {
    std::lock_guard<std::mutex> lk(_mx);
    if (injFaultTick(now)) return;
    if (_on && now>=_pulseEnd) {
        _on=false; _s.digital=false; _s.active=false; _s.summary="IDLE";
        int g=(int)(60000.0/_c.arrPerMin);
        _nextMs=now+rI(_c.retrigMs,g*2);
    }
    if (!_on && now>=_nextMs) {
        _on=true; _s.digital=true; _s.active=true;
        _pulseEnd=now+rI(_c.pulseLo,_c.pulseHi);
        _s.summary="PULSE"; _s.changedMs=now; queueEvt("PULSE advance detect");
    }
}

// ── PedSim ────────────────────────────────────────────────────────────────────
PedSim::PedSim(std::string id,std::string sn,PedCfg c)
    : Sim(id,sn,Category::PEDESTRIAN,Iface::DISCRETE,SigType::DIGITAL),_c(c) {
    _nextPressMs=nowMs()+rI(5000,(int)(120000.0/c.pressPerMin)); _s.summary="IDLE";
}
void PedSim::tick(int64_t now) {
    std::lock_guard<std::mutex> lk(_mx);
    if (injFaultTick(now)) return;
    if (_latched && now>=_svcMs) {
        _latched=false; _s.digital=false; _s.active=false; _s.summary="IDLE";
        int g=(int)(60000.0/_c.pressPerMin);
        _nextPressMs=now+rI(g/2,g*4); queueEvt("SERVICED (walk phase granted)");
    }
    if (!_latched && now>=_nextPressMs) {
        _latched=true; ++_pressCount; _s.digital=true; _s.active=true; _s.changedMs=now;
        char buf[48]; snprintf(buf,sizeof(buf),"PRESSED (#%d) LATCHED",_pressCount);
        _s.summary="LATCHED"; queueEvt(buf);
        _svcMs=now+rI(_c.svcLo,_c.svcHi);
    }
}

// ── EmergSim ──────────────────────────────────────────────────────────────────
EmergSim::EmergSim(std::string id,std::string sn,PreemptCfg c)
    : Sim(id,sn,Category::PRIORITY,Iface::RELAY,SigType::DIGITAL),_c(c) {
    double a=3600000.0/c.ratePerHr;
    _nextMs=nowMs()+rI((int)(a/3),(int)(a*3)); _s.summary="STANDBY";
}
void EmergSim::tick(int64_t now) {
    std::lock_guard<std::mutex> lk(_mx);
    if (_on && now>=_holdEnd) {
        _on=false; _s.digital=false; _s.active=false; _s.summary="STANDBY";
        double a=3600000.0/_c.ratePerHr;
        _nextMs=now+rI((int)(a/2),(int)(a*3)); queueEvt("PREEMPT RELEASED");
    }
    if (!_on && now>=_nextMs) {
        _on=true; _s.digital=true; _s.active=true;
        _holdEnd=now+rI(_c.holdLo,_c.holdHi);
        _s.summary="ACTIVE!"; _s.changedMs=now;
        queueEvt("EMERGENCY PREEMPT ACTIVE (priority=100, interrupting current phase)");
    }
}

void EmergSim::injectFault(const std::string& type) {
    std::lock_guard<std::mutex> lk(_mx);
    if (type == "CLEAR") {
        _on=false; _s.digital=false; _s.active=false; _s.fault=false;
        _s.faultType.clear(); _s.summary="STANDBY";
        queueEvt("PREEMPT CLEARED (manual)"); return;
    }
    if (type == "ACTIVE") {
        _on=true; _holdEnd=nowMs()+20000; _s.digital=true; _s.active=true;
        _s.summary="ACTIVE!"; _s.changedMs=nowMs();
        queueEvt("FAULT INJECTED: EMERGENCY PREEMPT ACTIVE (injected)");
    }
}

// ── RadarSim ──────────────────────────────────────────────────────────────────
RadarSim::RadarSim(std::string id,std::string sn,RadarCfg c)
    : Sim(id,sn,Category::SENSOR,Iface::RS485,SigType::PACKET),_c(c) {
    _nextMs=nowMs()+rI(0,1000/c.hz);
    _tranMs=nowMs()+rI(1000,(int)(60000.0/c.arrPerMin));
    _s.summary="init...";
}
void RadarSim::tick(int64_t now) {
    std::lock_guard<std::mutex> lk(_mx);
    if (injFaultTick(now)) return;
    if (now>=_tranMs) {
        bool was=_present; _present=!_present;
        if (_present) { _spd=rF(12,52); _occ=rF(0.2f,0.85f); _tranMs=now+rI(300,4000); }
        else          { _spd=0; _occ=rF(0,0.04f); _tranMs=now+rI(200,(int)(60000.0/_c.arrPerMin)*2); }
        if (_present!=was)
            queueEvt(std::string("PRESENCE ") + (_present?"DETECTED":"CLEARED") +
                     " spd="+std::to_string((int)_spd)+"mph");
    }
    if (now<_nextMs) return;
    _nextMs=now+(1000/_c.hz)+rI(-5,5);
    if (rP(_c.pktLoss)) return;
    float spd=_spd+rF(-1.5f,1.5f), occ=std::clamp(_occ+rF(-0.02f,0.02f),0.f,1.f);
    char buf[64];
    snprintf(buf,sizeof(buf),"pres=%-3s spd=%4.0fmph occ=%.2f",_present?"YES":"NO",spd,occ);
    _s.summary=buf; _s.digital=_present; _s.active=_present; _s.analogV=spd;
}

// ── CamSim ────────────────────────────────────────────────────────────────────
CamSim::CamSim(std::string id,std::string sn,CamCfg c)
    : Sim(id,sn,Category::SENSOR,Iface::ETHERNET,SigType::PACKET),_c(c) {
    _nextMs=nowMs()+rI(0,1000/c.hz);
    _tranMs=nowMs()+rI(500,5000); _s.summary="init...";
}
void CamSim::tick(int64_t now) {
    std::lock_guard<std::mutex> lk(_mx);
    if (_degraded && now>=_degradEnd) {
        _degraded=false; _s.fault=false; _s.faultType.clear(); queueEvt("CAMERA RECOVERED");
    }
    if (!_degraded && rP(_c.failP/10000.0)) {
        _degraded=true; _degradEnd=now+rI(5000,120000); _s.fault=true;
        bool rain=rP(0.4); _s.faultType=rain?"RAIN":"LOW LIGHT"; _s.summary="DEGRADED";
        queueEvt(std::string("CAMERA DEGRADED: ")+(rain?"rain obstruction":"low light/night"));
        return;
    }
    if (_degraded) return;
    if (now>=_tranMs) {
        bool was=_cnt>0; _cnt=rI(0,5); _occ=_cnt>0?rF(0.1f,0.75f):0.f;
        _tranMs=now+rI(500,(int)(60000.0/_c.arrPerMin));
        if ((_cnt>0)!=was)
            queueEvt(std::string("ZONE ")+(_cnt>0?"OCCUPIED":"CLEAR")+" cnt="+std::to_string(_cnt));
    }
    if (now<_nextMs) return;
    _nextMs=now+(1000/_c.hz)+rI(-10,10);
    if (rP(_c.pktLoss)) return;
    int cnt=std::max(0,_cnt+rI(-1,1));
    float occ=std::clamp(_occ+rF(-0.04f,0.04f),0.f,1.f);
    char buf[64];
    snprintf(buf,sizeof(buf),"cnt=%-2d occ=%.2f pres=%-3s",cnt,occ,cnt>0?"YES":"NO");
    _s.summary=buf; _s.digital=cnt>0; _s.active=cnt>0; _s.analogV=(float)cnt;
}

void CamSim::injectFault(const std::string& type) {
    std::lock_guard<std::mutex> lk(_mx);
    if (type == "CLEAR") {
        _degraded=false; _s.fault=false; _s.faultType.clear();
        queueEvt("FAULT CLEARED (manual)"); return;
    }
    _degraded=true; _degradEnd=nowMs()+30000; _s.fault=true;
    _s.faultType="INJECTED"; _s.summary="DEGRADED";
    queueEvt("FAULT INJECTED: camera degraded ("+type+")");
}

// ── TransitSim ────────────────────────────────────────────────────────────────
TransitSim::TransitSim(std::string id,std::string sn,TransitCfg c)
    : Sim(id,sn,Category::PRIORITY,Iface::ETHERNET,SigType::PACKET),_c(c) {
    double a=3600000.0/c.ratePerHr;
    _nextMs=nowMs()+rI((int)(a/4),(int)a); _s.summary="no request";
}
void TransitSim::tick(int64_t now) {
    std::lock_guard<std::mutex> lk(_mx);
    if (injFaultTick(now)) return;
    if (_active && now>=_activeEnd) {
        _active=false; _s.digital=false; _s.active=false; _s.summary="no request";
    }
    if (now<_nextMs) return;
    if (rP(_c.pktLoss)) {
        double a=3600000.0/_c.ratePerHr;
        _nextMs=now+rI(5000,(int)a); queueEvt("PKT LOSS - transit request dropped"); return;
    }
    int late=rI(-120,300);
    char buf[64]; snprintf(buf,sizeof(buf),"BUS#%d appr=%s late=%+ds",_vid++,_c.approach.c_str(),late);
    _s.summary=buf; _s.digital=true; _s.active=true; _s.changedMs=now;
    _active=true; _activeEnd=now+rI(5000,20000);
    double a=3600000.0/_c.ratePerHr;
    _nextMs=now+rI((int)(a/2),(int)(a*2));
    queueEvt(std::string("TRANSIT REQUEST: ")+buf);
}

// ── GpsSim ────────────────────────────────────────────────────────────────────
GpsSim::GpsSim(std::string id,std::string sn)
    : Sim(id,sn,Category::SYSTEM,Iface::SERIAL,SigType::PACKET) {
    _nextMs=nowMs()+1000; _lastEmitMs=nowMs(); _s.summary="acquiring...";
}
void GpsSim::tick(int64_t now) {
    std::lock_guard<std::mutex> lk(_mx);
    if (_dropped && now>=_dropEnd) {
        _dropped=false; _s.fault=false; _s.faultType.clear(); queueEvt("GPS SIGNAL RESTORED");
    }
    if (!_dropped && rP(0.00003)) {
        _dropped=true; _dropEnd=now+rI(5000,60000);
        _s.fault=true; _s.faultType="NO SIG"; _s.summary="SIGNAL LOST";
        queueEvt("GPS SIGNAL LOST"); return;
    }
    if (_dropped || now<_nextMs) return;
    _nextMs=now+1000+rI(-5,5);
    auto wt=std::chrono::system_clock::now();
    auto tt=std::chrono::system_clock::to_time_t(wt);
    struct tm tbuf; gmtime_r(&tt,&tbuf);
    char ts[20]; strftime(ts,sizeof(ts),"%H:%M:%S UTC",&tbuf);
    _s.summary=ts; _s.digital=true;
    if (now-_lastEmitMs>300000) { _lastEmitMs=now; queueEvt(std::string("GPS SYNC OK: ")+ts); }
}

void GpsSim::injectFault(const std::string& type) {
    std::lock_guard<std::mutex> lk(_mx);
    if (type == "CLEAR") {
        _dropped=false; _s.fault=false; _s.faultType.clear();
        queueEvt("GPS SIGNAL RESTORED (manual)"); return;
    }
    _dropped=true; _dropEnd=nowMs()+30000; _s.fault=true;
    _s.faultType="NO SIG"; _s.summary="SIGNAL LOST";
    queueEvt("FAULT INJECTED: GPS signal loss");
}

// ── PowerSim ──────────────────────────────────────────────────────────────────
PowerSim::PowerSim(std::string id,std::string sn,PowerCfg c)
    : Sim(id,sn,Category::SYSTEM,Iface::DISCRETE,SigType::DIGITAL),_c(c) {
    double a=3600000.0/std::max(c.outagePerHr,0.0001);
    _nextMs=nowMs()+rI((int)(a/2),(int)(a*4));
    _s.digital=true; _s.summary="NOMINAL 120VAC";
}
void PowerSim::tick(int64_t now) {
    std::lock_guard<std::mutex> lk(_mx);
    if (_out && now>=_outEnd) {
        _out=false; _s.digital=true; _s.fault=false; _s.active=false;
        _s.faultType.clear(); _s.summary="NOMINAL 120VAC";
        double a=3600000.0/_c.outagePerHr;
        _nextMs=now+rI((int)(a/2),(int)(a*3)); queueEvt("POWER RESTORED - 120VAC nominal");
    }
    if (!_out && now>=_nextMs) {
        _out=true; _s.digital=false; _s.fault=true; _s.active=true;
        _s.faultType="OUTAGE"; _outEnd=now+rI(_c.outageLo,_c.outageHi);
        _s.summary="OUTAGE! BATT"; _s.changedMs=now;
        queueEvt("POWER OUTAGE - switching to battery backup");
    }
    if (!_out && rP(_c.brownoutP/10000.0)) {
        _s.summary="BROWNOUT 89V"; queueEvt("BROWNOUT detected (89VAC)");
    } else if (!_out && !_s.fault) { _s.summary="NOMINAL 120VAC"; }
}

void PowerSim::injectFault(const std::string& type) {
    std::lock_guard<std::mutex> lk(_mx);
    if (type == "CLEAR") {
        _out=false; _s.fault=false; _s.faultType.clear(); _s.digital=true;
        _s.summary="NOMINAL 120VAC"; queueEvt("POWER RESTORED (manual)"); return;
    }
    if (type == "OUTAGE") {
        _out=true; _outEnd=nowMs()+30000; _s.fault=true; _s.faultType="OUTAGE";
        _s.digital=false; _s.active=true; _s.summary="OUTAGE! BATT";
        queueEvt("FAULT INJECTED: power outage - battery backup");
    } else { // BROWN
        _s.fault=true; _s.faultType="BROWNOUT"; _s.summary="BROWNOUT 89V";
        queueEvt("FAULT INJECTED: brownout 89VAC");
    }
}

// ── TempSim ───────────────────────────────────────────────────────────────────
TempSim::TempSim(std::string id,std::string sn,TempCfg c)
    : Sim(id,sn,Category::SYSTEM,Iface::ANALOG,SigType::ANALOG_SIG),_c(c) {
    _t=c.base+rF(-5,5); _nextMs=nowMs()+1000; _driftMs=nowMs();
    char buf[16]; snprintf(buf,sizeof(buf),"%.1f C",_t);
    _s.summary=buf; _s.analogV=_t;
}
void TempSim::tick(int64_t now) {
    std::lock_guard<std::mutex> lk(_mx);
    if (injFaultTick(now)) return;
    if (now<_nextMs) return;
    _nextMs=now+1000;
    if (now-_driftMs>60000) {
        _t+=_dir*_c.driftPerMin; _driftMs=now;
        if (_t>_c.base+12) { _dir=-1; }
        if (_t<_c.base-8)  { _dir= 1; }
    }
    float m=std::clamp(_t+rF(-_c.noise,_c.noise),_c.lo,_c.hi);
    _s.analogV=m; char buf[20]; snprintf(buf,sizeof(buf),"%.1f C",m);
    _s.summary=buf; _s.active=m>58;
    bool wasF=_s.fault; _s.fault=m>70;
    if (_s.fault) { _s.faultType="OVERTEMP"; if(!wasF) queueEvt("WARNING overtemperature!"); }
    else _s.faultType.clear();
}

// ── HumSim ────────────────────────────────────────────────────────────────────
HumSim::HumSim(std::string id,std::string sn)
    : Sim(id,sn,Category::SYSTEM,Iface::ANALOG,SigType::ANALOG_SIG) {
    _s.analogV=35; _s.summary="35%RH"; _nextMs=nowMs()+2000;
}
void HumSim::tick(int64_t now) {
    std::lock_guard<std::mutex> lk(_mx);
    if (injFaultTick(now)) return;
    if (now<_nextMs) return;
    _nextMs=now+2000;
    _rh=std::clamp(_rh+rF(-0.3f,0.3f),10.f,90.f);
    _s.analogV=_rh; char buf[12]; snprintf(buf,sizeof(buf),"%.0f%%RH",_rh);
    _s.summary=buf; bool wasF=_s.fault; _s.fault=_rh>80;
    if (_s.fault) { _s.faultType="HIGH HUM"; if(!wasF) queueEvt("WARNING high humidity - condensation risk"); }
    else _s.faultType.clear();
}

// ── DoorSim ───────────────────────────────────────────────────────────────────
DoorSim::DoorSim(std::string id,std::string sn)
    : Sim(id,sn,Category::SYSTEM,Iface::DISCRETE,SigType::DIGITAL) {
    _s.digital=false; _s.summary="CLOSED";
    _nextMs=nowMs()+rI(600000,7200000);
}
void DoorSim::tick(int64_t now) {
    std::lock_guard<std::mutex> lk(_mx);
    if (_open && now>=_closeMs) {
        _open=false; _s.digital=false; _s.active=false; _s.fault=false;
        _s.faultType.clear(); _s.summary="CLOSED";
        _nextMs=now+rI(600000,14400000); queueEvt("CABINET DOOR CLOSED");
    }
    if (!_open && now>=_nextMs) {
        _open=true; _s.digital=true; _s.active=true; _s.fault=true;
        _s.faultType="DOOR OPEN"; _closeMs=now+rI(30000,600000);
        _s.summary="OPEN"; _s.changedMs=now; queueEvt("CABINET DOOR OPENED (maintenance access)");
    }
}

void DoorSim::injectFault(const std::string& type) {
    std::lock_guard<std::mutex> lk(_mx);
    if (type == "CLEAR") {
        _open=false; _s.fault=false; _s.faultType.clear(); _s.digital=false;
        _s.active=false; _s.summary="CLOSED"; queueEvt("CABINET DOOR CLOSED (manual)"); return;
    }
    _open=true; _closeMs=nowMs()+60000; _s.fault=true; _s.faultType="DOOR OPEN";
    _s.digital=true; _s.active=true; _s.summary="OPEN";
    queueEvt("FAULT INJECTED: cabinet door opened");
}

// ── MmuSim ────────────────────────────────────────────────────────────────────
MmuSim::MmuSim(std::string id,std::string sn,double tripP)
    : Sim(id,sn,Category::SAFETY,Iface::DISCRETE,SigType::DIGITAL),_tripP(tripP) {
    _s.digital=true; _s.summary="HEALTHY"; _nextMs=nowMs()+100;
}
void MmuSim::tick(int64_t now) {
    std::lock_guard<std::mutex> lk(_mx);
    if (_tripped && now>=_resetMs) {
        _tripped=false; _s.digital=true; _s.fault=false; _s.active=false;
        _s.faultType.clear(); _s.summary="HEALTHY"; queueEvt("SAFETY SYSTEM RESET - cleared");
    }
    if (now<_nextMs) return;
    _nextMs=now+100;
    if (!_tripped && rP(_tripP)) {
        _tripped=true; _s.digital=false; _s.fault=true; _s.active=true;
        _s.faultType="CONFLICT"; _resetMs=now+rI(15000,300000);
        _s.summary="TRIPPED!"; _s.changedMs=now;
        queueEvt(_s.id+" TRIP - conflict detected, entering all-red flash mode");
    }
}

void MmuSim::injectFault(const std::string& type) {
    std::lock_guard<std::mutex> lk(_mx);
    if (type == "CLEAR") {
        _tripped=false; _s.fault=false; _s.faultType.clear(); _s.digital=true;
        _s.active=false; _s.summary="HEALTHY"; queueEvt("SAFETY SYSTEM RESET - cleared"); return;
    }
    _tripped=true; _resetMs=nowMs()+30000; _s.digital=false; _s.fault=true; _s.active=true;
    _s.faultType="CONFLICT"; _s.summary="TRIPPED!"; _s.changedMs=nowMs();
    queueEvt(_s.id+" TRIP INJECTED - conflict detected, all-red flash mode");
}

// ── SyncSim ───────────────────────────────────────────────────────────────────
SyncSim::SyncSim(std::string id,std::string sn,SyncCfg c)
    : Sim(id,sn,Category::COORD,Iface::ETHERNET,SigType::PACKET),_c(c) {
    _nextMs=nowMs()+rI(0,1000/c.hz); _lastEmitMs=nowMs(); _s.summary="syncing...";
}
void SyncSim::tick(int64_t now) {
    std::lock_guard<std::mutex> lk(_mx);
    if (injFaultTick(now)) return;
    if (now<_nextMs) return;
    _nextMs=now+(1000/_c.hz)+rI(-20,20);
    if (rP(_c.pktLoss)) { queueEvt("PKT LOSS sync packet dropped"); return; }
    _off=std::clamp(_off+rF(-1.5f,1.5f),-500.f,500.f);
    char buf[40]; snprintf(buf,sizeof(buf),"%-12.12s off=%+.0fms",_c.neighbor.c_str(),_off);
    _s.summary=buf; _s.digital=true; _s.active=std::fabs(_off)>100;
    bool wasF=_s.fault; _s.fault=std::fabs(_off)>300;
    if (_s.fault) {
        _s.faultType="OUT SYNC";
        if (!wasF) queueEvt("SYNC WARNING large offset "+std::to_string((int)_off)+"ms with "+_c.neighbor);
    } else _s.faultType.clear();
    if (now-_lastEmitMs>30000) {
        _lastEmitMs=now;
        queueEvt("SYNC OK: "+_c.neighbor+" off="+std::to_string((int)_off)+"ms");
    }
}
