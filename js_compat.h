#pragma once

extern "C" {
#include "quickjs.h"
}

class JSEngine;

// Initialize compatibility bindings (DOM methods, Web APIs, etc.)
void js_compat_init(JSEngine* engine);
