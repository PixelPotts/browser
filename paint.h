#pragma once
#include "layout.h"
#include <cairo.h>
#include <vector>
#include <string>

// ---- Paint commands (display list) ----

enum class PaintCmdType {
    FillRect,
    DrawText,
    DrawBorder,
    DrawImage,
    PushClip,
    PopClip,
    PushOpacity,
    PopOpacity,
    Translate,
    PopTranslate
};

struct CairoColor {
    double r = 0, g = 0, b = 0, a = 1.0;
};

struct PaintCommand {
    PaintCmdType type;

    // FillRect
    Rect rect;
    CairoColor color;

    // DrawText
    PangoLayout* pango_layout = nullptr;  // not owned
    float text_x = 0, text_y = 0;
    CairoColor text_color;

    // DrawBorder
    Rect border_rect;
    Edges border_widths;
    CairoColor border_color_val;
    int border_radius = 0;

    // DrawImage
    cairo_surface_t* surface = nullptr;  // not owned
    Rect dest_rect;

    // PushClip
    Rect clip_rect;

    // PushOpacity
    float opacity = 1.0;

    // Translate
    float tx = 0, ty = 0;

    // Back-pointer for hit testing
    DOMNode* dom_node = nullptr;
};

using DisplayList = std::vector<PaintCommand>;

// ---- Paint API ----

// Generate display list from layout tree
DisplayList generate_display_list(LayoutBox* root);

// Render display list to cairo context
void render_display_list(cairo_t* cr, const DisplayList& dl, float scroll_x, float scroll_y,
                          float viewport_width, float viewport_height);

// Parse CSS color string to CairoColor
CairoColor parse_css_color(const std::string& color);
