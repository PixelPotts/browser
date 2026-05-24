#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <cmath>
#include <functional>
#include <gtk/gtk.h>
#include <pango/pango.h>
#include "dom.h"

// ---- Core geometry types ----

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

struct Edges {
    float top = 0, right = 0, bottom = 0, left = 0;
    float horizontal() const { return left + right; }
    float vertical() const { return top + bottom; }
};

// ---- Layout box types ----

enum class LayoutBoxType {
    Block,
    Inline,
    InlineBlock,
    Text,
    Replaced,     // img, canvas, svg, video
    Flex,
    Grid,
    Anonymous,    // wrapper for mixed inline/block content
    None          // display:none - not in tree
};

// ---- Line box for inline formatting context ----

struct InlineFragment {
    DOMNode* node = nullptr;
    PangoLayout* pango_layout = nullptr; // owned by the fragment
    float x = 0, y = 0, w = 0, h = 0;  // position within line box
    float baseline = 0;                  // distance from top to baseline
    std::string text;                    // the text this fragment renders
    int start_index = 0;                 // byte offset into original text
    int end_index = 0;
    bool is_line_break = false;
};

struct LineBox {
    float x = 0, y = 0;   // position relative to containing block
    float width = 0;       // available width
    float height = 0;      // computed height (max of fragment heights)
    float baseline = 0;    // baseline position from top
    std::vector<InlineFragment> fragments;
};

// ---- The layout box ----

struct LayoutBox {
    LayoutBoxType type = LayoutBoxType::Block;
    DOMNode* dom_node = nullptr;   // back-pointer (nullptr for anonymous boxes)

    LayoutBox* parent = nullptr;
    std::vector<std::unique_ptr<LayoutBox>> children;

    // Box model geometry (all in document coordinates after layout)
    Rect content_rect;        // content area position + size
    Edges margin;
    Edges padding;
    Edges border;

    // Absolute document coordinates (for hit test + JS APIs)
    float abs_x = 0, abs_y = 0;

    // Inline formatting context data
    std::vector<LineBox> line_boxes;

    // Scroll state
    float scroll_x = 0, scroll_y = 0;
    float scroll_width = 0, scroll_height = 0;

    // For replaced elements (img, canvas, svg)
    cairo_surface_t* replaced_surface = nullptr;
    int natural_width = 0, natural_height = 0;

    // Text node data
    std::string text;
    PangoLayout* pango_layout = nullptr;  // for text nodes

    // Text run mapping (for inline hit testing - maps char ranges to DOM nodes)
    struct TextRunInfo {
        DOMNode* node;
        int start;   // byte offset in full_text
        int length;
    };
    std::vector<TextRunInfo> text_runs;

    // Style data read from DOMNode (cached for layout)
    int font_size = 16;
    int font_weight = 400;
    int font_style = 0;  // PangoStyle
    std::string font_family;
    std::string color;
    std::string bg_color;
    float opacity = 1.0;
    int text_align = 0;   // 0=left, 1=center, 2=right, 3=justify
    int white_space = 0;  // 0=normal, 1=nowrap, 2=pre, 3=pre-wrap, 4=pre-line
    int position = 0;     // 0=static, 1=relative, 2=absolute, 3=fixed
    float line_height_factor = 1.2;
    int overflow = 0;     // 0=visible, 1=hidden, 2=scroll, 3=auto
    std::string border_color;
    std::string border_style;
    int z_index = 0;

    // Cleanup
    ~LayoutBox();

    // Compute absolute positions after layout
    void compute_abs_positions(float parent_x, float parent_y);

    // Get the border box rect (content + padding + border)
    Rect border_box() const {
        return {
            content_rect.x - padding.left - border.left,
            content_rect.y - padding.top - border.top,
            content_rect.w + padding.horizontal() + border.horizontal(),
            content_rect.h + padding.vertical() + border.vertical()
        };
    }

    // Get the margin box rect
    Rect margin_box() const {
        auto bb = border_box();
        return {
            bb.x - margin.left,
            bb.y - margin.top,
            bb.w + margin.horizontal(),
            bb.h + margin.vertical()
        };
    }
};

// ---- Layout engine interface ----

// Build a layout tree from the DOM tree
std::unique_ptr<LayoutBox> build_layout_tree(DOMNode* node, PangoContext* pango_ctx);

// Perform layout (compute positions and sizes)
void perform_layout(LayoutBox* root, float viewport_width, float viewport_height,
                    PangoContext* pango_ctx);

// Get the total content height (for scrolling)
float get_content_height(LayoutBox* root);
