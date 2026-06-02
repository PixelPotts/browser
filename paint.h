#pragma once
#include "layout.h"
#include <cairo.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <glib.h>

// ---- Color ----

struct CairoColor {
    double r = 0, g = 0, b = 0, a = 1.0;
};

// ---- Paint commands (display list) ----

struct GradientStop {
    float pos;       // 0.0 – 1.0
    CairoColor color;
};

struct FilterDef {
    enum Type { Blur, Brightness, Grayscale, Opacity, Contrast, Invert, Saturate, Sepia } type;
    float value = 1.0f;  // amount (blur = pixels, others = multiplier)
};

enum class PaintCmdType {
    FillRect,
    DrawText,
    DrawBorder,
    DrawOutline,
    DrawLinearGradient,
    DrawRadialGradient,
    DrawImage,
    DrawBackgroundImage,
    PushClip,
    PopClip,
    PushOpacity,
    PopOpacity,
    Translate,
    PopTranslate,
    PushTransform,
    PopTransform,
    PushFilter,
    PopFilter
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
    // text-shadow (applied before main text)
    bool has_text_shadow = false;
    float shadow_dx = 0, shadow_dy = 0, shadow_blur = 0;
    CairoColor shadow_color;

    // DrawBorder / DrawOutline
    Rect border_rect;
    Edges border_widths;
    CairoColor border_color_val;
    int border_radius = 0;
    // outline-specific
    float outline_w = 0;      // stroke width for DrawOutline

    // DrawImage
    cairo_surface_t* surface = nullptr;  // not owned
    Rect dest_rect;
    int object_fit = 0;  // 0=fill, 1=contain, 2=cover, 3=scale-down, 4=none
    int natural_w = 0, natural_h = 0;  // intrinsic image dimensions

    // PushClip
    Rect clip_rect;

    // PushOpacity
    float opacity = 1.0;

    // Translate / PushTransform
    float tx = 0, ty = 0;
    cairo_matrix_t transform_matrix = {1,0,0,1,0,0};  // identity by default

    // Gradient (DrawLinearGradient, DrawRadialGradient)
    std::vector<GradientStop> gradient_stops;
    float grad_x0=0, grad_y0=0, grad_x1=0, grad_y1=0;  // linear endpoints
    float grad_cx=0, grad_cy=0, grad_r=0;               // radial center+radius

    // DrawBackgroundImage extras
    std::string bg_size_raw;
    std::string bg_position_raw;
    int bg_repeat_mode = 0;  // 0=repeat, 1=repeat-x, 2=repeat-y, 3=no-repeat

    // Filter (PushFilter / PopFilter)
    std::vector<FilterDef> filters;

    // Back-pointer for hit testing
    DOMNode* dom_node = nullptr;
};

using DisplayList = std::vector<PaintCommand>;

// ---- CSS Transition animation state ----

struct AnimState {
    uint32_t node_id = 0;
    CairoColor from_color, to_color;
    gint64 start_us = 0;   // g_get_monotonic_time() at start
    int duration_ms = 0;
};

// Global animation map: node_id → AnimState (defined in browser.cpp)
extern std::unordered_map<uint32_t, AnimState> g_animations;
extern bool g_anim_pending;   // true when a new animation was queued
extern guint g_anim_timer_id; // g_timeout handle (0 = not running)

// ---- Paint API ----

// Generate display list from layout tree
DisplayList generate_display_list(LayoutBox* root);

// Generate display list for position:fixed elements (rendered without scroll)
DisplayList generate_fixed_display_list(LayoutBox* root);

// Collect position:sticky elements for dynamic rendering
void collect_sticky_boxes(LayoutBox* root, std::vector<LayoutBox*>& out);

// Paint a single layout box into a display list (with explicit offset)
void paint_box(LayoutBox* box, DisplayList& dl, float offset_x, float offset_y);

// Render display list to cairo context
void render_display_list(cairo_t* cr, const DisplayList& dl, float scroll_x, float scroll_y,
                          float viewport_width, float viewport_height);

// Parse CSS color string to CairoColor
CairoColor parse_css_color(const std::string& color);
