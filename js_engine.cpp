#include "js_engine.h"
#include "js_bindings.h"
#include "js_event.h"
#include "dom.h"
#include <cstdio>
#include <thread>
#include <curl/curl.h>
#include <gtk/gtk.h>

extern "C" {
#include "quickjs.h"
}

// ---- fetch() support ----

struct FetchBuf { std::string data; };
static size_t fetch_write_cb(char* p, size_t s, size_t n, void* ud) {
    static_cast<FetchBuf*>(ud)->data.append(p, s*n); return s*n;
}

static bool do_fetch(const std::string& url, FetchBuf& out) {
    CURL* c = curl_easy_init(); if (!c) return false;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, fetch_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    CURLcode rc = curl_easy_perform(c); curl_easy_cleanup(c);
    return rc == CURLE_OK;
}

// Response class for fetch()
static JSClassID js_response_class_id = 0;

struct ResponseOpaque {
    std::string body;
    int status;
    bool ok;
};

static void js_response_finalizer(JSRuntime* rt, JSValue val) {
    auto* op = (ResponseOpaque*)JS_GetOpaque(val, js_response_class_id);
    delete op;
}

static const JSClassDef js_response_class_def = {
    "Response", js_response_finalizer, nullptr, nullptr, nullptr
};

static JSValue js_response_text(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* op = (ResponseOpaque*)JS_GetOpaque(this_val, js_response_class_id);
    if (!op) return JS_EXCEPTION;

    // Return a promise that resolves with the body text
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    JSValue text_val = JS_NewString(ctx, op->body.c_str());
    JSValue ret = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &text_val);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, text_val);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

static JSValue js_response_json(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* op = (ResponseOpaque*)JS_GetOpaque(this_val, js_response_class_id);
    if (!op) return JS_EXCEPTION;

    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    JSValue json_val = JS_ParseJSON(ctx, op->body.c_str(), op->body.size(), "<json>");
    if (JS_IsException(json_val)) {
        JSValue exc = JS_GetException(ctx);
        JSValue ret = JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &exc);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, exc);
    } else {
        JSValue ret = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &json_val);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, json_val);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

static JSValue js_response_get_ok(JSContext* ctx, JSValueConst this_val) {
    auto* op = (ResponseOpaque*)JS_GetOpaque(this_val, js_response_class_id);
    return op ? JS_NewBool(ctx, op->ok) : JS_FALSE;
}

static JSValue js_response_get_status(JSContext* ctx, JSValueConst this_val) {
    auto* op = (ResponseOpaque*)JS_GetOpaque(this_val, js_response_class_id);
    return op ? JS_NewInt32(ctx, op->status) : JS_NewInt32(ctx, 0);
}

static JSValue js_fetch(JSContext* ctx, JSValueConst this_val,
                         int argc, JSValueConst* argv) {
    if (argc < 1 || !g_js_engine) return JS_EXCEPTION;

    const char* url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;
    std::string url_str(url);
    JS_FreeCString(ctx, url);

    // Create promise
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);

    // Dup the resolve/reject functions for the background thread
    JSValue resolve = JS_DupValue(ctx, resolving[0]);
    JSValue reject = JS_DupValue(ctx, resolving[1]);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);

    // Fetch in background thread, resolve on main thread
    JSEngine* engine = g_js_engine;
    std::thread([engine, url_str, resolve, reject]() {
        FetchBuf buf;
        bool ok = do_fetch(url_str, buf);

        // Use g_idle_add to resolve/reject on main thread
        struct ResolveData {
            JSEngine* engine;
            JSValue resolve, reject;
            std::string body;
            bool ok;
        };
        auto* rd = new ResolveData{engine, resolve, reject, std::move(buf.data), ok};

        g_idle_add([](gpointer data) -> gboolean {
            auto* rd = static_cast<ResolveData*>(data);
            if (!rd->engine || !rd->engine->ctx) {
                delete rd;
                return G_SOURCE_REMOVE;
            }
            JSContext* ctx = rd->engine->ctx;

            if (rd->ok) {
                // Create Response object
                JSValue resp = JS_NewObjectClass(ctx, js_response_class_id);
                auto* op = new ResponseOpaque{std::move(rd->body), 200, true};
                JS_SetOpaque(resp, op);

                // Set methods
                JS_SetPropertyStr(ctx, resp, "text",
                    JS_NewCFunction(ctx, js_response_text, "text", 0));
                JS_SetPropertyStr(ctx, resp, "json",
                    JS_NewCFunction(ctx, js_response_json, "json", 0));
                JS_DefinePropertyGetSet(ctx, resp,
                    JS_NewAtom(ctx, "ok"),
                    JS_NewCFunction(ctx, (JSCFunction*)js_response_get_ok, "get ok", 0),
                    JS_UNDEFINED, JS_PROP_CONFIGURABLE);
                JS_DefinePropertyGetSet(ctx, resp,
                    JS_NewAtom(ctx, "status"),
                    JS_NewCFunction(ctx, (JSCFunction*)js_response_get_status, "get status", 0),
                    JS_UNDEFINED, JS_PROP_CONFIGURABLE);

                JSValue ret = JS_Call(ctx, rd->resolve, JS_UNDEFINED, 1, &resp);
                JS_FreeValue(ctx, ret);
                JS_FreeValue(ctx, resp);
            } else {
                JSValue err = JS_NewString(ctx, "Network error");
                JSValue ret = JS_Call(ctx, rd->reject, JS_UNDEFINED, 1, &err);
                JS_FreeValue(ctx, ret);
                JS_FreeValue(ctx, err);
            }

            JS_FreeValue(ctx, rd->resolve);
            JS_FreeValue(ctx, rd->reject);
            rd->engine->executePendingJobs();
            delete rd;
            return G_SOURCE_REMOVE;
        }, rd);
    }).detach();

    return promise;
}

JSEngine* g_js_engine = nullptr;

// ---- Helper: build message string from JS args ----

static std::string js_args_to_string(JSContext* ctx, int argc, JSValueConst* argv) {
    std::string msg;
    for (int i = 0; i < argc; i++) {
        if (i > 0) msg += ' ';
        const char* str = JS_ToCString(ctx, argv[i]);
        if (str) { msg += str; JS_FreeCString(ctx, str); }
    }
    return msg;
}

// ---- console.log / console.warn / console.error / console.info ----

static JSValue js_console_log(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    std::string msg = js_args_to_string(ctx, argc, argv);
    printf("%s\n", msg.c_str());
    if (g_js_engine) g_js_engine->addConsoleEntry(ConsoleLevel::LOG, msg);
    return JS_UNDEFINED;
}

static JSValue js_console_warn(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    std::string msg = js_args_to_string(ctx, argc, argv);
    fprintf(stderr, "[WARN] %s\n", msg.c_str());
    if (g_js_engine) g_js_engine->addConsoleEntry(ConsoleLevel::WARN, msg);
    return JS_UNDEFINED;
}

static JSValue js_console_error(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    std::string msg = js_args_to_string(ctx, argc, argv);
    fprintf(stderr, "[ERROR] %s\n", msg.c_str());
    if (g_js_engine) g_js_engine->addConsoleEntry(ConsoleLevel::ERROR, msg);
    return JS_UNDEFINED;
}

static JSValue js_console_info(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    std::string msg = js_args_to_string(ctx, argc, argv);
    printf("[INFO] %s\n", msg.c_str());
    if (g_js_engine) g_js_engine->addConsoleEntry(ConsoleLevel::INFO, msg);
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
    js_bindings_init(this);
    js_event_init(this);

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
    // Register Response class for fetch()
    JS_NewClassID(&js_response_class_id);
    JS_NewClass(rt, js_response_class_id, &js_response_class_def);
    JSValue resp_proto = JS_NewObject(ctx);
    JS_SetClassProto(ctx, js_response_class_id, resp_proto);

    JSValue global = JS_GetGlobalObject(ctx);

    // fetch()
    JS_SetPropertyStr(ctx, global, "fetch",
        JS_NewCFunction(ctx, js_fetch, "fetch", 1));

    // console object
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
        JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "warn",
        JS_NewCFunction(ctx, js_console_warn, "warn", 1));
    JS_SetPropertyStr(ctx, console, "error",
        JS_NewCFunction(ctx, js_console_error, "error", 1));
    JS_SetPropertyStr(ctx, console, "info",
        JS_NewCFunction(ctx, js_console_info, "info", 1));
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
        std::string err_msg;
        const char* str = JS_ToCString(ctx, exc);
        if (str) {
            err_msg = str;
            fprintf(stderr, "[JS Error] %s\n", str);
            JS_FreeCString(ctx, str);
        }
        // Print stack trace if available
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (!JS_IsUndefined(stack)) {
            const char* stack_str = JS_ToCString(ctx, stack);
            if (stack_str) {
                err_msg += "\n";
                err_msg += stack_str;
                fprintf(stderr, "%s\n", stack_str);
                JS_FreeCString(ctx, stack_str);
            }
        }
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, result);
        addConsoleEntry(ConsoleLevel::ERROR, err_msg, filename);
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
                if (s) {
                    fprintf(stderr, "[JS Timer Error] %s\n", s);
                    td->engine->addConsoleEntry(ConsoleLevel::ERROR, std::string(s), "setTimeout");
                    JS_FreeCString(td->engine->ctx, s);
                }
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
                if (s) {
                    fprintf(stderr, "[JS Interval Error] %s\n", s);
                    td->engine->addConsoleEntry(ConsoleLevel::ERROR, std::string(s), "setInterval");
                    JS_FreeCString(td->engine->ctx, s);
                }
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

// Defined in browser.cpp
extern void do_rerender(AppState* st);

gboolean JSEngine::rerender_callback(gpointer data) {
    auto* engine = static_cast<JSEngine*>(data);
    engine->rerender_idle_id = 0;
    if (engine->app_state && engine->document) {
        do_rerender(engine->app_state);
    }
    return G_SOURCE_REMOVE;
}

void JSEngine::dispatchEvent(uint32_t node_id, const std::string& type,
                              int clientX, int clientY) {
    js_dispatch_event(this, node_id, type, clientX, clientY);
}

void JSEngine::addConsoleEntry(ConsoleLevel level, const std::string& msg, const std::string& source) {
    console_log.push_back({level, msg, source});
    if (on_console_entry) on_console_entry();
}
