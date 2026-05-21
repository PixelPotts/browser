#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <gtk/gtk.h>

extern "C" {
#include "quickjs.h"
}

class Document;
struct AppState;
struct DOMNode;

// ---- Console log entry (for inspector Console tab) ----
enum class ConsoleLevel { LOG, INFO, WARN, ERROR };

struct ConsoleEntry {
    ConsoleLevel level;
    std::string message;
    std::string source;  // filename:line if available
};

class JSEngine {
public:
    JSEngine();
    ~JSEngine();

    void init(AppState* app_state, Document* doc);
    void shutdown();

    // Evaluate JavaScript code
    bool eval(const std::string& code, const std::string& filename = "<script>");

    // Execute pending microtasks/promises
    void executePendingJobs();

    // Timer management
    uint32_t setTimeout(JSValue func, int delay_ms);
    uint32_t setInterval(JSValue func, int interval_ms);
    void clearTimer(uint32_t id);

    // Dispatch an event (calls JS handlers)
    void dispatchEvent(uint32_t node_id, const std::string& type, int clientX = 0, int clientY = 0);

    // Schedule re-render after DOM mutation
    void scheduleRerender();

    // Console log capture
    void addConsoleEntry(ConsoleLevel level, const std::string& msg, const std::string& source = "");
    std::vector<ConsoleEntry> console_log;

    // Callback to notify inspector panel of new entries
    std::function<void()> on_console_entry;

    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    AppState* app_state = nullptr;
    Document* document = nullptr;
    std::string page_url; // current page URL for location object

    // Track currently executing script for document.currentScript
    std::string current_script_src; // empty for inline scripts
    bool has_current_script = false;
    DOMNode* current_script_node = nullptr; // points into DOM tree

private:
    void setupGlobals();
    void setupDocPolyfills();

    // Timer state
    struct TimerEntry {
        uint32_t id;
        JSValue func;
        int interval_ms; // 0 = setTimeout, >0 = setInterval
        guint gtk_source_id;
    };
    std::unordered_map<uint32_t, TimerEntry> timers;
    uint32_t next_timer_id = 1;
    guint job_pump_id = 0; // GLib source for pending jobs pump
    guint rerender_idle_id = 0;

    static gboolean timer_callback(gpointer data);
    static gboolean job_pump_callback(gpointer data);
    static gboolean rerender_callback(gpointer data);
};

// Global engine pointer (one per page, set during init)
extern JSEngine* g_js_engine;
