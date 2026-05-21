#include "js_engine.h"
#include "dom.h"
#include <cstdio>

extern "C" {
#include "quickjs.h"
}

JSEngine* g_js_engine = nullptr;

// ---- console.log / console.warn / console.error ----

static JSValue js_console_log(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    for (int i = 0; i < argc; i++) {
        if (i > 0) printf(" ");
        const char* str = JS_ToCString(ctx, argv[i]);
        if (str) { printf("%s", str); JS_FreeCString(ctx, str); }
    }
    printf("\n");
    return JS_UNDEFINED;
}

static JSValue js_console_warn(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    fprintf(stderr, "[WARN] ");
    for (int i = 0; i < argc; i++) {
        if (i > 0) fprintf(stderr, " ");
        const char* str = JS_ToCString(ctx, argv[i]);
        if (str) { fprintf(stderr, "%s", str); JS_FreeCString(ctx, str); }
    }
    fprintf(stderr, "\n");
    return JS_UNDEFINED;
}

static JSValue js_console_error(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    fprintf(stderr, "[ERROR] ");
    for (int i = 0; i < argc; i++) {
        if (i > 0) fprintf(stderr, " ");
        const char* str = JS_ToCString(ctx, argv[i]);
        if (str) { fprintf(stderr, "%s", str); JS_FreeCString(ctx, str); }
    }
    fprintf(stderr, "\n");
    return JS_UNDEFINED;
}

// ---- alert() ----

static JSValue js_alert(JSContext* ctx, JSValueConst this_val,
                         int argc, JSValueConst* argv) {
    const char* msg = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
    if (!g_js_engine || !g_js_engine->app_state) {
        printf("[alert] %s\n", msg ? msg : "");
        if (msg) JS_FreeCString(ctx, msg);
        return JS_UNDEFINED;
    }

    // Get the window from AppState
    struct AlertData { GtkWidget* window; std::string message; };
    // We need to cast through the header - but AppState is forward declared
    // We'll use a simpler approach - just print to stdout for now and show GTK dialog
    printf("[alert] %s\n", msg ? msg : "");

    // GTK dialog (must be on main thread - we are since JS runs on main thread)
    GtkWidget* window = g_js_engine->app_state ? nullptr : nullptr;
    // We'll get the window pointer through the engine
    // For now, use a message dialog without parent
    GtkWidget* dialog = gtk_message_dialog_new(
        nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "%s", msg ? msg : "");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (msg) JS_FreeCString(ctx, msg);
    return JS_UNDEFINED;
}

// ---- setTimeout / setInterval / clearTimeout / clearInterval ----

static JSValue js_set_timeout(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 1 || !g_js_engine) return JS_UNDEFINED;
    JSValue func = argv[0];
    if (!JS_IsFunction(ctx, func)) return JS_UNDEFINED;
    int delay = 0;
    if (argc >= 2) JS_ToInt32(ctx, &delay, argv[1]);
    if (delay < 0) delay = 0;

    uint32_t id = g_js_engine->setTimeout(JS_DupValue(ctx, func), delay);
    return JS_NewInt32(ctx, (int32_t)id);
}

static JSValue js_set_interval(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    if (argc < 1 || !g_js_engine) return JS_UNDEFINED;
    JSValue func = argv[0];
    if (!JS_IsFunction(ctx, func)) return JS_UNDEFINED;
    int interval = 0;
    if (argc >= 2) JS_ToInt32(ctx, &interval, argv[1]);
    if (interval < 1) interval = 1;

    uint32_t id = g_js_engine->setInterval(JS_DupValue(ctx, func), interval);
    return JS_NewInt32(ctx, (int32_t)id);
}

static JSValue js_clear_timeout(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    if (argc < 1 || !g_js_engine) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    g_js_engine->clearTimer((uint32_t)id);
    return JS_UNDEFINED;
}

// ---- JSEngine implementation ----

JSEngine::JSEngine() {}

JSEngine::~JSEngine() {
    shutdown();
}

void JSEngine::init(AppState* as, Document* doc) {
    app_state = as;
    document = doc;
    g_js_engine = this;

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);

    // Set memory limit (64MB)
    JS_SetMemoryLimit(rt, 64 * 1024 * 1024);

    setupGlobals();

    // Start the job pump (16ms interval for microtask execution)
    job_pump_id = g_timeout_add(16, job_pump_callback, this);
}

void JSEngine::shutdown() {
    // Stop job pump
    if (job_pump_id) {
        g_source_remove(job_pump_id);
        job_pump_id = 0;
    }

    // Cancel rerender
    if (rerender_idle_id) {
        g_source_remove(rerender_idle_id);
        rerender_idle_id = 0;
    }

    // Clear all timers
    for (auto& [id, entry] : timers) {
        g_source_remove(entry.gtk_source_id);
        JS_FreeValue(ctx, entry.func);
    }
    timers.clear();

    if (ctx) { JS_FreeContext(ctx); ctx = nullptr; }
    if (rt) { JS_FreeRuntime(rt); rt = nullptr; }

    if (g_js_engine == this) g_js_engine = nullptr;
    app_state = nullptr;
    document = nullptr;
}

void JSEngine::setupGlobals() {
    JSValue global = JS_GetGlobalObject(ctx);

    // console object
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
        JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "warn",
        JS_NewCFunction(ctx, js_console_warn, "warn", 1));
    JS_SetPropertyStr(ctx, console, "error",
        JS_NewCFunction(ctx, js_console_error, "error", 1));
    JS_SetPropertyStr(ctx, console, "info",
        JS_NewCFunction(ctx, js_console_log, "info", 1));
    JS_SetPropertyStr(ctx, global, "console", console);

    // alert
    JS_SetPropertyStr(ctx, global, "alert",
        JS_NewCFunction(ctx, js_alert, "alert", 1));

    // setTimeout / setInterval / clearTimeout / clearInterval
    JS_SetPropertyStr(ctx, global, "setTimeout",
        JS_NewCFunction(ctx, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "setInterval",
        JS_NewCFunction(ctx, js_set_interval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
        JS_NewCFunction(ctx, js_clear_timeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "clearInterval",
        JS_NewCFunction(ctx, js_clear_timeout, "clearInterval", 1));

    JS_FreeValue(ctx, global);
}

bool JSEngine::eval(const std::string& code, const std::string& filename) {
    JSValue result = JS_Eval(ctx, code.c_str(), code.size(),
                              filename.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* str = JS_ToCString(ctx, exc);
        if (str) {
            fprintf(stderr, "[JS Error] %s\n", str);
            JS_FreeCString(ctx, str);
        }
        // Print stack trace if available
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (!JS_IsUndefined(stack)) {
            const char* stack_str = JS_ToCString(ctx, stack);
            if (stack_str) {
                fprintf(stderr, "%s\n", stack_str);
                JS_FreeCString(ctx, stack_str);
            }
        }
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, result);
        return false;
    }
    JS_FreeValue(ctx, result);
    executePendingJobs();
    return true;
}

void JSEngine::executePendingJobs() {
    JSContext* pctx;
    while (JS_ExecutePendingJob(rt, &pctx) > 0) {}
}

uint32_t JSEngine::setTimeout(JSValue func, int delay_ms) {
    uint32_t id = next_timer_id++;
    TimerEntry entry;
    entry.id = id;
    entry.func = func;
    entry.interval_ms = 0; // one-shot

    struct TimerData { JSEngine* engine; uint32_t id; };
    auto* td = new TimerData{this, id};
    entry.gtk_source_id = g_timeout_add(delay_ms, [](gpointer data) -> gboolean {
        auto* td = static_cast<TimerData*>(data);
        auto it = td->engine->timers.find(td->id);
        if (it != td->engine->timers.end()) {
            JSValue ret = JS_Call(td->engine->ctx, it->second.func,
                                  JS_UNDEFINED, 0, nullptr);
            if (JS_IsException(ret)) {
                JSValue exc = JS_GetException(td->engine->ctx);
                const char* s = JS_ToCString(td->engine->ctx, exc);
                if (s) { fprintf(stderr, "[JS Timer Error] %s\n", s); JS_FreeCString(td->engine->ctx, s); }
                JS_FreeValue(td->engine->ctx, exc);
            }
            JS_FreeValue(td->engine->ctx, ret);
            td->engine->executePendingJobs();
            // One-shot: clean up
            JS_FreeValue(td->engine->ctx, it->second.func);
            td->engine->timers.erase(it);
        }
        delete td;
        return G_SOURCE_REMOVE;
    }, td);

    timers[id] = entry;
    return id;
}

uint32_t JSEngine::setInterval(JSValue func, int interval_ms) {
    uint32_t id = next_timer_id++;
    TimerEntry entry;
    entry.id = id;
    entry.func = func;
    entry.interval_ms = interval_ms;

    struct TimerData { JSEngine* engine; uint32_t id; };
    auto* td = new TimerData{this, id};
    entry.gtk_source_id = g_timeout_add(interval_ms, [](gpointer data) -> gboolean {
        auto* td = static_cast<TimerData*>(data);
        auto it = td->engine->timers.find(td->id);
        if (it != td->engine->timers.end()) {
            JSValue ret = JS_Call(td->engine->ctx, it->second.func,
                                  JS_UNDEFINED, 0, nullptr);
            if (JS_IsException(ret)) {
                JSValue exc = JS_GetException(td->engine->ctx);
                const char* s = JS_ToCString(td->engine->ctx, exc);
                if (s) { fprintf(stderr, "[JS Interval Error] %s\n", s); JS_FreeCString(td->engine->ctx, s); }
                JS_FreeValue(td->engine->ctx, exc);
            }
            JS_FreeValue(td->engine->ctx, ret);
            td->engine->executePendingJobs();
            return G_SOURCE_CONTINUE;
        }
        delete td;
        return G_SOURCE_REMOVE;
    }, td);

    timers[id] = entry;
    return id;
}

void JSEngine::clearTimer(uint32_t id) {
    auto it = timers.find(id);
    if (it != timers.end()) {
        g_source_remove(it->second.gtk_source_id);
        JS_FreeValue(ctx, it->second.func);
        timers.erase(it);
    }
}

gboolean JSEngine::job_pump_callback(gpointer data) {
    auto* engine = static_cast<JSEngine*>(data);
    if (engine->ctx) engine->executePendingJobs();
    return G_SOURCE_CONTINUE;
}

void JSEngine::scheduleRerender() {
    if (rerender_idle_id) return; // already scheduled
    rerender_idle_id = g_idle_add(rerender_callback, this);
}

gboolean JSEngine::rerender_callback(gpointer data) {
    auto* engine = static_cast<JSEngine*>(data);
    engine->rerender_idle_id = 0;
    // The actual re-render will be triggered from browser.cpp
    // via the document's on_mutated callback
    return G_SOURCE_REMOVE;
}

void JSEngine::dispatchEvent(uint32_t node_id, const std::string& type,
                              int clientX, int clientY) {
    // Will be implemented in Phase 7 (js_event.cpp)
    (void)node_id; (void)type; (void)clientX; (void)clientY;
}
