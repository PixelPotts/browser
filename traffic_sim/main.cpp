#include <signal.h>
#include <thread>
#include <atomic>
#include <deque>
#include <mutex>
#include <memory>
#include <vector>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <iostream>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ncurses.h>
#include "simulators.h"
#include "intersection.h"
#include "display.h"

static const char* FAULT_FIFO = "/tmp/traffic_sim_faults";

static std::atomic<bool> g_running{true};
static void onSig(int) { g_running = false; }

static std::string jsonEsc(const std::string& s) {
    std::string r; r.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if      (c == '"')  r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c < 0x20)  {}
        else r += c;
    }
    return r;
}

int main(int argc, char* argv[]) {
    signal(SIGINT,  onSig);
    signal(SIGTERM, onSig);
    signal(SIGPIPE, SIG_IGN);

    bool webMode = false;
    SimMode mode = SimMode::NORMAL1;
    for (int a = 1; a < argc; a++) {
        if (std::strcmp(argv[a], "--web") == 0) webMode = true;
        if (std::strcmp(argv[a], "--mode") == 0 && a+1 < argc) {
            ++a;
            if (std::strcmp(argv[a], "default") == 0) mode = SimMode::DEFAULT;
            // "normal1" is already the default
        }
    }

    auto sims  = createIntersection(mode);
    int64_t t0 = nowMs();

    // ── Fault FIFO setup ──────────────────────────────────────────────────────
    ::unlink(FAULT_FIFO);
    ::mkfifo(FAULT_FIFO, 0666);
    int faultFd = ::open(FAULT_FIFO, O_RDWR | O_NONBLOCK);

    auto faultReader = [&]() {
        std::string line;
        while (g_running) {
            char c;
            ssize_t n = ::read(faultFd, &c, 1);
            if (n == 1) {
                if (c == '\n' || c == '\r') {
                    if (!line.empty()) {
                        // parse "FAULT <id> <type>"
                        std::istringstream ss(line);
                        std::string cmd, id, type;
                        ss >> cmd >> id >> type;
                        if (cmd == "FAULT" && !id.empty() && !type.empty()) {
                            std::string result = injectFaultById(sims, id, type);
                            if (result == "CPU_CRASH") {
                                std::cerr << "[fault] CPU CRASH injected\n";
                                g_running = false;
                            }
                        }
                        line.clear();
                    }
                } else {
                    line += c;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    };
    std::thread faultThread(faultReader);

    // ── WEB MODE ──────────────────────────────────────────────────────────────
    if (webMode) {
        std::vector<std::string> tickEvts;

        for (auto& sim : sims) {
            sim->setEvtCb([&](const std::string& id, const std::string& msg) {
                auto wt = std::chrono::system_clock::now();
                auto tt = std::chrono::system_clock::to_time_t(wt);
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              wt.time_since_epoch()).count() % 1000;
                struct tm tb; localtime_r(&tt, &tb);
                char line[256];
                snprintf(line, sizeof(line), "%02d:%02d:%02d.%03lld  %-14s  %s",
                    tb.tm_hour, tb.tm_min, tb.tm_sec, (long long)ms,
                    id.c_str(), msg.c_str());
                tickEvts.push_back(line);
            });
        }

        while (g_running) {
            tickEvts.clear();
            int64_t t = nowMs();
            for (auto& s : sims) { s->tick(t); s->flushEvents(); }

            // JSON: {"t":T,"i":[[id,a,f,d,"summary",v,cat],...],"e":["..."]}
            std::cout << "{\"t\":" << t << ",\"i\":[";
            for (int i = 0; i < (int)sims.size(); i++) {
                auto s = sims[i]->getState();
                if (i) std::cout << ',';
                char vbuf[16]; snprintf(vbuf, sizeof(vbuf), "%.1f", s.analogV);
                std::cout << "[\"" << s.id << "\","
                          << (s.active?1:0) << ','
                          << (s.fault?1:0) << ','
                          << (s.digital?1:0) << ",\""
                          << jsonEsc(s.summary) << "\","
                          << vbuf << ','
                          << (int)s.cat << ']';
            }
            std::cout << "],\"e\":[";
            for (int i = 0; i < (int)tickEvts.size(); i++) {
                if (i) std::cout << ',';
                std::cout << '"' << jsonEsc(tickEvts[i]) << '"';
            }
            std::cout << "]}\n";
            std::cout.flush();

            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 10 fps
        }
        faultThread.join();
        ::close(faultFd); ::unlink(FAULT_FIFO);
        return 0;
    }

    // ── NCURSES MODE ──────────────────────────────────────────────────────────
    std::deque<std::string> eventLog;
    std::mutex              logMx;

    for (auto& sim : sims) {
        sim->setEvtCb([&](const std::string& id, const std::string& msg) {
            auto wt  = std::chrono::system_clock::now();
            auto tt  = std::chrono::system_clock::to_time_t(wt);
            auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                           wt.time_since_epoch()).count() % 1000;
            struct tm tb; localtime_r(&tt, &tb);
            char line[256];
            snprintf(line, sizeof(line), "%02d:%02d:%02d.%03lld  %-14s  %s",
                tb.tm_hour, tb.tm_min, tb.tm_sec, (long long)ms,
                id.c_str(), msg.c_str());
            std::lock_guard<std::mutex> lk(logMx);
            eventLog.push_back(line);
            if (eventLog.size() > 2000) eventLog.pop_front();
        });
    }

    std::thread simThread([&]() {
        while (g_running) {
            int64_t t = nowMs();
            for (auto& sim : sims) { sim->tick(t); sim->flushEvents(); }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    initDisplay();
    int leftScroll = 0, rightScroll = -1;

    while (g_running) {
        std::vector<State> states;
        states.reserve(sims.size());
        for (auto& sim : sims) states.push_back(sim->getState());

        std::vector<std::string> logSnap;
        { std::lock_guard<std::mutex> lk(logMx); logSnap.assign(eventLog.begin(), eventLog.end()); }

        renderDisplay(states, logSnap, leftScroll, rightScroll, t0);

        switch (getKey()) {
            case 'q': case 'Q': g_running = false; break;
            case KEY_UP:    leftScroll = std::max(0, leftScroll - 1); break;
            case KEY_DOWN:  leftScroll++;                              break;
            case KEY_PPAGE: leftScroll = std::max(0, leftScroll - 10); break;
            case KEY_NPAGE: leftScroll += 10;                          break;
            case 'j': { int sz=(int)logSnap.size();
                        if (rightScroll<0) rightScroll=std::max(0,sz-1);
                        rightScroll=std::max(0,rightScroll-1); break; }
            case 'k': rightScroll++; break;
            case 'f': rightScroll = -1; break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    cleanupDisplay();
    simThread.join();
    faultThread.join();
    ::close(faultFd); ::unlink(FAULT_FIFO);
    return 0;
}
