// fault_injector.cpp – Hardware Fault Injection Terminal
// Communicates with traffic_sim via named pipe (simulated RS-232 wire).
// Usage: ./fault_injector [--pipe /tmp/traffic_sim_faults] [--serial /dev/ttyS0]
#include <ncurses.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <string>
#include <deque>
#include <vector>

static const char* DEFAULT_PIPE = "/tmp/traffic_sim_faults";

// ── Color pairs ──────────────────────────────────────────────────────────────
#define CP_HDR_A  1   // header flash state A
#define CP_HDR_B  2   // header flash state B
#define CP_LED_OK 3
#define CP_LED_NO 4
#define CP_SECT   5
#define CP_KEY    6
#define CP_DESC   7
#define CP_LOG    8
#define CP_DANGER 9
#define CP_INJECT 10
#define CP_DIM    11
#define CP_BORDER 12

static int  g_fd   = -1;
static bool g_conn = false;
static std::string g_pipePath;

struct FaultCmd {
    char key;
    const char* id;
    const char* type;
    const char* label;
    bool danger;
};

// ── Fault command table ───────────────────────────────────────────────────────
static const FaultCmd CMDS[] = {
    // Vehicle detection
    {'1', "NB_T1",    "OPEN",   "NB_T1   open circuit",  false},
    {'2', "NB_T2",    "SHORT",  "NB_T2   short/stuck",   false},
    {'3', "SB_T1",    "OPEN",   "SB_T1   open circuit",  false},
    {'4', "SB_T2",    "STUCK",  "SB_T2   stuck on",      false},
    {'5', "EB_T1",    "OPEN",   "EB_T1   open circuit",  false},
    {'6', "WB_T1",    "SHORT",  "WB_T1   short circuit", false},
    // Sensors
    {'r', "RADAR_NB", "LOSS",   "RADAR_NB comms loss",   false},
    {'v', "CAM_NB",   "DEGRAD", "CAM_NB  degraded",      false},
    {'e', "RADAR_EB", "LOSS",   "RADAR_EB comms loss",   false},
    {'w', "CAM_WB",   "DEGRAD", "CAM_WB  degraded",      false},
    // Pedestrian
    {'p', "PED_NE_NS","STUCK",  "PED_NE_NS stuck btn",   false},
    {'k', "PED_SW_EW","STUCK",  "PED_SW_EW stuck btn",   false},
    // System / power
    {'7', "AC_MAIN",  "BROWN",  "AC_MAIN brownout 89V",  false},
    {'8', "AC_MAIN",  "OUTAGE", "AC_MAIN power outage",  true },
    {'9', "GPS",      "LOSS",   "GPS     signal loss",   false},
    {'0', "DOOR",     "OPEN",   "DOOR    cabinet open",  false},
    // Emergency (priority)
    {'!', "EMRG_N",   "ACTIVE", "EMRG_N  NB preempt",    true },
    {'@', "EMRG_S",   "ACTIVE", "EMRG_S  SB preempt",    true },
    // Safety
    {'m', "MMU",      "TRIP",   "MMU     conflict TRIP", true },
    {'n', "CMU",      "TRIP",   "CMU     conflict TRIP", true },
    // Control
    {'a', "ALL",      "CLEAR",  "CLEAR ALL faults",      false},
    {'X', "CPU",      "CRASH",  "CPU CRASH (kill proc)", true },
};
static const int NCMDS = (int)(sizeof(CMDS)/sizeof(CMDS[0]));

static std::deque<std::string> g_log;
static int64_t g_lastInject = 0;

static int64_t msNow() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static void logAdd(const std::string& s) {
    g_log.push_front(s);
    if ((int)g_log.size() > 40) g_log.pop_back();
}

static bool tryConnect() {
    if (g_fd >= 0) return true;
    int fd = open(g_pipePath.c_str(), O_WRONLY | O_NONBLOCK);
    if (fd < 0) return false;
    // Switch to blocking for writes
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    g_fd   = fd;
    g_conn = true;
    return true;
}

static void sendFault(const FaultCmd& c) {
    if (!g_conn && !tryConnect()) {
        logAdd("ERR: not connected");
        return;
    }
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "FAULT %s %s\n", c.id, c.type);
    ssize_t w = write(g_fd, buf, n);
    if (w < 0) {
        close(g_fd); g_fd = -1; g_conn = false;
        logAdd("ERR: write failed (disconnected)");
        return;
    }
    char ts[12];
    time_t t = time(nullptr);
    struct tm tb; localtime_r(&t, &tb);
    snprintf(ts, sizeof(ts), "%02d:%02d:%02d", tb.tm_hour, tb.tm_min, tb.tm_sec);
    logAdd(std::string(ts) + "  " + c.id + "  " + c.type);
    g_lastInject = msNow();
}

// ── Rendering ─────────────────────────────────────────────────────────────────
static void render(int frame) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();

    bool flash = (frame / 6) % 2 == 0;   // ~1.5 Hz flash
    bool injPulse = (msNow() - g_lastInject) < 800;

    // ── Header ────────────────────────────────────────────────────────────────
    int hdrCp = flash ? CP_HDR_A : CP_HDR_B;
    attron(COLOR_PAIR(hdrCp) | A_BOLD);
    mvhline(0, 0, ' ', cols);
    mvprintw(0, 2, "[!]  HARDWARE FAULT INJECTOR  v1.0");
    attroff(COLOR_PAIR(hdrCp) | A_BOLD);

    // Connection LED
    int ledCp = g_conn ? CP_LED_OK : (flash ? CP_LED_NO : CP_DIM);
    attron(COLOR_PAIR(ledCp) | A_BOLD);
    mvprintw(0, 38, g_conn ? " LIVE " : " WAIT ");
    attroff(COLOR_PAIR(ledCp) | A_BOLD);

    // Inject flash
    if (injPulse) {
        attron(COLOR_PAIR(CP_INJECT) | A_BOLD | A_BLINK);
        mvprintw(0, 45, " INJECTING ");
        attroff(COLOR_PAIR(CP_INJECT) | A_BOLD | A_BLINK);
    }

    // Timestamp
    char ts[12]; time_t t = time(nullptr); struct tm tb; localtime_r(&t,&tb);
    snprintf(ts,sizeof(ts),"%02d:%02d:%02d",tb.tm_hour,tb.tm_min,tb.tm_sec);
    attron(COLOR_PAIR(CP_DIM));
    mvprintw(0, cols-10, "%s", ts);
    attroff(COLOR_PAIR(CP_DIM));

    // ── Separator ─────────────────────────────────────────────────────────────
    attron(COLOR_PAIR(CP_BORDER));
    mvhline(1, 0, ACS_HLINE, cols);
    // two-column split
    int splitX = cols / 2;
    for (int r = 2; r < rows-6; r++) mvaddch(r, splitX, ACS_VLINE);
    attroff(COLOR_PAIR(CP_BORDER));

    // ── Left column: vehicle / sensors / ped ──────────────────────────────────
    int row = 2;
    auto sect = [&](int col, const char* title) {
        attron(COLOR_PAIR(CP_SECT) | A_BOLD);
        mvprintw(row++, col+1, " %s ", title);
        attroff(COLOR_PAIR(CP_SECT) | A_BOLD);
    };
    auto cmd_row = [&](int col, const FaultCmd& c) {
        int cp = c.danger ? CP_DANGER : CP_KEY;
        attron(COLOR_PAIR(cp) | A_BOLD);
        mvprintw(row, col+1, "[%c]", c.key);
        attroff(COLOR_PAIR(cp) | A_BOLD);
        attron(COLOR_PAIR(CP_DESC));
        mvprintw(row, col+5, "%s", c.label);
        attroff(COLOR_PAIR(CP_DESC));
        row++;
    };

    // Left: vehicle
    sect(0, "VEHICLE DETECTION");
    for (int i=0;i<6;i++) cmd_row(0, CMDS[i]);
    row++;
    sect(0, "SENSORS");
    for (int i=6;i<10;i++) cmd_row(0, CMDS[i]);
    row++;
    sect(0, "PEDESTRIAN");
    for (int i=10;i<12;i++) cmd_row(0, CMDS[i]);

    // Right column
    int rightRow = 2;
    auto rsect = [&](const char* title) {
        attron(COLOR_PAIR(CP_SECT) | A_BOLD);
        mvprintw(rightRow++, splitX+1, " %s ", title);
        attroff(COLOR_PAIR(CP_SECT) | A_BOLD);
    };
    auto rcmd = [&](const FaultCmd& c) {
        int cp = c.danger ? CP_DANGER : CP_KEY;
        attron(COLOR_PAIR(cp) | A_BOLD);
        mvprintw(rightRow, splitX+2, "[%c]", c.key);
        attroff(COLOR_PAIR(cp) | A_BOLD);
        attron(COLOR_PAIR(CP_DESC));
        mvprintw(rightRow, splitX+6, "%s", c.label + (c.danger ? 0 : 0));
        attroff(COLOR_PAIR(CP_DESC));
        rightRow++;
    };

    rsect("SYSTEM / POWER");
    for (int i=12;i<16;i++) rcmd(CMDS[i]);
    rightRow++;
    rsect("EMERGENCY");
    for (int i=16;i<18;i++) rcmd(CMDS[i]);
    rightRow++;
    rsect("SAFETY");
    for (int i=18;i<20;i++) rcmd(CMDS[i]);
    rightRow++;
    rsect("CONTROL");
    for (int i=20;i<NCMDS;i++) rcmd(CMDS[i]);
    attron(COLOR_PAIR(CP_DIM));
    mvprintw(rightRow++, splitX+2, "[q] quit");
    attroff(COLOR_PAIR(CP_DIM));

    // ── Log panel ─────────────────────────────────────────────────────────────
    int logY = rows - 6;
    attron(COLOR_PAIR(CP_BORDER));
    mvhline(logY, 0, ACS_HLINE, cols);
    attroff(COLOR_PAIR(CP_BORDER));
    attron(COLOR_PAIR(CP_SECT) | A_BOLD);
    mvprintw(logY, 1, " INJECTION LOG ");
    attroff(COLOR_PAIR(CP_SECT) | A_BOLD);

    attron(COLOR_PAIR(CP_DIM));
    mvprintw(logY, cols-30, "  wire: %s ", g_pipePath.c_str());
    attroff(COLOR_PAIR(CP_DIM));

    for (int i = 0; i < 5 && i < (int)g_log.size(); i++) {
        attron(COLOR_PAIR(CP_LOG));
        mvprintw(logY+1+i, 2, "%-*.*s", cols-4, cols-4, g_log[i].c_str());
        attroff(COLOR_PAIR(CP_LOG));
    }

    // ── Bottom status ─────────────────────────────────────────────────────────
    attron(COLOR_PAIR(CP_BORDER));
    mvhline(rows-1, 0, ACS_HLINE, cols);
    attroff(COLOR_PAIR(CP_BORDER));
    attron(COLOR_PAIR(CP_DIM));
    mvprintw(rows-1, 2, " Hardware Fault Injector  –  press key to inject  –  [q] quit ");
    attroff(COLOR_PAIR(CP_DIM));

    refresh();
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    g_pipePath = DEFAULT_PIPE;
    for (int i=1; i<argc; i++) {
        if ((strcmp(argv[i],"--pipe")==0 || strcmp(argv[i],"--serial")==0) && i+1<argc)
            g_pipePath = argv[++i];
    }

    tryConnect();

    initscr(); cbreak(); noecho();
    keypad(stdscr, TRUE); nodelay(stdscr, TRUE); curs_set(0);
    if (has_colors()) {
        start_color(); use_default_colors();
        init_pair(CP_HDR_A,  COLOR_BLACK,   COLOR_CYAN);
        init_pair(CP_HDR_B,  COLOR_BLACK,   COLOR_YELLOW);
        init_pair(CP_LED_OK, COLOR_GREEN,   -1);
        init_pair(CP_LED_NO, COLOR_RED,     -1);
        init_pair(CP_SECT,   COLOR_BLACK,   COLOR_BLUE);
        init_pair(CP_KEY,    COLOR_CYAN,    -1);
        init_pair(CP_DESC,   COLOR_WHITE,   -1);
        init_pair(CP_LOG,    COLOR_GREEN,   -1);
        init_pair(CP_DANGER, COLOR_RED,     -1);
        init_pair(CP_INJECT, COLOR_BLACK,   COLOR_YELLOW);
        init_pair(CP_DIM,    8,             -1);
        init_pair(CP_BORDER, COLOR_BLUE,    -1);
    }

    bool running = true;
    int  frame   = 0;
    int64_t nextConn = 0;

    while (running) {
        frame++;

        // Periodic reconnect attempt
        if (!g_conn && msNow() > nextConn) {
            tryConnect();
            nextConn = msNow() + 1000;
        }

        render(frame);

        int ch = getch();
        if (ch == 'q' || ch == 'Q') { running = false; break; }

        for (int i=0; i<NCMDS; i++) {
            if (ch == CMDS[i].key) { sendFault(CMDS[i]); break; }
        }

        napms(50);  // ~20 fps
    }

    endwin();
    if (g_fd >= 0) close(g_fd);
    return 0;
}
