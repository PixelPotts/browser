#pragma once

extern "C" {
#include "quickjs.h"
}

class JSEngine;
class Document;
struct DOMNode;

// Initialize DOM bindings in the given JS context
void js_bindings_init(JSEngine* engine);

// Wrap a DOMNode as a JS Element object
JSValue js_wrap_node(JSContext* ctx, DOMNode* node);

// Get DOMNode* from a JS Element object
DOMNode* js_get_node(JSContext* ctx, JSValueConst val);
