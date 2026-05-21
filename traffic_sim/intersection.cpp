// 7th Street & Camelback Road – Phoenix, AZ
// Large signalized intersection: ~70 simulated inputs
#include "intersection.h"
using UP = std::unique_ptr<Sim>;

static UP mkPres(std::string id, std::string sn, double arr,
                 int pLo=200, int pHi=3500, double faultP=0.0002) {
    PresenceCfg c; c.arrPerMin=arr; c.presLo=pLo; c.presHi=pHi; c.faultP=faultP;
    return std::make_unique<PresenceSim>(id,sn,c);
}
static UP mkPulse(std::string id, std::string sn, double arr) {
    PulseCfg c; c.arrPerMin=arr;
    return std::make_unique<PulseSim>(id,sn,c);
}
static UP mkPed(std::string id, std::string sn, double rate=1.5) {
    PedCfg c; c.pressPerMin=rate;
    return std::make_unique<PedSim>(id,sn,c);
}
static UP mkEmerg(std::string id, std::string sn, double rate=3.0) {
    PreemptCfg c; c.ratePerHr=rate;
    return std::make_unique<EmergSim>(id,sn,c);
}
static UP mkRadar(std::string id, std::string sn, double arr=18) {
    RadarCfg c; c.arrPerMin=arr;
    return std::make_unique<RadarSim>(id,sn,c);
}
static UP mkCam(std::string id, std::string sn, double arr=25) {
    CamCfg c; c.arrPerMin=arr;
    return std::make_unique<CamSim>(id,sn,c);
}
static UP mkTransit(std::string id, std::string sn, std::string appr, double rate=15) {
    TransitCfg c; c.approach=appr; c.ratePerHr=rate;
    return std::make_unique<TransitSim>(id,sn,c);
}
static UP mkSync(std::string id, std::string sn, std::string nbr) {
    SyncCfg c; c.neighbor=nbr;
    return std::make_unique<SyncSim>(id,sn,c);
}

// ── Mode parameter tables ──────────────────────────────────────────────────────
struct ModeParams {
    // vehicle arrivals/min
    double nb_thru, sb_thru, eb_thru, wb_thru;
    double nb_lt, sb_lt, eb_lt, wb_lt;
    double nb_rt, sb_rt, eb_rt, wb_rt;
    double bike;
    // ped presses/min
    double ped_main, ped_side;
    // emergency preempts/hr
    double emrg_main, emrg_cross;
    // transit requests/hr
    double tsp_ns, tsp_ew;
    // sensor arrivals/min
    double radar_main, radar_cross, cam_main, cam_cross;
    // fault probability multiplier (applied to base 0.0002)
    double faultMult;
};

static const ModeParams DEFAULT_PARAMS = {
    // NB/SB through, EB/WB through
    20, 20, 14, 14,
    // LT NB/SB/EB/WB
    4,  4,  3,  3,
    // RT NB/SB/EB/WB
    5,  5,  4,  4,
    // bike
    1.0,
    // ped main/side
    1.5, 1.2,
    // emrg main/cross
    3.0, 2.0,
    // tsp ns/ew
    18, 12,
    // radar main/cross, cam main/cross
    22, 14, 30, 20,
    // faultMult
    1.0
};

static const ModeParams NORMAL1_PARAMS = {
    // NB/SB through, EB/WB through  (moderate mid-day traffic)
    10, 10, 7, 7,
    // LT NB/SB/EB/WB
    2,  2,  1.5, 1.5,
    // RT NB/SB/EB/WB
    3,  3,  2,  2,
    // bike
    0.4,
    // ped main/side  (less frequent presses)
    0.8, 0.6,
    // emrg main/cross  (very infrequent – a few per hour citywide)
    0.5, 0.3,
    // tsp ns/ew  (Route 7 ~every 12min, Rte 50 ~every 15min)
    5, 4,
    // radar main/cross, cam main/cross
    10, 7, 15, 12,
    // faultMult  (near-zero faults – everything working normally)
    0.05
};

std::vector<UP> createIntersection(SimMode mode) {
    std::vector<UP> v;
    const ModeParams& p = (mode == SimMode::NORMAL1) ? NORMAL1_PARAMS : DEFAULT_PARAMS;
    const double fp = 0.0002 * p.faultMult;

    // ── VEHICLE DETECTION ─────────────────────────────────────────────────────
    // Northbound  (7th St NB – main arterial)
    v.push_back(mkPres ("NB_T1",   "NB Thru Lane 1",    p.nb_thru, 200,3500, fp));
    v.push_back(mkPres ("NB_T2",   "NB Thru Lane 2",    p.nb_thru, 200,3500, fp));
    v.push_back(mkPres ("NB_T3",   "NB Thru Lane 3",    p.nb_thru*0.9, 200,3500, fp));
    v.push_back(mkPres ("NB_LT1",  "NB Left Turn L1",   p.nb_lt,   600,8000, fp));
    v.push_back(mkPres ("NB_LT2",  "NB Left Turn L2",   p.nb_lt*0.75, 600,8000, fp));
    v.push_back(mkPres ("NB_RT1",  "NB Right Turn",     p.nb_rt,   200,2000, fp));
    v.push_back(mkPulse("NB_A1",   "NB Advance Ln1",    p.nb_thru));
    v.push_back(mkPulse("NB_A2",   "NB Advance Ln2",    p.nb_thru));
    v.push_back(mkPulse("NB_A3",   "NB Advance Ln3",    p.nb_thru*0.9));
    v.push_back(mkPres ("NB_BK",   "NB Bike Lane",      p.bike,    5000,30000, fp));

    // Southbound
    v.push_back(mkPres ("SB_T1",   "SB Thru Lane 1",    p.sb_thru, 200,3500, fp));
    v.push_back(mkPres ("SB_T2",   "SB Thru Lane 2",    p.sb_thru, 200,3500, fp));
    v.push_back(mkPres ("SB_T3",   "SB Thru Lane 3",    p.sb_thru*0.9, 200,3500, fp));
    v.push_back(mkPres ("SB_LT1",  "SB Left Turn L1",   p.sb_lt,   600,8000, fp));
    v.push_back(mkPres ("SB_LT2",  "SB Left Turn L2",   p.sb_lt*0.75, 600,8000, fp));
    v.push_back(mkPres ("SB_RT1",  "SB Right Turn",     p.sb_rt,   200,2000, fp));
    v.push_back(mkPulse("SB_A1",   "SB Advance Ln1",    p.sb_thru));
    v.push_back(mkPulse("SB_A2",   "SB Advance Ln2",    p.sb_thru));
    v.push_back(mkPulse("SB_A3",   "SB Advance Ln3",    p.sb_thru*0.9));
    v.push_back(mkPres ("SB_BK",   "SB Bike Lane",      p.bike,    5000,30000, fp));

    // Eastbound  (Camelback Rd EB – cross street)
    v.push_back(mkPres ("EB_T1",   "EB Thru Lane 1",    p.eb_thru, 200,3500, fp));
    v.push_back(mkPres ("EB_T2",   "EB Thru Lane 2",    p.eb_thru, 200,3500, fp));
    v.push_back(mkPres ("EB_T3",   "EB Thru Lane 3",    p.eb_thru*0.85, 200,3500, fp));
    v.push_back(mkPres ("EB_LT1",  "EB Left Turn",      p.eb_lt,   600,8000, fp));
    v.push_back(mkPres ("EB_RT1",  "EB Right Turn",     p.eb_rt,   200,2000, fp));
    v.push_back(mkPulse("EB_A1",   "EB Advance Ln1",    p.eb_thru));
    v.push_back(mkPulse("EB_A2",   "EB Advance Ln2",    p.eb_thru));
    v.push_back(mkPres ("EB_BK",   "EB Bike Lane",      p.bike*0.6, 5000,30000, fp));

    // Westbound
    v.push_back(mkPres ("WB_T1",   "WB Thru Lane 1",    p.wb_thru, 200,3500, fp));
    v.push_back(mkPres ("WB_T2",   "WB Thru Lane 2",    p.wb_thru, 200,3500, fp));
    v.push_back(mkPres ("WB_T3",   "WB Thru Lane 3",    p.wb_thru*0.85, 200,3500, fp));
    v.push_back(mkPres ("WB_LT1",  "WB Left Turn",      p.wb_lt,   600,8000, fp));
    v.push_back(mkPres ("WB_RT1",  "WB Right Turn",     p.wb_rt,   200,2000, fp));
    v.push_back(mkPulse("WB_A1",   "WB Advance Ln1",    p.wb_thru));
    v.push_back(mkPulse("WB_A2",   "WB Advance Ln2",    p.wb_thru));
    v.push_back(mkPres ("WB_BK",   "WB Bike Lane",      p.bike*0.6, 5000,30000, fp));

    // ── PEDESTRIAN (8 buttons – 2 per corner) ────────────────────────────────
    v.push_back(mkPed("PED_NE_NS", "PED NE corner(N-S)", p.ped_main));
    v.push_back(mkPed("PED_NE_EW", "PED NE corner(E-W)", p.ped_side));
    v.push_back(mkPed("PED_NW_NS", "PED NW corner(N-S)", p.ped_main));
    v.push_back(mkPed("PED_NW_EW", "PED NW corner(E-W)", p.ped_side));
    v.push_back(mkPed("PED_SE_NS", "PED SE corner(N-S)", p.ped_side));
    v.push_back(mkPed("PED_SE_EW", "PED SE corner(E-W)", p.ped_side));
    v.push_back(mkPed("PED_SW_NS", "PED SW corner(N-S)", p.ped_side));
    v.push_back(mkPed("PED_SW_EW", "PED SW corner(E-W)", p.ped_side));

    // ── PRIORITY / PREEMPTION ────────────────────────────────────────────────
    v.push_back(mkEmerg  ("EMRG_N",  "Preempt NORTH",      p.emrg_main));
    v.push_back(mkEmerg  ("EMRG_S",  "Preempt SOUTH",      p.emrg_main));
    v.push_back(mkEmerg  ("EMRG_E",  "Preempt EAST",       p.emrg_cross));
    v.push_back(mkEmerg  ("EMRG_W",  "Preempt WEST",       p.emrg_cross));
    v.push_back(mkTransit("TSP_NS",  "Transit NS (Rte 7)",  "N", p.tsp_ns));
    v.push_back(mkTransit("TSP_EW",  "Transit EW (Rte 50)", "E", p.tsp_ew));

    // ── SENSORS ───────────────────────────────────────────────────────────────
    v.push_back(mkRadar("RADAR_NB", "Radar NB appr",     p.radar_main));
    v.push_back(mkRadar("RADAR_SB", "Radar SB appr",     p.radar_main));
    v.push_back(mkRadar("RADAR_EB", "Radar EB appr",     p.radar_cross));
    v.push_back(mkRadar("RADAR_WB", "Radar WB appr",     p.radar_cross));
    v.push_back(mkCam  ("CAM_NB",   "Camera NB zone",    p.cam_main));
    v.push_back(mkCam  ("CAM_SB",   "Camera SB zone",    p.cam_main));
    v.push_back(mkCam  ("CAM_EB",   "Camera EB zone",    p.cam_cross));
    v.push_back(mkCam  ("CAM_WB",   "Camera WB zone",    p.cam_cross));

    // ── SYSTEM ────────────────────────────────────────────────────────────────
    v.push_back(std::make_unique<GpsSim>  ("GPS",        "GPS Clock"));
    v.push_back(std::make_unique<PowerSim>("AC_MAIN",    "AC Power Main",   PowerCfg{}));
    v.push_back(std::make_unique<TempSim> ("TEMP",       "Cabinet Temp",    TempCfg{}));
    v.push_back(std::make_unique<HumSim>  ("HUMIDITY",   "Cabinet Humidity"));
    v.push_back(std::make_unique<DoorSim> ("DOOR",       "Cabinet Door"));

    // ── SAFETY ────────────────────────────────────────────────────────────────
    double mmuP = 0.000005 * p.faultMult;
    double cmuP = 0.000003 * p.faultMult;
    v.push_back(std::make_unique<MmuSim>("MMU",  "MMU Safety",   mmuP));
    v.push_back(std::make_unique<MmuSim>("CMU",  "Conflict Mon", cmuP));

    // ── COORDINATION ─────────────────────────────────────────────────────────
    v.push_back(mkSync("SYNC_N",  "Sync 7th/Thomas",     "7th/Thomas"));
    v.push_back(mkSync("SYNC_S",  "Sync 7th/Indian Sch", "7th/Indian"));
    v.push_back(mkSync("SYNC_E",  "Sync 5th/Camelback",  "5th/Camelbk"));
    v.push_back(mkSync("SYNC_W",  "Sync 9th/Camelback",  "9th/Camelbk"));
    v.push_back(mkSync("TMC",     "Phoenix TMC Link",    "PhxTMC"));

    return v;
}

std::string injectFaultById(std::vector<std::unique_ptr<Sim>>& sims,
                             const std::string& id, const std::string& type) {
    if (id == "CPU" && type == "CRASH") return "CPU_CRASH";
    if (id == "ALL" && type == "CLEAR") {
        for (auto& s : sims) s->injectFault("CLEAR");
        return "ALL_CLEARED";
    }
    for (auto& s : sims) {
        if (s->id() == id) { s->injectFault(type); return "OK"; }
    }
    return "NOT_FOUND";
}
