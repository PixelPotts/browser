#include "js_event.h"
#include "js_engine.h"
#include "js_bindings.h"
#include "dom.h"
#include <cstdio>
#include <vector>
#include <ctime>

extern "C" {
#include "quickjs.h"
}

// ---- JS Event object ----

static JSClassID js_event_class_id = 0;

JSClassID js_get_event_class_id() { return js_event_class_id; }

struct EventOpaque {
    std::string type;
    uint32_t target_id;
    uint32_t current_target_id; // node currently handling event
    int clientX, clientY;
    bool defaultPrevented;
    bool propagationStopped;
    bool bubbles;
    bool cancelable;
    bool isTrusted;
    int eventPhase; // 0=none, 1=capturing, 2=at_target, 3=bubbling
    double timeStamp;
    // Keyboard event fields
    std::string key;
    std::string code;
    uint32_t keyCode;
    bool shiftKey, ctrlKey, altKey, metaKey;
    // Bubble path (node_ids)
    std::vector<uint32_t> composed_path;
};

static void js_event_finalizer(JSRuntime* rt, JSValue val) {
    auto* op = (EventOpaque*)JS_GetOpaque(val, js_event_class_id);
    delete op;
}

static JSValue js_event_get_type(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    if (!op) return JS_UNDEFINED;
    return JS_NewString(ctx, op->type.c_str());
}

static JSValue js_event_get_target(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    if (!op || !g_js_engine || !g_js_engine->document) return JS_NULL;
    auto it = g_js_engine->document->node_map.find(op->target_id);
    if (it == g_js_engine->document->node_map.end()) return JS_NULL;
    return js_wrap_node(ctx, it->second);
}

static JSValue js_event_get_currentTarget(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    if (!op || !g_js_engine || !g_js_engine->document) return JS_NULL;
    auto it = g_js_engine->document->node_map.find(op->current_target_id);
    if (it == g_js_engine->document->node_map.end()) return JS_NULL;
    return js_wrap_node(ctx, it->second);
}

static JSValue js_event_get_clientX(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewInt32(ctx, op->clientX) : JS_NewInt32(ctx, 0);
}

static JSValue js_event_get_clientY(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewInt32(ctx, op->clientY) : JS_NewInt32(ctx, 0);
}

static JSValue js_event_get_bubbles(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewBool(ctx, op->bubbles) : JS_FALSE;
}

static JSValue js_event_get_cancelable(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewBool(ctx, op->cancelable) : JS_FALSE;
}

static JSValue js_event_get_defaultPrevented(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewBool(ctx, op->defaultPrevented) : JS_FALSE;
}

static JSValue js_event_get_isTrusted(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewBool(ctx, op->isTrusted) : JS_FALSE;
}

static JSValue js_event_get_eventPhase(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewInt32(ctx, op->eventPhase) : JS_NewInt32(ctx, 0);
}

static JSValue js_event_get_timeStamp(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewFloat64(ctx, op->timeStamp) : JS_NewFloat64(ctx, 0);
}

// Keyboard event property getters
static JSValue js_event_get_key(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    if (!op) return JS_NewString(ctx, "");
    return JS_NewString(ctx, op->key.c_str());
}

static JSValue js_event_get_code(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    if (!op) return JS_NewString(ctx, "");
    return JS_NewString(ctx, op->code.c_str());
}

static JSValue js_event_get_keyCode(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewInt32(ctx, op->keyCode) : JS_NewInt32(ctx, 0);
}

static JSValue js_event_get_which(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewInt32(ctx, op->keyCode) : JS_NewInt32(ctx, 0);
}

static JSValue js_event_get_shiftKey(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewBool(ctx, op->shiftKey) : JS_FALSE;
}

static JSValue js_event_get_ctrlKey(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewBool(ctx, op->ctrlKey) : JS_FALSE;
}

static JSValue js_event_get_altKey(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewBool(ctx, op->altKey) : JS_FALSE;
}

static JSValue js_event_get_metaKey(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewBool(ctx, op->metaKey) : JS_FALSE;
}

static JSValue js_event_preventDefault(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    if (op) op->defaultPrevented = true;
    return JS_UNDEFINED;
}

static JSValue js_event_stopPropagation(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    if (op) op->propagationStopped = true;
    return JS_UNDEFINED;
}

static JSValue js_event_stopImmediatePropagation(JSContext* ctx, JSValueConst this_val,
                                                   int argc, JSValueConst* argv) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    if (op) op->propagationStopped = true;
    return JS_UNDEFINED;
}

static JSValue js_event_composedPath(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    if (!op || !g_js_engine || !g_js_engine->document) return JS_NewArray(ctx);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < op->composed_path.size(); i++) {
        auto it = g_js_engine->document->node_map.find(op->composed_path[i]);
        if (it != g_js_engine->document->node_map.end())
            JS_SetPropertyUint32(ctx, arr, i, js_wrap_node(ctx, it->second));
    }
    return arr;
}

static const JSClassDef js_event_class_def = {
    "Event", js_event_finalizer, nullptr, nullptr, nullptr
};

static double get_timestamp_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

JSValue js_create_event(JSContext* ctx, const std::string& type,
                         DOMNode* target, int clientX, int clientY) {
    JSValue obj = JS_NewObjectClass(ctx, js_event_class_id);
    auto* op = new EventOpaque();
    op->type = type;
    op->target_id = target ? target->node_id : 0;
    op->current_target_id = op->target_id;
    op->clientX = clientX;
    op->clientY = clientY;
    op->defaultPrevented = false;
    op->propagationStopped = false;
    op->bubbles = true;
    op->cancelable = true;
    op->isTrusted = true;
    op->eventPhase = 0;
    op->timeStamp = get_timestamp_ms();
    op->keyCode = 0;
    op->shiftKey = op->ctrlKey = op->altKey = op->metaKey = false;
    JS_SetOpaque(obj, op);
    return obj;
}

// ---- Event dispatch with bubbling ----

void js_dispatch_event(JSEngine* engine, uint32_t node_id,
                        const std::string& type, int clientX, int clientY) {
    if (!engine || !engine->ctx || !engine->document) return;

    auto it = engine->document->node_map.find(node_id);
    if (it == engine->document->node_map.end()) return;
    DOMNode* target = it->second;

    // Create event object
    JSValue event = js_create_event(engine->ctx, type, target, clientX, clientY);
    auto* event_op = (EventOpaque*)JS_GetOpaque(event, js_event_class_id);

    // Collect bubble path: target -> parent -> ... -> root
    std::vector<DOMNode*> path;
    DOMNode* cur = target;
    while (cur) {
        path.push_back(cur);
        event_op->composed_path.push_back(cur->node_id);
        cur = cur->parent;
    }

    // Dispatch to each node in bubble path
    JSValue global = JS_GetGlobalObject(engine->ctx);
    for (size_t pi = 0; pi < path.size(); pi++) {
        DOMNode* node = path[pi];
        if (event_op && event_op->propagationStopped) break;

        // Update currentTarget and eventPhase
        event_op->current_target_id = node->node_id;
        event_op->eventPhase = (pi == 0) ? 2 : 3; // AT_TARGET or BUBBLING

        for (const auto& listener : node->listeners) {
            if (listener.type != type) continue;

            // Get the stored handler function
            std::string key = "__handler_" + std::to_string(listener.handler_id);
            JSValue handler = JS_GetPropertyStr(engine->ctx, global, key.c_str());
            if (JS_IsFunction(engine->ctx, handler)) {
                JSValue this_obj = js_wrap_node(engine->ctx, node);
                JSValue args[1] = {event};
                JSValue ret = JS_Call(engine->ctx, handler, this_obj, 1, args);
                if (JS_IsException(ret)) {
                    JSValue exc = JS_GetException(engine->ctx);
                    const char* s = JS_ToCString(engine->ctx, exc);
                    if (s) {
                        fprintf(stderr, "[JS Event Error] %s\n", s);
                        engine->addConsoleEntry(ConsoleLevel::ERROR, std::string(s), "event:" + type);
                        JS_FreeCString(engine->ctx, s);
                    }
                    JS_FreeValue(engine->ctx, exc);
                }
                JS_FreeValue(engine->ctx, ret);
                JS_FreeValue(engine->ctx, this_obj);
            }
            JS_FreeValue(engine->ctx, handler);
        }
    }
    event_op->eventPhase = 0; // NONE after dispatch
    JS_FreeValue(engine->ctx, global);
    JS_FreeValue(engine->ctx, event);

    // Execute pending jobs after event handling
    engine->executePendingJobs();

    // Check if DOM was dirtied, schedule re-render
    if (engine->document->body && engine->document->body->dirty) {
        engine->scheduleRerender();
    }
}

// ---- Keyboard event dispatch ----

void js_dispatch_key_event(JSEngine* engine, uint32_t node_id,
                            const std::string& type, const std::string& key,
                            const std::string& code, uint32_t keyCode,
                            bool shiftKey, bool ctrlKey, bool altKey, bool metaKey) {
    if (!engine || !engine->ctx || !engine->document) return;

    auto it = engine->document->node_map.find(node_id);
    if (it == engine->document->node_map.end()) return;
    DOMNode* target = it->second;

    JSValue event = js_create_event(engine->ctx, type, target, 0, 0);
    auto* event_op = (EventOpaque*)JS_GetOpaque(event, js_event_class_id);
    event_op->key = key;
    event_op->code = code;
    event_op->keyCode = keyCode;
    event_op->shiftKey = shiftKey;
    event_op->ctrlKey = ctrlKey;
    event_op->altKey = altKey;
    event_op->metaKey = metaKey;

    // Collect bubble path
    std::vector<DOMNode*> path;
    DOMNode* cur = target;
    while (cur) {
        path.push_back(cur);
        event_op->composed_path.push_back(cur->node_id);
        cur = cur->parent;
    }

    // Dispatch with bubbling
    JSValue global = JS_GetGlobalObject(engine->ctx);
    for (size_t pi = 0; pi < path.size(); pi++) {
        DOMNode* node = path[pi];
        if (event_op->propagationStopped) break;
        event_op->current_target_id = node->node_id;
        event_op->eventPhase = (pi == 0) ? 2 : 3;

        for (const auto& listener : node->listeners) {
            if (listener.type != type) continue;
            std::string hkey = "__handler_" + std::to_string(listener.handler_id);
            JSValue handler = JS_GetPropertyStr(engine->ctx, global, hkey.c_str());
            if (JS_IsFunction(engine->ctx, handler)) {
                JSValue this_obj = js_wrap_node(engine->ctx, node);
                JSValue args[1] = {event};
                JSValue ret = JS_Call(engine->ctx, handler, this_obj, 1, args);
                if (JS_IsException(ret)) {
                    JSValue exc = JS_GetException(engine->ctx);
                    const char* s = JS_ToCString(engine->ctx, exc);
                    if (s) {
                        fprintf(stderr, "[JS Key Event Error] %s\n", s);
                        engine->addConsoleEntry(ConsoleLevel::ERROR, std::string(s), "event:" + type);
                        JS_FreeCString(engine->ctx, s);
                    }
                    JS_FreeValue(engine->ctx, exc);
                }
                JS_FreeValue(engine->ctx, ret);
                JS_FreeValue(engine->ctx, this_obj);
            }
            JS_FreeValue(engine->ctx, handler);
        }
    }

    // Also dispatch to window listeners
    if (!event_op->propagationStopped) {
        extern void js_dispatch_to_window_listeners(JSEngine* engine, const std::string& type, JSValue event);
        js_dispatch_to_window_listeners(engine, type, event);
    }

    JS_FreeValue(engine->ctx, global);
    JS_FreeValue(engine->ctx, event);
    engine->executePendingJobs();
    if (engine->document->body && engine->document->body->dirty)
        engine->scheduleRerender();
}

// ---- Init: register Event class ----

void js_event_init(JSEngine* engine) {
    JSContext* ctx = engine->ctx;
    JSRuntime* rt = engine->rt;

    JS_NewClassID(&js_event_class_id);
    JS_NewClass(rt, js_event_class_id, &js_event_class_def);

    JSValue proto = JS_NewObject(ctx);

    // Standard event properties
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "type"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_type, "get type", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "target"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_target, "get target", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "currentTarget"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_currentTarget, "get currentTarget", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "clientX"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_clientX, "get clientX", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "clientY"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_clientY, "get clientY", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "bubbles"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_bubbles, "get bubbles", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "cancelable"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_cancelable, "get cancelable", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "defaultPrevented"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_defaultPrevented, "get defaultPrevented", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "isTrusted"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_isTrusted, "get isTrusted", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "eventPhase"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_eventPhase, "get eventPhase", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "timeStamp"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_timeStamp, "get timeStamp", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);

    // Keyboard event properties
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "key"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_key, "get key", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "code"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_code, "get code", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "keyCode"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_keyCode, "get keyCode", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "which"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_which, "get which", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "shiftKey"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_shiftKey, "get shiftKey", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "ctrlKey"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_ctrlKey, "get ctrlKey", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "altKey"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_altKey, "get altKey", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "metaKey"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_metaKey, "get metaKey", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);

    // Methods
    JS_SetPropertyStr(ctx, proto, "preventDefault",
        JS_NewCFunction(ctx, js_event_preventDefault, "preventDefault", 0));
    JS_SetPropertyStr(ctx, proto, "stopPropagation",
        JS_NewCFunction(ctx, js_event_stopPropagation, "stopPropagation", 0));
    JS_SetPropertyStr(ctx, proto, "stopImmediatePropagation",
        JS_NewCFunction(ctx, js_event_stopImmediatePropagation, "stopImmediatePropagation", 0));
    JS_SetPropertyStr(ctx, proto, "composedPath",
        JS_NewCFunction(ctx, js_event_composedPath, "composedPath", 0));

    JS_SetClassProto(ctx, js_event_class_id, proto);
}
