#pragma once
#include "layout.h"

// ---- Hit testing ----

struct HitTestResult {
    DOMNode* node = nullptr;
    float local_x = 0, local_y = 0;  // coordinates relative to the hit element
};

// Find the deepest element at document coordinates (x, y)
HitTestResult hit_test(LayoutBox* root, float x, float y);
extern bool g_hit_debug;

// Find the element at viewport coordinates, accounting for scroll
HitTestResult hit_test_viewport(LayoutBox* root, float vx, float vy,
                                 float scroll_x, float scroll_y);
