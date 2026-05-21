#include "display.h"
#include <ncurses.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <algorithm>
#include <cmath>

// Color pair IDs
#define CP_DEFAULT  1
#define CP_ACTIVE   2
#define CP_FAULT    3
#define CP_WARN     4
#define CP_PACKET   5
#define CP_PRIOR    6
#define CP_HEADER   7
#define CP_DIM      8
#define CP_SAFE     9
#define CP_BORDER  10

void initDisplay() {
    initscr(); cbreak(); noecho();
    keypad(stdscr, TRUE); nodelay(stdscr, TRUE); curs_set(0);
    if (has_colors()) {
        start_color(); use_default_colors();
        init_pair(CP_DEFAULT, COLOR_WHITE,   -1);
        init_pair(CP_ACTIVE,  COLOR_GREEN,   -1);
        init_pair(CP_FAULT,   COLOR_RED,     -1);
        init_pair(CP_WARN,    COLOR_YELLOW,  -1);
        init_pair(CP_PACKET,  COLOR_CYAN,    -1);
        init_pair(CP_PRIOR,   COLOR_MAGENTA, -1);
        init_pair(CP_HEADER,  COLOR_BLACK,   COLOR_CYAN);
        init_pair(CP_DIM,     8,             -1);   // dark gray
        init_pair(CP_SAFE,    COLOR_RED,     -1);
        init_pair(CP_BORDER,  COLOR_BLUE,    -1);
    }
}
void cleanupDisplay() { endwin(); }
int  getKey()         { return getch(); }

// ── Helpers ───────────────────────────────────────────────────────────────────
static const char* catLabel(Category c) {
    switch(c) {
        case Category::VEHICLE:    return "VEHICLE DETECTION";
        case Category::PEDESTRIAN: return "PEDESTRIAN";
        case Category::PRIORITY:   return "PRIORITY / PREEMPTION";
        case Category::SENSOR:     return "SENSORS  (RADAR / VIDEO)";
        case Category::SYSTEM:     return "SYSTEM";
        case Category::SAFETY:     return "SAFETY SYSTEMS";
        case Category::COORD:      return "COORDINATION";
        default:                   return "OTHER";
    }
}

static int rowColor(const State& s) {
    if (s.fault) return CP_FAULT;
    switch (s.cat) {
        case Category::VEHICLE:    return s.active ? CP_ACTIVE  : CP_DIM;
        case Category::PEDESTRIAN: return s.active ? CP_ACTIVE  : CP_DIM;
        case Category::PRIORITY:   return s.active ? CP_PRIOR   : CP_DIM;
        case Category::SENSOR:     return s.active ? CP_PACKET  : CP_DIM;
        case Category::SYSTEM:     return s.active ? CP_WARN    : CP_DEFAULT;
        case Category::SAFETY:     return s.active ? CP_FAULT   : CP_DEFAULT;
        case Category::COORD:      return s.active ? CP_PACKET  : CP_DIM;
        default:                   return CP_DEFAULT;
    }
}

static std::string fmtAge(int64_t changedMs, int64_t now) {
    int64_t a = now - changedMs;
    if (a < 0) a = 0;
    if (a < 1000) { char b[8]; snprintf(b,sizeof(b),"%lldms",(long long)a); return b; }
    if (a < 60000) { char b[8]; snprintf(b,sizeof(b),"%.1fs",a/1000.0); return b; }
    char b[20]; snprintf(b,sizeof(b),"%lldm",(long long)(a/60000)); return b;
}

static const char* icon(const State& s) {
    if (s.fault)                         return "[!]";
    if (s.sig == SigType::PACKET)        return s.active ? "[~]" : "[ ]";
    if (s.sig == SigType::ANALOG_SIG)    return "[=]";
    if (s.cat == Category::PEDESTRIAN && s.digital) return "[@]";
    return s.digital ? "[*]" : "[ ]";
}

// ── Render ────────────────────────────────────────────────────────────────────
void renderDisplay(
    const std::vector<State>& states,
    const std::vector<std::string>& log,
    int leftScroll, int& rightScroll, int64_t startMs)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    if (rows < 8 || cols < 60) {
        erase(); mvprintw(0,0,"Terminal too small (need 60x8)"); refresh(); return;
    }

    const int leftW  = 47;
    const int rightW = cols - leftW - 1;
    const int content = rows - 3;   // header(1) + subhdr(1) + status(1)
    const int64_t now = nowMs();

    erase();

    // ── Title bar ─────────────────────────────────────────────────────────────
    attron(COLOR_PAIR(CP_HEADER)|A_BOLD);
    mvhline(0, 0, ' ', cols);
    mvprintw(0, 1, " 7TH ST & CAMELBACK RD  PHOENIX AZ  [%d INPUTS]", (int)states.size());
    auto wt = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(wt);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(wt.time_since_epoch()).count() % 1000;
    struct tm tb; localtime_r(&tt, &tb);
    char ts[24]; snprintf(ts,sizeof(ts),"%02d:%02d:%02d.%03lld",
        tb.tm_hour, tb.tm_min, tb.tm_sec, (long long)ms);
    mvprintw(0, cols - (int)strlen(ts) - 2, "%s", ts);
    attroff(COLOR_PAIR(CP_HEADER)|A_BOLD);

    // ── Column headers ────────────────────────────────────────────────────────
    attron(COLOR_PAIR(CP_BORDER)|A_BOLD);
    mvprintw(1, 1,         "INPUTS  [UP/DN/PgUp/PgDn]");
    mvprintw(1, leftW + 2, "REAL-TIME OUTPUT  [j/k scroll  f=follow  q=quit]");
    mvhline(2, 0, ACS_HLINE, leftW);
    mvaddch(2, leftW, ACS_PLUS);
    mvhline(2, leftW + 1, ACS_HLINE, rightW);
    for (int r = 1; r < rows - 1; r++) mvaddch(r, leftW, ACS_VLINE);
    attroff(COLOR_PAIR(CP_BORDER)|A_BOLD);

    // ── Build left panel rows ─────────────────────────────────────────────────
    struct Row { bool hdr; int cp; bool bold; std::string txt; };
    std::vector<Row> lrows;
    lrows.reserve(states.size() + 8);

    Category prevCat = (Category)(-1);
    for (const auto& s : states) {
        if (s.cat != prevCat) {
            prevCat = s.cat;
            char hbuf[64];
            snprintf(hbuf, sizeof(hbuf), " %s ", catLabel(s.cat));
            lrows.push_back({true, CP_HEADER, true, hbuf});
        }
        int cp   = rowColor(s);
        bool bld = s.active || s.fault;
        std::string age = fmtAge(s.changedMs, now);
        char line[80];
        snprintf(line, sizeof(line), " %s %-20.20s %-12.12s %6s",
                 icon(s), s.sname.c_str(), s.summary.c_str(), age.c_str());
        lrows.push_back({false, cp, bld, line});
    }

    int maxLS = std::max(0, (int)lrows.size() - content);
    if (leftScroll > maxLS) leftScroll = maxLS;

    for (int r = 0; r < content && (r + leftScroll) < (int)lrows.size(); r++) {
        const auto& row = lrows[r + leftScroll];
        int y = r + 3;
        int attr = COLOR_PAIR(row.cp) | (row.bold ? A_BOLD : 0);
        attron(attr);
        mvhline(y, 0, ' ', leftW);
        if (row.hdr) {
            mvprintw(y, 0, "%s", row.txt.c_str());
            int used = (int)row.txt.size();
            attron(A_DIM);
            for (int c = used; c < leftW; c++) mvaddch(y, c, ACS_HLINE);
            attroff(A_DIM);
        } else {
            mvprintw(y, 0, "%-*.*s", leftW, leftW, row.txt.c_str());
        }
        attroff(attr);
    }

    // ── Right panel: event log ────────────────────────────────────────────────
    int maxRS = std::max(0, (int)log.size() - content);
    if (rightScroll < 0 || rightScroll > maxRS) rightScroll = maxRS;

    for (int r = 0; r < content; r++) {
        int idx = rightScroll + r;
        if (idx >= (int)log.size()) break;
        const std::string& line = log[idx];
        int cp = CP_DEFAULT;
        if (line.find("FAULT")     != std::string::npos ||
            line.find("TRIP")      != std::string::npos ||
            line.find("OUTAGE")    != std::string::npos) cp = CP_FAULT;
        else if (line.find("EMERGENCY") != std::string::npos ||
                 line.find("PREEMPT")   != std::string::npos) cp = CP_PRIOR;
        else if (line.find("TRANSIT")   != std::string::npos ||
                 line.find("BUS#")      != std::string::npos) cp = CP_PACKET;
        else if (line.find("PRESSED")   != std::string::npos ||
                 line.find("LATCHED")   != std::string::npos) cp = CP_ACTIVE;
        else if (line.find("WARNING")   != std::string::npos ||
                 line.find("BROWNOUT")  != std::string::npos) cp = CP_WARN;
        else if (line.find("pres=")     != std::string::npos ||
                 line.find("cnt=")      != std::string::npos ||
                 line.find("SYNC OK")   != std::string::npos) cp = CP_PACKET;
        attron(COLOR_PAIR(cp));
        int x = leftW + 1;
        mvprintw(r + 3, x, "%-*.*s", rightW, rightW, line.c_str());
        attroff(COLOR_PAIR(cp));
    }

    // ── Status bar ────────────────────────────────────────────────────────────
    int active=0, faults=0;
    for (const auto& s : states) { if(s.active) active++; if(s.fault) faults++; }
    int64_t upS = (now - startMs) / 1000;
    char sbuf[256];
    snprintf(sbuf, sizeof(sbuf),
             " Active:%d/%d  Faults:%d  Events:%d  Uptime:%llds",
             active, (int)states.size(), faults, (int)log.size(), (long long)upS);
    attron(COLOR_PAIR(CP_HEADER)|A_BOLD);
    mvhline(rows-1, 0, ' ', cols);
    mvprintw(rows-1, 0, "%s", sbuf);
    attroff(COLOR_PAIR(CP_HEADER)|A_BOLD);

    refresh();
}
