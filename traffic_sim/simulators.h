#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <random>
#include <chrono>
#include <algorithm>

inline int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

enum class Category { VEHICLE, PEDESTRIAN, PRIORITY, SENSOR, SYSTEM, SAFETY, COORD };
enum class Iface    { DISCRETE, RELAY, RS485, ETHERNET, SERIAL, ANALOG };
enum class SigType  { DIGITAL, ANALOG_SIG, PACKET };

struct State {
    std::string id, sname;
    Category    cat   = Category::VEHICLE;
    Iface       iface = Iface::DISCRETE;
    SigType     sig   = SigType::DIGITAL;
    bool        digital  = false;
    float       analogV  = 0.f;
    std::string summary;
    bool        fault    = false;
    std::string faultType;
    bool        active   = false;
    int64_t     changedMs = 0;
    int         evtCount  = 0;
};

using EvtCb = std::function<void(const std::string& id, const std::string& msg)>;

// ── Base ─────────────────────────────────────────────────────────────────────
class Sim {
public:
    Sim(std::string id, std::string sn, Category c, Iface i, SigType s);
    virtual ~Sim() = default;
    virtual void tick(int64_t now) = 0;
    void     flushEvents();
    State    getState() const;
    void     setEvtCb(EvtCb cb) { _cb = cb; }
    const std::string& id() const { return _s.id; }
    virtual void injectFault(const std::string& type);  // external fault injection
protected:
    void  queueEvt(const std::string& msg);
    float rF(float lo, float hi);
    int   rI(int lo, int hi);
    bool  rP(double p);
    // Call at top of derived tick() (under _mx) for sims without own fault state.
    // Returns true if an injected fault is active (caller should return).
    bool  injFaultTick(int64_t now);

    mutable std::mutex       _mx;
    State                    _s;
    EvtCb                    _cb;
    std::mt19937             _rng;
    std::vector<std::string> _pending;  // sim-thread only
    bool    _injActive = false;
    int64_t _injEndMs  = 0;
};

// ── Configs ───────────────────────────────────────────────────────────────────
struct PresenceCfg {
    double arrPerMin = 18;
    int presLo=200, presHi=4000, gapLo=200, gapHi=8000;
    double faultP=0.0002, glitchP=0.01;
};
struct PulseCfg {
    double arrPerMin = 18;
    int pulseLo=50, pulseHi=150, retrigMs=200;
};
struct PedCfg {
    double pressPerMin = 1.5;
    int svcLo=8000, svcHi=90000;
};
struct PreemptCfg {
    double ratePerHr = 3.0;
    int holdLo=8000, holdHi=25000;
};
struct RadarCfg {
    int hz = 10;
    double pktLoss=0.01, arrPerMin=18;
};
struct CamCfg {
    int hz = 15;
    double pktLoss=0.02, failP=0.015, arrPerMin=25;
};
struct TransitCfg {
    double ratePerHr=15, pktLoss=0.05;
    std::string approach = "N";
};
struct PowerCfg {
    double outagePerHr=0.0005, brownoutP=0.001;
    int outageLo=1000, outageHi=60000;
};
struct TempCfg {
    float base=42.f, noise=0.5f, driftPerMin=0.05f, lo=-10.f, hi=85.f;
};
struct SyncCfg {
    int hz=2;
    double pktLoss=0.02;
    std::string neighbor;
};

// ── Derived Simulators ────────────────────────────────────────────────────────
class PresenceSim : public Sim {
public:
    PresenceSim(std::string id, std::string sn, PresenceCfg c);
    void tick(int64_t now) override;
    void injectFault(const std::string& type) override;
private:
    PresenceCfg _c;
    bool _on=false;
    int64_t _nextMs=0, _faultEndMs=0;
    std::string _faultKind;
};

class PulseSim : public Sim {
public:
    PulseSim(std::string id, std::string sn, PulseCfg c);
    void tick(int64_t now) override;
private:
    PulseCfg _c;
    bool _on=false;
    int64_t _pulseEnd=0, _nextMs=0;
};

class PedSim : public Sim {
public:
    PedSim(std::string id, std::string sn, PedCfg c);
    void tick(int64_t now) override;
private:
    PedCfg _c;
    bool _latched=false;
    int64_t _nextPressMs=0, _svcMs=0;
    int _pressCount=0;
};

class EmergSim : public Sim {
public:
    EmergSim(std::string id, std::string sn, PreemptCfg c);
    void tick(int64_t now) override;
    void injectFault(const std::string& type) override;
private:
    PreemptCfg _c;
    bool _on=false;
    int64_t _nextMs=0, _holdEnd=0;
};

class RadarSim : public Sim {
public:
    RadarSim(std::string id, std::string sn, RadarCfg c);
    void tick(int64_t now) override;
private:
    RadarCfg _c;
    int64_t _nextMs=0, _tranMs=0;
    bool _present=false;
    float _spd=0.f, _occ=0.f;
};

class CamSim : public Sim {
public:
    CamSim(std::string id, std::string sn, CamCfg c);
    void tick(int64_t now) override;
    void injectFault(const std::string& type) override;
private:
    CamCfg _c;
    int64_t _nextMs=0, _tranMs=0, _degradEnd=0;
    bool _degraded=false;
    int _cnt=0;
    float _occ=0.f;
};

class TransitSim : public Sim {
public:
    TransitSim(std::string id, std::string sn, TransitCfg c);
    void tick(int64_t now) override;
private:
    TransitCfg _c;
    int64_t _nextMs=0, _activeEnd=0;
    bool _active=false;
    int _vid=1000;
};

class GpsSim : public Sim {
public:
    GpsSim(std::string id, std::string sn);
    void tick(int64_t now) override;
    void injectFault(const std::string& type) override;
private:
    int64_t _nextMs=0, _dropEnd=0, _lastEmitMs=0;
    bool _dropped=false;
};

class PowerSim : public Sim {
public:
    PowerSim(std::string id, std::string sn, PowerCfg c);
    void tick(int64_t now) override;
    void injectFault(const std::string& type) override;
private:
    PowerCfg _c;
    int64_t _nextMs=0, _outEnd=0;
    bool _out=false;
};

class TempSim : public Sim {
public:
    TempSim(std::string id, std::string sn, TempCfg c);
    void tick(int64_t now) override;
private:
    TempCfg _c;
    int64_t _nextMs=0, _driftMs=0;
    float _t, _dir=1.f;
};

class HumSim : public Sim {
public:
    HumSim(std::string id, std::string sn);
    void tick(int64_t now) override;
private:
    int64_t _nextMs=0;
    float _rh=35.f;
};

class DoorSim : public Sim {
public:
    DoorSim(std::string id, std::string sn);
    void tick(int64_t now) override;
    void injectFault(const std::string& type) override;
private:
    bool _open=false;
    int64_t _nextMs=0, _closeMs=0;
};

class MmuSim : public Sim {
public:
    MmuSim(std::string id, std::string sn, double tripProbPerTick=0.000005);
    void tick(int64_t now) override;
    void injectFault(const std::string& type) override;
private:
    double _tripP;
    bool _tripped=false;
    int64_t _resetMs=0, _nextMs=0;
};

class SyncSim : public Sim {
public:
    SyncSim(std::string id, std::string sn, SyncCfg c);
    void tick(int64_t now) override;
private:
    SyncCfg _c;
    int64_t _nextMs=0, _lastEmitMs=0;
    float _off=0.f;
};
