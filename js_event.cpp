#include "js_event.h"
#include "js_engine.h"
#include "js_bindings.h"
#include "dom.h"
#include <cstdio>
#include <vector>

extern "C" {
#include "quickjs.h"
}

// ---- JS Event object ----

static JSClassID js_event_class_id = 0;

struct EventOpaque {
    std::string type;
    uint32_t target_id;
    int clientX, clientY;
    bool defaultPrevented;
    bool propagationStopped;
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

static JSValue js_event_get_clientX(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewInt32(ctx, op->clientX) : JS_NewInt32(ctx, 0);
}

static JSValue js_event_get_clientY(JSContext* ctx, JSValueConst this_val) {
    auto* op = (EventOpaque*)JS_GetOpaque(this_val, js_event_class_id);
    return op ? JS_NewInt32(ctx, op->clientY) : JS_NewInt32(ctx, 0);
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

static const JSClassDef js_event_class_def = {
    "Event", js_event_finalizer, nullptr, nullptr, nullptr
};

JSValue js_create_event(JSContext* ctx, const std::string& type,
                         DOMNode* target, int clientX, int clientY) {
    JSValue obj = JS_NewObjectClass(ctx, js_event_class_id);
    auto* op = new EventOpaque{type, target ? target->node_id : 0,
                                clientX, clientY, false, false};
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
        cur = cur->parent;
    }

    // Dispatch to each node in bubble path
    JSValue global = JS_GetGlobalObject(engine->ctx);
    for (DOMNode* node : path) {
        if (event_op && event_op->propagationStopped) break;

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
    JS_FreeValue(engine->ctx, global);
    JS_FreeValue(engine->ctx, event);

    // Execute pending jobs after event handling
    engine->executePendingJobs();

    // Check if DOM was dirtied, schedule re-render
    if (engine->document->body && engine->document->body->dirty) {
        engine->scheduleRerender();
    }
}

// ---- Init: register Event class ----

void js_event_init(JSEngine* engine) {
    JSContext* ctx = engine->ctx;
    JSRuntime* rt = engine->rt;

    JS_NewClassID(&js_event_class_id);
    JS_NewClass(rt, js_event_class_id, &js_event_class_def);

    JSValue proto = JS_NewObject(ctx);

    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "type"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_type, "get type", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "target"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_target, "get target", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "clientX"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_clientX, "get clientX", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, proto,
        JS_NewAtom(ctx, "clientY"),
        JS_NewCFunction(ctx, (JSCFunction*)js_event_get_clientY, "get clientY", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);

    JS_SetPropertyStr(ctx, proto, "preventDefault",
        JS_NewCFunction(ctx, js_event_preventDefault, "preventDefault", 0));
    JS_SetPropertyStr(ctx, proto, "stopPropagation",
        JS_NewCFunction(ctx, js_event_stopPropagation, "stopPropagation", 0));

    JS_SetClassProto(ctx, js_event_class_id, proto);
}
