#pragma once
#include <string>
#include <cstdint>

extern "C" {
#include "quickjs.h"
}

class JSEngine;
struct DOMNode;
struct AppState;

// Initialize the event system (connect GTK signals to JS dispatch)
void js_event_init(JSEngine* engine);

// Dispatch a JS event on a DOM node with bubbling
void js_dispatch_event(JSEngine* engine, uint32_t node_id,
                        const std::string& type,
                        int clientX = 0, int clientY = 0);

// Create a JS Event object
JSValue js_create_event(JSContext* ctx, const std::string& type,
                         DOMNode* target, int clientX, int clientY);

// Hook GTK click events to JS event dispatch
void js_event_hook_gtk(AppState* st);
