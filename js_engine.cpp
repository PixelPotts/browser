#include "js_engine.h"
#include "js_bindings.h"
#include "js_event.h"
#include "dom.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <curl/curl.h>
#include <gtk/gtk.h>
#include <time.h>

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
    fflush(stdout);
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
    fflush(stdout);
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
    setupDocPolyfills(); // must run after js_bindings_init creates document

    // Wire mutation observer hook
    if (document) {
        document->on_mutation = [this](uint32_t node_id, const std::string& type) {
            if (!ctx) return;
            JSValue global = JS_GetGlobalObject(ctx);
            JSValue notify = JS_GetPropertyStr(ctx, global, "__mutationObserverNotify");
            if (JS_IsFunction(ctx, notify)) {
                JSValue args[2] = {
                    JS_NewInt32(ctx, (int32_t)node_id),
                    JS_NewString(ctx, type.c_str())
                };
                JSValue ret = JS_Call(ctx, notify, JS_UNDEFINED, 2, args);
                JS_FreeValue(ctx, ret);
                JS_FreeValue(ctx, args[0]);
                JS_FreeValue(ctx, args[1]);
            }
            JS_FreeValue(ctx, notify);
            JS_FreeValue(ctx, global);
        };
    }

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

// ---- URL parsing helper ----
struct ParsedURL {
    std::string protocol, hostname, port, pathname, search, hash, host, origin, href;
};

static ParsedURL parse_url(const std::string& url) {
    ParsedURL u;
    u.href = url;
    size_t pos = 0;
    // protocol
    size_t colon = url.find("://");
    if (colon != std::string::npos) {
        u.protocol = url.substr(0, colon + 1); // "https:"
        pos = colon + 3;
    } else {
        u.protocol = "https:";
    }
    // hostname[:port]
    size_t slash = url.find('/', pos);
    std::string hostport = (slash != std::string::npos) ? url.substr(pos, slash - pos) : url.substr(pos);
    size_t cpos = hostport.rfind(':');
    if (cpos != std::string::npos && cpos > 0) {
        u.hostname = hostport.substr(0, cpos);
        u.port = hostport.substr(cpos + 1);
    } else {
        u.hostname = hostport;
    }
    u.host = u.port.empty() ? u.hostname : (u.hostname + ":" + u.port);
    u.origin = u.protocol + "//" + u.host;
    // pathname + search + hash
    if (slash != std::string::npos) {
        std::string rest = url.substr(slash);
        size_t hpos = rest.find('#');
        if (hpos != std::string::npos) { u.hash = rest.substr(hpos); rest = rest.substr(0, hpos); }
        size_t qpos = rest.find('?');
        if (qpos != std::string::npos) { u.search = rest.substr(qpos); u.pathname = rest.substr(0, qpos); }
        else u.pathname = rest;
    } else {
        u.pathname = "/";
    }
    return u;
}

// ---- performance.now() ----
static JSValue js_performance_now(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double ms = ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
    return JS_NewFloat64(ctx, ms);
}

// ---- requestAnimationFrame / cancelAnimationFrame ----
static JSValue js_request_animation_frame(JSContext* ctx, JSValueConst this_val,
                                           int argc, JSValueConst* argv) {
    if (argc < 1 || !g_js_engine || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    // Use setTimeout(fn, 16) as a simple approximation (~60fps)
    uint32_t id = g_js_engine->setTimeout(JS_DupValue(ctx, argv[0]), 16);
    return JS_NewInt32(ctx, (int32_t)id);
}

static JSValue js_cancel_animation_frame(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    if (argc < 1 || !g_js_engine) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    g_js_engine->clearTimer((uint32_t)id);
    return JS_UNDEFINED;
}

// ---- window.addEventListener / removeEventListener ----
static JSValue js_window_addEventListener(JSContext* ctx, JSValueConst this_val,
                                           int argc, JSValueConst* argv) {
    if (argc < 2 || !g_js_engine) return JS_UNDEFINED;
    const char* type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    JSValue func = argv[1];
    if (!JS_IsFunction(ctx, func)) {
        JS_FreeCString(ctx, type);
        return JS_UNDEFINED;
    }
    uint32_t hid = g_js_engine->next_window_handler_id++;
    g_js_engine->window_listeners.push_back({type, hid});
    // Store handler on global
    JSValue global = JS_GetGlobalObject(ctx);
    std::string key = "__whandler_" + std::to_string(hid);
    JS_SetPropertyStr(ctx, global, key.c_str(), JS_DupValue(ctx, func));
    JS_FreeValue(ctx, global);
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

static JSValue js_window_removeEventListener(JSContext* ctx, JSValueConst this_val,
                                              int argc, JSValueConst* argv) {
    if (argc < 2 || !g_js_engine) return JS_UNDEFINED;
    const char* type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    for (auto it = g_js_engine->window_listeners.begin();
         it != g_js_engine->window_listeners.end(); ++it) {
        if (it->type == type) {
            JSValue global = JS_GetGlobalObject(ctx);
            std::string key = "__whandler_" + std::to_string(it->handler_id);
            JSAtom atom = JS_NewAtom(ctx, key.c_str());
            JS_DeleteProperty(ctx, global, atom, 0);
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, global);
            g_js_engine->window_listeners.erase(it);
            break;
        }
    }
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

// Helper: dispatch to window listeners (called from js_event.cpp and browser.cpp)
void js_dispatch_to_window_listeners(JSEngine* engine, const std::string& type, JSValue event) {
    if (!engine || !engine->ctx) return;
    JSValue global = JS_GetGlobalObject(engine->ctx);
    // Copy listeners in case they modify the list during dispatch
    auto listeners = engine->window_listeners;
    for (const auto& wl : listeners) {
        if (wl.type != type) continue;
        std::string key = "__whandler_" + std::to_string(wl.handler_id);
        JSValue handler = JS_GetPropertyStr(engine->ctx, global, key.c_str());
        if (JS_IsFunction(engine->ctx, handler)) {
            JSValue ret = JS_Call(engine->ctx, handler, global, 1, &event);
            if (JS_IsException(ret)) {
                JSValue exc = JS_GetException(engine->ctx);
                const char* s = JS_ToCString(engine->ctx, exc);
                if (s) {
                    fprintf(stderr, "[JS Window Event Error] %s\n", s);
                    engine->addConsoleEntry(ConsoleLevel::ERROR, std::string(s), "window:" + type);
                    JS_FreeCString(engine->ctx, s);
                }
                JS_FreeValue(engine->ctx, exc);
            }
            JS_FreeValue(engine->ctx, ret);
        }
        JS_FreeValue(engine->ctx, handler);
    }
    JS_FreeValue(engine->ctx, global);
}

// No-op stub for properties that don't need implementation
static JSValue js_noop_func(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    return JS_UNDEFINED;
}

// ---- getComputedStyle stub ----
static JSValue js_getComputedStyle(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    // Return the element's style object as a simple proxy
    if (argc < 1) return JS_NewObject(ctx);
    // Try to get the element's style
    JSValue style = JS_GetPropertyStr(ctx, argv[0], "style");
    if (JS_IsUndefined(style) || JS_IsNull(style)) {
        JS_FreeValue(ctx, style);
        return JS_NewObject(ctx);
    }
    return style;
}

// ---- matchMedia stub ----
static JSValue js_matchMedia(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "matches", JS_FALSE);
    const char* media = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
    JS_SetPropertyStr(ctx, result, "media", JS_NewString(ctx, media ? media : ""));
    if (media) JS_FreeCString(ctx, media);
    JS_SetPropertyStr(ctx, result, "addEventListener", JS_NewCFunction(ctx, js_noop_func, "addEventListener", 2));
    JS_SetPropertyStr(ctx, result, "removeEventListener", JS_NewCFunction(ctx, js_noop_func, "removeEventListener", 2));
    // addListener/removeListener (deprecated but still used)
    JS_SetPropertyStr(ctx, result, "addListener", JS_NewCFunction(ctx, js_noop_func, "addListener", 1));
    JS_SetPropertyStr(ctx, result, "removeListener", JS_NewCFunction(ctx, js_noop_func, "removeListener", 1));
    return result;
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
    JSValue console_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console_obj, "log",
        JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console_obj, "warn",
        JS_NewCFunction(ctx, js_console_warn, "warn", 1));
    JS_SetPropertyStr(ctx, console_obj, "error",
        JS_NewCFunction(ctx, js_console_error, "error", 1));
    JS_SetPropertyStr(ctx, console_obj, "info",
        JS_NewCFunction(ctx, js_console_info, "info", 1));
    JS_SetPropertyStr(ctx, global, "console", console_obj);

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

    // requestAnimationFrame / cancelAnimationFrame
    JS_SetPropertyStr(ctx, global, "requestAnimationFrame",
        JS_NewCFunction(ctx, js_request_animation_frame, "requestAnimationFrame", 1));
    JS_SetPropertyStr(ctx, global, "cancelAnimationFrame",
        JS_NewCFunction(ctx, js_cancel_animation_frame, "cancelAnimationFrame", 1));

    // window.addEventListener / removeEventListener
    JS_SetPropertyStr(ctx, global, "addEventListener",
        JS_NewCFunction(ctx, js_window_addEventListener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, global, "removeEventListener",
        JS_NewCFunction(ctx, js_window_removeEventListener, "removeEventListener", 2));

    // getComputedStyle
    JS_SetPropertyStr(ctx, global, "getComputedStyle",
        JS_NewCFunction(ctx, js_getComputedStyle, "getComputedStyle", 1));

    // matchMedia
    JS_SetPropertyStr(ctx, global, "matchMedia",
        JS_NewCFunction(ctx, js_matchMedia, "matchMedia", 1));

    // ---- navigator object ----
    JSValue nav = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, nav, "userAgent",
        JS_NewString(ctx, "Mozilla/5.0 (X11; Linux x86_64) MiniBrowser/1.0"));
    JS_SetPropertyStr(ctx, nav, "language", JS_NewString(ctx, "en-US"));
    JSValue langs = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, langs, 0, JS_NewString(ctx, "en-US"));
    JS_SetPropertyUint32(ctx, langs, 1, JS_NewString(ctx, "en"));
    JS_SetPropertyStr(ctx, nav, "languages", langs);
    JS_SetPropertyStr(ctx, nav, "platform", JS_NewString(ctx, "Linux x86_64"));
    JS_SetPropertyStr(ctx, nav, "vendor", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, nav, "appName", JS_NewString(ctx, "Netscape"));
    JS_SetPropertyStr(ctx, nav, "appVersion", JS_NewString(ctx, "5.0 (X11; Linux x86_64) MiniBrowser/1.0"));
    JS_SetPropertyStr(ctx, nav, "product", JS_NewString(ctx, "Gecko"));
    JS_SetPropertyStr(ctx, nav, "cookieEnabled", JS_FALSE);
    JS_SetPropertyStr(ctx, nav, "onLine", JS_TRUE);
    JS_SetPropertyStr(ctx, nav, "doNotTrack", JS_NewString(ctx, "1"));
    JS_SetPropertyStr(ctx, nav, "maxTouchPoints", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, nav, "hardwareConcurrency", JS_NewInt32(ctx, 4));
    // navigator.mediaDevices stub
    JSValue mediaDevices = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, nav, "mediaDevices", mediaDevices);
    // navigator.serviceWorker stub
    JSValue sw = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, nav, "serviceWorker", sw);
    // navigator.geolocation stub
    JSValue geo = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, nav, "geolocation", geo);
    JS_SetPropertyStr(ctx, global, "navigator", nav);

    // ---- location object ----
    ParsedURL pu = parse_url(page_url);
    JSValue loc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, loc, "href", JS_NewString(ctx, pu.href.c_str()));
    JS_SetPropertyStr(ctx, loc, "protocol", JS_NewString(ctx, pu.protocol.c_str()));
    JS_SetPropertyStr(ctx, loc, "host", JS_NewString(ctx, pu.host.c_str()));
    JS_SetPropertyStr(ctx, loc, "hostname", JS_NewString(ctx, pu.hostname.c_str()));
    JS_SetPropertyStr(ctx, loc, "port", JS_NewString(ctx, pu.port.c_str()));
    JS_SetPropertyStr(ctx, loc, "pathname", JS_NewString(ctx, pu.pathname.c_str()));
    JS_SetPropertyStr(ctx, loc, "search", JS_NewString(ctx, pu.search.c_str()));
    JS_SetPropertyStr(ctx, loc, "hash", JS_NewString(ctx, pu.hash.c_str()));
    JS_SetPropertyStr(ctx, loc, "origin", JS_NewString(ctx, pu.origin.c_str()));
    JS_SetPropertyStr(ctx, loc, "assign", JS_NewCFunction(ctx, js_noop_func, "assign", 1));
    JS_SetPropertyStr(ctx, loc, "replace", JS_NewCFunction(ctx, js_noop_func, "replace", 1));
    JS_SetPropertyStr(ctx, loc, "reload", JS_NewCFunction(ctx, js_noop_func, "reload", 0));
    JS_SetPropertyStr(ctx, loc, "toString",
        JS_NewCFunction(ctx, [](JSContext* cx, JSValueConst tv, int ac, JSValueConst* av) -> JSValue {
            JSValue h = JS_GetPropertyStr(cx, tv, "href");
            return h;
        }, "toString", 0));
    JS_SetPropertyStr(ctx, global, "location", loc);

    // ---- history object ----
    JSValue history = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, history, "length", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, history, "state", JS_NULL);
    JS_SetPropertyStr(ctx, history, "pushState", JS_NewCFunction(ctx, js_noop_func, "pushState", 3));
    JS_SetPropertyStr(ctx, history, "replaceState", JS_NewCFunction(ctx, js_noop_func, "replaceState", 3));
    JS_SetPropertyStr(ctx, history, "back", JS_NewCFunction(ctx, js_noop_func, "back", 0));
    JS_SetPropertyStr(ctx, history, "forward", JS_NewCFunction(ctx, js_noop_func, "forward", 0));
    JS_SetPropertyStr(ctx, history, "go", JS_NewCFunction(ctx, js_noop_func, "go", 1));
    JS_SetPropertyStr(ctx, global, "history", history);

    // ---- screen object ----
    JSValue screen = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, screen, "width", JS_NewInt32(ctx, 1920));
    JS_SetPropertyStr(ctx, screen, "height", JS_NewInt32(ctx, 1080));
    JS_SetPropertyStr(ctx, screen, "availWidth", JS_NewInt32(ctx, 1920));
    JS_SetPropertyStr(ctx, screen, "availHeight", JS_NewInt32(ctx, 1040));
    JS_SetPropertyStr(ctx, screen, "colorDepth", JS_NewInt32(ctx, 24));
    JS_SetPropertyStr(ctx, screen, "pixelDepth", JS_NewInt32(ctx, 24));
    JS_SetPropertyStr(ctx, screen, "orientation", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, global, "screen", screen);

    // ---- performance object ----
    JSValue perf = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, perf, "now",
        JS_NewCFunction(ctx, js_performance_now, "now", 0));
    JSValue timing = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, timing, "navigationStart", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, perf, "timing", timing);
    JS_SetPropertyStr(ctx, perf, "getEntries", JS_NewCFunction(ctx,
        [](JSContext* cx, JSValueConst, int, JSValueConst*) -> JSValue { return JS_NewArray(cx); }, "getEntries", 0));
    JS_SetPropertyStr(ctx, perf, "getEntriesByType", JS_NewCFunction(ctx,
        [](JSContext* cx, JSValueConst, int, JSValueConst*) -> JSValue { return JS_NewArray(cx); }, "getEntriesByType", 1));
    JS_SetPropertyStr(ctx, perf, "getEntriesByName", JS_NewCFunction(ctx,
        [](JSContext* cx, JSValueConst, int, JSValueConst*) -> JSValue { return JS_NewArray(cx); }, "getEntriesByName", 1));
    JS_SetPropertyStr(ctx, perf, "mark", JS_NewCFunction(ctx, js_noop_func, "mark", 1));
    JS_SetPropertyStr(ctx, perf, "measure", JS_NewCFunction(ctx, js_noop_func, "measure", 1));
    JS_SetPropertyStr(ctx, global, "performance", perf);

    // ---- window properties ----
    JS_SetPropertyStr(ctx, global, "innerWidth", JS_NewInt32(ctx, 1200));
    JS_SetPropertyStr(ctx, global, "innerHeight", JS_NewInt32(ctx, 800));
    JS_SetPropertyStr(ctx, global, "outerWidth", JS_NewInt32(ctx, 1200));
    JS_SetPropertyStr(ctx, global, "outerHeight", JS_NewInt32(ctx, 900));
    JS_SetPropertyStr(ctx, global, "scrollX", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "scrollY", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "pageXOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "pageYOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "devicePixelRatio", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "top", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "parent", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "frames", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "name", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, global, "status", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, global, "closed", JS_FALSE);
    JS_SetPropertyStr(ctx, global, "length", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "opener", JS_NULL);
    JS_SetPropertyStr(ctx, global, "frameElement", JS_NULL);

    // scrollTo / scrollBy / scroll (no-op stubs)
    JS_SetPropertyStr(ctx, global, "scrollTo", JS_NewCFunction(ctx, js_noop_func, "scrollTo", 2));
    JS_SetPropertyStr(ctx, global, "scrollBy", JS_NewCFunction(ctx, js_noop_func, "scrollBy", 2));
    JS_SetPropertyStr(ctx, global, "scroll", JS_NewCFunction(ctx, js_noop_func, "scroll", 2));
    JS_SetPropertyStr(ctx, global, "focus", JS_NewCFunction(ctx, js_noop_func, "focus", 0));
    JS_SetPropertyStr(ctx, global, "blur", JS_NewCFunction(ctx, js_noop_func, "blur", 0));
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_noop_func, "print", 0));
    JS_SetPropertyStr(ctx, global, "stop", JS_NewCFunction(ctx, js_noop_func, "stop", 0));
    JS_SetPropertyStr(ctx, global, "open", JS_NewCFunction(ctx, js_noop_func, "open", 1));
    JS_SetPropertyStr(ctx, global, "close", JS_NewCFunction(ctx, js_noop_func, "close", 0));
    JS_SetPropertyStr(ctx, global, "postMessage", JS_NewCFunction(ctx, js_noop_func, "postMessage", 1));
    JS_SetPropertyStr(ctx, global, "dispatchEvent",
        JS_NewCFunction(ctx, [](JSContext* cx, JSValueConst, int, JSValueConst*) -> JSValue {
            return JS_TRUE;
        }, "dispatchEvent", 1));

    JS_FreeValue(ctx, global);

    // ---- JS polyfills (localStorage, sessionStorage, constructors, etc.) ----
    const char* polyfills = R"JS(
// localStorage / sessionStorage (in-memory)
(function() {
    function makeStorage() {
        var _data = {};
        return {
            getItem: function(k) { return _data.hasOwnProperty(k) ? _data[k] : null; },
            setItem: function(k, v) { _data[k] = String(v); },
            removeItem: function(k) { delete _data[k]; },
            clear: function() { _data = {}; },
            key: function(i) { var keys = Object.keys(_data); return i < keys.length ? keys[i] : null; },
            get length() { return Object.keys(_data).length; }
        };
    }
    globalThis.localStorage = makeStorage();
    globalThis.sessionStorage = makeStorage();
})();

// Event / CustomEvent constructors
globalThis.Event = function Event(type, opts) {
    this.type = type || '';
    this.bubbles = (opts && opts.bubbles) || false;
    this.cancelable = (opts && opts.cancelable) || false;
    this.composed = (opts && opts.composed) || false;
    this.defaultPrevented = false;
    this.target = null;
    this.currentTarget = null;
    this.timeStamp = performance.now();
    this.preventDefault = function() { this.defaultPrevented = true; };
    this.stopPropagation = function() {};
    this.stopImmediatePropagation = function() {};
};

globalThis.CustomEvent = function CustomEvent(type, opts) {
    Event.call(this, type, opts);
    this.detail = (opts && opts.detail) || null;
};

// DOM type constructors — must be defined before extending prototypes
globalThis.Node = function Node() {};
Node.ELEMENT_NODE = 1; Node.TEXT_NODE = 3; Node.COMMENT_NODE = 8; Node.DOCUMENT_NODE = 9;
Node.DOCUMENT_FRAGMENT_NODE = 11;
Node.prototype = {
    nodeType: 1, nodeName: '', parentNode: null, childNodes: [], children: [],
    firstChild: null, lastChild: null, nextSibling: null, previousSibling: null,
    ownerDocument: null, textContent: '',
    appendChild: function(c) { return c; },
    removeChild: function(c) { return c; },
    insertBefore: function(n, r) { return n; },
    replaceChild: function(n, o) { return o; },
    cloneNode: function() { return new Node(); },
    contains: function(o) { return false; },
    hasChildNodes: function() { return false; },
    addEventListener: function() {},
    removeEventListener: function() {},
    dispatchEvent: function() { return true; }
};
globalThis.EventTarget = function EventTarget() {};
EventTarget.prototype = {
    addEventListener: function() {},
    removeEventListener: function() {},
    dispatchEvent: function() { return true; }
};
globalThis.Element = function Element() {};
Element.prototype = Object.create(Node.prototype);
Element.prototype.constructor = Element;
Element.prototype.matches = function(sel) { return false; };
Element.prototype.closest = function(sel) { return null; };
Element.prototype.getAttribute = function() { return null; };
Element.prototype.setAttribute = function() {};
Element.prototype.removeAttribute = function() {};
Element.prototype.hasAttribute = function() { return false; };
Element.prototype.getElementsByTagName = function() { return []; };
Element.prototype.getElementsByClassName = function() { return []; };
Element.prototype.querySelector = function() { return null; };
Element.prototype.querySelectorAll = function() { return []; };
Element.prototype.getBoundingClientRect = function() {
    return { top: 0, left: 0, bottom: 0, right: 0, width: 0, height: 0, x: 0, y: 0 };
};
Element.prototype.scrollIntoView = function() {};
Element.prototype.focus = function() {};
Element.prototype.blur = function() {};
Element.prototype.click = function() {};
Element.prototype.remove = function() { if (this.parentNode) this.parentNode.removeChild(this); };
globalThis.HTMLElement = function HTMLElement() {};
HTMLElement.prototype = Object.create(Element.prototype);
HTMLElement.prototype.constructor = HTMLElement;
HTMLElement.prototype.style = {};
HTMLElement.prototype.offsetWidth = 0;
HTMLElement.prototype.offsetHeight = 0;
HTMLElement.prototype.offsetTop = 0;
HTMLElement.prototype.offsetLeft = 0;
HTMLElement.prototype.scrollWidth = 0;
HTMLElement.prototype.scrollHeight = 0;
HTMLElement.prototype.clientWidth = 0;
HTMLElement.prototype.clientHeight = 0;

// Web Components API stubs (prevents webcomponents-loader from crashing)
Element.prototype.attachShadow = function(opts) {
    var shadow = { host: this, mode: (opts && opts.mode) || 'open', childNodes: [], children: [],
        appendChild: function(c) { this.childNodes.push(c); this.children.push(c); return c; },
        removeChild: function(c) { var i = this.childNodes.indexOf(c); if (i>=0) this.childNodes.splice(i,1); return c; },
        querySelector: function() { return null; },
        querySelectorAll: function() { return []; },
        getElementById: function() { return null; },
        innerHTML: '' };
    this.shadowRoot = shadow;
    return shadow;
};
Element.prototype.getRootNode = function() {
    var n = this;
    while (n.parentNode) n = n.parentNode;
    return n;
};

// customElements registry stub
if (!globalThis.customElements) {
    globalThis.customElements = {
        _registry: {},
        define: function(name, ctor, opts) { this._registry[name] = ctor; },
        get: function(name) { return this._registry[name] || undefined; },
        whenDefined: function(name) { return Promise.resolve(); },
        upgrade: function() {},
        forcePolyfill: false
    };
}

// ShadowRoot constructor stub
globalThis.ShadowRoot = function ShadowRoot() {};

// DOMException constructor stub
globalThis.DOMException = function DOMException(msg, name) {
    this.message = msg || '';
    this.name = name || 'Error';
    this.code = 0;
};

// Blob / File / FormData stubs for instanceof checks
if (typeof globalThis.Blob === 'undefined') {
    globalThis.Blob = function Blob(parts, opts) {
        this.size = 0; this.type = (opts && opts.type) || '';
    };
}
if (typeof globalThis.File === 'undefined') {
    globalThis.File = function File(parts, name, opts) {
        this.name = name || ''; this.size = 0; this.type = (opts && opts.type) || '';
    };
}
if (typeof globalThis.FormData === 'undefined') {
    globalThis.FormData = function FormData() { this._data = {}; };
    FormData.prototype.append = function(k, v) { this._data[k] = v; };
    FormData.prototype.get = function(k) { return this._data[k] || null; };
    FormData.prototype.has = function(k) { return k in this._data; };
    FormData.prototype.delete = function(k) { delete this._data[k]; };
}
if (typeof globalThis.AbortController === 'undefined') {
    globalThis.AbortController = function AbortController() {
        this.signal = { aborted: false, addEventListener: function(){}, removeEventListener: function(){} };
    };
    AbortController.prototype.abort = function() { this.signal.aborted = true; };
}
if (typeof globalThis.Headers === 'undefined') {
    globalThis.Headers = function Headers(init) { this._h = init || {}; };
    Headers.prototype.get = function(n) { return this._h[n.toLowerCase()] || null; };
    Headers.prototype.set = function(n, v) { this._h[n.toLowerCase()] = v; };
    Headers.prototype.has = function(n) { return n.toLowerCase() in this._h; };
    Headers.prototype.forEach = function(cb) { for (var k in this._h) cb(this._h[k], k, this); };
}
if (typeof globalThis.Response === 'undefined') {
    globalThis.Response = function Response(body, opts) {
        this.ok = true; this.status = 200; this.statusText = 'OK'; this.body = body;
    };
    Response.prototype.json = function() { return Promise.resolve({}); };
    Response.prototype.text = function() { return Promise.resolve(''); };
}
if (typeof globalThis.Request === 'undefined') {
    globalThis.Request = function Request(url, opts) {
        this.url = url; this.method = (opts && opts.method) || 'GET';
    };
}

// PointerEvent constructor stub
globalThis.PointerEvent = function PointerEvent(type, opts) {
    Event.call(this, type, opts);
    this.pointerId = (opts && opts.pointerId) || 0;
    this.width = (opts && opts.width) || 1;
    this.height = (opts && opts.height) || 1;
    this.pressure = (opts && opts.pressure) || 0;
    this.pointerType = (opts && opts.pointerType) || 'mouse';
    this.isPrimary = (opts && opts.isPrimary !== undefined) ? opts.isPrimary : true;
};

// MouseEvent constructor stub
globalThis.MouseEvent = function MouseEvent(type, opts) {
    Event.call(this, type, opts);
    this.clientX = (opts && opts.clientX) || 0;
    this.clientY = (opts && opts.clientY) || 0;
    this.button = (opts && opts.button) || 0;
};

// KeyboardEvent constructor stub
globalThis.KeyboardEvent = function KeyboardEvent(type, opts) {
    Event.call(this, type, opts);
    this.key = (opts && opts.key) || '';
    this.code = (opts && opts.code) || '';
    this.keyCode = (opts && opts.keyCode) || 0;
};

// FocusEvent constructor stub
globalThis.FocusEvent = function FocusEvent(type, opts) {
    Event.call(this, type, opts);
    this.relatedTarget = (opts && opts.relatedTarget) || null;
};

// MutationObserver - functional implementation
(function() {
    var _observers = [];
    var _pendingRecords = new Map(); // observer -> records[]
    var _scheduled = false;

    function MutationObserver(callback) {
        this._callback = callback;
        this._targets = [];
        this._records = [];
    }
    MutationObserver.prototype.observe = function(target, options) {
        if (!target || !target.nodeType) return;
        var entry = { target: target, options: options || {} };
        this._targets.push(entry);
        // Register globally
        if (_observers.indexOf(this) === -1) _observers.push(this);
        // Store on target element for the C++ hook to find
        if (!target._mutationObservers) target._mutationObservers = [];
        target._mutationObservers.push({ observer: this, options: entry.options });
    };
    MutationObserver.prototype.disconnect = function() {
        // Remove from all targets
        for (var i = 0; i < this._targets.length; i++) {
            var t = this._targets[i].target;
            if (t && t._mutationObservers) {
                t._mutationObservers = t._mutationObservers.filter(function(e) {
                    return e.observer !== this;
                }.bind(this));
            }
        }
        this._targets = [];
        this._records = [];
        var idx = _observers.indexOf(this);
        if (idx >= 0) _observers.splice(idx, 1);
    };
    MutationObserver.prototype.takeRecords = function() {
        var r = this._records.slice();
        this._records = [];
        return r;
    };

    // Internal: queue a record and schedule delivery
    MutationObserver._notify = function(nodeId, mutationType) {
        // Find observers registered on this node
        // We need to search through all observers since we can't easily look up by nodeId from JS
        for (var i = 0; i < _observers.length; i++) {
            var obs = _observers[i];
            for (var j = 0; j < obs._targets.length; j++) {
                var entry = obs._targets[j];
                var target = entry.target;
                // Match by checking if the target's internal ID matches
                // We use a simple approach: check subtree option
                var opts = entry.options;
                var match = false;
                if (mutationType === 'childList' && opts.childList) match = true;
                if (mutationType === 'attributes' && opts.attributes) match = true;
                if (mutationType === 'characterData' && opts.characterData) match = true;
                if (opts.subtree) match = true; // subtree watches everything
                if (match) {
                    var record = {
                        type: mutationType,
                        target: target,
                        addedNodes: [],
                        removedNodes: [],
                        previousSibling: null,
                        nextSibling: null,
                        attributeName: null,
                        attributeNamespace: null,
                        oldValue: null
                    };
                    obs._records.push(record);
                }
            }
        }
        // Schedule microtask delivery
        if (!_scheduled) {
            _scheduled = true;
            Promise.resolve().then(function() {
                _scheduled = false;
                for (var i = 0; i < _observers.length; i++) {
                    var obs = _observers[i];
                    if (obs._records.length > 0) {
                        var records = obs._records.slice();
                        obs._records = [];
                        try { obs._callback(records, obs); } catch(e) { console.error('MutationObserver error:', e); }
                    }
                }
            });
        }
    };

    globalThis.MutationObserver = MutationObserver;
    // Store reference for C++ to call
    globalThis.__mutationObserverNotify = MutationObserver._notify;
})();

// ResizeObserver stub
globalThis.ResizeObserver = function ResizeObserver(cb) {
    this.observe = function() {};
    this.unobserve = function() {};
    this.disconnect = function() {};
};

// IntersectionObserver stub
globalThis.IntersectionObserver = function IntersectionObserver(cb, opts) {
    this.observe = function() {};
    this.unobserve = function() {};
    this.disconnect = function() {};
    this.root = (opts && opts.root) || null;
    this.rootMargin = (opts && opts.rootMargin) || '0px';
    this.thresholds = (opts && opts.threshold) ? [].concat(opts.threshold) : [0];
};

// Image constructor stub
globalThis.Image = function Image(w, h) {
    this.src = '';
    this.width = w || 0;
    this.height = h || 0;
    this.onload = null;
    this.onerror = null;
    this.addEventListener = function() {};
    this.removeEventListener = function() {};
};

// btoa / atob
if (typeof globalThis.btoa === 'undefined') {
    var _chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=';
    globalThis.btoa = function(s) {
        var r = '', i = 0;
        while (i < s.length) {
            var a = s.charCodeAt(i++), b = i < s.length ? s.charCodeAt(i++) : NaN, c = i < s.length ? s.charCodeAt(i++) : NaN;
            var e1 = a >> 2, e2 = ((a & 3) << 4) | (isNaN(b) ? 0 : b >> 4);
            var e3 = isNaN(b) ? 64 : ((b & 15) << 2) | (isNaN(c) ? 0 : c >> 6);
            var e4 = isNaN(c) ? 64 : c & 63;
            r += _chars[e1] + _chars[e2] + _chars[e3] + _chars[e4];
        }
        return r;
    };
    globalThis.atob = function(s) {
        var r = '', i = 0;
        s = s.replace(/[^A-Za-z0-9+/=]/g, '');
        while (i < s.length) {
            var e1 = _chars.indexOf(s[i++]), e2 = _chars.indexOf(s[i++]);
            var e3 = _chars.indexOf(s[i++]), e4 = _chars.indexOf(s[i++]);
            r += String.fromCharCode((e1 << 2) | (e2 >> 4));
            if (e3 !== 64) r += String.fromCharCode(((e2 & 15) << 4) | (e3 >> 2));
            if (e4 !== 64) r += String.fromCharCode(((e3 & 3) << 6) | e4);
        }
        return r;
    };
}

// URLSearchParams basic stub
globalThis.URLSearchParams = function URLSearchParams(init) {
    this._params = {};
    if (typeof init === 'string') {
        var s = init.startsWith('?') ? init.slice(1) : init;
        s.split('&').forEach(function(pair) {
            var kv = pair.split('=');
            if (kv[0]) this._params[decodeURIComponent(kv[0])] = decodeURIComponent(kv[1] || '');
        }.bind(this));
    }
    this.get = function(k) { return this._params.hasOwnProperty(k) ? this._params[k] : null; };
    this.set = function(k, v) { this._params[k] = String(v); };
    this.has = function(k) { return this._params.hasOwnProperty(k); };
    this.delete = function(k) { delete this._params[k]; };
    this.toString = function() {
        return Object.keys(this._params).map(function(k) {
            return encodeURIComponent(k) + '=' + encodeURIComponent(this._params[k]);
        }.bind(this)).join('&');
    };
    this.forEach = function(cb) {
        for (var k in this._params) if (this._params.hasOwnProperty(k)) cb(this._params[k], k, this);
    };
};

// URL constructor basic stub
globalThis.URL = function URL(url, base) {
    if (base && !url.match(/^https?:\/\//)) {
        url = base.replace(/\/[^/]*$/, '/') + url;
    }
    this.href = url;
    var m = url.match(/^(https?:)\/\/([^/:]+)(?::(\d+))?(\/[^?#]*)?(\?[^#]*)?(#.*)?$/);
    if (m) {
        this.protocol = m[1]; this.hostname = m[2]; this.port = m[3] || '';
        this.pathname = m[4] || '/'; this.search = m[5] || ''; this.hash = m[6] || '';
        this.host = this.port ? this.hostname + ':' + this.port : this.hostname;
        this.origin = this.protocol + '//' + this.host;
    } else {
        this.protocol = ''; this.hostname = ''; this.port = ''; this.pathname = url;
        this.search = ''; this.hash = ''; this.host = ''; this.origin = '';
    }
    this.searchParams = new URLSearchParams(this.search);
    this.toString = function() { return this.href; };
};

// DOMParser stub
globalThis.DOMParser = function DOMParser() {
    this.parseFromString = function(str, type) { return { documentElement: null }; };
};

// XMLHttpRequest stub
globalThis.XMLHttpRequest = function XMLHttpRequest() {
    this.readyState = 0; this.status = 0; this.statusText = '';
    this.responseText = ''; this.responseXML = null; this.response = '';
    this.onreadystatechange = null; this.onload = null; this.onerror = null;
    this.open = function() { this.readyState = 1; };
    this.send = function() {};
    this.setRequestHeader = function() {};
    this.getResponseHeader = function() { return null; };
    this.getAllResponseHeaders = function() { return ''; };
    this.abort = function() {};
    this.addEventListener = function() {};
    this.removeEventListener = function() {};
};

// HTMLDocument and remaining constructors (Node/Element/HTMLElement defined earlier)
globalThis.HTMLDocument = function HTMLDocument() {};
HTMLDocument.prototype = Object.create(Node.prototype);
HTMLDocument.prototype.constructor = HTMLDocument;

globalThis.DocumentFragment = function DocumentFragment() {};
DocumentFragment.prototype = Object.create(Node.prototype);
DocumentFragment.prototype.constructor = DocumentFragment;

globalThis.Text = function Text(data) { this.data = data || ''; this.textContent = this.data; this.nodeType = 3; this.nodeName = '#text'; };
Text.prototype = Object.create(Node.prototype);
Text.prototype.constructor = Text;

globalThis.Comment = function Comment(data) { this.data = data || ''; this.nodeType = 8; this.nodeName = '#comment'; };
Comment.prototype = Object.create(Node.prototype);
Comment.prototype.constructor = Comment;

// NodeList / HTMLCollection constructors
globalThis.NodeList = function NodeList() {};
NodeList.prototype = { length: 0, item: function(i) { return this[i] || null; }, forEach: Array.prototype.forEach };
globalThis.HTMLCollection = function HTMLCollection() {};
HTMLCollection.prototype = { length: 0, item: function(i) { return this[i] || null; }, namedItem: function() { return null; } };

// Window constructor
globalThis.Window = function Window() {};
Window.prototype = {};

// HTML*Element constructor stubs
['HTMLDivElement','HTMLSpanElement','HTMLIFrameElement','HTMLFormElement',
 'HTMLInputElement','HTMLSelectElement','HTMLOptionElement','HTMLTextAreaElement',
 'HTMLButtonElement','HTMLAnchorElement','HTMLImageElement','HTMLScriptElement',
 'HTMLLinkElement','HTMLStyleElement','HTMLTableElement','HTMLCanvasElement',
 'HTMLVideoElement','HTMLAudioElement','HTMLMediaElement','HTMLLabelElement',
 'HTMLUListElement','HTMLOListElement','HTMLLIElement','HTMLParagraphElement',
 'HTMLHeadingElement','HTMLBRElement','HTMLHRElement','HTMLPreElement',
 'HTMLBodyElement','HTMLHeadElement','HTMLMetaElement','HTMLTitleElement'
].forEach(function(n) {
    globalThis[n] = function() {};
    globalThis[n].prototype = Object.create(HTMLElement.prototype);
    globalThis[n].prototype.constructor = globalThis[n];
});

// Misc constructors that libraries check for
globalThis.CSSStyleDeclaration = function CSSStyleDeclaration() {};
CSSStyleDeclaration.prototype = { getPropertyValue: function() { return ''; }, setProperty: function() {}, removeProperty: function() {} };
globalThis.DOMTokenList = function DOMTokenList() {};
DOMTokenList.prototype = { add: function(){}, remove: function(){}, toggle: function(){ return false; }, contains: function(){ return false; }, length: 0 };
globalThis.NamedNodeMap = function NamedNodeMap() {};
NamedNodeMap.prototype = { length: 0, getNamedItem: function(){ return null; }, setNamedItem: function(){}, removeNamedItem: function(){} };

)JS";

    eval(polyfills, "<browser-polyfills>");
}

void JSEngine::setupDocPolyfills() {
    // These polyfills need 'document' to exist (set by js_bindings_init)
    const char* doc_polyfills = R"JS(
// document additions
document.addEventListener = function() {};
document.removeEventListener = function() {};
document.dispatchEvent = function() { return true; };
document.createEvent = function(type) { return new Event(type); };
document.createDocumentFragment = function() {
    var frag = {
        childNodes: [], children: [],
        appendChild: function(c) { this.childNodes.push(c); this.children.push(c); return c; },
        querySelectorAll: function() { return []; },
        querySelector: function() { return null; },
        getElementById: function() { return null; },
        cloneNode: function() { return document.createDocumentFragment(); }
    };
    return frag;
};
document.createComment = function(text) {
    return { nodeType: 8, textContent: text || '', nodeName: '#comment' };
};
document.cookie = '';
document.readyState = 'complete';
document.title = '';
document.domain = location.hostname || '';
document.referrer = '';
document.compatMode = 'CSS1Compat';
document.characterSet = 'UTF-8';
document.contentType = 'text/html';
document.defaultView = window;
document.nodeType = 9;
document.nodeName = '#document';
document.ownerDocument = null;
document.URL = location.href || '';
document.documentURI = location.href || '';
document.location = location;

// document.implementation (needed by jQuery)
document.implementation = {
    createHTMLDocument: function(title) {
        return {
            body: document.createElement('div'),
            head: document.createElement('div'),
            createElement: document.createElement.bind(document),
            createTextNode: document.createTextNode.bind(document),
            querySelector: function() { return null; },
            querySelectorAll: function() { return []; }
        };
    },
    hasFeature: function() { return true; },
    createDocumentType: function() { return {}; },
    createDocument: function() { return document; }
};

// document.getElementsByTagName / getElementsByClassName
document.getElementsByTagName = function(tag) {
    return document.querySelectorAll(tag);
};
document.getElementsByClassName = function(cls) {
    return document.querySelectorAll('.' + cls);
};
document.getElementsByName = function(name) {
    return document.querySelectorAll('[name="' + name + '"]');
};

// Element prototype additions that jQuery/other libs expect
(function() {
    // We need to patch the Element prototype used by our wrapped nodes
    // Since we can't access the C++ class prototype directly from JS,
    // we'll add these as fallbacks on wrapped elements via document methods
    var _origGetById = document.getElementById;
    var _origQS = document.querySelector;

    function patchElement(el) {
        if (!el || el._patched) return el;
        el._patched = true;
        if (!el.hasAttribute) el.hasAttribute = function(n) {
            return this.getAttribute(n) !== null;
        };
        if (!el.matches) el.matches = function(sel) {
            var all = document.querySelectorAll(sel);
            for (var i = 0; i < all.length; i++) if (all[i] === this) return true;
            return false;
        };
        if (!el.closest) el.closest = function(sel) {
            var cur = this;
            while (cur) {
                if (cur.matches && cur.matches(sel)) return cur;
                cur = cur.parentNode;
            }
            return null;
        };
        if (!el.contains) el.contains = function(other) {
            var cur = other;
            while (cur) {
                if (cur === this) return true;
                cur = cur.parentNode;
            }
            return false;
        };
        if (!el.getBoundingClientRect) el.getBoundingClientRect = function() {
            return { top: 0, left: 0, bottom: 0, right: 0, width: 0, height: 0, x: 0, y: 0 };
        };
        if (!el.getElementsByTagName) el.getElementsByTagName = function(tag) {
            return this.querySelectorAll(tag);
        };
        if (!el.getElementsByClassName) el.getElementsByClassName = function(cls) {
            return this.querySelectorAll('.' + cls);
        };
        if (!el.cloneNode) el.cloneNode = function(deep) {
            var c = document.createElement(this.tagName || 'div');
            c.innerHTML = deep ? this.innerHTML : '';
            return c;
        };
        if (!el.insertBefore) el.insertBefore = function(newNode, refNode) {
            this.appendChild(newNode);
            return newNode;
        };
        if (!el.replaceChild) el.replaceChild = function(newNode, oldNode) {
            this.insertBefore(newNode, oldNode);
            this.removeChild(oldNode);
            return oldNode;
        };
        if (!el.dispatchEvent) el.dispatchEvent = function() { return true; };
        if (!el.focus) el.focus = function() {};
        if (!el.blur) el.blur = function() {};
        if (!el.click) el.click = function() {};
        if (el.offsetWidth === undefined) el.offsetWidth = 0;
        if (el.offsetHeight === undefined) el.offsetHeight = 0;
        if (el.offsetTop === undefined) el.offsetTop = 0;
        if (el.offsetLeft === undefined) el.offsetLeft = 0;
        if (el.scrollWidth === undefined) el.scrollWidth = 0;
        if (el.scrollHeight === undefined) el.scrollHeight = 0;
        if (el.scrollTop === undefined) el.scrollTop = 0;
        if (el.scrollLeft === undefined) el.scrollLeft = 0;
        if (el.clientWidth === undefined) el.clientWidth = 0;
        if (el.clientHeight === undefined) el.clientHeight = 0;
        if (el.ownerDocument === undefined) el.ownerDocument = document;
        if (!el.setPointerCapture) el.setPointerCapture = function() {};
        if (!el.releasePointerCapture) el.releasePointerCapture = function() {};
        if (!el.getAnimations) el.getAnimations = function() { return []; };
        if (!el.animate) el.animate = function() { return { finished: Promise.resolve(), cancel: function(){}, onfinish: null }; };
        if (!el.remove) el.remove = function() { if (this.parentNode) this.parentNode.removeChild(this); };
        if (el.nodeType === undefined) el.nodeType = 1;
        if (el.nodeName === undefined) el.nodeName = (el.tagName || 'DIV').toUpperCase();
        return el;
    }

    // Wrap document query methods to auto-patch results
    document.getElementById = function(id) {
        return patchElement(_origGetById.call(document, id));
    };
    document.querySelector = function(sel) {
        return patchElement(_origQS.call(document, sel));
    };
    var _origQSA = document.querySelectorAll;
    document.querySelectorAll = function(sel) {
        var results = _origQSA.call(document, sel);
        if (results && results.length) {
            for (var i = 0; i < results.length; i++) patchElement(results[i]);
        }
        return results;
    };
    var _origCreate = document.createElement;
    document.createElement = function(tag) {
        return patchElement(_origCreate.call(document, tag));
    };
    var _origCreateText = document.createTextNode;
    document.createTextNode = function(text) {
        var n = _origCreateText.call(document, text);
        if (n) { n.ownerDocument = document; n.nodeType = 3; }
        return n;
    };
})();

// Google Ads / Analytics stubs to prevent crashes
if (typeof window.google === 'undefined') window.google = {};
if (typeof window.google.ima === 'undefined') window.google.ima = { AdDisplayContainer: function(){}, AdsLoader: function(){}, AdsManagerLoadedEvent: { Type: {} }, AdsRenderingSettings: function(){}, ViewMode: {} };
if (typeof window.googletag === 'undefined') window.googletag = { cmd: [], apiReady: false, pubads: function() { return { enableSingleRequest: function(){}, collapseEmptyDivs: function(){}, addEventListener: function(){}, set: function(){ return this; }, setTargeting: function(){ return this; }, refresh: function(){}, getSlots: function(){ return []; } }; }, enableServices: function(){}, defineSlot: function() { return { addService: function(){ return this; }, setTargeting: function(){ return this; }, defineSizeMapping: function(){ return this; } }; }, defineOutOfPageSlot: function() { return { addService: function(){ return this; } }; }, sizeMapping: function() { return { addSize: function(){ return this; }, build: function(){ return []; } }; }, display: function(){}, destroySlots: function(){} };
if (typeof window.ga === 'undefined') window.ga = function() {};
if (typeof window.gtag === 'undefined') window.gtag = function() {};
if (typeof window.dataLayer === 'undefined') window.dataLayer = [];
if (typeof window.adsbygoogle === 'undefined') window.adsbygoogle = [];

// Google Ads bootstrapper stubs
if (typeof window.__google_ad_request_done === 'undefined') window.__google_ad_request_done = function() {};
if (typeof window._gaq === 'undefined') window._gaq = { push: function() {} };

// picturefill stub (responsive images library)
if (typeof window.picturefill === 'undefined') window.picturefill = function() {};

// Web Components readiness stub
window.WebComponents = window.WebComponents || {};
window.WebComponents.ready = true;
window.WebComponents.waitFor = window.WebComponents.waitFor || function(cb) { if (cb) cb(); };

// HTMLTemplateElement stub
if (typeof HTMLTemplateElement === 'undefined') {
    globalThis.HTMLTemplateElement = function HTMLTemplateElement() {};
    HTMLTemplateElement.prototype = Object.create(HTMLElement.prototype);
    HTMLTemplateElement.prototype.constructor = HTMLTemplateElement;
    HTMLTemplateElement.bootstrap = function() {};
}

)JS";

    eval(doc_polyfills, "<browser-doc-polyfills>");
}

bool JSEngine::eval(const std::string& code, const std::string& filename) {
    JSValue result = JS_Eval(ctx, code.c_str(), code.size(),
                              filename.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        std::string err_msg;
        bool is_syntax_error = false;
        const char* str = JS_ToCString(ctx, exc);
        if (str) {
            err_msg = str;
            if (strncmp(str, "SyntaxError", 11) == 0)
                is_syntax_error = true;
            if (is_syntax_error)
                fprintf(stderr, "[JS SyntaxError] %s\n", str);
            else
                fprintf(stderr, "[JS Error] %s\n", str);
            JS_FreeCString(ctx, str);
        }
        // Print stack trace if available
        if (!is_syntax_error) {
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
        }
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, result);
        if (!is_syntax_error)
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
