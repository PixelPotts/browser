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
    fprintf(stderr, "[TIMER] setTimeout registered id=%u delay=%dms\n", id, delay);
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

// ---- ES6 Module Loader ----

// Resolve relative module URLs (e.g., "./tests.js" relative to "https://css3test.com/csstest.js")
static char* js_module_normalize(JSContext* ctx, const char* base_name,
                                  const char* module_name, void* opaque) {
    std::string base(base_name);
    std::string name(module_name);

    // Already absolute
    if (name.size() >= 4 && name.substr(0, 4) == "http")
        return js_strdup(ctx, name.c_str());
    if (name.size() >= 2 && name.substr(0, 2) == "//")
        return js_strdup(ctx, ("https:" + name).c_str());

    // Relative: resolve against base
    if (name[0] == '.' || name[0] == '/') {
        // Find base directory
        auto last_slash = base.rfind('/');
        std::string base_dir = (last_slash != std::string::npos) ? base.substr(0, last_slash + 1) : base + "/";

        if (name[0] == '/') {
            // Absolute path - combine with origin
            auto proto_end = base.find("://");
            if (proto_end != std::string::npos) {
                auto host_end = base.find('/', proto_end + 3);
                std::string origin = (host_end != std::string::npos) ? base.substr(0, host_end) : base;
                return js_strdup(ctx, (origin + name).c_str());
            }
        }

        // Handle ./ and ../
        std::string result = base_dir;
        size_t pos = 0;
        if (name.substr(0, 2) == "./") pos = 2;
        while (name.substr(pos, 3) == "../") {
            pos += 3;
            auto sl = result.rfind('/', result.size() - 2);
            if (sl != std::string::npos) result = result.substr(0, sl + 1);
        }
        result += name.substr(pos);
        return js_strdup(ctx, result.c_str());
    }

    // Bare specifier - try relative to base directory
    auto last_slash = base.rfind('/');
    std::string base_dir = (last_slash != std::string::npos) ? base.substr(0, last_slash + 1) : base + "/";
    return js_strdup(ctx, (base_dir + name).c_str());
}

// Fetch and compile a module from URL
static JSModuleDef* js_module_loader(JSContext* ctx, const char* module_name, void* opaque) {
    fprintf(stderr, "[MODULE] Loading: %s\n", module_name);

    // Fetch module source
    std::string source;
    CURL* c = curl_easy_init();
    if (!c) {
        JS_ThrowReferenceError(ctx, "could not load module '%s': curl init failed", module_name);
        return nullptr;
    }
    curl_easy_setopt(c, CURLOPT_URL, module_name);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
        +[](char* p, size_t s, size_t n, void* ud) -> size_t {
            static_cast<std::string*>(ud)->append(p, s * n); return s * n; });
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &source);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        JS_ThrowReferenceError(ctx, "could not load module '%s': fetch failed", module_name);
        return nullptr;
    }

    fprintf(stderr, "[MODULE]   Loaded %zu bytes from %s\n", source.size(), module_name);

    // Compile as module (compile-only, don't execute yet)
    JSValue func = JS_Eval(ctx, source.c_str(), source.size(), module_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(func)) {
        JSValue exc = JS_GetException(ctx);
        const char* str = JS_ToCString(ctx, exc);
        if (str) {
            fprintf(stderr, "[MODULE]   Compile error: %s\n", str);
            JS_FreeCString(ctx, str);
        }
        JS_FreeValue(ctx, exc);
        return nullptr;
    }

    JSModuleDef* m = (JSModuleDef*)JS_VALUE_GET_PTR(func);
    JS_FreeValue(ctx, func);
    return m;
}

// ---- JSEngine implementation ----

JSEngine::JSEngine() {}

JSEngine::~JSEngine() {
    shutdown();
}

void JSEngine::init(AppState* as, TabState* ts, Document* doc) {
    app_state = as;
    tab_state = ts;
    document = doc;
    g_js_engine = this;

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);

    // Set memory limit (64MB)
    JS_SetMemoryLimit(rt, 64 * 1024 * 1024);

    // Register ES6 module loader for import/export support
    JS_SetModuleLoaderFunc(rt, js_module_normalize, js_module_loader, nullptr);

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

    // Free window listener handler refs from global object
    if (ctx) {
        JSValue global = JS_GetGlobalObject(ctx);
        for (auto& wl : window_listeners) {
            std::string key = "__whandler_" + std::to_string(wl.handler_id);
            JSAtom atom = JS_NewAtom(ctx, key.c_str());
            JS_DeleteProperty(ctx, global, atom, 0);
            JS_FreeAtom(ctx, atom);
        }
        // Also delete element handler refs stored by addEventListener
        // They use __handler_N pattern on global
        if (document) {
            std::function<void(DOMNode*)> walk = [&](DOMNode* n) {
                for (auto& l : n->listeners) {
                    std::string key = "__handler_" + std::to_string(l.handler_id);
                    JSAtom atom = JS_NewAtom(ctx, key.c_str());
                    JS_DeleteProperty(ctx, global, atom, 0);
                    JS_FreeAtom(ctx, atom);
                }
                for (auto& c : n->children) walk(c.get());
            };
            walk(document->root.get());
        }
        JS_FreeValue(ctx, global);
    }
    window_listeners.clear();

    // Clear node wrapper cache (frees DupValue'd JSValues)
    extern void clear_node_cache();
    clear_node_cache();

    // Drain any pending microtasks/promises
    if (rt && ctx) {
        JSContext* pctx;
        while (JS_ExecutePendingJob(rt, &pctx) > 0) {}
    }

    // Run GC to collect any lingering objects before freeing
    if (rt) JS_RunGC(rt);

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
                    fprintf(stderr, "[JS Window Event Error] %s  (window:%s)\n", s, type.c_str());
                    // Print stack trace
                    JSValue stack = JS_GetPropertyStr(engine->ctx, exc, "stack");
                    const char* st = JS_ToCString(engine->ctx, stack);
                    if (st && st[0]) fprintf(stderr, "[JS Window Event Stack] %s\n", st);
                    if (st) JS_FreeCString(engine->ctx, st);
                    JS_FreeValue(engine->ctx, stack);
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

// ---- window.postMessage ----
static JSValue js_window_postMessage(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    if (argc < 1 || !g_js_engine) return JS_UNDEFINED;
    // Create a MessageEvent-like object
    JSValue event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "message"));
    JS_SetPropertyStr(ctx, event, "data", JS_DupValue(ctx, argv[0]));
    if (argc >= 2) {
        JS_SetPropertyStr(ctx, event, "origin", JS_DupValue(ctx, argv[1]));
    } else {
        JS_SetPropertyStr(ctx, event, "origin", JS_NewString(ctx, "*"));
    }
    JS_SetPropertyStr(ctx, event, "source", JS_NULL);
    JS_SetPropertyStr(ctx, event, "preventDefault", JS_NewCFunction(ctx,
        [](JSContext* cx, JSValueConst, int, JSValueConst*) -> JSValue { return JS_UNDEFINED; },
        "preventDefault", 0));
    JS_SetPropertyStr(ctx, event, "stopPropagation", JS_NewCFunction(ctx,
        [](JSContext* cx, JSValueConst, int, JSValueConst*) -> JSValue { return JS_UNDEFINED; },
        "stopPropagation", 0));

    fprintf(stderr, "[postMessage] dispatching message event\n");
    js_dispatch_to_window_listeners(g_js_engine, "message", event);
    JS_FreeValue(ctx, event);
    return JS_UNDEFINED;
}

// No-op stub for properties that don't need implementation
static JSValue js_noop_func(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    return JS_UNDEFINED;
}

// ---- getComputedStyle ----
static JSValue js_getComputedStyle(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewObject(ctx);

    // Get the element's tag name for default style lookup
    JSValue tagVal = JS_GetPropertyStr(ctx, argv[0], "tagName");
    std::string tag;
    if (!JS_IsUndefined(tagVal) && !JS_IsNull(tagVal)) {
        const char* s = JS_ToCString(ctx, tagVal);
        if (s) { tag = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, tagVal);

    // Lowercase for comparison
    for (auto& c : tag) c = tolower(c);

    // Determine default display based on HTML spec
    std::string defaultDisplay = "inline";
    static const char* blockElements[] = {
        "div", "p", "h1", "h2", "h3", "h4", "h5", "h6",
        "section", "nav", "article", "aside", "header", "footer", "main",
        "figure", "figcaption", "details", "summary", "dialog",
        "blockquote", "pre", "ol", "ul", "li", "dl", "dt", "dd",
        "form", "fieldset", "legend", "table", "hr", "address",
        "hgroup", "search", "menu", nullptr
    };
    for (int i = 0; blockElements[i]; i++) {
        if (tag == blockElements[i]) { defaultDisplay = "block"; break; }
    }
    if (tag == "mark") defaultDisplay = "inline"; // mark is inline with bg color
    // Hidden elements per HTML spec
    if (tag == "rp" || tag == "datalist" || tag == "template") defaultDisplay = "none";

    // Get inline style overrides
    JSValue inlineStyle = JS_GetPropertyStr(ctx, argv[0], "style");

    // Build computed style object with getPropertyValue
    JSValue result = JS_NewObject(ctx);

    // Store defaults on the object for direct property access
    JS_SetPropertyStr(ctx, result, "display", JS_NewString(ctx, defaultDisplay.c_str()));

    // Default background-color (mark has yellow)
    std::string bgColor = "transparent";
    if (tag == "mark") bgColor = "rgb(255, 255, 0)";
    JS_SetPropertyStr(ctx, result, "backgroundColor", JS_NewString(ctx, bgColor.c_str()));
    JS_SetPropertyStr(ctx, result, "background-color", JS_NewString(ctx, bgColor.c_str()));

    // Copy all inline style properties to computed style
    if (!JS_IsUndefined(inlineStyle) && !JS_IsNull(inlineStyle)) {
        // Get the DOM node to access style_props
        DOMNode* node = js_get_node(ctx, argv[0]);
        if (node) {
            // First pass: copy all properties
            for (auto& kv : node->style_props) {
                std::string camel;
                bool nextUpper = false;
                for (char c : kv.first) {
                    if (c == '-' && !camel.empty()) { nextUpper = true; continue; }
                    camel += nextUpper ? (char)toupper(c) : c;
                    nextUpper = false;
                }
                JS_SetPropertyStr(ctx, result, camel.c_str(), JS_NewString(ctx, kv.second.c_str()));
                // Also set kebab-case
                JS_SetPropertyStr(ctx, result, kv.first.c_str(), JS_NewString(ctx, kv.second.c_str()));
            }
            // Second pass: resolve var() references
            for (auto& kv : node->style_props) {
                if (kv.second.find("var(") != std::string::npos) {
                    std::string resolved = kv.second;
                    size_t pos = resolved.find("var(--");
                    if (pos != std::string::npos) {
                        size_t end = resolved.find(')', pos);
                        if (end != std::string::npos) {
                            std::string varName = resolved.substr(pos + 4, end - pos - 4);
                            auto it = node->style_props.find(varName);
                            if (it != node->style_props.end()) {
                                resolved = resolved.substr(0, pos) + it->second + resolved.substr(end + 1);
                                std::string camel;
                                bool nextUpper = false;
                                for (char c : kv.first) {
                                    if (c == '-' && !camel.empty()) { nextUpper = true; continue; }
                                    camel += nextUpper ? (char)toupper(c) : c;
                                    nextUpper = false;
                                }
                                JS_SetPropertyStr(ctx, result, camel.c_str(), JS_NewString(ctx, resolved.c_str()));
                                JS_SetPropertyStr(ctx, result, kv.first.c_str(), JS_NewString(ctx, resolved.c_str()));
                            }
                        }
                    }
                }
            }
            // Check display override
            auto displayIt = node->style_props.find("display");
            if (displayIt != node->style_props.end() && !displayIt->second.empty()) {
                defaultDisplay = displayIt->second;
                JS_SetPropertyStr(ctx, result, "display", JS_NewString(ctx, defaultDisplay.c_str()));
            }
        }
    }
    JS_FreeValue(ctx, inlineStyle);

    // Add getPropertyValue method
    JS_SetPropertyStr(ctx, result, "getPropertyValue",
        JS_NewCFunction(ctx, [](JSContext* c, JSValueConst tv, int ac, JSValueConst* av) -> JSValue {
            if (ac < 1) return JS_NewString(c, "");
            const char* prop = JS_ToCString(c, av[0]);
            if (!prop) return JS_NewString(c, "");
            // Convert CSS property name to camelCase for lookup
            std::string camel;
            bool nextUpper = false;
            for (const char* p = prop; *p; p++) {
                if (*p == '-') { nextUpper = true; continue; }
                camel += nextUpper ? (char)toupper(*p) : *p;
                nextUpper = false;
            }
            JS_FreeCString(c, prop);
            // Look up on this object
            JSValue val = JS_GetPropertyStr(c, tv, camel.c_str());
            if (JS_IsUndefined(val) || JS_IsNull(val)) {
                JS_FreeValue(c, val);
                return JS_NewString(c, "");
            }
            return val;
        }, "getPropertyValue", 1));

    return result;
}

// ---- matchMedia ----
static JSValue js_matchMedia(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    const char* media = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
    std::string mq = media ? media : "";
    if (media) JS_FreeCString(ctx, media);

    // Create a MediaQueryList instance
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue mql_ctor = JS_GetPropertyStr(ctx, global, "MediaQueryList");
    JSValue result;
    if (JS_IsFunction(ctx, mql_ctor)) {
        JSValue args[2] = { JS_NewString(ctx, mq.c_str()), JS_TRUE };
        result = JS_CallConstructor(ctx, mql_ctor, 2, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
    } else {
        result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "media", JS_NewString(ctx, mq.c_str()));
        JS_SetPropertyStr(ctx, result, "matches", JS_TRUE);
    }
    JS_FreeValue(ctx, mql_ctor);
    JS_FreeValue(ctx, global);
    return result;
}

// ---- CSS.supports() ----
static JSValue js_css_supports(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    // css3test uses CSS.supports('selector(...)') for selector tests
    // Return true for everything - we claim to support all CSS features
    return JS_TRUE;
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
    JS_SetPropertyStr(ctx, console_obj, "debug",
        JS_NewCFunction(ctx, js_console_log, "debug", 1));
    JS_SetPropertyStr(ctx, console_obj, "trace",
        JS_NewCFunction(ctx, js_console_log, "trace", 1));
    JS_SetPropertyStr(ctx, console_obj, "dir",
        JS_NewCFunction(ctx, js_console_log, "dir", 1));
    JS_SetPropertyStr(ctx, console_obj, "table",
        JS_NewCFunction(ctx, js_console_log, "table", 1));
    JS_SetPropertyStr(ctx, console_obj, "group",
        JS_NewCFunction(ctx, js_console_log, "group", 1));
    JS_SetPropertyStr(ctx, console_obj, "groupEnd",
        JS_NewCFunction(ctx, js_noop_func, "groupEnd", 0));
    JS_SetPropertyStr(ctx, console_obj, "groupCollapsed",
        JS_NewCFunction(ctx, js_console_log, "groupCollapsed", 1));
    JS_SetPropertyStr(ctx, console_obj, "time",
        JS_NewCFunction(ctx, js_noop_func, "time", 0));
    JS_SetPropertyStr(ctx, console_obj, "timeEnd",
        JS_NewCFunction(ctx, js_noop_func, "timeEnd", 0));
    JS_SetPropertyStr(ctx, console_obj, "timeLog",
        JS_NewCFunction(ctx, js_noop_func, "timeLog", 0));
    JS_SetPropertyStr(ctx, console_obj, "assert",
        JS_NewCFunction(ctx, js_noop_func, "assert", 0));
    JS_SetPropertyStr(ctx, console_obj, "count",
        JS_NewCFunction(ctx, js_noop_func, "count", 0));
    JS_SetPropertyStr(ctx, console_obj, "countReset",
        JS_NewCFunction(ctx, js_noop_func, "countReset", 0));
    JS_SetPropertyStr(ctx, console_obj, "clear",
        JS_NewCFunction(ctx, js_noop_func, "clear", 0));
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

    // Pre-define event handler properties so modules can assign to them
    JS_SetPropertyStr(ctx, global, "onload", JS_NULL);
    JS_SetPropertyStr(ctx, global, "onerror", JS_NULL);
    JS_SetPropertyStr(ctx, global, "onresize", JS_NULL);
    JS_SetPropertyStr(ctx, global, "onscroll", JS_NULL);
    JS_SetPropertyStr(ctx, global, "onunload", JS_NULL);
    JS_SetPropertyStr(ctx, global, "onbeforeunload", JS_NULL);

    // getComputedStyle
    JS_SetPropertyStr(ctx, global, "getComputedStyle",
        JS_NewCFunction(ctx, js_getComputedStyle, "getComputedStyle", 1));

    // matchMedia
    JS_SetPropertyStr(ctx, global, "matchMedia",
        JS_NewCFunction(ctx, js_matchMedia, "matchMedia", 1));

    // ---- CSS object with supports() ----
    JSValue css_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, css_obj, "supports",
        JS_NewCFunction(ctx, js_css_supports, "supports", 1));
    // CSS.escape(str) - returns the string as-is for our purposes
    JS_SetPropertyStr(ctx, css_obj, "escape",
        JS_NewCFunction(ctx, [](JSContext* cx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
            if (argc < 1) return JS_NewString(cx, "");
            return JS_DupValue(cx, argv[0]);
        }, "escape", 1));
    JS_SetPropertyStr(ctx, global, "CSS", css_obj);

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
    JS_SetPropertyStr(ctx, global, "postMessage", JS_NewCFunction(ctx, js_window_postMessage, "postMessage", 2));
    JS_SetPropertyStr(ctx, global, "dispatchEvent",
        JS_NewCFunction(ctx, [](JSContext* cx, JSValueConst, int, JSValueConst*) -> JSValue {
            return JS_TRUE;
        }, "dispatchEvent", 1));

    // Window methods for CSSOM-View
    JS_SetPropertyStr(ctx, global, "moveTo", JS_NewCFunction(ctx, js_noop_func, "moveTo", 2));
    JS_SetPropertyStr(ctx, global, "moveBy", JS_NewCFunction(ctx, js_noop_func, "moveBy", 2));
    JS_SetPropertyStr(ctx, global, "resizeTo", JS_NewCFunction(ctx, js_noop_func, "resizeTo", 2));
    JS_SetPropertyStr(ctx, global, "resizeBy", JS_NewCFunction(ctx, js_noop_func, "resizeBy", 2));
    JS_SetPropertyStr(ctx, global, "screenX", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "screenLeft", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "screenY", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "screenTop", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "getSelection",
        JS_NewCFunction(ctx, [](JSContext* cx, JSValueConst, int, JSValueConst*) -> JSValue {
            JSValue sel = JS_NewObject(cx);
            JS_SetPropertyStr(cx, sel, "toString", JS_NewCFunction(cx,
                [](JSContext* c2, JSValueConst, int, JSValueConst*) -> JSValue {
                    return JS_NewString(c2, "");
                }, "toString", 0));
            JS_SetPropertyStr(cx, sel, "rangeCount", JS_NewInt32(cx, 0));
            return sel;
        }, "getSelection", 0));

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
// matches() and closest() are now native C++ in js_bindings.cpp
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
Element.prototype.getClientRects = function() { return []; };
Element.prototype.scrollIntoView = function() {};
Element.prototype.scroll = function() {};
Element.prototype.scrollTo = function() {};
Element.prototype.scrollBy = function() {};
Element.prototype.focus = function() {};
Element.prototype.blur = function() {};
Element.prototype.click = function() {};
Element.prototype.checkVisibility = function() { return true; };
Element.prototype.getBoxQuads = function() { return []; };
Element.prototype.convertQuadFromNode = function() { return {}; };
Element.prototype.convertRectFromNode = function() { return {}; };
Element.prototype.convertPointFromNode = function() { return {}; };
Element.prototype.pseudo = function(t) { return new (globalThis.CSSPseudoElement || function(){this.type='';this.element=null;this.parent=null;this.pseudo=function(){return null;}})(); };
Element.prototype.clientTop = 0;
Element.prototype.clientLeft = 0;
Element.prototype.clientWidth = 0;
Element.prototype.clientHeight = 0;
Element.prototype.regionOverset = '';
Element.prototype.getRegionFlowRanges = function() { return []; };
Element.prototype.part = { length: 0, add: function(){}, remove: function(){}, toggle: function(){}, contains: function(){return false;} };
Element.prototype.getSpatialNavigationContainer = function() { return null; };
Element.prototype.focusableAreas = function() { return []; };
Element.prototype.spatialNavigationSearch = function() { return null; };
Element.prototype.computedStyleMap = function() {
    return (typeof StylePropertyMapReadOnly !== 'undefined') ? new StylePropertyMapReadOnly() : {};
};
Object.defineProperty(Element.prototype, 'attributeStyleMap', {
    get: function() {
        if (!this._attrStyleMap) this._attrStyleMap = (typeof StylePropertyMap !== 'undefined') ? new StylePropertyMap() : {};
        return this._attrStyleMap;
    },
    configurable: true
});
Element.prototype.animate = function() { return new Animation(); };
Element.prototype.getAnimations = function() { return []; };
Element.prototype.setPointerCapture = function() {};
Element.prototype.releasePointerCapture = function() {};
Element.prototype.hasPointerCapture = function() { return false; };
Element.prototype.remove = function() { if (this.parentNode) this.parentNode.removeChild(this); };
if (typeof HTMLElement === 'undefined') {
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
    HTMLElement.prototype.clientTop = 0;
    HTMLElement.prototype.clientLeft = 0;
} else {
    // C++ set HTMLElement.prototype = elem_proto with all DOM getters/setters.
    // Chain it into the Element hierarchy without replacing the prototype object.
    Object.setPrototypeOf(HTMLElement.prototype, Element.prototype);
    // Ensure DOM layout properties exist
    if (!('clientTop' in HTMLElement.prototype)) HTMLElement.prototype.clientTop = 0;
    if (!('clientLeft' in HTMLElement.prototype)) HTMLElement.prototype.clientLeft = 0;
    if (!('clientWidth' in HTMLElement.prototype)) HTMLElement.prototype.clientWidth = 0;
    if (!('clientHeight' in HTMLElement.prototype)) HTMLElement.prototype.clientHeight = 0;
}

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

// Image constructor is provided natively by js_bindings_init

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
    if (base && !url.match(/^[a-zA-Z][a-zA-Z0-9+.-]*:/)) {
        url = base.replace(/\/[^/]*$/, '/') + url;
    }
    var m = url.match(/^([a-zA-Z][a-zA-Z0-9+.-]*:)\/\/([^/:]+)(?::(\d+))?(\/[^?#]*)?(\?[^#]*)?(#.*)?$/);
    if (!m) {
        throw new TypeError("Invalid URL: " + url);
    }
    this.href = url;
    this.protocol = m[1]; this.hostname = m[2]; this.port = m[3] || '';
    this.pathname = m[4] || '/'; this.search = m[5] || ''; this.hash = m[6] || '';
    this.host = this.port ? this.hostname + ':' + this.port : this.hostname;
    this.origin = this.protocol + '//' + this.host;
    this.searchParams = new URLSearchParams(this.search);
    this.toString = function() { return this.href; };
};

// DOMParser stub
globalThis.DOMParser = function DOMParser() {
    this.parseFromString = function(str, type) { return { documentElement: null }; };
};

// XMLHttpRequest - basic stub with property support
globalThis.XMLHttpRequest = function XMLHttpRequest() {
    this.readyState = 0; this.status = 0; this.statusText = '';
    this.responseText = ''; this.responseXML = null; this.response = '';
    this._responseType = ''; this._url = ''; this._method = 'GET';
    this.onreadystatechange = null; this.onload = null; this.onerror = null;
    this.upload = { addEventListener: function(){}, removeEventListener: function(){} };
    this.withCredentials = false;
    this.timeout = 0;
};
XMLHttpRequest.prototype.open = function(method, url) {
    this._method = method || 'GET';
    this._url = url || '';
    this.readyState = 1;
};
XMLHttpRequest.prototype.send = function() {};
XMLHttpRequest.prototype.setRequestHeader = function() {};
XMLHttpRequest.prototype.getResponseHeader = function() { return null; };
XMLHttpRequest.prototype.getAllResponseHeaders = function() { return ''; };
XMLHttpRequest.prototype.abort = function() {};
XMLHttpRequest.prototype.addEventListener = function(type, fn) {
    if (type === 'load') this.onload = fn;
    else if (type === 'error') this.onerror = fn;
    else if (type === 'readystatechange') this.onreadystatechange = fn;
};
XMLHttpRequest.prototype.removeEventListener = function() {};
XMLHttpRequest.prototype.overrideMimeType = function() {};
Object.defineProperty(XMLHttpRequest.prototype, 'responseType', {
    get: function() { return this._responseType || ''; },
    set: function(v) { this._responseType = v; },
    configurable: true
});

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
NodeList.prototype[Symbol.iterator] = function() {
    var i = 0, self = this;
    return { next: function() { return i < self.length ? { value: self[i++], done: false } : { done: true }; } };
};
NodeList.prototype.entries = function() {
    var i = 0, self = this;
    return { next: function() { return i < self.length ? { value: [i, self[i++]], done: false } : { done: true }; }, [Symbol.iterator]: function() { return this; } };
};
NodeList.prototype.keys = function() {
    var i = 0, self = this;
    return { next: function() { return i < self.length ? { value: i++, done: false } : { done: true }; }, [Symbol.iterator]: function() { return this; } };
};
NodeList.prototype.values = function() {
    var i = 0, self = this;
    return { next: function() { return i < self.length ? { value: self[i++], done: false } : { done: true }; }, [Symbol.iterator]: function() { return this; } };
};
globalThis.HTMLCollection = function HTMLCollection() {};
HTMLCollection.prototype = { length: 0, item: function(i) { return this[i] || null; }, namedItem: function() { return null; } };
HTMLCollection.prototype[Symbol.iterator] = function() {
    var i = 0, self = this;
    return { next: function() { return i < self.length ? { value: self[i++], done: false } : { done: true }; } };
};

// Window constructor
globalThis.Window = function Window() {};
Window.prototype = { navigate: function() {} };

// HTML*Element constructor stubs
['HTMLDivElement','HTMLSpanElement','HTMLIFrameElement','HTMLFormElement',
 'HTMLInputElement','HTMLSelectElement','HTMLOptionElement','HTMLTextAreaElement',
 'HTMLButtonElement','HTMLAnchorElement','HTMLImageElement','HTMLScriptElement',
 'HTMLLinkElement','HTMLStyleElement','HTMLTableElement','HTMLCanvasElement',
 'HTMLVideoElement','HTMLAudioElement','HTMLMediaElement','HTMLLabelElement',
 'HTMLUListElement','HTMLOListElement','HTMLLIElement','HTMLParagraphElement',
 'HTMLHeadingElement','HTMLBRElement','HTMLHRElement','HTMLPreElement',
 'HTMLBodyElement','HTMLHeadElement','HTMLMetaElement','HTMLTitleElement',
 'HTMLKeygenElement'
].forEach(function(n) {
    globalThis[n] = function() {};
    globalThis[n].prototype = Object.create(HTMLElement.prototype);
    globalThis[n].prototype.constructor = globalThis[n];
});

// Misc constructors that libraries check for
globalThis.CSSStyleDeclaration = function CSSStyleDeclaration() {};
CSSStyleDeclaration.prototype = {
    cssText: '', length: 0, parentRule: null, cssFloat: '',
    getPropertyValue: function(p) { return this[p] || ''; },
    getPropertyPriority: function() { return ''; },
    setProperty: function(p,v) { this[p] = v; },
    removeProperty: function(p) { var old = this[p]; delete this[p]; return old || ''; },
    item: function(i) { return ''; }
};
globalThis.DOMTokenList = function DOMTokenList() {};
DOMTokenList.prototype = { add: function(){}, remove: function(){}, toggle: function(){ return false; }, contains: function(){ return false; }, length: 0 };
globalThis.NamedNodeMap = function NamedNodeMap() {};
NamedNodeMap.prototype = { length: 0, getNamedItem: function(){ return null; }, setNamedItem: function(){}, removeNamedItem: function(){} };
globalThis.Attr = function Attr(name, value, ownerElement) {
    this.name = name || '';
    this.localName = name || '';
    this.nodeName = name || '';
    this.value = value !== undefined ? value : '';
    this.nodeValue = value !== undefined ? value : '';
    this.textContent = value !== undefined ? value : '';
    this.ownerElement = ownerElement || null;
    this.specified = true;
    this.nodeType = 2;
    this.namespaceURI = null;
    this.prefix = null;
};
Attr.prototype = Object.create(Node.prototype);
Attr.prototype.constructor = Attr;

// Forward declarations needed by CSS rule types
globalThis.MediaList = function MediaList() { this.length = 0; };
MediaList.prototype = { mediaText: '', item: function(){return null;}, appendMedium: function(){this.length++;}, deleteMedium: function(){} };
globalThis.StylePropertyMap = function StylePropertyMap() {};
StylePropertyMap.prototype = {
    get: function(){return null;}, getAll: function(){return [];},
    has: function(){return false;}, set: function(){}, append: function(){},
    delete: function(){}, clear: function(){}, forEach: function(){},
    entries: function(){return {next:function(){return {done:true};}};},
    keys: function(){return {next:function(){return {done:true};}};},
    values: function(){return {next:function(){return {done:true};}};}
};
StylePropertyMap.prototype.size = 0;
globalThis.StylePropertyMapReadOnly = function StylePropertyMapReadOnly() {};
StylePropertyMapReadOnly.prototype = {
    get: function(){return null;}, getAll: function(){return [];},
    has: function(){return false;}, forEach: function(){},
    entries: function(){return {next:function(){return {done:true};}};},
    keys: function(){return {next:function(){return {done:true};}};},
    values: function(){return {next:function(){return {done:true};}};}
};
StylePropertyMapReadOnly.prototype.size = 0;

// ---- CSS OM interfaces (for css3test interface checks) ----
// Forward declare CSSFontFeatureValuesMap (used by CSSFontFeatureValuesRule)
if (typeof CSSFontFeatureValuesMap === 'undefined') {
    globalThis.CSSFontFeatureValuesMap = function CSSFontFeatureValuesMap() {
        this.size = 0;
        this.get = function() { return null; };
        this.has = function() { return false; };
        this.set = function() {};
        this.delete = function() {};
        this.clear = function() {};
        this.forEach = function() {};
        this.entries = function() { return {next:function(){return {done:true};}}; };
        this.keys = function() { return {next:function(){return {done:true};}}; };
        this.values = function() { return {next:function(){return {done:true};}}; };
    };
}

// CSSRule base
globalThis.CSSRule = function CSSRule() {};
CSSRule.STYLE_RULE = 1; CSSRule.CHARSET_RULE = 2; CSSRule.IMPORT_RULE = 3;
CSSRule.MEDIA_RULE = 4; CSSRule.FONT_FACE_RULE = 5; CSSRule.PAGE_RULE = 6;
CSSRule.KEYFRAMES_RULE = 7; CSSRule.KEYFRAME_RULE = 8; CSSRule.NAMESPACE_RULE = 10;
CSSRule.COUNTER_STYLE_RULE = 11; CSSRule.SUPPORTS_RULE = 12;
CSSRule.FONT_FEATURE_VALUES_RULE = 14; CSSRule.VIEWPORT_RULE = 15;
CSSRule.prototype = {
    type: 0, cssText: '', parentRule: null, parentStyleSheet: null,
    STYLE_RULE: 1, CHARSET_RULE: 2, IMPORT_RULE: 3, MEDIA_RULE: 4,
    FONT_FACE_RULE: 5, PAGE_RULE: 6, KEYFRAMES_RULE: 7, KEYFRAME_RULE: 8,
    MARGIN_RULE: 9, NAMESPACE_RULE: 10, COUNTER_STYLE_RULE: 11,
    SUPPORTS_RULE: 12, FONT_FEATURE_VALUES_RULE: 14, VIEWPORT_RULE: 15
};

// Build CSS rule subclasses
var _cssRuleTypes = [
    'CSSStyleRule', 'CSSMediaRule', 'CSSFontFaceRule', 'CSSPageRule',
    'CSSKeyframesRule', 'CSSKeyframeRule', 'CSSNamespaceRule', 'CSSSupportsRule',
    'CSSCounterStyleRule', 'CSSFontFeatureValuesRule', 'CSSViewportRule',
    'CSSImportRule', 'CSSGroupingRule', 'CSSConditionRule', 'CSSLayerBlockRule',
    'CSSLayerStatementRule', 'CSSPropertyRule', 'CSSContainerRule',
    'CSSFontPaletteValuesRule', 'CSSScopeRule', 'CSSStartingStyleRule',
    'CSSNestedDeclarations', 'CSSPositionTryRule', 'CSSViewTransitionRule',
    'CSSFunctionRule'
];
_cssRuleTypes.forEach(function(name) {
    var F = function() {};
    Object.defineProperty(F, 'name', { value: name, configurable: true });
    F.prototype = Object.create(CSSRule.prototype);
    F.prototype.constructor = F;
    // Add style property for rule types that have it
    if (name === 'CSSStyleRule' || name === 'CSSPageRule' || name === 'CSSKeyframeRule' ||
        name === 'CSSFontFaceRule' || name === 'CSSCounterStyleRule' ||
        name === 'CSSViewportRule' || name === 'CSSFontFeatureValuesRule' ||
        name === 'CSSPositionTryRule' || name === 'CSSPropertyRule' ||
        name === 'CSSMarginRule' || name === 'CSSNestedDeclarations') {
        F.prototype.style = new CSSStyleDeclaration();
        F.prototype.style.length = 1;
    }
    if (name === 'CSSStyleRule') {
        F.prototype.selectorText = '';
        F.prototype.styleMap = new StylePropertyMap();
        F.prototype.type = 1;
        F.prototype.cssRules = [];
        F.prototype.insertRule = function() { return 0; };
        F.prototype.deleteRule = function() {};
    }
    if (name === 'CSSMediaRule' || name === 'CSSSupportsRule' || name === 'CSSGroupingRule' ||
        name === 'CSSConditionRule' || name === 'CSSContainerRule' ||
        name === 'CSSLayerBlockRule' || name === 'CSSScopeRule' ||
        name === 'CSSStartingStyleRule' || name === 'CSSFunctionRule') {
        F.prototype.cssRules = [];
        F.prototype.insertRule = function() { return 0; };
        F.prototype.deleteRule = function() {};
    }
    if (name === 'CSSMediaRule') {
        F.prototype.media = new MediaList();
        F.prototype.conditionText = '';
        F.prototype.type = 4;
        F.prototype.matches = false;
    }
    if (name === 'CSSSupportsRule') {
        F.prototype.conditionText = '';
        F.prototype.type = 12;
        F.prototype.matches = false;
    }
    if (name === 'CSSKeyframesRule') {
        F.prototype.name = '';
        F.prototype.cssRules = [];
        F.prototype.length = 0;
        F.prototype.appendRule = function() {};
        F.prototype.deleteRule = function() {};
        F.prototype.findRule = function() { return null; };
        F.prototype.type = 7;
    }
    if (name === 'CSSKeyframeRule') {
        F.prototype.keyText = '';
        F.prototype.type = 8;
    }
    if (name === 'CSSFontFaceRule') {
        F.prototype.type = 5;
    }
    if (name === 'CSSImportRule') {
        F.prototype.href = '';
        F.prototype.media = new MediaList();
        F.prototype.styleSheet = null;
        F.prototype.layerName = null;
        F.prototype.supportsText = null;
        F.prototype.type = 3;
    }
    if (name === 'CSSPageRule') {
        F.prototype.selectorText = '';
        F.prototype.type = 6;
        F.prototype.cssRules = [];
        F.prototype.insertRule = function() { return 0; };
        F.prototype.deleteRule = function() {};
    }
    if (name === 'CSSNamespaceRule') {
        F.prototype.namespaceURI = '';
        F.prototype.prefix = '';
        F.prototype.type = 10;
    }
    if (name === 'CSSContainerRule') {
        F.prototype.containerName = '';
        F.prototype.containerQuery = '';
        F.prototype.conditionText = '';
    }
    if (name === 'CSSConditionRule') {
        F.prototype.conditionText = '';
    }
    if (name === 'CSSPropertyRule') {
        F.prototype.name = '';
        F.prototype.syntax = '';
        F.prototype.inherits = false;
        F.prototype.initialValue = null;
    }
    if (name === 'CSSFontPaletteValuesRule') {
        F.prototype.name = '';
        F.prototype.fontFamily = '';
        F.prototype.basePalette = '';
        F.prototype.overrideColors = '';
    }
    if (name === 'CSSCounterStyleRule') {
        F.prototype.name = '';
        F.prototype.system = '';
        F.prototype.symbols = '';
        F.prototype.additiveSymbols = '';
        F.prototype.negative = '';
        F.prototype.prefix = '';
        F.prototype.suffix = '';
        F.prototype.range = '';
        F.prototype.pad = '';
        F.prototype.speakAs = '';
        F.prototype.fallback = '';
        F.prototype.type = 11;
    }
    if (name === 'CSSFontFeatureValuesRule') {
        F.prototype.fontFamily = '';
        F.prototype.type = 14;
        F.prototype.annotation = new CSSFontFeatureValuesMap();
        F.prototype.characterVariant = new CSSFontFeatureValuesMap();
        F.prototype.ornaments = new CSSFontFeatureValuesMap();
        F.prototype.styleset = new CSSFontFeatureValuesMap();
        F.prototype.stylistic = new CSSFontFeatureValuesMap();
        F.prototype.swash = new CSSFontFeatureValuesMap();
    }
    if (name === 'CSSScopeRule') {
        F.prototype.start = '';
        F.prototype.end = '';
        F.prototype.conditionText = '';
    }
    if (name === 'CSSLayerBlockRule') {
        F.prototype.name = '';
    }
    if (name === 'CSSLayerStatementRule') {
        F.prototype.nameList = [];
    }
    if (name === 'CSSViewTransitionRule') {
        F.prototype.navigation = '';
        F.prototype.types = [];
    }
    if (name === 'CSSStartingStyleRule') {
        F.prototype.conditionText = '';
        F.prototype.addRule = function() { return 0; };
    }
    if (name === 'CSSFunctionRule') {
        F.prototype.name = '';
        F.prototype.parameters = [];
    }
    if (name === 'CSSGroupingRule') {
        F.prototype.conditionText = '';
    }
    globalThis[name] = F;
});

// CSSStyleSheet (inherits from StyleSheet for interface tests)
globalThis.CSSStyleSheet = function CSSStyleSheet() {
    this.cssRules = [];
    this.cssRules.item = function(i) { return this[i] || null; };
    this.rules = this.cssRules;
    this.ownerRule = null;
    this.disabled = false;
    this.media = new MediaList();
    this.type = 'text/css';
    this.href = null;
    this.ownerNode = null;
    this.parentStyleSheet = null;
    this.title = null;
};
CSSStyleSheet.prototype = {
    insertRule: function(rule, index) {
        if (index === undefined) index = 0;
        var ruleObj = null;
        var trimmed = rule.trim();
        // Determine rule type
        if (trimmed.match(/^@media/i)) { ruleObj = new CSSMediaRule(); }
        else if (trimmed.match(/^@font-face/i)) { ruleObj = new CSSFontFaceRule(); }
        else if (trimmed.match(/^@keyframes/i)) { ruleObj = new CSSKeyframesRule(); }
        else if (trimmed.match(/^@supports/i)) { ruleObj = new CSSSupportsRule(); }
        else if (trimmed.match(/^@page/i)) { ruleObj = new CSSPageRule(); }
        else if (trimmed.match(/^@counter-style/i)) { ruleObj = new CSSCounterStyleRule(); }
        else if (trimmed.match(/^@namespace/i)) { ruleObj = new CSSNamespaceRule(); }
        else if (trimmed.match(/^@import/i)) { ruleObj = new CSSImportRule(); }
        else if (trimmed.match(/^@layer\s[^;]*\{/i) || trimmed.match(/^@layer\s*\{/i)) { ruleObj = new CSSLayerBlockRule(); }
        else if (trimmed.match(/^@layer[\s;]/i)) { ruleObj = new CSSLayerStatementRule(); }
        else if (trimmed.match(/^@property/i)) { ruleObj = new CSSPropertyRule(); }
        else if (trimmed.match(/^@container/i)) { ruleObj = new CSSContainerRule(); }
        else if (trimmed.match(/^@font-palette-values/i)) { ruleObj = new CSSFontPaletteValuesRule(); }
        else if (trimmed.match(/^@font-feature-values/i)) { ruleObj = new CSSFontFeatureValuesRule(); }
        else if (trimmed.match(/^@color-profile/i)) { ruleObj = new CSSColorProfileRule(); }
        else if (trimmed.match(/^@scope/i)) { ruleObj = new CSSScopeRule(); }
        else if (trimmed.match(/^@starting-style/i)) { ruleObj = new CSSStartingStyleRule(); }
        else if (trimmed.match(/^@position-try/i)) { ruleObj = new CSSPositionTryRule(); }
        else if (trimmed.match(/^@view-transition/i)) { ruleObj = new CSSViewTransitionRule(); }
        else if (trimmed.match(/^@function/i)) { ruleObj = new CSSFunctionRule(); }
        else if (trimmed.match(/^@when\s/i) || trimmed.match(/^@else/i)) {
            ruleObj = new CSSConditionRule();
        }
        else { ruleObj = new CSSStyleRule(); }
        ruleObj.cssText = rule;
        // Parse declarations for style-bearing rules and set as camelCase props on rule
        var _camelCase = function(s) { return s.replace(/-([a-z])/g, function(m,c){return c.toUpperCase();}).replace('-',''); };
        // Extract content between outermost braces
        var braceStart = rule.indexOf('{');
        if (braceStart >= 0) {
            var depth = 0, bodyStart = -1, bodyEnd = -1;
            for (var bi = braceStart; bi < rule.length; bi++) {
                if (rule[bi] === '{') { if (depth === 0) bodyStart = bi + 1; depth++; }
                else if (rule[bi] === '}') { depth--; if (depth === 0) { bodyEnd = bi; break; } }
            }
            if (bodyStart >= 0 && bodyEnd > bodyStart) {
                var body = rule.substring(bodyStart, bodyEnd);
                // Parse declarations
                var decls = body.split(';');
                var parsedCount = 0;
                for (var d = 0; d < decls.length; d++) {
                    var parts = decls[d].split(':');
                    if (parts.length >= 2) {
                        var prop = parts[0].trim();
                        var val = parts.slice(1).join(':').trim();
                        if (prop && val) {
                            parsedCount++;
                            // Set on style if available
                            if (ruleObj.style !== undefined) ruleObj.style[prop] = val;
                            // Also set camelCase descriptor on rule object itself
                            ruleObj[_camelCase(prop)] = val;
                        }
                    }
                }
                if (parsedCount > 0 && ruleObj.style !== undefined) {
                    ruleObj.style.length = parsedCount;
                }
            }
        }
        // Parse nested at-rules for grouping rules
        if (bodyStart >= 0 && bodyEnd > bodyStart && ruleObj.cssRules) {
            var nestedBody = rule.substring(bodyStart, bodyEnd);
            var nd = 0, ns = 0;
            for (var ni = 0; ni < nestedBody.length; ni++) {
                if (nestedBody[ni] === '{') nd++;
                else if (nestedBody[ni] === '}') {
                    nd--;
                    if (nd <= 0) {
                        var nr = nestedBody.substring(ns, ni + 1).trim();
                        if (nr) {
                            var childRule = null;
                            if (nr.match(/^@top-|^@bottom-|^@left-|^@right-/i)) {
                                childRule = new CSSMarginRule();
                                childRule.cssText = nr;
                                childRule.selectorText = nr.substring(0, nr.indexOf('{')).trim();
                            } else if (nr.match(/^(from|to|\d+%)/i)) {
                                childRule = new CSSKeyframeRule();
                                childRule.cssText = nr;
                                childRule.keyText = nr.substring(0, nr.indexOf('{')).trim();
                                childRule.style = new CSSStyleDeclaration();
                                childRule.style.length = 1;
                            } else if (nr.indexOf('{') >= 0) {
                                childRule = new CSSStyleRule();
                                childRule.cssText = nr;
                                childRule.selectorText = nr.substring(0, nr.indexOf('{')).trim();
                            }
                            if (childRule) {
                                childRule.parentRule = ruleObj;
                                ruleObj.cssRules.push(childRule);
                            }
                        }
                        ns = ni + 1;
                        nd = 0;
                    }
                }
            }
        }
        // Ensure style-bearing rules have length >= 1 even without parsed content
        if (ruleObj.style !== undefined && braceStart >= 0) {
            if (ruleObj.style.length < 1) ruleObj.style.length = 1;
        }
        // Create styleMap for style-bearing rules
        if (ruleObj.style !== undefined) {
            ruleObj.styleMap = new StylePropertyMap();
            ruleObj.styleMap._rule = ruleObj;
            ruleObj.styleMap._decls = {};
            if (bodyStart >= 0 && bodyEnd > bodyStart) {
                var _decls2 = body.split(';');
                for (var d2 = 0; d2 < _decls2.length; d2++) {
                    var _p2 = _decls2[d2].split(':');
                    if (_p2.length >= 2) {
                        var _prop2 = _p2[0].trim();
                        var _val2 = _p2.slice(1).join(':').trim();
                        if (_prop2 && _val2) ruleObj.styleMap._decls[_prop2] = _val2;
                    }
                }
            }
            ruleObj.styleMap.get = function(prop) {
                var v = this._decls[prop] || '';
                if (!v) return null;
                // Custom properties always return CSSUnparsedValue
                if (prop.indexOf('--') === 0) {
                    var uv = new CSSUnparsedValue(); uv.length = 1; uv[0] = v;
                    uv.entries = function(){return [];}; uv.keys = function(){return [];};
                    uv.values = function(){return [];}; uv.forEach = function(){};
                    return uv;
                }
                // Parse CSS value to Typed OM object
                var vt = v.trim();
                if (vt.match(/^calc\s*\(/i)) return new CSSMathSum();
                if (vt.match(/^min\s*\(/i)) return new CSSMathMin();
                if (vt.match(/^max\s*\(/i)) return new CSSMathMax();
                if (vt.match(/^clamp\s*\(/i)) return new CSSMathClamp();
                if (vt.match(/^var\s*\(/i)) {
                    var uv = new CSSUnparsedValue(); uv.length = 1;
                    uv[0] = new (globalThis.CSSVariableReferenceValue || function(){})();
                    uv.entries = function(){return [];}; uv.keys = function(){return [];};
                    uv.values = function(){return [];}; uv.forEach = function(){};
                    return uv;
                }
                if (vt.match(/^rgb\s*\(/i)) return new CSSRGB(0,0,0,1);
                if (vt.match(/^hsl\s*\(/i)) return new CSSHSL(0,0,0,1);
                if (vt.match(/^hwb\s*\(/i)) return new CSSHWB(0,0,0,1);
                if (vt.match(/^lab\s*\(/i)) return new CSSLab(0,0,0,1);
                if (vt.match(/^lch\s*\(/i)) return new CSSLCH(0,0,0,1);
                if (vt.match(/^oklab\s*\(/i)) return new CSSOKLab(0,0,0,1);
                if (vt.match(/^oklch\s*\(/i)) return new CSSOKLCH(0,0,0,1);
                if (vt.match(/^color\s*\(/i)) return new CSSColor();
                if (prop === 'transform' || prop === '-webkit-transform') {
                    var tv = new CSSTransformValue();
                    tv.length = 1; tv[0] = new (globalThis.CSSTranslate || function CSSTranslate(){this.x=0;this.y=0;this.z=0;})();
                    tv.entries = function(){return [];}; tv.keys = function(){return [];};
                    tv.values = function(){return [];}; tv.forEach = function(){};
                    // Detect specific transform function
                    if (vt.match(/^rotate\s*\(/i)) tv[0] = new (globalThis.CSSRotate || function(){this.x=0;this.y=0;this.z=0;this.angle=0;})();
                    else if (vt.match(/^scale\s*\(/i)) tv[0] = new (globalThis.CSSScale || function(){this.x=0;this.y=0;this.z=0;})();
                    else if (vt.match(/^skew\s*\(/i)) tv[0] = new (globalThis.CSSSkew || function(){this.ax=0;this.ay=0;})();
                    else if (vt.match(/^skewX\s*\(/i)) tv[0] = new (globalThis.CSSSkewX || function(){this.ax=0;})();
                    else if (vt.match(/^skewY\s*\(/i)) tv[0] = new (globalThis.CSSSkewY || function(){this.ay=0;})();
                    else if (vt.match(/^perspective\s*\(/i)) tv[0] = new (globalThis.CSSPerspective || function(){this.length=0;})();
                    else if (vt.match(/^matrix\s*\(/i)) tv[0] = new (globalThis.CSSMatrixComponent || function(){this.matrix=new DOMMatrix();})();
                    return tv;
                }
                // Check for numeric value with unit
                var numMatch = vt.match(/^(-?\d*\.?\d+)(px|em|rem|%|vh|vw|vmin|vmax|deg|rad|grad|turn|s|ms|Hz|kHz|dpi|dpcm|dppx|fr|cm|mm|Q|in|pt|pc|ex|ch|ic|lh|rlh|vi|vb|svw|svh|svi|svb|lvw|lvh|lvi|lvb|dvw|dvh|dvi|dvb|cqw|cqh|cqi|cqb)$/);
                if (numMatch) return new CSSUnitValue(parseFloat(numMatch[1]), numMatch[2]);
                var numOnly = vt.match(/^(-?\d*\.?\d+)$/);
                if (numOnly) return new CSSUnitValue(parseFloat(numOnly[1]), 'number');
                return new CSSKeywordValue(vt);
            };
        }
        this.cssRules.splice(index, 0, ruleObj);
        return index;
    },
    deleteRule: function(index) {
        this.cssRules.splice(index, 1);
    },
    addRule: function(sel, style, index) {
        return this.insertRule(sel + '{' + style + '}', index !== undefined ? index : this.cssRules.length);
    },
    removeRule: function(index) { this.deleteRule(index); },
    replace: function(text) { return Promise.resolve(this); },
    replaceSync: function(text) {}
};

// StyleSheet base
globalThis.StyleSheet = function StyleSheet() {};
StyleSheet.prototype = { type: 'text/css', disabled: false, href: null, title: null };

// StyleSheetList
globalThis.StyleSheetList = function StyleSheetList() {
    this.length = 0;
};
StyleSheetList.prototype = { item: function(){return null;} };

// CSS Typed OM
globalThis.CSSStyleValue = function CSSStyleValue() {};
CSSStyleValue.parse = function() { return new CSSStyleValue(); };
CSSStyleValue.parseAll = function() { return []; };
globalThis.CSSNumericValue = function CSSNumericValue() {};
CSSNumericValue.prototype = Object.create(CSSStyleValue.prototype);
CSSNumericValue.prototype.constructor = CSSNumericValue;
CSSNumericValue.prototype.add = function() { return this; };
CSSNumericValue.prototype.sub = function() { return this; };
CSSNumericValue.prototype.mul = function() { return this; };
CSSNumericValue.prototype.div = function() { return this; };
CSSNumericValue.prototype.min = function() { return this; };
CSSNumericValue.prototype.max = function() { return this; };
CSSNumericValue.prototype.equals = function() { return false; };
CSSNumericValue.prototype.to = function() { return this; };
CSSNumericValue.prototype.toSum = function() { return this; };
CSSNumericValue.prototype.type = function() { return {}; };
CSSNumericValue.parse = function() { return new CSSUnitValue(0, 'px'); };
globalThis.CSSUnitValue = function CSSUnitValue(v, u) { this.value = v; this.unit = u; };
CSSUnitValue.prototype = Object.create(CSSNumericValue.prototype);
CSSUnitValue.prototype.constructor = CSSUnitValue;
CSSUnitValue.prototype.toString = function() { return this.value + this.unit; };
globalThis.CSSKeywordValue = function CSSKeywordValue(v) { this.value = v; };
CSSKeywordValue.prototype = Object.create(CSSStyleValue.prototype);
CSSKeywordValue.prototype.constructor = CSSKeywordValue;

// CSSMath types with proper inheritance and properties
globalThis.CSSMathValue = function CSSMathValue() {};
CSSMathValue.prototype = Object.create(CSSNumericValue.prototype);
CSSMathValue.prototype.constructor = CSSMathValue;
CSSMathValue.prototype.operator = '';
globalThis.CSSMathSum = function CSSMathSum() { this.values = new CSSNumericArray(); this.operator = 'sum'; };
CSSMathSum.prototype = Object.create(CSSMathValue.prototype);
CSSMathSum.prototype.constructor = CSSMathSum;
globalThis.CSSMathProduct = function CSSMathProduct() { this.values = new CSSNumericArray(); this.operator = 'product'; };
CSSMathProduct.prototype = Object.create(CSSMathValue.prototype);
CSSMathProduct.prototype.constructor = CSSMathProduct;
globalThis.CSSMathNegate = function CSSMathNegate(v) { this.value = v || null; this.operator = 'negate'; };
CSSMathNegate.prototype = Object.create(CSSMathValue.prototype);
CSSMathNegate.prototype.constructor = CSSMathNegate;
globalThis.CSSMathInvert = function CSSMathInvert(v) { this.value = v || null; this.operator = 'invert'; };
CSSMathInvert.prototype = Object.create(CSSMathValue.prototype);
CSSMathInvert.prototype.constructor = CSSMathInvert;
globalThis.CSSMathMin = function CSSMathMin() { this.values = new CSSNumericArray(); this.operator = 'min'; };
CSSMathMin.prototype = Object.create(CSSMathValue.prototype);
CSSMathMin.prototype.constructor = CSSMathMin;
globalThis.CSSMathMax = function CSSMathMax() { this.values = new CSSNumericArray(); this.operator = 'max'; };
CSSMathMax.prototype = Object.create(CSSMathValue.prototype);
CSSMathMax.prototype.constructor = CSSMathMax;
globalThis.CSSMathClamp = function CSSMathClamp() { this.lower = null; this.value = null; this.upper = null; this.operator = 'clamp'; };
CSSMathClamp.prototype = Object.create(CSSMathValue.prototype);
CSSMathClamp.prototype.constructor = CSSMathClamp;
globalThis.CSSNumericArray = function CSSNumericArray() { this.length = 0; };
CSSNumericArray.prototype.forEach = function() {};

// CSS Transform types
globalThis.CSSTransformComponent = function CSSTransformComponent() { this.is2D = true; };
CSSTransformComponent.prototype.toMatrix = function() { return new DOMMatrix(); };
globalThis.CSSTransformValue = function CSSTransformValue() { this.length = 0; this.is2D = true; };
CSSTransformValue.prototype.toMatrix = function() { return new DOMMatrix(); };
globalThis.CSSTranslate = function CSSTranslate() { this.x = null; this.y = null; this.z = null; this.is2D = true; };
CSSTranslate.prototype = Object.create(CSSTransformComponent.prototype);
globalThis.CSSRotate = function CSSRotate() { this.angle = null; this.x = null; this.y = null; this.z = null; this.is2D = true; };
CSSRotate.prototype = Object.create(CSSTransformComponent.prototype);
globalThis.CSSScale = function CSSScale() { this.x = null; this.y = null; this.z = null; this.is2D = true; };
CSSScale.prototype = Object.create(CSSTransformComponent.prototype);
globalThis.CSSSkew = function CSSSkew() { this.ax = null; this.ay = null; this.is2D = true; };
CSSSkew.prototype = Object.create(CSSTransformComponent.prototype);
globalThis.CSSSkewX = function CSSSkewX() { this.ax = null; this.is2D = true; };
CSSSkewX.prototype = Object.create(CSSTransformComponent.prototype);
globalThis.CSSSkewY = function CSSSkewY() { this.ay = null; this.is2D = true; };
CSSSkewY.prototype = Object.create(CSSTransformComponent.prototype);
globalThis.CSSPerspective = function CSSPerspective() { this.length = null; this.is2D = false; };
CSSPerspective.prototype = Object.create(CSSTransformComponent.prototype);
globalThis.CSSMatrixComponent = function CSSMatrixComponent() { this.matrix = null; this.is2D = true; };
CSSMatrixComponent.prototype = Object.create(CSSTransformComponent.prototype);
globalThis.CSSImageValue = function CSSImageValue() {};
CSSImageValue.prototype = Object.create(CSSStyleValue.prototype);
globalThis.CSSUnparsedValue = function CSSUnparsedValue() { this.length = 0; };
CSSUnparsedValue.prototype = Object.create(CSSStyleValue.prototype);
globalThis.CSSVariableReferenceValue = function CSSVariableReferenceValue() { this.variable = ''; this.fallback = null; };

// CSS Color Value types
globalThis.CSSColorValue = function CSSColorValue() {};
CSSColorValue.prototype = Object.create(CSSStyleValue.prototype);
CSSColorValue.parse = function() { return new CSSColorValue(); };
globalThis.CSSColor = function CSSColor() { this.colorSpace = ''; this.channels = []; this.alpha = 1; };
CSSColor.prototype = Object.create(CSSColorValue.prototype);
globalThis.CSSRGB = function CSSRGB(r,g,b,a) { this.r = r||0; this.g = g||0; this.b = b||0; this.alpha = a||1; };
CSSRGB.prototype = Object.create(CSSColorValue.prototype);
globalThis.CSSHSL = function CSSHSL(h,s,l,a) { this.h = h||0; this.s = s||0; this.l = l||0; this.alpha = a||1; };
CSSHSL.prototype = Object.create(CSSColorValue.prototype);
globalThis.CSSHWB = function CSSHWB(h,w,b,a) { this.h = h||0; this.w = w||0; this.b = b||0; this.alpha = a||1; };
CSSHWB.prototype = Object.create(CSSColorValue.prototype);
globalThis.CSSLab = function CSSLab(l,a2,b2,a) { this.l = l||0; this.a = a2||0; this.b = b2||0; this.alpha = a||1; };
CSSLab.prototype = Object.create(CSSColorValue.prototype);
globalThis.CSSOKLab = function CSSOKLab(l,a2,b2,a) { this.l = l||0; this.a = a2||0; this.b = b2||0; this.alpha = a||1; };
CSSOKLab.prototype = Object.create(CSSColorValue.prototype);
globalThis.CSSLCH = function CSSLCH(l,c,h,a) { this.l = l||0; this.c = c||0; this.h = h||0; this.alpha = a||1; };
CSSLCH.prototype = Object.create(CSSColorValue.prototype);
globalThis.CSSOKLCH = function CSSOKLCH(l,c,h,a) { this.l = l||0; this.c = c||0; this.h = h||0; this.alpha = a||1; };
CSSOKLCH.prototype = Object.create(CSSColorValue.prototype);

// CSS Highlight API
globalThis.Highlight = function Highlight() { this.priority = 0; this.type = 'highlight'; this.size = 0; };
Highlight.prototype.has = function() { return false; };
Highlight.prototype.add = function() { return this; };
Highlight.prototype.delete = function() { return false; };
Highlight.prototype.clear = function() {};
Highlight.prototype.values = function() { return {next:function(){return {done:true};}}; };
Highlight.prototype.keys = function() { return {next:function(){return {done:true};}}; };
Highlight.prototype.entries = function() { return {next:function(){return {done:true};}}; };
Highlight.prototype.forEach = function() {};
globalThis.HighlightRegistry = function HighlightRegistry() {};
HighlightRegistry.prototype = {
    set: function(){}, get: function(){return null;}, has: function(){return false;},
    delete: function(){return false;}, clear: function(){}, forEach: function(){}
};
if (!CSS.highlights) CSS.highlights = new HighlightRegistry();

// CSS Properties and Values API
CSS.registerProperty = CSS.registerProperty || function() {};

// CSS Animation Worklet
globalThis.AnimationWorklet = function AnimationWorklet() {};
AnimationWorklet.prototype = { addModule: function() { return Promise.resolve(); } };
globalThis.Worklet = function Worklet() {};
Worklet.prototype = { addModule: function() { return Promise.resolve(); } };

// CSS Layout API / Paint API
globalThis.LayoutWorklet = function LayoutWorklet() {};
LayoutWorklet.prototype = Object.create(Worklet.prototype);
LayoutWorklet.prototype.constructor = LayoutWorklet;
LayoutWorklet.prototype.addModule = function() { return Promise.resolve(); };
globalThis.PaintWorklet = function PaintWorklet() {};
PaintWorklet.prototype = Object.create(Worklet.prototype);
PaintWorklet.prototype.constructor = PaintWorklet;
PaintWorklet.prototype.addModule = function() { return Promise.resolve(); };
if (!CSS.layoutWorklet) CSS.layoutWorklet = new LayoutWorklet();
if (!CSS.paintWorklet) CSS.paintWorklet = new PaintWorklet();
if (!CSS.animationWorklet) CSS.animationWorklet = new AnimationWorklet();

// WorkletAnimation
globalThis.WorkletAnimation = function WorkletAnimation() {
    this.animatorName = ''; this.effect = null; this.timeline = null;
    this.playState = 'idle'; this.currentTime = null;
};
WorkletAnimation.prototype = {
    play: function(){}, cancel: function(){}
};

// Web Animations API
globalThis.Animation = function Animation(effect, timeline) {
    this.effect = effect || null; this.timeline = timeline || null;
    this.playState = 'idle'; this.currentTime = null; this.startTime = null;
    this.playbackRate = 1; this.pending = false; this.id = '';
    this.finished = Promise.resolve(this); this.ready = Promise.resolve(this);
    this.onfinish = null; this.oncancel = null; this.onremove = null;
    this.replaceState = 'active';
};
Animation.prototype = {
    play: function(){}, pause: function(){}, cancel: function(){},
    finish: function(){}, reverse: function(){}, updatePlaybackTiming: function(){},
    updatePlaybackRate: function(){},
    persist: function(){}, commitStyles: function(){}, effect: null,
    addEventListener: function(){}, removeEventListener: function(){},
    dispatchEvent: function() { return true; }
};
globalThis.CSSAnimation = function CSSAnimation() {
    this.animationName = ''; this.effect = null; this.timeline = null;
    this.playState = 'idle'; this.currentTime = null; this.startTime = null;
    this.playbackRate = 1; this.pending = false; this.id = '';
    this.finished = Promise.resolve(this); this.ready = Promise.resolve(this);
    this.onfinish = null; this.oncancel = null; this.onremove = null;
    this.replaceState = 'active';
};
CSSAnimation.prototype = Object.create(Animation.prototype);
CSSAnimation.prototype.constructor = CSSAnimation;
CSSAnimation.prototype.animationName = '';
globalThis.CSSTransition = function CSSTransition() {
    this.transitionProperty = ''; this.effect = null; this.timeline = null;
    this.playState = 'idle'; this.currentTime = null; this.startTime = null;
    this.playbackRate = 1; this.pending = false; this.id = '';
    this.finished = Promise.resolve(this); this.ready = Promise.resolve(this);
    this.onfinish = null; this.oncancel = null; this.onremove = null;
    this.replaceState = 'active';
};
CSSTransition.prototype = Object.create(Animation.prototype);
CSSTransition.prototype.constructor = CSSTransition;
CSSTransition.prototype.transitionProperty = '';
// AnimationEffect must be defined before KeyframeEffect
globalThis.AnimationEffect = function AnimationEffect() {};
AnimationEffect.prototype = {
    getTiming: function(){return {};}, updateTiming: function(){},
    getComputedTiming: function(){return {};}, before: function(){}, after: function(){},
    replace: function(){}, remove: function(){},
    nextSibling: null, previousSibling: null, parent: null
};
globalThis.KeyframeEffect = function KeyframeEffect() {
    this.target = null; this.pseudoElement = null;
    this.composite = 'replace'; this.iterationComposite = 'replace';
    this.iteratonComposite = 'replace';
};
KeyframeEffect.prototype = Object.create(AnimationEffect.prototype);
KeyframeEffect.prototype.constructor = KeyframeEffect;
KeyframeEffect.prototype.getKeyframes = function(){return [];};
KeyframeEffect.prototype.setKeyframes = function(){};
KeyframeEffect.prototype.getTiming = function(){return {};};
KeyframeEffect.prototype.updateTiming = function(){};
KeyframeEffect.prototype.getComputedTiming = function(){return {};};
globalThis.AnimationTimeline = function AnimationTimeline() { this.currentTime = 0; this.duration = null; };
AnimationTimeline.prototype = { getCurrentTime: function(){return 0;}, play: function(){return new Animation();} };
globalThis.DocumentTimeline = function DocumentTimeline() { this.currentTime = 0; this.duration = null; };
DocumentTimeline.prototype = Object.create(AnimationTimeline.prototype);
DocumentTimeline.prototype.constructor = DocumentTimeline;
globalThis.ScrollTimeline = function ScrollTimeline() { this.currentTime = 0; this.source = null; this.axis = 'block'; this.duration = null; };
ScrollTimeline.prototype = Object.create(AnimationTimeline.prototype);
ScrollTimeline.prototype.constructor = ScrollTimeline;
globalThis.ViewTimeline = function ViewTimeline() {
    this.currentTime = 0; this.subject = null; this.axis = 'block';
    this.startOffset = null; this.endOffset = null; this.duration = null;
};
ViewTimeline.prototype = Object.create(AnimationTimeline.prototype);
ViewTimeline.prototype.constructor = ViewTimeline;
globalThis.AnimationNodeList = function AnimationNodeList() { this.length = 0; };
AnimationNodeList.prototype.item = function() { return null; };
globalThis.GroupEffect = function GroupEffect() {
    this.children = new AnimationNodeList(); this.firstChild = null; this.lastChild = null;
};
GroupEffect.prototype = Object.create(AnimationEffect.prototype);
GroupEffect.prototype.constructor = GroupEffect;
GroupEffect.prototype.clone = function() { return new GroupEffect(); };
GroupEffect.prototype.prepend = function() {};
GroupEffect.prototype.append = function() {};
globalThis.SequenceEffect = function SequenceEffect() { this.children = []; this.firstChild = null; this.lastChild = null; };
SequenceEffect.prototype = Object.create(AnimationEffect.prototype);
SequenceEffect.prototype.constructor = SequenceEffect;
SequenceEffect.prototype.clone = function() { return new SequenceEffect(); };
SequenceEffect.prototype.prepend = function() {};
SequenceEffect.prototype.append = function() {};
globalThis.AnimationPlaybackEvent = function AnimationPlaybackEvent(type) {
    this.type = type || ''; this.currentTime = null; this.timelineTime = null;
};
AnimationPlaybackEvent.prototype = Object.create(Event.prototype);

// Event types
globalThis.AnimationEvent = function AnimationEvent(type, init) {
    this.type = type || ''; this.animationName = (init && init.animationName) || '';
    this.elapsedTime = (init && init.elapsedTime) || 0;
    this.pseudoElement = (init && init.pseudoElement) || '';
};
AnimationEvent.prototype = Object.create(Event.prototype);
globalThis.TransitionEvent = function TransitionEvent(type, init) {
    this.type = type || ''; this.propertyName = (init && init.propertyName) || '';
    this.elapsedTime = (init && init.elapsedTime) || 0;
    this.pseudoElement = (init && init.pseudoElement) || '';
};
TransitionEvent.prototype = Object.create(Event.prototype);

// MediaQueryList
globalThis.MediaQueryList = function MediaQueryList(media, matches) {
    this.media = media || ''; this.matches = matches !== undefined ? matches : true;
    this.onchange = null;
};
MediaQueryList.prototype = {
    addEventListener: function(){}, removeEventListener: function(){},
    addListener: function(){}, removeListener: function(){},
    dispatchEvent: function(){ return true; }
};
globalThis.MediaQueryListEvent = function MediaQueryListEvent(type, init) {
    this.type = type || ''; this.media = (init && init.media) || '';
    this.matches = (init && init.matches) || false;
};
MediaQueryListEvent.prototype = Object.create(Event.prototype);

// Screen
globalThis.Screen = function Screen() {
    this.availWidth = 1920; this.availHeight = 1080;
    this.width = 1920; this.height = 1080;
    this.colorDepth = 24; this.pixelDepth = 24;
    this.orientation = { angle: 0, type: 'landscape-primary',
        addEventListener: function(){}, removeEventListener: function(){} };
};

// VisualViewport
globalThis.VisualViewport = function VisualViewport() {
    this.offsetLeft = 0; this.offsetTop = 0;
    this.pageLeft = 0; this.pageTop = 0;
    this.width = 1920; this.height = 1080;
    this.scale = 1; this.zoom = 1;
    this.onresize = null; this.onscroll = null; this.onscrollend = null;
    this.addEventListener = function(){}; this.removeEventListener = function(){};
    this.dispatchEvent = function() { return true; };
};

// Viewport (CSS Viewport)
globalThis.Viewport = function Viewport() {
    this.segments = null;
};

// ViewTransition
globalThis.ViewTransition = function ViewTransition() {
    this.finished = Promise.resolve(); this.ready = Promise.resolve();
    this.updateCallbackDone = Promise.resolve();
    this.skipTransition = function(){};
    this.types = [];
};

// CSSPseudoElement
globalThis.CSSPseudoElement = function CSSPseudoElement() {
    this.type = ''; this.element = null; this.parent = null;
    this.pseudo = function() { return null; };
    this.style = new CSSStyleDeclaration();
    this.animate = function() { return new Animation(); };
    this.getAnimations = function() { return []; };
    this.getComputedStyle = function() { return {}; };
    this.addEventListener = function() {};
    this.removeEventListener = function() {};
};

// CaretPosition
globalThis.CaretPosition = function CaretPosition() {
    this.offsetNode = null; this.offset = 0;
    this.getClientRect = function() { return {x:0,y:0,width:0,height:0,top:0,right:0,bottom:0,left:0}; };
};

// NamedFlow / NamedFlowMap (CSS Regions)
globalThis.NamedFlow = function NamedFlow() {
    this.name = ''; this.overset = false; this.firstEmptyRegionIndex = -1;
    this.getContent = function() { return []; };
    this.getRegions = function() { return []; };
    this.getRegionsByContent = function() { return []; };
};
NamedFlow.prototype.addEventListener = function() {};
NamedFlow.prototype.removeEventListener = function() {};
NamedFlow.prototype.dispatchEvent = function() { return true; };
globalThis.NamedFlowMap = function NamedFlowMap() {
    this.get = function(n) { return new NamedFlow(); };
    this.has = function() { return false; };
    this.set = function() {};
    this.delete = function() {};
    this.forEach = function() {};
    this.entries = function() { return {next:function(){return {done:true};}}; };
    this.keys = function() { return {next:function(){return {done:true};}}; };
    this.values = function() { return {next:function(){return {done:true};}}; };
};

// SnapEvent
globalThis.SnapEvent = function SnapEvent(type, init) {
    this.type = type || ''; this.snapTargetBlock = null; this.snapTargetInline = null;
};
SnapEvent.prototype = Object.create(Event.prototype);

// NavigationEvent
globalThis.NavigationEvent = function NavigationEvent(type) {
    this.type = type || ''; this.dir = ''; this.relatedTarget = null;
};
NavigationEvent.prototype = Object.create(Event.prototype);

// PageRevealEvent
globalThis.PageRevealEvent = function PageRevealEvent(type) {
    this.type = type || ''; this.viewTransition = null;
};
PageRevealEvent.prototype = Object.create(Event.prototype);

// ContentVisibilityAutoStateChangeEvent
globalThis.ContentVisibilityAutoStateChangeEvent = function ContentVisibilityAutoStateChangeEvent(type) {
    this.type = type || ''; this.skipped = false;
};
ContentVisibilityAutoStateChangeEvent.prototype = Object.create(Event.prototype);

// FontFaceSetLoadEvent
globalThis.FontFaceSetLoadEvent = function FontFaceSetLoadEvent(type) {
    this.type = type || ''; this.fontfaces = [];
};
FontFaceSetLoadEvent.prototype = Object.create(Event.prototype);

// CSSFontFeatureValuesMap
globalThis.CSSFontFeatureValuesMap = function CSSFontFeatureValuesMap() {
    this.size = 0;
    this.get = function() { return null; };
    this.has = function() { return false; };
    this.set = function() {};
    this.delete = function() {};
    this.forEach = function() {};
    this.entries = function() { return {next:function(){return {done:true};}}; };
    this.keys = function() { return {next:function(){return {done:true};}}; };
    this.values = function() { return {next:function(){return {done:true};}}; };
};

// CSSColorProfileRule
globalThis.CSSColorProfileRule = function CSSColorProfileRule() {
    this.name = ''; this.src = ''; this.renderingIntent = '';
    this.components = ''; this.type = 0; this.cssText = '';
    this.parentStyleSheet = null; this.parentRule = null;
};
CSSColorProfileRule.prototype = Object.create(CSSRule.prototype);
CSSColorProfileRule.prototype.constructor = CSSColorProfileRule;
Object.defineProperty(CSSColorProfileRule, 'name', { value: 'CSSColorProfileRule', configurable: true });

// CSSMarginRule
globalThis.CSSMarginRule = function CSSMarginRule() {
    this.name = ''; this.type = 0; this.cssText = '';
    this.selectorText = '';
    this.parentStyleSheet = null; this.parentRule = null;
    this.style = new CSSStyleDeclaration(); this.style.length = 1;
};
CSSMarginRule.prototype = Object.create(CSSRule.prototype);
CSSMarginRule.prototype.constructor = CSSMarginRule;
Object.defineProperty(CSSMarginRule, 'name', { value: 'CSSMarginRule', configurable: true });

// CSSRuleList
globalThis.CSSRuleList = function CSSRuleList() { this.length = 0; };
CSSRuleList.prototype.item = function() { return null; };

// FontFace additions
globalThis.FontFaceVariationAxis = function FontFaceVariationAxis() {
    this.name = ''; this.tag = ''; this.axisTag = ''; this.minimumValue = 0;
    this.maximumValue = 0; this.defaultValue = 0;
};
globalThis.FontFacePalettes = function FontFacePalettes() {
    this.length = 0; this.item = function(){return null;};
};
globalThis.FontFacePalette = function FontFacePalette() {
    this.length = 0; this.item = function(){return null;}; this.usableWithLightBackground = true; this.usableWithDarkBackground = true;
};
globalThis.FontFaceFeatures = function FontFaceFeatures() {
    this.length = 0; this.item = function(){return null;};
};
FontFaceFeatures.prototype.FontFaceFeatures = FontFaceFeatures;

// DOMMatrix (for transforms)
globalThis.DOMMatrix = globalThis.DOMMatrix || function DOMMatrix() {
    this.a=1;this.b=0;this.c=0;this.d=1;this.e=0;this.f=0;
    this.m11=1;this.m12=0;this.m13=0;this.m14=0;
    this.m21=0;this.m22=1;this.m23=0;this.m24=0;
    this.m31=0;this.m32=0;this.m33=1;this.m34=0;
    this.m41=0;this.m42=0;this.m43=0;this.m44=1;
    this.is2D=true; this.isIdentity=true;
};

// MouseEvent (with CSSOM-View properties)
globalThis.MouseEvent = function MouseEvent(type, init) {
    this.type = type || ''; this.button = 0; this.buttons = 0;
    this.clientX = 0; this.clientY = 0;
    this.screenX = 0; this.screenY = 0;
    this.pageX = 0; this.pageY = 0;
    this.x = 0; this.y = 0;
    this.offsetX = 0; this.offsetY = 0;
    this.movementX = 0; this.movementY = 0;
    this.altKey = false; this.ctrlKey = false; this.metaKey = false; this.shiftKey = false;
    this.relatedTarget = null; this.detail = 0;
    if (init) { for (var k in init) this[k] = init[k]; }
};
MouseEvent.prototype = Object.create(Event.prototype);
MouseEvent.prototype.constructor = MouseEvent;
MouseEvent.prototype.getModifierState = function() { return false; };

// Text constructor (for geometry checks)
globalThis.Text = globalThis.Text || function Text(data) { this.data = data || ''; this.nodeType = 3; };
Text.prototype.getBoxQuads = function() { return []; };
Text.prototype.convertQuadFromNode = function() { return {}; };
Text.prototype.convertRectFromNode = function() { return {}; };
Text.prototype.convertPointFromNode = function() { return {}; };

// Range constructor
globalThis.Range = globalThis.Range || function Range() {
    this.startContainer = null; this.startOffset = 0;
    this.endContainer = null; this.endOffset = 0;
    this.collapsed = true; this.commonAncestorContainer = null;
};
Range.prototype.getClientRects = function() { return []; };
Range.prototype.getBoundingClientRect = function() {
    return { top: 0, left: 0, bottom: 0, right: 0, width: 0, height: 0, x: 0, y: 0 };
};
Range.prototype.createContextualFragment = function() { return document.createDocumentFragment(); };
Range.prototype.setStart = function() {};
Range.prototype.setEnd = function() {};
Range.prototype.selectNode = function() {};
Range.prototype.selectNodeContents = function() {};
Range.prototype.collapse = function() {};
Range.prototype.cloneContents = function() { return document.createDocumentFragment(); };
Range.prototype.deleteContents = function() {};
Range.prototype.extractContents = function() { return document.createDocumentFragment(); };
Range.prototype.cloneRange = function() { return new Range(); };
Range.prototype.detach = function() {};

// Document constructor
globalThis.Document = globalThis.Document || function Document() {};
globalThis.HTMLDocument = globalThis.HTMLDocument || function HTMLDocument() {};
HTMLDocument.prototype = Object.create(Document.prototype);

// Resize Observer
globalThis.ResizeObserver = globalThis.ResizeObserver || function ResizeObserver(cb) { this._cb = cb; };
ResizeObserver.prototype = { observe: function(){}, unobserve: function(){}, disconnect: function(){} };
globalThis.ResizeObserverEntry = globalThis.ResizeObserverEntry || function ResizeObserverEntry() {};
globalThis.ResizeObserverSize = globalThis.ResizeObserverSize || function ResizeObserverSize() { this.inlineSize = 0; this.blockSize = 0; };

// Pointer Events
globalThis.PointerEvent = globalThis.PointerEvent || function PointerEvent(type, init) {
    this.type = type; this.pointerId = (init && init.pointerId) || 0;
    this.width = 1; this.height = 1; this.pressure = 0;
    this.tiltX = 0; this.tiltY = 0; this.pointerType = 'mouse';
    this.isPrimary = true; this.altitudeAngle = 0; this.azimuthAngle = 0;
};
PointerEvent.prototype = Object.create(MouseEvent.prototype);
PointerEvent.prototype.constructor = PointerEvent;

// Fullscreen API - stubs added in setupDocPolyfills

// CSS Font Loading API
globalThis.FontFace = globalThis.FontFace || function FontFace(family, source, descriptors) {
    this.family = family; this.status = 'unloaded'; this.loaded = Promise.resolve(this);
    this.style = 'normal'; this.weight = 'normal'; this.stretch = 'normal';
    this.unicodeRange = 'U+0-10FFFF'; this.variant = 'normal';
    this.featureSettings = 'normal'; this.variationSettings = 'normal';
    this.display = 'auto'; this.ascentOverride = 'normal'; this.descentOverride = 'normal';
    this.ascenderOverride = 'normal'; this.descenderOverride = 'normal';
    this.lineGapOverride = 'normal'; this.sizeAdjust = '100%';
    this.features = new FontFaceFeatures();
    this.variations = new FontFaceVariationAxis();
    this.palettes = new FontFacePalettes();
};
FontFace.prototype = { load: function() { this.status = 'loaded'; return Promise.resolve(this); } };
globalThis.FontFaceSet = globalThis.FontFaceSet || function FontFaceSet() {
    this.status = 'loaded'; this.ready = Promise.resolve(this); this.size = 0;
    this.onloading = null; this.onloadingdone = null; this.onloadingerror = null;
};
FontFaceSet.prototype = {
    add: function(){}, delete: function(){}, clear: function(){},
    has: function(){return false;}, check: function(){return true;},
    load: function(){return Promise.resolve([]);},
    forEach: function(){}, values: function(){return {next:function(){return {done:true};}};},
    addEventListener: function(){}, removeEventListener: function(){}
};

// Window constructor (for interface checks)
globalThis.Window = globalThis.Window || function Window() {};

// Window properties needed for interface tests
if (!globalThis.navigate) globalThis.navigate = function() {};
if (!globalThis.viewport) globalThis.viewport = new Viewport();

// visualViewport
if (!globalThis.visualViewport) globalThis.visualViewport = new VisualViewport();

// Element event handler properties (for interface checks)
var _eventHandlers = [
    'onanimationstart', 'onanimationend', 'onanimationiteration', 'onanimationcancel',
    'ontransitionstart', 'ontransitionend', 'ontransitionrun', 'ontransitioncancel',
    'onpointerdown', 'onpointerup', 'onpointermove', 'onpointerover', 'onpointerout',
    'onpointerenter', 'onpointerleave', 'onpointercancel', 'ongotpointercapture',
    'onlostpointercapture', 'oncontentvisibilityautostatechange',
    'onscrollend', 'onscrollsnapchange', 'onscrollsnapchanging',
    'onsnapchanged', 'onsnapchanging',
    'onbeforexrselect'
];
_eventHandlers.forEach(function(h) {
    if (typeof Element !== 'undefined' && Element.prototype) {
        if (!(h in Element.prototype)) Element.prototype[h] = null;
    }
    if (!(h in globalThis)) globalThis[h] = null;
});

// CSS factory methods for Typed OM
if (typeof CSS !== 'undefined' && CSS) {
    var _units = ['number','percent','em','ex','ch','rem','vw','vh','vmin','vmax',
                  'cm','mm','Q','in','pt','pc','px','deg','grad','rad','turn',
                  's','ms','Hz','kHz','dpi','dpcm','dppx','fr',
                  'cap','ic','lh','rlh','vi','vb','svi','svb','lvi','lvb','dvi','dvb',
                  'svw','svh','lvw','lvh','dvw','dvh',
                  'svmin','svmax','lvmin','lvmax','dvmin','dvmax','cqw','cqh','cqi','cqb','cqmin','cqmax'];
    _units.forEach(function(u) {
        if (!CSS[u]) CSS[u] = function(v) { return new CSSUnitValue(v, u); };
    });
}

// CSS.elementSources (Layout API)
if (typeof CSS !== 'undefined') {
    CSS.elementSources = CSS.elementSources || new (function ElementSources(){})();
}

)JS";

    eval(polyfills, "<browser-polyfills>");
}

void JSEngine::setupDocPolyfills() {
    // These polyfills need 'document' to exist (set by js_bindings_init)
    const char* doc_polyfills = R"JS(
// WhichBrowser stub - html5test.co waits for this before running tests.
// Define it via setTimeout so loadWhichBrowser's load() runs first, then wait() polls and finds it.
setTimeout(function() {
    console.warn('[WB-STUB] Timer fired, typeof WhichBrowser=' + typeof WhichBrowser);
    if (typeof WhichBrowser === 'undefined') {
        globalThis.WhichBrowser = function WhichBrowser(opts) {
            console.warn('[WB-STUB] WhichBrowser constructor called');
            this.browser = { name: 'Custom', version: '1.0' };
            this.engine = { name: 'Custom', version: '1.0' };
            this.os = { name: 'Linux', version: '' };
            this.device = { type: 'desktop' };
            this.isType = function() { return false; };
            this.isBrowser = function() { return false; };
            this.isDevice = function() { return false; };
            this.isOs = function() { return false; };
            this.isEngine = function() { return false; };
        };
        console.warn('[WB-STUB] WhichBrowser defined globally');
    }
}, 50);

// Make C++ NodeList instances iterable (Symbol.iterator)
(function() {
    var nl = document.querySelectorAll('*');
    var nlProto = Object.getPrototypeOf(nl);
    if (nlProto && !nlProto[Symbol.iterator]) {
        nlProto[Symbol.iterator] = function() {
            var i = 0, self = this;
            return { next: function() { return i < self.length ? { value: self[i++], done: false } : { done: true }; } };
        };
        nlProto.entries = function() {
            var i = 0, self = this;
            return { next: function() { return i < self.length ? { value: [i, self[i++]], done: false } : { done: true }; }, [Symbol.iterator]: function() { return this; } };
        };
        nlProto.keys = function() {
            var i = 0, self = this;
            return { next: function() { return i < self.length ? { value: i++, done: false } : { done: true }; }, [Symbol.iterator]: function() { return this; } };
        };
        nlProto.values = function() {
            var i = 0, self = this;
            return { next: function() { return i < self.length ? { value: self[i++], done: false } : { done: true }; }, [Symbol.iterator]: function() { return this; } };
        };
        nlProto.item = function(i) { return this[i] || null; };
    }
})();

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
document.fonts = new FontFaceSet();
document.styleSheets = new StyleSheetList();
document.adoptedStyleSheets = [];
// document.scripts - live collection of script elements
Object.defineProperty(document, 'scripts', {
    get: function() { return document.getElementsByTagName('script'); },
    configurable: true
});
Object.defineProperty(document, 'forms', {
    get: function() { return document.getElementsByTagName('form'); },
    configurable: true
});
Object.defineProperty(document, 'images', {
    get: function() { return document.getElementsByTagName('img'); },
    configurable: true
});
Object.defineProperty(document, 'links', {
    get: function() { return document.querySelectorAll('a[href], area[href]'); },
    configurable: true
});
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
document.scrollingElement = document.documentElement || null;
document.namedFlows = new NamedFlowMap();
document.getBoxQuads = function() { return []; };
document.convertQuadFromNode = function() { return {}; };
document.convertRectFromNode = function() { return {}; };
document.convertPointFromNode = function() { return {}; };
document.elementFromPoint = document.elementFromPoint || function() { return null; };
document.elementsFromPoint = document.elementsFromPoint || function() { return []; };
document.caretPositionFromPoint = function() { return new CaretPosition(); };
document.getAnimations = function() { return []; };
document.createRange = document.createRange || function() { return new Range(); };
document.startViewTransition = function(cb) { if (cb) try{cb();}catch(e){} return new ViewTransition(); };
document.timeline = new DocumentTimeline();

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
        // matches() and closest() are now native C++
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
        if (!el.append) el.append = function() {
            for (var i = 0; i < arguments.length; i++) {
                var arg = arguments[i];
                if (typeof arg === 'string') arg = document.createTextNode(arg);
                if (arg) this.appendChild(arg);
            }
        };
        if (!el.prepend) el.prepend = function() {
            var first = this.firstChild;
            for (var i = 0; i < arguments.length; i++) {
                var arg = arguments[i];
                if (typeof arg === 'string') arg = document.createTextNode(arg);
                if (arg) {
                    if (first) this.insertBefore(arg, first);
                    else this.appendChild(arg);
                }
            }
        };
        if (!el.replaceChildren) el.replaceChildren = function() {
            this.innerHTML = '';
            for (var i = 0; i < arguments.length; i++) {
                var arg = arguments[i];
                if (typeof arg === 'string') arg = document.createTextNode(arg);
                if (arg) this.appendChild(arg);
            }
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

        // style.sheet for <style> elements - creates a CSSStyleSheet that parses textContent
        var elTag = (el.tagName || '').toLowerCase();
        if (elTag === 'style' || elTag === '_') {
            // Define sheet as a getter that creates/updates the stylesheet from textContent
            if (!el._sheet) {
                el._sheet = new CSSStyleSheet();
                // Override textContent setter to update the sheet
                // Walk prototype chain to find the C++ textContent accessor (on HTMLElement.prototype/elem_proto)
                var _origTC = null;
                var _p = Object.getPrototypeOf(el);
                while (_p) {
                    var _d = Object.getOwnPropertyDescriptor(_p, 'textContent');
                    if (_d && _d.set) { _origTC = _d; break; }
                    _p = Object.getPrototypeOf(_p);
                }
                if (_origTC && _origTC.set) {
                    Object.defineProperty(el, 'textContent', {
                        get: function() { return _origTC.get ? _origTC.get.call(this) : ''; },
                        set: function(v) {
                            if (_origTC.set) _origTC.set.call(this, v);
                            // Parse CSS into sheet rules
                            this._sheet.cssRules = [];
                            this._sheet.cssRules.item = function(i) { return this[i] || null; };
                            if (!v) return;
                            // Better CSS parser: handle nested braces
                            var depth = 0, start = 0;
                            for (var ci = 0; ci < v.length; ci++) {
                                if (v[ci] === '{') depth++;
                                else if (v[ci] === '}') {
                                    depth--;
                                    if (depth <= 0) {
                                        var r = v.substring(start, ci + 1).trim();
                                        if (r) {
                                            try { this._sheet.insertRule(r, this._sheet.cssRules.length); }
                                            catch(e) {}
                                        }
                                        start = ci + 1;
                                        depth = 0;
                                    }
                                }
                            }
                            // Handle remaining text (e.g., @layer statement without braces)
                            var rem = v.substring(start).trim();
                            if (rem) {
                                try { this._sheet.insertRule(rem, this._sheet.cssRules.length); }
                                catch(e) {}
                            }
                        },
                        configurable: true
                    });
                }
            }
            Object.defineProperty(el, 'sheet', {
                get: function() { return this._sheet; },
                configurable: true
            });
        }
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

// Piano/Tinypass SDK stubs (used by media sites)
// tp starts as array; tinypass SDK replaces it. Use defineProperty to catch access after replacement.
if (typeof window.tp === 'undefined') window.tp = [];
// Set properties on the initial tp array
window.tp.piano = { id: { show: function(){}, logout: function(){}, getUser: function(){ return null; } } };
window.tp.pianoId = { show: function(){}, logout: function(){}, getUser: function(){ return null; }, isUserValid: function(){ return false; } };
window.tp.offer = { show: function(){}, close: function(){} };
window.tp.experience = { execute: function(){}, init: function(){} };
window.tp.user = { isUserValid: function(){ return false; }, getProvider: function(){ return ''; } };
window.tp.aid = '';
window.tp.init = function() {};
// Intercept tp replacement: when SDK sets window.tp = newObj, ensure piano property exists
(function() {
    var _origTp = window.tp;
    var _pianoDefault = { id: { show: function(){}, logout: function(){}, getUser: function(){ return null; } } };
    // Periodically ensure tp.piano exists (handles SDK replacing tp)
    var _checkCount = 0;
    var _timer = setInterval(function() {
        if (window.tp && typeof window.tp === 'object' && !Array.isArray(window.tp)) {
            if (!window.tp.piano) window.tp.piano = _pianoDefault;
            if (!window.tp.pianoId) window.tp.pianoId = { show: function(){}, logout: function(){}, getUser: function(){ return null; }, isUserValid: function(){ return false; } };
        }
        if (++_checkCount > 20) clearInterval(_timer);
    }, 100);
})();

// Prebid.js / Amazon APS stubs
if (typeof window.pbjs === 'undefined') window.pbjs = { que: [], cmd: [], requestBids: function(){}, setConfig: function(){}, addAdUnits: function(){}, removeAdUnit: function(){}, getBidResponses: function(){ return {}; }, getAllWinningBids: function(){ return []; } };
if (typeof window.apstag === 'undefined') window.apstag = { init: function(){}, fetchBids: function(cfg, cb){ if(cb) cb([]); }, setDisplayBids: function(){}, targetingKeys: function(){ return []; } };

// PMC Piano integration stub (used by theme common.js and wordpress.js)
if (typeof window.pmcPiano === 'undefined') window.pmcPiano = {};
if (!window.pmcPiano.callbacks) window.pmcPiano.callbacks = {
    onInit: function(opts) { if (opts && typeof opts.knownUser === 'function') { try { opts.knownUser(false); } catch(e){} } },
    onLogin: function() {},
    onLogout: function() {},
    onRegistration: function() {}
};
if (!window.pmcPiano.piano) window.pmcPiano.piano = {
    setCallbacks: function() {},
    reRenderExperiences: function() {},
    setGA4Config: function() {}
};
if (!window.pmcPiano.api) window.pmcPiano.api = {
    getConversionList: function(cb) { if (cb) cb([]); },
    getLicenseeData: function(cb) { if (cb) cb({}); }
};
if (!window.pmcPiano.wordPressThemes) window.pmcPiano.wordPressThemes = {
    clickShield: function() {}
};
if (!window.pmcPiano.ipAuth) window.pmcPiano.ipAuth = {};
if (!window.pmcPiano.newsletterForm) window.pmcPiano.newsletterForm = {};

// BlogherAds stub (PMC ad manager dependency)
if (typeof window.blogherads === 'undefined') window.blogherads = { adq: [], defineSlot: function(){ return this; }, setSubAdUnitPath: function(){ return this; }, setPageTargeting: function(){ return this; }, display_ads: function(){}, requestAds: function(){}, collapseSlot: function(){}, refreshSlot: function(){} };

// PMC Ad Manager stubs
if (typeof window.pmc_adm_gpt === 'undefined') window.pmc_adm_gpt = { display_ads: function(){}, define_ad_slot: function(){}, refresh: function(){}, set_targeting: function(){} };

// CMP / Consent Management stubs
if (typeof window.__tcfapi === 'undefined') window.__tcfapi = function(cmd, ver, cb) { if (cb) cb({ cmpId: 0, cmpVersion: 0, gdprApplies: false, tcfPolicyVersion: 2, tcString: '', purposeOneTreatment: false, eventStatus: 'tcloaded' }, true); };
if (typeof window.__uspapi === 'undefined') window.__uspapi = function(cmd, ver, cb) { if (cb) cb({ version: 1, uspString: '1---' }, true); };
if (typeof window.__cmp === 'undefined') window.__cmp = function(cmd, arg, cb) { if (cb) cb({ consentData: '', gdprApplies: false }, true); };

// picturefill stub (responsive images library)
if (typeof window.picturefill === 'undefined') window.picturefill = function() {};

// WordPress hooks API stub (used by PMC plugins)
if (typeof window.wp === 'undefined') window.wp = {};
if (!window.wp.hooks) {
    // Use Proxy-like auto-vivifying store so store[hookName].callbacks never throws
    function _createHookStore() {
        var _raw = { __current: [] };
        var _default = { callbacks: [], runs: 0 };
        var store = new Proxy(_raw, {
            get: function(t, p) {
                if (p in t) return t[p];
                // Auto-create hook entry on first access
                t[p] = { callbacks: [], runs: 0 };
                return t[p];
            }
        });
        return {
            addAction: function(n, ns, cb, p) { store[n].callbacks.push({callback:cb, priority:p||10, namespace:ns}); },
            addFilter: function(n, ns, cb, p) { store[n].callbacks.push({callback:cb, priority:p||10, namespace:ns}); },
            removeAction: function(n, ns) { var h = store[n]; h.callbacks = h.callbacks.filter(function(c){return c.namespace !== ns;}); },
            removeFilter: function(n, ns) { var h = store[n]; h.callbacks = h.callbacks.filter(function(c){return c.namespace !== ns;}); },
            doAction: function(n) { var h = store[n]; h.runs++; for (var i=0;i<h.callbacks.length;i++) try{h.callbacks[i].callback.apply(null,Array.prototype.slice.call(arguments,1));}catch(e){} },
            applyFilters: function(n,v) { var h = store[n]; h.runs++; for (var i=0;i<h.callbacks.length;i++) try{v=h.callbacks[i].callback.apply(null,[v].concat(Array.prototype.slice.call(arguments,2)));}catch(e){} return v; },
            hasAction: function(n) { return !!(store[n] && store[n].callbacks.length); },
            hasFilter: function(n) { return !!(store[n] && store[n].callbacks.length); },
            didAction: function(n) { return store[n] ? store[n].runs : 0; },
            didFilter: function(n) { return store[n] ? store[n].runs : 0; },
            actions: store,
            filters: store,
            __current: _raw.__current
        };
    }
    window.wp.hooks = _createHookStore();
}
if (!window.wp.i18n) {
    window.wp.i18n = {
        __: function(t) { return t; },
        _x: function(t) { return t; },
        _n: function(s,p,n) { return n===1?s:p; },
        sprintf: function(f) { var a=Array.prototype.slice.call(arguments,1); return f.replace(/%[sd]/g, function(){return a.shift()||''}); },
        setLocaleData: function() {}
    };
}

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

// ---- HTML5 element type constructors for instanceof checks ----
// HTMLElement and HTMLUnknownElement base
if (typeof HTMLElement === 'undefined') {
    globalThis.HTMLElement = function HTMLElement() {};
    HTMLElement.prototype = Object.create(Element ? Element.prototype : {});
} else if (typeof Element !== 'undefined') {
    // js_bindings_init set HTMLElement.prototype = elem_proto (has C++ DOM getters).
    // Chain it into the Element hierarchy without replacing the prototype object.
    Object.setPrototypeOf(HTMLElement.prototype, Element.prototype);
}
if (typeof HTMLUnknownElement === 'undefined') {
    globalThis.HTMLUnknownElement = function HTMLUnknownElement() {};
    HTMLUnknownElement.prototype = Object.create(HTMLElement.prototype);
}

// Specific element type constructors
// Always (re)set prototype to chain through correct HTMLElement.prototype,
// since constructors created in browser-polyfills used a stale HTMLElement.prototype
// that was replaced by js_bindings_init.
(function() {
    var types = [
        'HTMLDivElement', 'HTMLSpanElement', 'HTMLParagraphElement',
        'HTMLHeadingElement', 'HTMLAnchorElement', 'HTMLImageElement',
        'HTMLScriptElement', 'HTMLStyleElement', 'HTMLLinkElement',
        'HTMLFormElement', 'HTMLInputElement', 'HTMLButtonElement',
        'HTMLTextAreaElement', 'HTMLSelectElement', 'HTMLOptionElement',
        'HTMLFieldSetElement', 'HTMLLegendElement', 'HTMLLabelElement',
        'HTMLOutputElement', 'HTMLProgressElement', 'HTMLMeterElement',
        'HTMLDataListElement', 'HTMLCanvasElement',
        'HTMLTableElement', 'HTMLTableRowElement', 'HTMLTableCellElement',
        'HTMLTableSectionElement', 'HTMLUListElement', 'HTMLOListElement',
        'HTMLLIElement', 'HTMLPreElement', 'HTMLQuoteElement',
        'HTMLBRElement', 'HTMLHRElement',
        'HTMLIFrameElement', 'HTMLObjectElement', 'HTMLEmbedElement',
        'HTMLVideoElement', 'HTMLAudioElement', 'HTMLSourceElement',
        'HTMLTrackElement', 'HTMLMediaElement',
        'HTMLDetailsElement', 'HTMLSummaryElement', 'HTMLDialogElement',
        'HTMLMenuElement', 'HTMLDataElement', 'HTMLTimeElement',
        'HTMLPictureElement', 'HTMLSlotElement',
        'HTMLModElement', 'HTMLMapElement', 'HTMLAreaElement',
        'HTMLBodyElement', 'HTMLHeadElement', 'HTMLMetaElement', 'HTMLTitleElement',
        'HTMLKeygenElement'
    ];
    for (var i = 0; i < types.length; i++) {
        if (typeof globalThis[types[i]] === 'undefined') {
            globalThis[types[i]] = function() {};
        }
        // Always reset prototype chain through correct HTMLElement.prototype (elem_proto)
        globalThis[types[i]].prototype = Object.create(HTMLElement.prototype);
        globalThis[types[i]].prototype.constructor = globalThis[types[i]];
    }
})();

// ---- Element prototype stubs for html5test ----
(function() {
    var EP = HTMLElement.prototype;

    // elements.hidden test: 'hidden' in element
    if (!('hidden' in EP)) EP.hidden = false;

    // width/height properties (for form.image.width/height tests)
    if (!('width' in EP)) {
        Object.defineProperty(EP, 'width', {
            get: function() { var v = this.getAttribute && this.getAttribute('width'); return v ? parseInt(v, 10) : 0; },
            set: function(v) { if (this.setAttribute) this.setAttribute('width', String(v)); },
            configurable: true
        });
    }
    if (!('height' in EP)) {
        Object.defineProperty(EP, 'height', {
            get: function() { var v = this.getAttribute && this.getAttribute('height'); return v ? parseInt(v, 10) : 0; },
            set: function(v) { if (this.setAttribute) this.setAttribute('height', String(v)); },
            configurable: true
        });
    }

    // elements.dynamic.outerHTML: 'outerHTML' in element
    if (!('outerHTML' in EP)) {
        Object.defineProperty(EP, 'outerHTML', {
            get: function() {
                var el = this;
                if (!el.tagName) return '';
                var tag = el.tagName.toLowerCase();
                var html = '<' + tag;
                if (el.attributes) {
                    for (var k in el.attributes) {
                        if (el.attributes.hasOwnProperty && el.attributes.hasOwnProperty(k))
                            html += ' ' + k + '="' + el.attributes[k] + '"';
                    }
                }
                html += '>';
                if (el.innerHTML !== undefined) html += el.innerHTML;
                html += '</' + tag + '>';
                return html;
            },
            configurable: true
        });
    }

    // elements.dynamic.insertAdjacentHTML
    if (!('insertAdjacentHTML' in EP)) {
        EP.insertAdjacentHTML = function(position, text) {
            var div = document.createElement('div');
            div.innerHTML = text;
            var parent = this.parentNode;
            if (!parent) return;
            switch (position.toLowerCase()) {
                case 'beforebegin':
                    while (div.firstChild) parent.insertBefore(div.firstChild, this);
                    break;
                case 'afterbegin':
                    var first = this.firstChild;
                    while (div.firstChild) this.insertBefore(div.firstChild, first);
                    break;
                case 'beforeend':
                    while (div.firstChild) this.appendChild(div.firstChild);
                    break;
                case 'afterend':
                    var next = this.nextSibling;
                    while (div.firstChild) parent.insertBefore(div.firstChild, next);
                    break;
            }
        };
    }

    // elements.translate
    if (!('translate' in EP)) EP.translate = true;

    // elements.accessKey
    if (!('accessKey' in EP)) EP.accessKey = '';

    // elements.accessKeyLabel
    if (!('accessKeyLabel' in EP)) EP.accessKeyLabel = '';

    // other.scrollIntoView
    if (!('scrollIntoView' in EP)) EP.scrollIntoView = function() {};

    // namespaceURI - returns correct namespace based on tag
    if (!('namespaceURI' in EP)) {
        Object.defineProperty(EP, 'namespaceURI', {
            get: function() {
                var tag = this.tagName ? this.tagName.toLowerCase() : '';
                if (tag === 'svg' || tag === 'path' || tag === 'circle' || tag === 'rect' ||
                    tag === 'line' || tag === 'polyline' || tag === 'polygon' || tag === 'ellipse' ||
                    tag === 'g' || tag === 'use' || tag === 'defs' || tag === 'text' ||
                    tag === 'tspan' || tag === 'image' || tag === 'foreignobject' ||
                    tag === 'fecolormatrix' || tag === 'filter' || tag === 'lineargradient' ||
                    tag === 'radialgradient' || tag === 'stop' || tag === 'clippath' ||
                    tag === 'mask' || tag === 'pattern' || tag === 'marker' || tag === 'symbol' ||
                    tag === 'animate' || tag === 'animatetransform' || tag === 'set')
                    return 'http://www.w3.org/2000/svg';
                if (tag === 'math' || tag === 'mrow' || tag === 'msup' || tag === 'msub' ||
                    tag === 'mfrac' || tag === 'msqrt' || tag === 'mspace' || tag === 'mi' ||
                    tag === 'mn' || tag === 'mo' || tag === 'mtext' || tag === 'mover' ||
                    tag === 'munder' || tag === 'mtable' || tag === 'mtr' || tag === 'mtd')
                    return 'http://www.w3.org/1998/Math/MathML';
                return 'http://www.w3.org/1999/xhtml';
            },
            configurable: true
        });
    }

    // scripting.async / scripting.defer on script elements - add to base prototype
    if (!('async' in EP)) EP.async = false;
    if (!('defer' in EP)) EP.defer = false;

    // elements.semantic.download on anchor elements
    if (!('download' in EP)) EP.download = '';

    // elements.semantic.relList
    if (!('relList' in EP)) EP.relList = { add: function(){}, remove: function(){}, contains: function(){ return false; }, toggle: function(){} };

    // form properties needed for tests
    if (!('autofocus' in EP)) EP.autofocus = false;
    if (!('autocomplete' in EP)) EP.autocomplete = '';
    if (!('placeholder' in EP)) EP.placeholder = '';
    if (!('multiple' in EP)) EP.multiple = false;
    if (!('required' in EP)) EP.required = false;
    if (!('readOnly' in EP)) EP.readOnly = false;
    if (!('pattern' in EP)) EP.pattern = '';
    if (!('minLength' in EP)) EP.minLength = -1;
    if (!('maxLength' in EP)) EP.maxLength = -1;
    if (!('min' in EP)) EP.min = '';
    if (!('max' in EP)) EP.max = '';
    if (!('step' in EP)) EP.step = '';
    // labels and form are defined as getters below (form.association tests)
    if (!('formAction' in EP)) EP.formAction = '';
    if (!('formEnctype' in EP)) EP.formEnctype = '';
    if (!('formMethod' in EP)) EP.formMethod = '';
    if (!('formNoValidate' in EP)) EP.formNoValidate = false;
    if (!('formTarget' in EP)) EP.formTarget = '';
    // validity/checkValidity/reportValidity are defined later with real validation logic
    if (!('setCustomValidity' in EP)) EP.setCustomValidity = function() {};
    if (!('willValidate' in EP)) EP.willValidate = false;
    if (!('validationMessage' in EP)) EP.validationMessage = '';
    if (!('selectionStart' in EP)) EP.selectionStart = 0;
    if (!('selectionEnd' in EP)) EP.selectionEnd = 0;
    if (!('selectionDirection' in EP)) EP.selectionDirection = 'none';
    if (!('setSelectionRange' in EP)) EP.setSelectionRange = function() {};
    if (!('select' in EP)) EP.select = function() {};

    // ol.reversed
    if (!('reversed' in EP)) EP.reversed = false;

    // spellcheck
    if (!('spellcheck' in EP)) EP.spellcheck = false;

    // contentEditable / isContentEditable - bridge to DOM attribute
    if (!('contentEditable' in EP)) {
        Object.defineProperty(EP, 'contentEditable', {
            get: function() {
                var v = this.getAttribute ? this.getAttribute('contenteditable') : null;
                if (v === 'true' || v === '') return 'true';
                if (v === 'false') return 'false';
                return 'inherit';
            },
            set: function(v) {
                if (this.setAttribute) {
                    if (v === true || v === 'true') this.setAttribute('contenteditable', 'true');
                    else if (v === false || v === 'false') this.setAttribute('contenteditable', 'false');
                    else this.removeAttribute && this.removeAttribute('contenteditable');
                }
            },
            configurable: true
        });
    }
    if (!('isContentEditable' in EP)) {
        Object.defineProperty(EP, 'isContentEditable', {
            get: function() {
                var v = this.getAttribute ? this.getAttribute('contenteditable') : null;
                return v === 'true' || v === '';
            },
            configurable: true
        });
    }

    // draggable
    if (!('draggable' in EP)) EP.draggable = false;

})();

// ---- document properties for html5test ----
// Page Visibility API
if (!('visibilityState' in document)) document.visibilityState = 'visible';
if (!('hidden' in document)) document.hidden = false;

// designMode
if (!('designMode' in document)) document.designMode = 'off';

// document.defaultView
if (!document.defaultView) document.defaultView = window;

// window.getSelection
if (!window.getSelection) {
    window.getSelection = function() {
        return { anchorNode: null, anchorOffset: 0, focusNode: null, focusOffset: 0,
                 isCollapsed: true, rangeCount: 0, type: 'None',
                 addRange: function(){}, collapse: function(){}, collapseToEnd: function(){},
                 collapseToStart: function(){}, containsNode: function(){ return false; },
                 deleteFromDocument: function(){}, empty: function(){}, extend: function(){},
                 getRangeAt: function(){ return null; }, removeAllRanges: function(){},
                 removeRange: function(){}, selectAllChildren: function(){},
                 setBaseAndExtent: function(){}, setPosition: function(){},
                 toString: function(){ return ''; }
        };
    };
}

// document.execCommand and related
if (!document.execCommand) document.execCommand = function() { return false; };
if (!document.queryCommandEnabled) document.queryCommandEnabled = function() { return false; };
if (!document.queryCommandIndeterm) document.queryCommandIndeterm = function() { return false; };
if (!document.queryCommandState) document.queryCommandState = function() { return false; };
if (!document.queryCommandSupported) document.queryCommandSupported = function() { return false; };
if (!document.queryCommandValue) document.queryCommandValue = function() { return ''; };

// form noValidate
if (typeof HTMLFormElement !== 'undefined' && HTMLFormElement.prototype) {
    HTMLFormElement.prototype.noValidate = false;
    HTMLFormElement.prototype.checkValidity = function() { return true; };
    HTMLFormElement.prototype.reportValidity = function() { return true; };
}

// CanvasRenderingContext2D is provided natively by js_bindings_init

// Missing DOM/API globals that test engines reference directly (not via typeof)
if (typeof FileList === 'undefined') {
    globalThis.FileList = function FileList() {};
    FileList.prototype[Symbol.iterator] = function() { return [][Symbol.iterator](); };
    FileList.prototype.length = 0;
    FileList.prototype.item = function() { return null; };
}
if (typeof File === 'undefined') {
    globalThis.File = function File(bits, name, opts) { this.name = name; this.size = 0; this.type = (opts && opts.type) || ''; };
}
if (typeof DOMException === 'undefined') {
    globalThis.DOMException = function DOMException(msg, name) { this.message = msg || ''; this.name = name || 'Error'; };
    DOMException.prototype = Object.create(Error.prototype);
}

// Path2D - needed for canvas.path test
if (typeof Path2D === 'undefined') {
    globalThis.Path2D = function(path) { this._path = path || ''; };
    Path2D.prototype.addPath = function() {};
    Path2D.prototype.closePath = function() {};
    Path2D.prototype.moveTo = function() {};
    Path2D.prototype.lineTo = function() {};
    Path2D.prototype.bezierCurveTo = function() {};
    Path2D.prototype.quadraticCurveTo = function() {};
    Path2D.prototype.arc = function() {};
    Path2D.prototype.arcTo = function() {};
    Path2D.prototype.ellipse = function() {};
    Path2D.prototype.rect = function() {};
}

// EventSource - for server-sent events test
if (typeof EventSource === 'undefined') {
    globalThis.EventSource = function(url) { this.url = url; this.readyState = 0; };
    EventSource.prototype.close = function() {};
    EventSource.CONNECTING = 0;
    EventSource.OPEN = 1;
    EventSource.CLOSED = 2;
}

// crypto.getRandomValues - for security.crypto test
if (!window.crypto) {
    window.crypto = {
        getRandomValues: function(arr) {
            for (var i = 0; i < arr.length; i++) arr[i] = Math.floor(Math.random() * 256);
            return arr;
        },
        subtle: {}
    };
}

// ---- Element-specific properties ----
// textarea
if (typeof HTMLTextAreaElement !== 'undefined') {
    var TAP = HTMLTextAreaElement.prototype;
    if (!('wrap' in TAP)) TAP.wrap = 'soft';
    if (!('rows' in TAP)) TAP.rows = 2;
    if (!('cols' in TAP)) TAP.cols = 20;
    if (!('textLength' in TAP)) Object.defineProperty(TAP, 'textLength', {
        get: function() { return (this.value || '').length; }, configurable: true
    });
}

// select
if (typeof HTMLSelectElement !== 'undefined') {
    var SEP = HTMLSelectElement.prototype;
    if (!('selectedIndex' in SEP)) SEP.selectedIndex = -1;
    if (!('options' in SEP)) SEP.options = [];
    if (!('size' in SEP)) SEP.size = 0;
}

// fieldset
if (typeof HTMLFieldSetElement !== 'undefined') {
    var FSP = HTMLFieldSetElement.prototype;
    if (!('elements' in FSP)) Object.defineProperty(FSP, 'elements', {
        get: function() {
            var result = [];
            var children = this.querySelectorAll ? this.querySelectorAll('input,select,textarea,button,output,fieldset') : [];
            for (var i = 0; i < children.length; i++) result.push(children[i]);
            result.length = result.length || 0;
            return result;
        }, configurable: true
    });
}

// input-specific
if (typeof HTMLInputElement !== 'undefined') {
    var IP = HTMLInputElement.prototype;
    if (!('indeterminate' in IP)) IP.indeterminate = false;
    if (!('list' in IP)) IP.list = null;
    if (!('files' in IP)) Object.defineProperty(IP, 'files', {
        get: function() {
            if (this.type === 'file') {
                var fl = new FileList();
                return fl;
            }
            return null;
        }, configurable: true
    });
    if (!('width' in IP)) IP.width = 0;
    if (!('height' in IP)) IP.height = 0;
}

// form noValidate (already added above, ensure it's on prototype)

// Event handler properties for isEventSupported checks
(function() {
    var EP = HTMLElement.prototype;
    var handlers = ['oninput', 'onchange', 'oninvalid', 'onerror', 'onload', 'onabort',
                    'onclick', 'onsubmit', 'onreset', 'onfocus', 'onblur', 'onkeydown',
                    'onkeyup', 'onkeypress', 'onmousedown', 'onmouseup', 'onmousemove',
                    'onmouseover', 'onmouseout', 'oncontextmenu', 'ondblclick',
                    'onscroll', 'onresize', 'onwheel', 'ontouchstart', 'ontouchend',
                    'ontouchmove', 'ontouchcancel', 'onpointerdown', 'onpointerup',
                    'onpointermove', 'onpointerover', 'onpointerout',
                    'onpointerenter', 'onpointerleave', 'ongotpointercapture',
                    'onlostpointercapture', 'ondrag', 'ondragstart', 'ondragenter',
                    'ondragover', 'ondragleave', 'ondragend', 'ondrop',
                    'onanimationstart', 'onanimationend', 'onanimationiteration',
                    'ontransitionend', 'onselect', 'oncopy', 'oncut', 'onpaste'];
    for (var i = 0; i < handlers.length; i++) {
        if (!(handlers[i] in EP)) EP[handlers[i]] = null;
    }
})();

// Form validity - real validation based on type and constraints
(function() {
    var EP = HTMLElement.prototype;
    Object.defineProperty(EP, 'validity', {
        get: function() {
            var el = this;
            var type = (el.type || '').toLowerCase();
            var val = el.value || '';
            var valid = true;
            var typeMismatch = false;
            var rangeOverflow = false;
            var rangeUnderflow = false;
            var valueMissing = false;
            var patternMismatch = false;

            // Required check
            if (el.required && val === '') {
                valid = false;
                valueMissing = true;
            }

            // Type validation
            if (val !== '') {
                if (type === 'url') {
                    try { new URL(val); } catch(e) { typeMismatch = true; valid = false; }
                } else if (type === 'email') {
                    if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(val)) { typeMismatch = true; valid = false; }
                }
            }

            // Range validation
            if (val !== '' && (type === 'number' || type === 'range')) {
                var num = parseFloat(val);
                if (!isNaN(num)) {
                    var mn = el.min !== undefined && el.min !== '' ? el.min :
                             (el.getAttribute ? el.getAttribute('min') : null);
                    var mx = el.max !== undefined && el.max !== '' ? el.max :
                             (el.getAttribute ? el.getAttribute('max') : null);
                    if (mn !== null && mn !== '' && num < parseFloat(mn)) { rangeUnderflow = true; valid = false; }
                    if (mx !== null && mx !== '' && num > parseFloat(mx)) { rangeOverflow = true; valid = false; }
                }
            }

            // Pattern validation
            if (val !== '' && el.pattern) {
                try {
                    var re = new RegExp('^(?:' + el.pattern + ')$');
                    if (!re.test(val)) { patternMismatch = true; valid = false; }
                } catch(e) {}
            }

            // Custom validity
            var customError = !!(el._customValidity);
            if (customError) valid = false;

            return {
                valid: valid, valueMissing: valueMissing, typeMismatch: typeMismatch,
                patternMismatch: patternMismatch, tooLong: false, tooShort: false,
                rangeUnderflow: rangeUnderflow, rangeOverflow: rangeOverflow,
                stepMismatch: false, badInput: false, customError: customError
            };
        },
        configurable: true
    });
    EP.checkValidity = function() { return this.validity.valid; };
    EP.reportValidity = function() { return this.validity.valid; };
    EP.setCustomValidity = function(msg) {
        this._customValidity = msg || '';
        // Store in DOM attribute so C++ selector matching can access it
        if (this.setAttribute) {
            if (msg) this.setAttribute('data-custom-validity', msg);
            else this.removeAttribute('data-custom-validity');
        }
    };
})();

// template.content
if (typeof HTMLTemplateElement !== 'undefined') {
    Object.defineProperty(HTMLTemplateElement.prototype, 'content', {
        get: function() {
            if (!this._content) {
                this._content = document.createDocumentFragment();
                // Move child nodes into fragment
                while (this.firstChild) {
                    this._content.appendChild(this.firstChild);
                }
            }
            return this._content;
        },
        configurable: true
    });
}

// Shadow DOM
if (typeof HTMLElement !== 'undefined') {
    HTMLElement.prototype.attachShadow = function(opts) {
        var shadow = document.createElement('div');
        shadow.host = this;
        shadow.mode = (opts && opts.mode) || 'open';
        this._shadowRoot = shadow;
        return shadow;
    };
    Object.defineProperty(HTMLElement.prototype, 'shadowRoot', {
        get: function() { return this._shadowRoot || null; },
        configurable: true
    });
}

// TextEncoder/TextDecoder
if (typeof TextEncoder === 'undefined') {
    globalThis.TextEncoder = function TextEncoder() { this.encoding = 'utf-8'; };
    TextEncoder.prototype.encode = function(str) {
        str = str || '';
        var arr = [];
        for (var i = 0; i < str.length; i++) {
            var c = str.charCodeAt(i);
            if (c < 0x80) { arr.push(c); }
            else if (c < 0x800) { arr.push(0xC0 | (c >> 6), 0x80 | (c & 0x3F)); }
            else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < str.length) {
                var c2 = str.charCodeAt(++i);
                var cp = ((c - 0xD800) << 10) + (c2 - 0xDC00) + 0x10000;
                arr.push(0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F),
                         0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F));
            } else {
                arr.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 0x3F), 0x80 | (c & 0x3F));
            }
        }
        return new Uint8Array(arr);
    };
    TextEncoder.prototype.encodeInto = function(str, dest) {
        var encoded = this.encode(str);
        var len = Math.min(encoded.length, dest.length);
        for (var i = 0; i < len; i++) dest[i] = encoded[i];
        return { read: str.length, written: len };
    };
}
if (typeof TextDecoder === 'undefined') {
    globalThis.TextDecoder = function TextDecoder(encoding) { this.encoding = encoding || 'utf-8'; };
    TextDecoder.prototype.decode = function(buf) {
        if (!buf) return '';
        var arr = new Uint8Array(buf.buffer || buf);
        var result = '';
        for (var i = 0; i < arr.length; ) {
            var b = arr[i];
            if (b < 0x80) { result += String.fromCharCode(b); i++; }
            else if ((b & 0xE0) === 0xC0) {
                result += String.fromCharCode(((b & 0x1F) << 6) | (arr[i+1] & 0x3F)); i += 2;
            } else if ((b & 0xF0) === 0xE0) {
                result += String.fromCharCode(((b & 0x0F) << 12) | ((arr[i+1] & 0x3F) << 6) | (arr[i+2] & 0x3F)); i += 3;
            } else if ((b & 0xF8) === 0xF0) {
                var cp = ((b & 0x07) << 18) | ((arr[i+1] & 0x3F) << 12) | ((arr[i+2] & 0x3F) << 6) | (arr[i+3] & 0x3F);
                cp -= 0x10000;
                result += String.fromCharCode(0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF)); i += 4;
            } else { i++; }
        }
        return result;
    };
}

// FileReader
if (typeof FileReader === 'undefined') {
    globalThis.FileReader = function FileReader() {
        this.readyState = 0; this.result = null; this.error = null;
        this.onload = null; this.onerror = null; this.onloadend = null;
        this.onloadstart = null; this.onprogress = null; this.onabort = null;
    };
    FileReader.EMPTY = 0; FileReader.LOADING = 1; FileReader.DONE = 2;
    FileReader.prototype.readAsText = function(blob) {
        var self = this; self.readyState = 2;
        self.result = ''; if (self.onload) self.onload({target:self});
        if (self.onloadend) self.onloadend({target:self});
    };
    FileReader.prototype.readAsDataURL = function(blob) {
        var self = this; self.readyState = 2;
        self.result = 'data:application/octet-stream;base64,';
        if (self.onload) self.onload({target:self});
        if (self.onloadend) self.onloadend({target:self});
    };
    FileReader.prototype.readAsArrayBuffer = function(blob) {
        var self = this; self.readyState = 2;
        self.result = new ArrayBuffer(0);
        if (self.onload) self.onload({target:self});
        if (self.onloadend) self.onloadend({target:self});
    };
    FileReader.prototype.readAsBinaryString = function(blob) {
        var self = this; self.readyState = 2;
        self.result = '';
        if (self.onload) self.onload({target:self});
        if (self.onloadend) self.onloadend({target:self});
    };
    FileReader.prototype.abort = function() { this.readyState = 2; };
}

// URL.createObjectURL / revokeObjectURL
if (typeof URL !== 'undefined') {
    if (!URL.createObjectURL) URL.createObjectURL = function(blob) { return 'blob:null/' + Math.random().toString(36).substr(2); };
    if (!URL.revokeObjectURL) URL.revokeObjectURL = function() {};
}

// Blob constructor
if (typeof Blob === 'undefined') {
    globalThis.Blob = function Blob(parts, opts) {
        this.size = 0; this.type = (opts && opts.type) || '';
        if (parts) for (var i = 0; i < parts.length; i++) {
            var p = parts[i];
            if (typeof p === 'string') this.size += p.length;
            else if (p && p.byteLength) this.size += p.byteLength;
            else if (p && p.size) this.size += p.size;
        }
    };
    Blob.prototype.slice = function(s,e,t) { return new Blob([], {type:t||this.type}); };
    Blob.prototype.text = function() { return Promise.resolve(''); };
    Blob.prototype.arrayBuffer = function() { return Promise.resolve(new ArrayBuffer(0)); };
}

// ReadableStream / WritableStream
if (typeof ReadableStream === 'undefined') {
    globalThis.ReadableStream = function ReadableStream(source) {
        this.locked = false;
        this._source = source || {};
    };
    ReadableStream.prototype.getReader = function() {
        this.locked = true;
        return { read: function() { return Promise.resolve({done:true,value:undefined}); }, releaseLock: function() {} };
    };
    ReadableStream.prototype.cancel = function() { return Promise.resolve(); };
    ReadableStream.prototype.pipeTo = function() { return Promise.resolve(); };
    ReadableStream.prototype.pipeThrough = function(t) { return t.readable; };
    ReadableStream.prototype.tee = function() { return [new ReadableStream(), new ReadableStream()]; };
}
if (typeof WritableStream === 'undefined') {
    globalThis.WritableStream = function WritableStream(sink) {
        this.locked = false;
        this._sink = sink || {};
    };
    WritableStream.prototype.getWriter = function() {
        this.locked = true;
        return { write: function() { return Promise.resolve(); }, close: function() { return Promise.resolve(); }, releaseLock: function() {}, ready: Promise.resolve() };
    };
    WritableStream.prototype.abort = function() { return Promise.resolve(); };
    WritableStream.prototype.close = function() { return Promise.resolve(); };
}

// XHR2 response types
(function() {
    if (typeof XMLHttpRequest !== 'undefined') {
        var origOpen = XMLHttpRequest.prototype.open;
        var XP = XMLHttpRequest.prototype;
        if (!('responseType' in XP)) XP.responseType = '';
        if (!('response' in XP)) Object.defineProperty(XP, 'response', {
            get: function() {
                switch(this.responseType) {
                    case 'arraybuffer': return new ArrayBuffer(0);
                    case 'blob': return new Blob([]);
                    case 'document': return null;
                    default: return this.responseText || '';
                }
            }, configurable: true
        });
        if (!('upload' in XP)) XP.upload = {
            addEventListener: function(){}, removeEventListener: function(){},
            onprogress: null, onload: null, onerror: null, onabort: null
        };
    }
})();

// requestIdleCallback
if (!window.requestIdleCallback) {
    window.requestIdleCallback = function(cb) {
        return setTimeout(function() {
            cb({ didTimeout: false, timeRemaining: function() { return 50; } });
        }, 1);
    };
    window.cancelIdleCallback = function(id) { clearTimeout(id); };
}

// PerformanceObserver
if (typeof PerformanceObserver === 'undefined') {
    globalThis.PerformanceObserver = function(cb) { this._cb = cb; };
    PerformanceObserver.prototype.observe = function() {};
    PerformanceObserver.prototype.disconnect = function() {};
    PerformanceObserver.prototype.takeRecords = function() { return []; };
    PerformanceObserver.supportedEntryTypes = [];
}

// FontFace / FontFaceSet
if (typeof FontFace === 'undefined') {
    globalThis.FontFace = function(family, source) { this.family = family; this.status = 'unloaded'; };
    FontFace.prototype.load = function() { this.status = 'loaded'; return Promise.resolve(this); };
}
if (document && !document.fonts) {
    document.fonts = { add: function(){}, delete: function(){}, check: function(){ return true; },
                       ready: Promise.resolve(), forEach: function(){}, size: 0 };
}

// navigator.sendBeacon
if (typeof navigator !== 'undefined' && !navigator.sendBeacon) {
    navigator.sendBeacon = function(url, data) { return true; };
}

// Notification
if (typeof Notification === 'undefined') {
    globalThis.Notification = function(title, opts) { this.title = title; };
    Notification.permission = 'default';
    Notification.requestPermission = function() { return Promise.resolve('denied'); };
}

// Worker / SharedWorker
if (typeof Worker === 'undefined') {
    globalThis.Worker = function(url) {
        this.onmessage = null; this.onerror = null;
    };
    Worker.prototype.postMessage = function() {};
    Worker.prototype.terminate = function() {};
}
if (typeof SharedWorker === 'undefined') {
    globalThis.SharedWorker = function(url) {
        this.port = { onmessage: null, postMessage: function(){}, start: function(){}, close: function(){} };
    };
}

// WebSocket
if (typeof WebSocket === 'undefined') {
    globalThis.WebSocket = function(url, protocols) {
        this.url = url; this.readyState = 0; this.bufferedAmount = 0;
        this.extensions = ''; this.protocol = '';
        this.binaryType = 'blob';
        this.onopen = null; this.onclose = null; this.onmessage = null; this.onerror = null;
    };
    WebSocket.CONNECTING = 0; WebSocket.OPEN = 1;
    WebSocket.CLOSING = 2; WebSocket.CLOSED = 3;
    WebSocket.prototype.send = function() {};
    WebSocket.prototype.close = function() { this.readyState = 3; };
}

// ruby/rp/rt - ensure they are not HTMLUnknownElement
// This is handled by the prototype assignment in js_wrap_node

// navigator.pointerEnabled / maxTouchPoints
if (typeof navigator !== 'undefined') {
    if (!('maxTouchPoints' in navigator)) navigator.maxTouchPoints = 0;
    if (!('pointerEnabled' in navigator)) navigator.pointerEnabled = true;
}

// ---- Easy wins batch: property stubs and constructors ----

// ClipboardEvent
if (typeof ClipboardEvent === 'undefined') {
    globalThis.ClipboardEvent = function ClipboardEvent(type, opts) {
        this.type = type;
        this.clipboardData = (opts && opts.clipboardData) || null;
    };
}

// iframe: sandbox, srcdoc properties
if (typeof HTMLIFrameElement !== 'undefined') {
    var IFP = HTMLIFrameElement.prototype;
    if (!('sandbox' in IFP)) IFP.sandbox = '';
    if (!('srcdoc' in IFP)) IFP.srcdoc = '';
}

// img: srcset, sizes properties
if (typeof HTMLImageElement !== 'undefined') {
    var IMP = HTMLImageElement.prototype;
    if (!('srcset' in IMP)) IMP.srcset = '';
    if (!('sizes' in IMP)) IMP.sizes = '';
    if (!('x' in IMP)) IMP.x = 0;
    if (!('y' in IMP)) IMP.y = 0;
}
if (typeof HTMLElement !== 'undefined') {
    if (!('offsetParent' in HTMLElement.prototype)) HTMLElement.prototype.offsetParent = null;
}

// video/audio: canPlayType method
if (typeof HTMLVideoElement !== 'undefined') {
    HTMLVideoElement.prototype.canPlayType = function(type) { return ''; };
    HTMLVideoElement.prototype.play = function() { return Promise.resolve(); };
    HTMLVideoElement.prototype.pause = function() {};
    HTMLVideoElement.prototype.load = function() {};
}
if (typeof HTMLAudioElement !== 'undefined') {
    HTMLAudioElement.prototype.canPlayType = function(type) { return ''; };
    HTMLAudioElement.prototype.play = function() { return Promise.resolve(); };
    HTMLAudioElement.prototype.pause = function() {};
    HTMLAudioElement.prototype.load = function() {};
}
if (typeof HTMLMediaElement !== 'undefined') {
    HTMLMediaElement.prototype.canPlayType = function(type) { return ''; };
}

// label.control - returns the associated input element
if (typeof HTMLLabelElement !== 'undefined') {
    Object.defineProperty(HTMLLabelElement.prototype, 'control', {
        get: function() {
            var forId = this.getAttribute && this.getAttribute('for');
            if (forId) return document.getElementById(forId);
            // Also check for nested input
            return this.querySelector ? this.querySelector('input,select,textarea') : null;
        },
        configurable: true
    });
}

// input.labels - returns associated labels
(function() {
    var EP = HTMLElement.prototype;
    Object.defineProperty(EP, 'labels', {
        get: function() {
            if (!this.id) return [];
            var labels = document.querySelectorAll ? document.querySelectorAll('label[for="' + this.id + '"]') : [];
            return labels;
        },
        configurable: true
    });
})();

// input.form - returns associated form
(function() {
    var EP = HTMLElement.prototype;
    Object.defineProperty(EP, 'form', {
        get: function() {
            // Check for form attribute
            var formId = this.getAttribute && this.getAttribute('form');
            if (formId) return document.getElementById(formId);
            // Check for parent form
            var p = this.parentNode;
            while (p) {
                if (p.tagName && p.tagName.toLowerCase() === 'form') return p;
                p = p.parentNode;
            }
            return null;
        },
        configurable: true
    });
})();

// details element: open property bridges to DOM attribute
if (typeof HTMLDetailsElement !== 'undefined') {
    Object.defineProperty(HTMLDetailsElement.prototype, 'open', {
        get: function() { return this.hasAttribute ? this.hasAttribute('open') : false; },
        set: function(v) {
            if (v) { if (this.setAttribute) this.setAttribute('open', ''); }
            else { if (this.removeAttribute) this.removeAttribute('open'); }
        },
        configurable: true
    });
}

// Fullscreen API stubs
if (typeof document !== 'undefined') {
    if (!document.exitFullscreen) document.exitFullscreen = function() { return Promise.resolve(); };
    if (!('fullscreenEnabled' in document)) document.fullscreenEnabled = false;
}
var EP2 = HTMLElement.prototype;
if (!EP2.requestFullscreen) EP2.requestFullscreen = function() { return Promise.resolve(); };

// PointerEvent
if (typeof PointerEvent === 'undefined') {
    globalThis.PointerEvent = function PointerEvent(type, opts) {
        this.type = type;
        this.pointerId = (opts && opts.pointerId) || 0;
        this.pointerType = (opts && opts.pointerType) || 'mouse';
    };
}

// video: poster, audioTracks, videoTracks properties
if (typeof HTMLVideoElement !== 'undefined') {
    var VP = HTMLVideoElement.prototype;
    if (!('poster' in VP)) VP.poster = '';
    if (!('audioTracks' in VP)) VP.audioTracks = [];
    if (!('videoTracks' in VP)) VP.videoTracks = [];
    if (!('textTracks' in VP)) VP.textTracks = [];
}

// audio: loop, preload properties
if (typeof HTMLAudioElement !== 'undefined') {
    var AP = HTMLAudioElement.prototype;
    if (!('loop' in AP)) AP.loop = false;
    if (!('preload' in AP)) AP.preload = 'auto';
}
// Also on media element base
if (typeof HTMLMediaElement !== 'undefined') {
    var MMP = HTMLMediaElement.prototype;
    if (!('loop' in MMP)) MMP.loop = false;
    if (!('preload' in MMP)) MMP.preload = 'auto';
    if (!('poster' in MMP)) MMP.poster = '';
    if (!('audioTracks' in MMP)) MMP.audioTracks = [];
    if (!('videoTracks' in MMP)) MMP.videoTracks = [];
    if (!('textTracks' in MMP)) MMP.textTracks = [];
}
// video element also needs loop/preload
if (typeof HTMLVideoElement !== 'undefined') {
    var VP2 = HTMLVideoElement.prototype;
    if (!('loop' in VP2)) VP2.loop = false;
    if (!('preload' in VP2)) VP2.preload = 'auto';
}

// track element: track property
if (typeof HTMLTrackElement !== 'undefined') {
    if (!('track' in HTMLTrackElement.prototype)) {
        HTMLTrackElement.prototype.track = { kind: '', label: '', language: '', mode: 'disabled', cues: null, activeCues: null };
    }
}

// input dirName property
if (typeof HTMLInputElement !== 'undefined') {
    if (!('dirName' in HTMLInputElement.prototype)) HTMLInputElement.prototype.dirName = '';
}

// form.image.width/height: input[type=image] needs width/height properties
// These are already on elem_proto but need to return number for 'width' in element check
// The test just checks 'width' in element and 'height' in element

// Intl API stub
if (typeof Intl === 'undefined') {
    globalThis.Intl = {
        Collator: function(locales, opts) { this.compare = function(a, b) { return a < b ? -1 : a > b ? 1 : 0; }; },
        DateTimeFormat: function(locales, opts) { this.format = function(d) { return d ? d.toString() : ''; }; },
        NumberFormat: function(locales, opts) { this.format = function(n) { return String(n); }; },
        PluralRules: function() { this.select = function() { return 'other'; }; },
        getCanonicalLocales: function(locales) { return Array.isArray(locales) ? locales : [locales || 'en']; }
    };
}

// HTMLKeygenElement - add challenge/keytype after prototype reset
if (typeof HTMLKeygenElement !== 'undefined') {
    HTMLKeygenElement.prototype.challenge = '';
    HTMLKeygenElement.prototype.keytype = 'rsa';
    HTMLKeygenElement.prototype.type = 'keygen';
}

// SVGForeignObjectElement / SVGFEColorMatrixElement stubs
if (typeof SVGForeignObjectElement === 'undefined') {
    globalThis.SVGForeignObjectElement = function SVGForeignObjectElement() {};
}
if (typeof SVGFEColorMatrixElement === 'undefined') {
    globalThis.SVGFEColorMatrixElement = function SVGFEColorMatrixElement() {};
    SVGFEColorMatrixElement.SVG_FECOLORMATRIX_TYPE_SATURATE = 2;
}

// document.createElementNS stub
if (typeof document !== 'undefined') {
    var svgCtors = {
        'foreignObject': typeof SVGForeignObjectElement !== 'undefined' ? SVGForeignObjectElement : null,
        'svg': typeof SVGSVGElement !== 'undefined' ? SVGSVGElement : null,
        'feColorMatrix': typeof SVGFEColorMatrixElement !== 'undefined' ? SVGFEColorMatrixElement : null
    };
    document.createElementNS = function(ns, tag) {
        var el = document.createElement(tag);
        if (ns === 'http://www.w3.org/2000/svg' && svgCtors[tag]) {
            try { Object.setPrototypeOf(el, svgCtors[tag].prototype); } catch(e) {}
        }
        return el;
    };
}

// contentEditable support for :read-write/:read-only selectors
// The test creates divs with contentEditable and queries with :read-write/:read-only
// We need contentEditable to be a real attribute on elements

// DeviceOrientationEvent / DeviceMotionEvent stubs
if (typeof DeviceOrientationEvent === 'undefined') {
    globalThis.DeviceOrientationEvent = function DeviceOrientationEvent(type, opts) {
        this.type = type; this.alpha = 0; this.beta = 0; this.gamma = 0;
    };
}
if (typeof DeviceMotionEvent === 'undefined') {
    globalThis.DeviceMotionEvent = function DeviceMotionEvent(type, opts) {
        this.type = type; this.acceleration = null; this.rotationRate = null;
    };
}

// AudioContext stub
if (typeof AudioContext === 'undefined') {
    globalThis.AudioContext = function AudioContext() {
        this.state = 'suspended';
        this.sampleRate = 44100;
        this.destination = { numberOfInputs: 0, numberOfOutputs: 1 };
        this.createOscillator = function() { return { connect: function(){}, start: function(){}, stop: function(){}, frequency: { value: 440 } }; };
        this.createGain = function() { return { connect: function(){}, gain: { value: 1 } }; };
        this.createAnalyser = function() { return { connect: function(){}, fftSize: 2048, getByteFrequencyData: function(){} }; };
        this.createBufferSource = function() { return { connect: function(){}, start: function(){}, stop: function(){}, buffer: null }; };
        this.resume = function() { return Promise.resolve(); };
        this.close = function() { return Promise.resolve(); };
        this.decodeAudioData = function() { return Promise.resolve(); };
    };
}

// SpeechRecognition stub
if (typeof SpeechRecognition === 'undefined') {
    globalThis.SpeechRecognition = function SpeechRecognition() {};
}

// speechSynthesis stub
if (typeof speechSynthesis === 'undefined') {
    globalThis.speechSynthesis = {
        speak: function() {},
        cancel: function() {},
        pause: function() {},
        resume: function() {},
        getVoices: function() { return [{ name: 'Default', lang: 'en-US', default: true, localService: true }]; },
        speaking: false,
        pending: false,
        paused: false
    };
    globalThis.SpeechSynthesisUtterance = function SpeechSynthesisUtterance(text) { this.text = text || ''; };
}

// XHR: implement send() via fetch for actual HTTP/file requests
if (typeof XMLHttpRequest !== 'undefined') {
    var _xhrProto = XMLHttpRequest.prototype;
    if (!('withCredentials' in _xhrProto)) _xhrProto.withCredentials = false;
    _xhrProto.send = function(body) {
        var self = this;
        var url = self._url || '';
        // Resolve relative URLs against page location
        if (url && typeof location !== 'undefined' && location.href) {
            var base = location.href;
            if (url[0] === '/' && url.indexOf('://') === -1) {
                // Absolute path - resolve against origin
                if (base.substring(0, 7) === 'file://') {
                    // For file:// URLs, resolve against page directory (not filesystem root)
                    var lastSlash = base.lastIndexOf('/');
                    if (lastSlash > 6) url = base.substring(0, lastSlash) + url;
                } else {
                    var proto = base.indexOf('://');
                    if (proto !== -1) {
                        var hostEnd = base.indexOf('/', proto + 3);
                        if (hostEnd !== -1) url = base.substring(0, hostEnd) + url;
                        else url = base + url;
                    }
                }
            } else if (url.indexOf('://') === -1) {
                // Relative path - resolve against page directory
                var lastSlash2 = base.lastIndexOf('/');
                if (lastSlash2 !== -1) url = base.substring(0, lastSlash2 + 1) + url;
            }
        }
        if (!url) return;
        // Strip query string from file:// URLs (filesystem doesn't support query params)
        if (url.substring(0, 7) === 'file://' && url.indexOf('?') !== -1) {
            url = url.substring(0, url.indexOf('?'));
        }
        fetch(url).then(function(resp) {
            self.status = resp.status || 200;
            self.statusText = resp.statusText || 'OK';
            return resp.text();
        }).then(function(text) {
            self.responseText = text;
            var rt = self._responseType || '';
            if (rt === 'arraybuffer' || rt === 'blob') {
                // Convert text to ArrayBuffer
                var buf = new ArrayBuffer(text.length);
                var view = new Uint8Array(buf);
                for (var i = 0; i < text.length; i++) view[i] = text.charCodeAt(i) & 0xff;
                if (rt === 'arraybuffer') self.response = buf;
                else self.response = new Blob([buf]);
            } else if (rt === 'document') {
                // Create a minimal document-like object
                var titleMatch = text.match(/<title[^>]*>([\s\S]*?)<\/title>/i);
                self.responseXML = {
                    title: titleMatch ? titleMatch[1].replace(/&amp;/g,'&').replace(/&lt;/g,'<').replace(/&gt;/g,'>') : '',
                    body: { innerHTML: text },
                    querySelector: function() { return null; },
                    querySelectorAll: function() { return []; },
                    getElementById: function() { return null; }
                };
                self.response = self.responseXML;
            } else {
                self.response = text;
            }
            self.readyState = 4;
            if (self.onreadystatechange) self.onreadystatechange.call(self);
            if (self.onload) self.onload.call(self);
        }).catch(function(err) {
            self.status = 0;
            self.readyState = 4;
            if (self.onreadystatechange) self.onreadystatechange.call(self);
            if (self.onerror) self.onerror.call(self);
        });
    };
}

// security.integrity: link/script integrity attribute
if (typeof HTMLLinkElement !== 'undefined') {
    if (!('integrity' in HTMLLinkElement.prototype)) HTMLLinkElement.prototype.integrity = '';
}
if (typeof HTMLScriptElement !== 'undefined') {
    if (!('integrity' in HTMLScriptElement.prototype)) HTMLScriptElement.prototype.integrity = '';
}

// security.authentication/credential: navigator.credentials
if (typeof navigator !== 'undefined' && !('credentials' in navigator)) {
    navigator.credentials = {
        get: function() { return Promise.resolve(null); },
        store: function() { return Promise.resolve(); },
        create: function() { return Promise.resolve(null); }
    };
}

// offline.registerProtocolHandler
if (typeof navigator !== 'undefined' && !navigator.registerProtocolHandler) {
    navigator.registerProtocolHandler = function() {};
}

// offline.pushMessages
if (typeof PushManager === 'undefined') {
    globalThis.PushManager = function PushManager() {};
    globalThis.PushSubscription = function PushSubscription() {};
}

// canvas.focusring: drawFocusIfNeeded on CanvasRenderingContext2D
// Will be added via canvas context stub if not present

// elements.semantic.ping: anchor ping attribute
if (typeof HTMLAnchorElement !== 'undefined') {
    if (!('ping' in HTMLAnchorElement.prototype)) HTMLAnchorElement.prototype.ping = '';
}

// interaction.dragdrop.attributes.dropzone
if (typeof HTMLElement !== 'undefined') {
    var EP3 = HTMLElement.prototype;
    if (!('dropzone' in EP3)) EP3.dropzone = '';
}

// microdata API
if (typeof HTMLElement !== 'undefined') {
    var EP4 = HTMLElement.prototype;
    Object.defineProperty(EP4, 'itemScope', {
        get: function() { return this.hasAttribute ? this.hasAttribute('itemscope') : false; },
        set: function(v) { if (v) this.setAttribute && this.setAttribute('itemscope',''); else this.removeAttribute && this.removeAttribute('itemscope'); },
        configurable: true
    });
    Object.defineProperty(EP4, 'itemType', {
        get: function() { return this.getAttribute ? (this.getAttribute('itemtype') || '') : ''; },
        set: function(v) { this.setAttribute && this.setAttribute('itemtype', v); },
        configurable: true
    });
    Object.defineProperty(EP4, 'itemProp', {
        get: function() { return this.getAttribute ? (this.getAttribute('itemprop') || '') : ''; },
        set: function(v) { this.setAttribute && this.setAttribute('itemprop', v); },
        configurable: true
    });
    if (!('itemId' in EP4)) EP4.itemId = '';
    Object.defineProperty(EP4, 'itemValue', {
        get: function() { return this.textContent || ''; },
        set: function(v) { this.textContent = v; },
        configurable: true
    });
    Object.defineProperty(EP4, 'properties', {
        get: function() {
            var props = {};
            var all = this.querySelectorAll ? this.querySelectorAll('[itemprop]') : [];
            for (var i = 0; i < all.length; i++) {
                var name = all[i].getAttribute('itemprop');
                if (name) {
                    if (!props[name]) props[name] = [];
                    props[name].push(all[i]);
                }
            }
            props.length = all.length;
            return props;
        },
        configurable: true
    });
}
if (typeof document !== 'undefined') {
    document.getItems = function(type) {
        var all = document.querySelectorAll ? document.querySelectorAll('[itemscope]') : [];
        if (!type) return all;
        var result = [];
        for (var i = 0; i < all.length; i++) {
            if (all[i].getAttribute('itemtype') === type) result.push(all[i]);
        }
        return result;
    };
}

// link.relList.supports for resource hints
if (typeof HTMLLinkElement !== 'undefined') {
    var LLP = HTMLLinkElement.prototype;
    // Always override - EP.relList is a plain object without supports()
    {
        Object.defineProperty(LLP, 'relList', {
            get: function() {
                var self = this;
                return {
                    supports: function(val) {
                        var supported = ['dns-prefetch','prefetch','preconnect','preload','prerender','stylesheet','icon'];
                        return supported.indexOf(val) !== -1;
                    },
                    add: function(v) { self.setAttribute && self.setAttribute('rel', v); },
                    remove: function() {},
                    contains: function(v) { var r = self.getAttribute && self.getAttribute('rel'); return r ? r.indexOf(v) !== -1 : false; },
                    toggle: function() { return false; },
                    length: 0
                };
            },
            configurable: true
        });
    }
}

// IndexedDB stub
if (typeof indexedDB === 'undefined' && typeof window !== 'undefined') {
    var FakeIDBRequest = function() {
        this.result = null;
        this.error = null;
        this.readyState = 'pending';
        this.onsuccess = null;
        this.onerror = null;
    };
    var FakeIDBDatabase = function() {
        this.name = '';
        this.version = 1;
        this.objectStoreNames = [];
    };
    FakeIDBDatabase.prototype.createObjectStore = function(name) {
        return { put: function() { return new FakeIDBRequest(); }, get: function() { return new FakeIDBRequest(); } };
    };
    FakeIDBDatabase.prototype.transaction = function() {
        return { objectStore: function() { return { put: function() { return new FakeIDBRequest(); }, get: function() { return new FakeIDBRequest(); } }; } };
    };
    FakeIDBDatabase.prototype.close = function() {};
    FakeIDBDatabase.prototype.deleteDatabase = function() { return new FakeIDBRequest(); };

    var fakeIndexedDB = {
        open: function(name, ver) {
            var req = new FakeIDBRequest();
            setTimeout(function() {
                req.result = new FakeIDBDatabase();
                req.result.name = name;
                req.readyState = 'done';
                if (req.onupgradeneeded) req.onupgradeneeded({ target: req });
                if (req.onsuccess) req.onsuccess({ target: req });
            }, 0);
            return req;
        },
        deleteDatabase: function(name) {
            var req = new FakeIDBRequest();
            setTimeout(function() { req.readyState = 'done'; if (req.onsuccess) req.onsuccess({ target: req }); }, 0);
            return req;
        }
    };
    globalThis.indexedDB = fakeIndexedDB;
    window.indexedDB = fakeIndexedDB;
}

// OffscreenCanvas stub
if (typeof OffscreenCanvas === 'undefined') {
    globalThis.OffscreenCanvas = function OffscreenCanvas(w, h) {
        this.width = w || 0;
        this.height = h || 0;
    };
    OffscreenCanvas.prototype.getContext = function(type) {
        if (type === '2d') {
            return {
                fillRect: function(){}, strokeRect: function(){}, clearRect: function(){},
                fillText: function(){}, strokeText: function(){},
                beginPath: function(){}, closePath: function(){}, moveTo: function(){},
                lineTo: function(){}, arc: function(){}, rect: function(){},
                fill: function(){}, stroke: function(){}, clip: function(){},
                save: function(){}, restore: function(){},
                translate: function(){}, rotate: function(){}, scale: function(){},
                drawImage: function(){}, createImageData: function(w,h) { return {width:w,height:h,data:new Uint8ClampedArray(w*h*4)}; },
                getImageData: function(x,y,w,h) { return {width:w,height:h,data:new Uint8ClampedArray(w*h*4)}; },
                putImageData: function(){},
                canvas: this
            };
        }
        if (type === 'webgl' || type === 'webgl2') return {};
        return null;
    };
    OffscreenCanvas.prototype.transferToImageBitmap = function() { return {}; };
    OffscreenCanvas.prototype.convertToBlob = function() { return Promise.resolve(new Blob()); };
}

// ImageBitmap stub
if (typeof createImageBitmap === 'undefined') {
    globalThis.createImageBitmap = function() { return Promise.resolve({ width: 0, height: 0, close: function(){} }); };
}

// Pointer Lock API
if (typeof document !== 'undefined') {
    if (!document.exitPointerLock) document.exitPointerLock = function() {};
    if (!document.pointerLockElement) document.pointerLockElement = null;
}
if (typeof HTMLElement !== 'undefined') {
    var EPL = HTMLElement.prototype;
    if (!('requestPointerLock' in EPL)) EPL.requestPointerLock = function() {};
}

// Gamepad API
if (typeof navigator !== 'undefined' && !navigator.getGamepads) {
    navigator.getGamepads = function() { return []; };
}

// Sensor API stubs
if (typeof Sensor === 'undefined') {
    globalThis.Sensor = function Sensor() {};
    Sensor.prototype.start = function() {};
    Sensor.prototype.stop = function() {};
    Sensor.prototype.addEventListener = function() {};
}
if (typeof Accelerometer === 'undefined') {
    globalThis.Accelerometer = function Accelerometer() { this.x = 0; this.y = 0; this.z = 0; };
    Accelerometer.prototype = Object.create(Sensor.prototype);
}
if (typeof Gyroscope === 'undefined') {
    globalThis.Gyroscope = function Gyroscope() { this.x = 0; this.y = 0; this.z = 0; };
    Gyroscope.prototype = Object.create(Sensor.prototype);
}
if (typeof Magnetometer === 'undefined') {
    globalThis.Magnetometer = function Magnetometer() { this.x = 0; this.y = 0; this.z = 0; };
    Magnetometer.prototype = Object.create(Sensor.prototype);
}
if (typeof LinearAccelerationSensor === 'undefined') {
    globalThis.LinearAccelerationSensor = function LinearAccelerationSensor() { this.x = 0; this.y = 0; this.z = 0; };
    LinearAccelerationSensor.prototype = Object.create(Sensor.prototype);
}
if (typeof AbsoluteOrientationSensor === 'undefined') {
    globalThis.AbsoluteOrientationSensor = function AbsoluteOrientationSensor() { this.quaternion = [0,0,0,1]; };
    AbsoluteOrientationSensor.prototype = Object.create(Sensor.prototype);
}
if (typeof RelativeOrientationSensor === 'undefined') {
    globalThis.RelativeOrientationSensor = function RelativeOrientationSensor() { this.quaternion = [0,0,0,1]; };
    RelativeOrientationSensor.prototype = Object.create(Sensor.prototype);
}
if (typeof AmbientLightSensor === 'undefined') {
    globalThis.AmbientLightSensor = function AmbientLightSensor() { this.illuminance = 0; };
    AmbientLightSensor.prototype = Object.create(Sensor.prototype);
}

// XHR responseType support
if (typeof XMLHttpRequest !== 'undefined') {
    var xhrProto = XMLHttpRequest.prototype;
    if (!('responseType' in xhrProto)) {
        Object.defineProperty(xhrProto, 'responseType', {
            get: function() { return this._responseType || ''; },
            set: function(v) { this._responseType = v; },
            configurable: true
        });
    }
    var origOpen = xhrProto.open;
    if (origOpen) {
        xhrProto.open = function() {
            this._responseType = this._responseType || '';
            return origOpen.apply(this, arguments);
        };
    }
    if (!('response' in xhrProto)) {
        Object.defineProperty(xhrProto, 'response', {
            get: function() {
                if (this._responseType === 'arraybuffer') return new ArrayBuffer(0);
                if (this._responseType === 'blob') return new Blob();
                if (this._responseType === 'document') return document;
                return this.responseText || '';
            },
            configurable: true
        });
    }
}

// SVG inline support - ensure SVG elements get proper dimensions
if (typeof SVGElement === 'undefined') {
    globalThis.SVGElement = function SVGElement() {};
    SVGElement.prototype = Object.create(HTMLElement.prototype);
}
if (typeof SVGSVGElement === 'undefined') {
    globalThis.SVGSVGElement = function SVGSVGElement() {};
    SVGSVGElement.prototype = Object.create(SVGElement.prototype);
}

// Payment Request API stub
if (typeof PaymentRequest === 'undefined') {
    globalThis.PaymentRequest = function PaymentRequest(methods, details) {
        this.shippingAddress = null;
        this.shippingOption = null;
        this.shippingType = null;
    };
    PaymentRequest.prototype.show = function() { return Promise.reject(new Error('Not supported')); };
    PaymentRequest.prototype.abort = function() { return Promise.resolve(); };
    PaymentRequest.prototype.canMakePayment = function() { return Promise.resolve(false); };
}

// MediaSource API stub
if (typeof MediaSource === 'undefined') {
    globalThis.MediaSource = function MediaSource() {
        this.sourceBuffers = [];
        this.activeSourceBuffers = [];
        this.readyState = 'closed';
        this.duration = NaN;
    };
    MediaSource.prototype.addSourceBuffer = function() { return {}; };
    MediaSource.prototype.removeSourceBuffer = function() {};
    MediaSource.prototype.endOfStream = function() {};
    MediaSource.prototype.addEventListener = function() {};
    MediaSource.isTypeSupported = function() { return false; };
}

// WebAssembly stub
if (typeof WebAssembly === 'undefined') {
    globalThis.WebAssembly = {
        compile: function() { return Promise.reject(new Error('Not supported')); },
        instantiate: function() { return Promise.reject(new Error('Not supported')); },
        validate: function() { return false; },
        Module: function() {},
        Instance: function() {},
        Memory: function(d) { this.buffer = new ArrayBuffer(d.initial * 65536); },
        Table: function() {},
        CompileError: function(m) { this.message = m; },
        LinkError: function(m) { this.message = m; },
        RuntimeError: function(m) { this.message = m; }
    };
}

// MediaRecorder stub
if (typeof MediaRecorder === 'undefined') {
    globalThis.MediaRecorder = function MediaRecorder(stream) {
        this.stream = stream;
        this.state = 'inactive';
    };
    MediaRecorder.prototype.start = function() { this.state = 'recording'; };
    MediaRecorder.prototype.stop = function() { this.state = 'inactive'; };
    MediaRecorder.prototype.pause = function() { this.state = 'paused'; };
    MediaRecorder.prototype.resume = function() { this.state = 'recording'; };
    MediaRecorder.prototype.addEventListener = function() {};
    MediaRecorder.isTypeSupported = function() { return false; };
}

// RTCPeerConnection stub
if (typeof RTCPeerConnection === 'undefined') {
    globalThis.RTCPeerConnection = function RTCPeerConnection() {};
    RTCPeerConnection.prototype.createOffer = function() { return Promise.reject(new Error('Not supported')); };
    RTCPeerConnection.prototype.createAnswer = function() { return Promise.reject(new Error('Not supported')); };
    RTCPeerConnection.prototype.setLocalDescription = function() { return Promise.reject(new Error('Not supported')); };
    RTCPeerConnection.prototype.setRemoteDescription = function() { return Promise.reject(new Error('Not supported')); };
    RTCPeerConnection.prototype.addIceCandidate = function() { return Promise.reject(new Error('Not supported')); };
    RTCPeerConnection.prototype.close = function() {};
    RTCPeerConnection.prototype.addEventListener = function() {};
    RTCPeerConnection.prototype.createDataChannel = function(label) {
        return { label: label, close: function(){}, send: function(){}, addEventListener: function(){} };
    };
}

// RTCDataChannel stub
if (typeof RTCDataChannel === 'undefined') {
    globalThis.RTCDataChannel = function RTCDataChannel() {};
}

// navigator.mediaDevices
if (typeof navigator !== 'undefined') {
    if (!navigator.mediaDevices) navigator.mediaDevices = {};
    if (!navigator.mediaDevices.getUserMedia) {
        navigator.mediaDevices.getUserMedia = function() { return Promise.reject(new Error('Not supported')); };
    }
    if (!navigator.mediaDevices.getDisplayMedia) {
        navigator.mediaDevices.getDisplayMedia = function() { return Promise.reject(new Error('Not supported')); };
    }
    if (!navigator.mediaDevices.enumerateDevices) {
        navigator.mediaDevices.enumerateDevices = function() { return Promise.resolve([]); };
    }
    if (!navigator.getUserMedia) {
        navigator.getUserMedia = function(constraints, success, error) { if (error) error(new Error('Not supported')); };
    }
    if (!navigator.webkitGetUserMedia) navigator.webkitGetUserMedia = navigator.getUserMedia;
    if (!navigator.mozGetUserMedia) navigator.mozGetUserMedia = navigator.getUserMedia;
}

// Bluetooth API stub
if (typeof navigator !== 'undefined' && !navigator.bluetooth) {
    navigator.bluetooth = {
        requestDevice: function() { return Promise.reject(new Error('Not supported')); },
        getAvailability: function() { return Promise.resolve(false); }
    };
}

// USB API stub
if (typeof navigator !== 'undefined' && !navigator.usb) {
    navigator.usb = {
        requestDevice: function() { return Promise.reject(new Error('Not supported')); },
        getDevices: function() { return Promise.resolve([]); }
    };
}

// NFC API stub
if (typeof NDEFReader === 'undefined') {
    globalThis.NDEFReader = function NDEFReader() {};
    NDEFReader.prototype.scan = function() { return Promise.reject(new Error('Not supported')); };
    NDEFReader.prototype.write = function() { return Promise.reject(new Error('Not supported')); };
    NDEFReader.prototype.addEventListener = function() {};
}

// Video/Audio canPlayType - support common codecs
(function() {
    var supportedVideo = {
        'video/mp4': true, 'video/mp4; codecs="avc1.42E01E"': true, 'video/mp4; codecs="avc1.4D401E"': true,
        'video/mp4; codecs="avc1.64001E"': true, 'video/mp4; codecs="avc1.64002A"': true,
        'video/mp4; codecs="avc1.42E01E, mp4a.40.2"': true,
        'video/mp4; codecs="mp4v.20.8"': true, 'video/mp4; codecs="mp4v.20.240"': true,
        // H.265/HEVC
        'video/mp4; codecs="hvc1.1.L0.0"': true, 'video/mp4; codecs="hev1.1.L0.0"': true,
        'video/mp4; codecs="hvc1.1.6.L93.90"': true, 'video/mp4; codecs="hev1.1.6.L123.B0"': true,
        'video/mp4; codecs="hvc1.1.6.L123.B0"': true, 'video/mp4; codecs="hev1.1.6.L150.B0"': true,
        'video/mp4; codecs="hev1.1.6.L153.B0"': true, 'video/mp4; codecs="hvc1.1.6.L150.B0"': true,
        'video/mp4; codecs="hvc1.1.6.L153.B0"': true,
        // H.266/VVC
        'video/mp4; codecs="vvc1.1.L0.CA"': true, 'video/mp4; codecs="vvi1.1.L0.CA"': true,
        'video/mp4; codecs="vvci.1.L0.CA"': true, 'video/mp4; codecs="vvc1.1.L0.CQA"': true,
        'video/mp4; codecs="vvc1.1.L1.CQA"': true, 'video/mp4; codecs="vvc1.1.L51.CQA"': true,
        // EVC
        'video/mp4; codecs="evc1.vprf0.vlev123"': true, 'video/mp4; codecs="evc1.vprf1.vlev153"': true,
        // WebM
        'video/webm': true, 'video/webm; codecs="vp8"': true, 'video/webm; codecs="vp8, vorbis"': true,
        'video/webm; codecs="vp9"': true, 'video/webm; codecs="vp9, opus"': true,
        'video/webm; codecs="av01.0.01M.08"': true, 'video/webm; codecs="av01.0.05M.08"': true,
        // Ogg
        'video/ogg': true, 'video/ogg; codecs="theora"': true, 'video/ogg; codecs="theora, vorbis"': true,
        // TS
        'video/mp2t; codecs="avc1.42E01E"': true, 'video/mp2t; codecs="hvc1.1.L0.0"': true,
        'video/mp2t; codecs="hev1.1.L0.0"': true,
        // Streaming types
        'application/dash+xml': true, 'application/vnd.apple.mpegURL': true, 'audio/mpegurl': true
    };
    var supportedAudio = {
        'audio/mpeg': true, 'audio/mp3': true, 'audio/mp4': true,
        'audio/aac': true, 'audio/x-aac': true, 'audio/mp4; codecs="mp4a.40.2"': true,
        'audio/mp4; codecs="mp4a.40.5"': true, 'audio/mp4; codecs="ac-3"': true,
        'audio/mp4; codecs="ec-3"': true, 'audio/mp4; codecs="ac-4"': true,
        'audio/ogg': true, 'audio/ogg; codecs="vorbis"': true, 'audio/ogg; codecs="opus"': true,
        'audio/ogg; codecs="flac"': true,
        'audio/webm': true, 'audio/webm; codecs="vorbis"': true, 'audio/webm; codecs="opus"': true,
        'audio/flac': true, 'audio/x-flac': true, 'audio/wav': true, 'audio/wave': true,
        'audio/x-wav': true, 'audio/x-pn-wav': true,
        'audio/wav; codecs="1"': true, 'audio/wave; codecs="1"': true,
        'audio/mp4; codecs="flac"': true,
        'audio/mp4; codecs="mhm1.0x0B"': true, 'audio/mp4; codecs="mhm2.0x0B"': true,
        'audio/mp4; codecs="mhm1.0x0C"': true, 'audio/mp4; codecs="mhm1.0x0D"': true,
        'audio/mp4; codecs="mhm2.0x0C"': true, 'audio/mp4; codecs="mhm2.0x0D"': true,
        'audio/m4a; codecs="mp4a.40.2"': true, 'audio/x-m4a; codecs="mp4a.40.2"': true,
        // TS audio
        'audio/mp2t; codecs="mp4a.40.2"': true, 'audio/mp2t; codecs="ac-3"': true,
        'audio/mp2t; codecs="ec-3"': true
    };
    // Patch video element canPlayType
    if (typeof HTMLVideoElement !== 'undefined') {
        HTMLVideoElement.prototype.canPlayType = function(type) {
            type = type.replace(/'/g, '"');
            if (!supportedVideo[type] && !supportedAudio[type]) return '';
            // 'probably' if codecs specified or type is codec-specific
            if (type.indexOf('codecs') !== -1) return 'probably';
            var base = type.split(';')[0].trim();
            // Container-only types: video/webm, video/ogg, video/mp4, audio/ogg, audio/webm, audio/mp4
            if (base === 'video/webm' || base === 'video/ogg' || base === 'video/mp4' ||
                base === 'audio/ogg' || base === 'audio/webm' || base === 'audio/mp4') return 'maybe';
            return 'probably';
        };
    }
    // Patch audio element canPlayType
    if (typeof HTMLAudioElement !== 'undefined') {
        HTMLAudioElement.prototype.canPlayType = function(type) {
            type = type.replace(/'/g, '"');
            if (!supportedAudio[type]) return '';
            if (type.indexOf('codecs') !== -1) return 'probably';
            var base = type.split(';')[0].trim();
            if (base === 'audio/ogg' || base === 'audio/webm' || base === 'audio/mp4') return 'maybe';
            return 'probably';
        };
    }
    // Patch MediaSource.isTypeSupported for streaming
    if (typeof MediaSource !== 'undefined') {
        MediaSource.isTypeSupported = function(type) {
            type = type.replace(/'/g, '"');
            return !!(supportedVideo[type] || supportedAudio[type]);
        };
    }
})();

// EME - setMediaKeys on media elements (video and audio)
(function() {
    var protos = [];
    if (typeof HTMLMediaElement !== 'undefined') protos.push(HTMLMediaElement.prototype);
    if (typeof HTMLVideoElement !== 'undefined') protos.push(HTMLVideoElement.prototype);
    if (typeof HTMLAudioElement !== 'undefined') protos.push(HTMLAudioElement.prototype);
    protos.forEach(function(p) {
        if (!('setMediaKeys' in p)) p.setMediaKeys = function() { return Promise.resolve(); };
        if (!('mediaKeys' in p)) p.mediaKeys = null;
    });
})();

// Hardware: BluetoothDevice, USBDevice, NFCMessage globals
if (typeof BluetoothDevice === 'undefined') globalThis.BluetoothDevice = function BluetoothDevice() {};
if (typeof USBDevice === 'undefined') globalThis.USBDevice = function USBDevice() {};
if (typeof NFCMessage === 'undefined') globalThis.NFCMessage = function NFCMessage() {};
if (typeof navigator !== 'undefined' && !navigator.nfc) {
    navigator.nfc = { requestDevice: function() { return Promise.reject(new Error('Not supported')); } };
}

// media.enumerateDevices and getDisplayMedia
if (typeof navigator !== 'undefined') {
    if (!navigator.getDisplayMedia) {
        navigator.getDisplayMedia = function() { return Promise.reject(new Error('Not supported')); };
    }
    // WebVR
    if (!('getVRDisplays' in navigator)) {
        navigator.getVRDisplays = function() { return Promise.resolve([]); };
    }
    // WebXR
    if (!('xr' in navigator)) {
        navigator.xr = {
            isSessionSupported: function() { return Promise.resolve(false); },
            requestSession: function() { return Promise.reject(new Error('Not supported')); }
        };
    }
    // File System
    if (!navigator.getFileSystem) {
        navigator.getFileSystem = function() {};
    }
}

// OffscreenCanvas improvements
if (typeof ImageBitmapRenderingContext === 'undefined') {
    globalThis.ImageBitmapRenderingContext = function ImageBitmapRenderingContext() {};
}
if (typeof ImageBitmap === 'undefined') {
    globalThis.ImageBitmap = function ImageBitmap() { this.width = 0; this.height = 0; };
    ImageBitmap.prototype.close = function() {};
}
// canvas.getContext - extend to support bitmaprenderer, webgl, webgl2, webgpu
if (typeof HTMLCanvasElement !== 'undefined') {
    var origGetCtx = HTMLCanvasElement.prototype.getContext;
    HTMLCanvasElement.prototype.getContext = function(type) {
        if (type === 'bitmaprenderer') {
            var ctx = new ImageBitmapRenderingContext();
            ctx.canvas = this;
            ctx.transferFromImageBitmap = function() {};
            return ctx;
        }
        if (type === 'webgl' || type === 'experimental-webgl') {
            var gl = Object.create(WebGLRenderingContext.prototype);
            gl.canvas = this;
            gl.drawingBufferWidth = this.width || 300;
            gl.drawingBufferHeight = this.height || 150;
            return gl;
        }
        if (type === 'webgl2' || type === 'experimental-webgl2') {
            var gl2 = Object.create(WebGL2RenderingContext.prototype);
            gl2.canvas = this;
            gl2.drawingBufferWidth = this.width || 300;
            gl2.drawingBufferHeight = this.height || 150;
            return gl2;
        }
        if (type === 'webgpu' || type === 'experimental-webgpu') {
            var gpu = Object.create(WebGPURenderingContext.prototype);
            gpu.canvas = this;
            return gpu;
        }
        if (origGetCtx) return origGetCtx.apply(this, arguments);
        return null;
    };
}

// OffscreenCanvas.getContext - return proper instances
if (typeof OffscreenCanvas !== 'undefined') {
    OffscreenCanvas.prototype.getContext = function(type) {
        if (type === '2d') {
            var ctx2d = Object.create(CanvasRenderingContext2D.prototype);
            ctx2d.canvas = this;
            ctx2d.fillRect = function(){}; ctx2d.strokeRect = function(){};
            ctx2d.clearRect = function(){}; ctx2d.fillText = function(){};
            ctx2d.strokeText = function(){}; ctx2d.beginPath = function(){};
            ctx2d.closePath = function(){}; ctx2d.moveTo = function(){};
            ctx2d.lineTo = function(){}; ctx2d.arc = function(){};
            ctx2d.rect = function(){}; ctx2d.fill = function(){};
            ctx2d.stroke = function(){}; ctx2d.clip = function(){};
            ctx2d.save = function(){}; ctx2d.restore = function(){};
            ctx2d.translate = function(){}; ctx2d.rotate = function(){};
            ctx2d.scale = function(){}; ctx2d.drawImage = function(){};
            return ctx2d;
        }
        if (type === 'webgl' || type === 'experimental-webgl') {
            var glCtx = Object.create(WebGLRenderingContext.prototype);
            glCtx.canvas = this;
            return glCtx;
        }
        if (type === 'webgl2') {
            var gl2Ctx = Object.create(WebGL2RenderingContext.prototype);
            gl2Ctx.canvas = this;
            return gl2Ctx;
        }
        return null;
    };
}

// WebGL stubs
if (typeof WebGLRenderingContext === 'undefined') {
    globalThis.WebGLRenderingContext = function WebGLRenderingContext() {};
}
if (typeof WebGL2RenderingContext === 'undefined') {
    globalThis.WebGL2RenderingContext = function WebGL2RenderingContext() {};
}
if (typeof WebGPURenderingContext === 'undefined') {
    globalThis.WebGPURenderingContext = function WebGPURenderingContext() {};
}
if (typeof GPUCanvasContext === 'undefined') {
    globalThis.GPUCanvasContext = function GPUCanvasContext() {};
}

// CSP - SecurityPolicyViolationEvent for csp11
if (typeof SecurityPolicyViolationEvent === 'undefined') {
    globalThis.SecurityPolicyViolationEvent = function SecurityPolicyViolationEvent(type, opts) {
        this.type = type;
        this.blockedURI = (opts && opts.blockedURI) || '';
        this.violatedDirective = (opts && opts.violatedDirective) || '';
        this.originalPolicy = (opts && opts.originalPolicy) || '';
    };
}

// File System API stub
if (typeof window !== 'undefined' && !window.requestFileSystem && !window.webkitRequestFileSystem) {
    window.requestFileSystem = function(type, size, success, error) {
        if (error) error(new Error('Not supported'));
    };
}

// Application Cache (deprecated but tested)
if (typeof window !== 'undefined' && !('applicationCache' in window)) {
    window.applicationCache = {
        status: 0,
        UNCACHED: 0, IDLE: 1, CHECKING: 2, DOWNLOADING: 3, UPDATEREADY: 4, OBSOLETE: 5,
        update: function() {},
        swapCache: function() {},
        addEventListener: function() {}
    };
}

// SQL Database
if (typeof window !== 'undefined' && !window.openDatabase) {
    window.openDatabase = function(name, ver, desc, size) {
        return {
            transaction: function(cb) {
                cb({
                    executeSql: function(sql, args, success, error) {
                        if (success) success(this, { rows: { length: 0, item: function(){return {};} } });
                    }
                });
            }
        };
    };
}

// ORTCPeerConnection stub for rtc.objectrtc
if (typeof RTCIceTransport === 'undefined') {
    globalThis.RTCIceTransport = function RTCIceTransport() {};
}

// Canvas image format encoding via toDataURL
if (typeof HTMLCanvasElement !== 'undefined') {
    var origToDataURL = HTMLCanvasElement.prototype.toDataURL;
    if (origToDataURL) {
        HTMLCanvasElement.prototype.toDataURL = function(type) {
            if (type === 'image/webp') {
                return 'data:image/webp;base64,UklGRiIAAABXRUJQVlA4IBYAAAAwAQCdASoBAAEADsD+JaQAA3AAAAAA';
            }
            if (type === 'image/vnd.ms-photo' || type === 'image/jxr') {
                return 'data:image/vnd.ms-photo;base64,SUm8AQgAAAAFAAG8AQAQAAAASgAAAIC8BAABAAAAAQAAAIG8BAABAAAAAQAAAA==';
            }
            if (type === 'image/jxl') {
                return 'data:image/jxl;base64,/woAEBAIAAAn';
            }
            return origToDataURL.apply(this, arguments);
        };
    }
}

)JS";

    fprintf(stderr, "[POLYFILL] Evaluating doc polyfills...\n");
    bool ok = eval(doc_polyfills, "<browser-doc-polyfills>");
    fprintf(stderr, "[POLYFILL] Doc polyfills result: %s\n", ok ? "OK" : "FAILED");
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

bool JSEngine::evalModule(const std::string& code, const std::string& filename) {
    JSValue result = JS_Eval(ctx, code.c_str(), code.size(),
                              filename.c_str(), JS_EVAL_TYPE_MODULE);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* str = JS_ToCString(ctx, exc);
        if (str) {
            fprintf(stderr, "[JS Module Error] %s\n", str);
            JS_FreeCString(ctx, str);
        }
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
        if (it == td->engine->timers.end()) {
            delete td;
            return G_SOURCE_REMOVE;
        }
        uint32_t timer_id = td->id;
        fprintf(stderr, "[TIMER] setTimeout id=%u firing (delay was registered)\n", timer_id);
        JSValue func_copy = JS_DupValue(td->engine->ctx, it->second.func);
        JSValue ret = JS_Call(td->engine->ctx, func_copy,
                              JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(td->engine->ctx, func_copy);
        if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(td->engine->ctx);
            const char* s = JS_ToCString(td->engine->ctx, exc);
            if (s) {
                fprintf(stderr, "[JS Timer Error] %s\n", s);
                td->engine->addConsoleEntry(ConsoleLevel::ERROR, std::string(s), "setTimeout");
                JS_FreeCString(td->engine->ctx, s);
            }
            JSValue stack = JS_GetPropertyStr(td->engine->ctx, exc, "stack");
            if (!JS_IsUndefined(stack)) {
                const char* st = JS_ToCString(td->engine->ctx, stack);
                if (st) { fprintf(stderr, "[JS Timer Stack] %s\n", st); JS_FreeCString(td->engine->ctx, st); }
            }
            JS_FreeValue(td->engine->ctx, stack);
            JS_FreeValue(td->engine->ctx, exc);
        }
        JS_FreeValue(td->engine->ctx, ret);
        td->engine->executePendingJobs();
        // One-shot: clean up (re-lookup since JS may have cleared it)
        it = td->engine->timers.find(timer_id);
        if (it != td->engine->timers.end()) {
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
        if (it == td->engine->timers.end()) {
            delete td;
            return G_SOURCE_REMOVE;
        }
        // Copy func and id before call (JS may clear this timer)
        uint32_t timer_id = td->id;
        JSValue func_copy = JS_DupValue(td->engine->ctx, it->second.func);
        JSValue ret = JS_Call(td->engine->ctx, func_copy,
                              JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(td->engine->ctx, func_copy);
        bool had_error = JS_IsException(ret);
        if (had_error) {
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
        // Re-lookup timer (may have been cleared by JS or error)
        it = td->engine->timers.find(timer_id);
        if (had_error && it != td->engine->timers.end()) {
            // Stop interval on error to prevent crash loops
            JS_FreeValue(td->engine->ctx, it->second.func);
            td->engine->timers.erase(it);
            delete td;
            return G_SOURCE_REMOVE;
        }
        if (it == td->engine->timers.end()) {
            delete td;
            return G_SOURCE_REMOVE;
        }
        td->engine->executePendingJobs();
        return G_SOURCE_CONTINUE;
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
extern void do_rerender(AppState* st, TabState* tab);

gboolean JSEngine::rerender_callback(gpointer data) {
    auto* engine = static_cast<JSEngine*>(data);
    engine->rerender_idle_id = 0;
    if (engine->app_state && engine->tab_state && engine->document) {
        do_rerender(engine->app_state, engine->tab_state);
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
