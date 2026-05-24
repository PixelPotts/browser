// layout.cpp - Layout tree construction and block formatting context
#include "layout.h"
#include <cstring>
#include <algorithm>
#include <cassert>
#include <cstdio>

// ---- LayoutBox destructor ----

LayoutBox::~LayoutBox() {
    if (pango_layout) g_object_unref(pango_layout);
    for (auto& lb : line_boxes)
        for (auto& frag : lb.fragments)
            if (frag.pango_layout) g_object_unref(frag.pango_layout);
}

void LayoutBox::compute_abs_positions(float parent_x, float parent_y) {
    float bx = content_rect.x - padding.left - border.left;
    float by = content_rect.y - padding.top - border.top;
    abs_x = parent_x + bx;
    abs_y = parent_y + by;
    for (auto& child : children)
        child->compute_abs_positions(abs_x + padding.left + border.left,
                                      abs_y + padding.top + border.top);
}

// ---- Helper: determine display type from DOMNode ----

static LayoutBoxType display_type_for(DOMNode* node) {
    if (!node) return LayoutBoxType::None;
    if (node->node_type == DOMNode::TEXT) return LayoutBoxType::Text;
    if (node->node_type == DOMNode::COMMENT) return LayoutBoxType::None;

    auto d = node->display;
    if (d == DOMNode::Display::None) return LayoutBoxType::None;
    if (d == DOMNode::Display::Flex) return LayoutBoxType::Flex;
    if (d == DOMNode::Display::InlineBlock) return LayoutBoxType::InlineBlock;

    // Check if inline by tag or display property
    if (d == DOMNode::Display::Inline) return LayoutBoxType::Inline;

    // Block by default for elements, or explicit Block
    if (d == DOMNode::Display::Block) return LayoutBoxType::Block;

    // Inherit: determine from tag
    const auto& tag = node->tag_name;
    if (tag == "span" || tag == "a" || tag == "b" || tag == "i" || tag == "em" ||
        tag == "strong" || tag == "u" || tag == "s" || tag == "small" || tag == "sub" ||
        tag == "sup" || tag == "abbr" || tag == "code" || tag == "kbd" || tag == "var" ||
        tag == "cite" || tag == "q" || tag == "mark" || tag == "time" || tag == "label" ||
        tag == "font" || tag == "bdi" || tag == "bdo" || tag == "data" || tag == "wbr")
        return LayoutBoxType::Inline;

    // Replaced elements
    if (tag == "img" || tag == "canvas" || tag == "video" || tag == "svg" ||
        tag == "iframe" || tag == "object" || tag == "embed")
        return LayoutBoxType::Replaced;

    return LayoutBoxType::Block;
}

// ---- Helper: is this a block-level box type? ----

static bool is_block_level(LayoutBoxType t) {
    return t == LayoutBoxType::Block || t == LayoutBoxType::Flex ||
           t == LayoutBoxType::Grid || t == LayoutBoxType::Anonymous;
}

// ---- Helper: copy style from DOMNode to LayoutBox ----

static void copy_style(LayoutBox* box, DOMNode* node) {
    if (!node) return;
    box->font_size = node->fs_computed > 0 ? node->fs_computed : 16;
    box->font_weight = node->fw_computed >= 0 ? node->fw_computed : 400;
    box->font_style = node->fi_computed >= 0 ? node->fi_computed : 0;
    box->font_family = node->font_family;
    box->color = node->color_computed;
    box->bg_color = node->bg_color;
    box->opacity = node->opacity;
    box->text_align = node->text_align_computed >= 0 ? node->text_align_computed : 0;
    box->white_space = node->white_space >= 0 ? node->white_space : 0;
    box->position = node->position;
    box->line_height_factor = node->lh_computed > 0 ? node->lh_computed : 1.2;
    box->overflow = node->overflow >= 0 ? node->overflow : 0;
    box->z_index = node->z_index;
    box->border_color = node->border_color;
    box->border_style = node->border_style;

    // Margins
    box->margin.top = std::max(0, node->margin[0]);
    box->margin.right = std::max(0, node->margin[1]);
    box->margin.bottom = std::max(0, node->margin[2]);
    box->margin.left = std::max(0, node->margin[3]);

    // Padding
    box->padding.top = std::max(0, node->padding[0]);
    box->padding.right = std::max(0, node->padding[1]);
    box->padding.bottom = std::max(0, node->padding[2]);
    box->padding.left = std::max(0, node->padding[3]);

    // Borders
    box->border.top = std::max(0, node->border_width[0]);
    box->border.right = std::max(0, node->border_width[1]);
    box->border.bottom = std::max(0, node->border_width[2]);
    box->border.left = std::max(0, node->border_width[3]);
}

// ---- Helper: resolve width from DOMNode ----

static float resolve_width(DOMNode* node, float containing_width) {
    if (!node) return -1;
    if (node->width_pct >= 0) return containing_width * node->width_pct / 100.0f;
    if (node->width >= 0) return (float)node->width;
    return -1; // auto
}

static float resolve_height(DOMNode* node, float containing_height) {
    if (!node) return -1;
    if (node->height_pct >= 0 && containing_height > 0)
        return containing_height * node->height_pct / 100.0f;
    if (node->height >= 0) return (float)node->height;
    return -1; // auto
}

// ---- Clamp with min/max ----

static float clamp_size(float size, DOMNode* node, bool is_width) {
    if (!node) return size;
    float min_val = is_width ? (node->min_width >= 0 ? (float)node->min_width : 0)
                             : (node->min_height >= 0 ? (float)node->min_height : 0);
    float max_val = is_width ? (node->max_width >= 0 ? (float)node->max_width : 1e9f)
                             : (node->max_height >= 0 ? (float)node->max_height : 1e9f);
    if (size < min_val) size = min_val;
    if (size > max_val) size = max_val;
    return size;
}

// ---- Build layout tree from DOM ----

std::unique_ptr<LayoutBox> build_layout_tree(DOMNode* node, PangoContext* pango_ctx) {
    if (!node) return nullptr;

    LayoutBoxType dtype = display_type_for(node);
    if (dtype == LayoutBoxType::None) return nullptr;

    auto box = std::make_unique<LayoutBox>();
    box->type = dtype;
    box->dom_node = node;
    copy_style(box.get(), node);

    if (dtype == LayoutBoxType::Text) {
        box->text = node->text_content;
        return box;
    }

    // Skip script, style, head, template, noscript elements
    const auto& tag = node->tag_name;
    if (tag == "script" || tag == "style" || tag == "head" || tag == "template" ||
        tag == "noscript" || tag == "link" || tag == "meta" || tag == "base" ||
        tag == "title")
        return nullptr;

    // For replaced elements, we just note the type; painting handles the rest
    if (dtype == LayoutBoxType::Replaced) {
        // Get natural dimensions from attributes
        auto wit = node->attributes.find("width");
        auto hit = node->attributes.find("height");
        if (wit != node->attributes.end()) {
            try { box->natural_width = std::stoi(wit->second); } catch (...) {}
        }
        if (hit != node->attributes.end()) {
            try { box->natural_height = std::stoi(hit->second); } catch (...) {}
        }
        if (box->natural_width <= 0) box->natural_width = 300;
        if (box->natural_height <= 0) box->natural_height = 150;
        return box;
    }

    // Build children
    for (auto& child_node : node->children) {
        auto child_box = build_layout_tree(child_node.get(), pango_ctx);
        if (child_box) {
            child_box->parent = box.get();
            box->children.push_back(std::move(child_box));
        }
    }

    // If this is a block container with mixed block/inline children,
    // wrap consecutive inline children in anonymous block boxes
    if (is_block_level(dtype) && !box->children.empty()) {
        bool has_block = false, has_inline = false;
        for (auto& child : box->children) {
            if (is_block_level(child->type))
                has_block = true;
            else
                has_inline = true;
        }

        if (has_block && has_inline) {
            // Wrap runs of inline children in anonymous blocks
            std::vector<std::unique_ptr<LayoutBox>> new_children;
            std::vector<std::unique_ptr<LayoutBox>> inline_run;

            auto flush_inline = [&]() {
                if (inline_run.empty()) return;
                auto anon = std::make_unique<LayoutBox>();
                anon->type = LayoutBoxType::Anonymous;
                anon->parent = box.get();
                for (auto& ib : inline_run) {
                    ib->parent = anon.get();
                    anon->children.push_back(std::move(ib));
                }
                inline_run.clear();
                new_children.push_back(std::move(anon));
            };

            for (auto& child : box->children) {
                if (is_block_level(child->type)) {
                    flush_inline();
                    child->parent = box.get();
                    new_children.push_back(std::move(child));
                } else {
                    inline_run.push_back(std::move(child));
                }
            }
            flush_inline();
            box->children = std::move(new_children);
        }
    }

    return box;
}

// ---- Text measurement ----

static PangoLayout* create_pango_layout_for_text(PangoContext* pango_ctx, LayoutBox* box,
                                                    float max_width) {
    PangoLayout* layout = pango_layout_new(pango_ctx);

    // Build font description
    PangoFontDescription* fd = pango_font_description_new();

    // Font family - inherit from box or ancestors
    std::string family = box->font_family;
    if (family.empty()) {
        // Walk up to find inherited font
        LayoutBox* p = box->parent;
        while (p) {
            if (!p->font_family.empty()) { family = p->font_family; break; }
            p = p->parent;
        }
    }
    if (family.empty()) family = "sans-serif";
    pango_font_description_set_family(fd, family.c_str());

    // Font size
    int fs = box->font_size;
    if (fs <= 0) {
        LayoutBox* p = box->parent;
        while (p) {
            if (p->font_size > 0) { fs = p->font_size; break; }
            p = p->parent;
        }
    }
    if (fs <= 0) fs = 16;
    pango_font_description_set_size(fd, fs * PANGO_SCALE);

    // Font weight
    int fw = box->font_weight;
    if (fw <= 0) {
        LayoutBox* p = box->parent;
        while (p) {
            if (p->font_weight > 0) { fw = p->font_weight; break; }
            p = p->parent;
        }
    }
    if (fw <= 0) fw = 400;
    pango_font_description_set_weight(fd, (PangoWeight)fw);

    // Font style
    if (box->font_style > 0)
        pango_font_description_set_style(fd, (PangoStyle)box->font_style);

    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);

    // Text content - inherit from text node or box
    std::string text = box->text;

    // Apply text transforms from parent
    if (box->dom_node && box->dom_node->parent) {
        int transform = box->dom_node->parent->text_transform;
        if (transform < 0) {
            // Inherit
            DOMNode* p = box->dom_node->parent;
            while (p && p->text_transform < 0) p = p->parent;
            if (p) transform = p->text_transform;
        }
        if (transform == 1) { // uppercase
            std::string upper;
            upper.reserve(text.size());
            for (unsigned char c : text) upper += (char)toupper(c);
            text = upper;
        } else if (transform == 2) { // lowercase
            std::string lower;
            lower.reserve(text.size());
            for (unsigned char c : text) lower += (char)tolower(c);
            text = lower;
        }
    }

    // White space handling
    int ws = box->white_space;
    if (ws <= 0) {
        // Inherit
        LayoutBox* p = box->parent;
        while (p) {
            if (p->white_space > 0) { ws = p->white_space; break; }
            p = p->parent;
        }
    }

    if (ws == 0 || ws == 1) {
        // normal or nowrap: collapse whitespace
        std::string collapsed;
        bool in_ws = false;
        for (char c : text) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!in_ws) { collapsed += ' '; in_ws = true; }
            } else {
                collapsed += c;
                in_ws = false;
            }
        }
        text = collapsed;
    }

    pango_layout_set_text(layout, text.c_str(), -1);

    // Set width for wrapping
    if (ws != 1 && ws != 2) { // not nowrap, not pre
        if (max_width > 0)
            pango_layout_set_width(layout, (int)(max_width * PANGO_SCALE));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    }

    return layout;
}

// ---- Resolve inherited color ----

static std::string resolve_color(LayoutBox* box) {
    LayoutBox* b = box;
    while (b) {
        if (!b->color.empty()) return b->color;
        b = b->parent;
    }
    return "black";
}

static std::string resolve_bg_color(LayoutBox* box) {
    return box->bg_color;
}

static int resolve_text_align(LayoutBox* box) {
    LayoutBox* b = box;
    while (b) {
        if (b->text_align >= 0 && b->dom_node && b->dom_node->text_align_computed >= 0)
            return b->text_align;
        b = b->parent;
    }
    return 0;
}

// ---- Intrinsic width measurement ----

static float measure_intrinsic_width(LayoutBox* box, PangoContext* pango_ctx);

// ---- Block Formatting Context layout ----

static void layout_block(LayoutBox* box, float containing_width, float containing_height,
                          PangoContext* pango_ctx);
static void layout_inline(LayoutBox* box, float containing_width, PangoContext* pango_ctx);
static void layout_flex(LayoutBox* box, float containing_width, float containing_height,
                         PangoContext* pango_ctx);

// Measure the max-content (intrinsic) width of a box
// This is the minimum width at which the box can render without overflow
static float measure_intrinsic_width(LayoutBox* box, PangoContext* pango_ctx) {
    if (!box) return 0;

    // Explicit CSS width wins
    if (box->dom_node) {
        float w = resolve_width(box->dom_node, 0);
        if (w >= 0) {
            if (box->dom_node->box_sizing == 1) {
                // border-box: w already includes padding+border
                return w;
            }
            return w + box->padding.horizontal() + box->border.horizontal();
        }
    }

    float max_child_w = 0;

    // Check if all children are inline
    bool all_inline = true;
    for (auto& child : box->children) {
        if (is_block_level(child->type) || child->type == LayoutBoxType::Replaced) {
            all_inline = false;
            break;
        }
    }

    if (all_inline && !box->children.empty()) {
        // Gather text and measure with no wrapping
        std::string full_text;
        std::function<void(LayoutBox*)> gather = [&](LayoutBox* child) {
            if (child->type == LayoutBoxType::Text) {
                std::string text = child->text;
                // Collapse whitespace
                std::string collapsed;
                bool in_ws = false;
                for (char c : text) {
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                        if (!in_ws) { collapsed += ' '; in_ws = true; }
                    } else {
                        collapsed += c;
                        in_ws = false;
                    }
                }
                full_text += collapsed;
            } else if (child->type == LayoutBoxType::Inline) {
                for (auto& gc : child->children) gather(gc.get());
            }
        };
        for (auto& child : box->children) gather(child.get());

        // Trim
        while (!full_text.empty() && full_text.front() == ' ') full_text.erase(full_text.begin());
        while (!full_text.empty() && full_text.back() == ' ') full_text.pop_back();

        if (!full_text.empty()) {
            PangoLayout* pl = pango_layout_new(pango_ctx);
            PangoFontDescription* fd = pango_font_description_new();
            std::string family = box->font_family.empty() ? "sans-serif" : box->font_family;
            pango_font_description_set_family(fd, family.c_str());
            int fs = box->font_size > 0 ? box->font_size : 16;
            pango_font_description_set_size(fd, fs * PANGO_SCALE);
            int fw = box->font_weight > 0 ? box->font_weight : 400;
            pango_font_description_set_weight(fd, (PangoWeight)fw);
            pango_layout_set_font_description(pl, fd);
            pango_font_description_free(fd);
            pango_layout_set_text(pl, full_text.c_str(), -1);
            // No width constraint - measure natural width
            int pw, ph;
            pango_layout_get_pixel_size(pl, &pw, &ph);
            max_child_w = (float)pw;
            g_object_unref(pl);
        }
    } else {
        // Block children: max of their intrinsic widths
        for (auto& child : box->children) {
            float cw = measure_intrinsic_width(child.get(), pango_ctx);
            if (cw > max_child_w) max_child_w = cw;
        }
    }

    return max_child_w + box->padding.horizontal() + box->border.horizontal();
}

static void layout_box(LayoutBox* box, float containing_width, float containing_height,
                        PangoContext* pango_ctx) {
    switch (box->type) {
        case LayoutBoxType::Block:
        case LayoutBoxType::Anonymous:
            layout_block(box, containing_width, containing_height, pango_ctx);
            break;
        case LayoutBoxType::InlineBlock:
            layout_block(box, containing_width, containing_height, pango_ctx);
            break;
        case LayoutBoxType::Flex:
            layout_flex(box, containing_width, containing_height, pango_ctx);
            break;
        case LayoutBoxType::Text:
            // Text nodes are laid out by their parent's inline context
            break;
        case LayoutBoxType::Replaced:
            // Replaced elements get their natural size or CSS size
            {
                float w = -1, h = -1;
                if (box->dom_node) {
                    w = resolve_width(box->dom_node, containing_width);
                    h = resolve_height(box->dom_node, containing_height);
                }
                if (w < 0) w = (float)box->natural_width;
                if (h < 0) h = (float)box->natural_height;
                w = clamp_size(w, box->dom_node, true);
                h = clamp_size(h, box->dom_node, false);

                // Handle box-sizing
                if (box->dom_node && box->dom_node->box_sizing == 1) {
                    w -= box->padding.horizontal() + box->border.horizontal();
                    h -= box->padding.vertical() + box->border.vertical();
                    if (w < 0) w = 0;
                    if (h < 0) h = 0;
                }

                box->content_rect.w = w;
                box->content_rect.h = h;
            }
            break;
        default:
            break;
    }
}

// ---- Layout a block container ----

static void layout_block(LayoutBox* box, float containing_width, float containing_height,
                          PangoContext* pango_ctx) {
    DOMNode* node = box->dom_node;

    // Resolve content width
    float content_width = -1;
    if (node) {
        content_width = resolve_width(node, containing_width);
        if (content_width >= 0) {
            // box-sizing: border-box
            if (node->box_sizing == 1) {
                content_width -= box->padding.horizontal() + box->border.horizontal();
                if (content_width < 0) content_width = 0;
            }
            content_width = clamp_size(content_width, node, true);
        }
    }

    // If auto width, use containing width minus margin+padding+border
    if (content_width < 0) {
        content_width = containing_width - box->margin.horizontal()
                        - box->padding.horizontal() - box->border.horizontal();
        if (content_width < 0) content_width = 0;
    }

    box->content_rect.w = content_width;

    // Handle halign_center (margin: auto)
    if (node && node->halign_center) {
        float total_w = content_width + box->padding.horizontal() + box->border.horizontal();
        float avail = containing_width;
        float leftover = avail - total_w - box->margin.left - box->margin.right;
        if (leftover > 0) {
            box->margin.left = leftover / 2;
            box->margin.right = leftover / 2;
        }
    }

    // Check if all children are inline (or text)
    bool all_inline = true;
    for (auto& child : box->children) {
        if (is_block_level(child->type) || child->type == LayoutBoxType::Replaced) {
            all_inline = false;
            break;
        }
    }

    if (all_inline && !box->children.empty()) {
        // Inline formatting context
        layout_inline(box, content_width, pango_ctx);
    } else {
        // Block formatting context: stack children vertically
        float y_cursor = 0;
        float prev_margin_bottom = 0;

        for (auto& child : box->children) {
            layout_box(child.get(), content_width, containing_height, pango_ctx);

            // Margin collapsing (simplified: collapse adjacent margins)
            float top_margin = child->margin.top;
            float collapsed = std::max(prev_margin_bottom, top_margin);
            float effective_gap = collapsed;

            if (y_cursor == 0 && prev_margin_bottom == 0) {
                effective_gap = top_margin;
            } else {
                effective_gap = collapsed;
                y_cursor -= prev_margin_bottom; // remove previously added bottom margin
            }

            float child_x = child->margin.left + child->border.left + child->padding.left;
            float child_y = y_cursor + effective_gap + child->border.top + child->padding.top;

            child->content_rect.x = child_x;
            child->content_rect.y = child_y;

            y_cursor = child_y + child->content_rect.h + child->padding.bottom +
                       child->border.bottom;
            prev_margin_bottom = child->margin.bottom;
        }

        y_cursor += prev_margin_bottom;

        // Resolve content height
        float content_height = resolve_height(node, containing_height);
        if (content_height >= 0) {
            if (node && node->box_sizing == 1) {
                content_height -= box->padding.vertical() + box->border.vertical();
                if (content_height < 0) content_height = 0;
            }
            content_height = clamp_size(content_height, node, false);
            box->content_rect.h = content_height;
        } else {
            box->content_rect.h = y_cursor;
        }
    }

    // Store scroll dimensions
    box->scroll_width = box->content_rect.w;
    box->scroll_height = box->content_rect.h;
}

// ---- Inline formatting context ----

static void layout_inline(LayoutBox* box, float containing_width, PangoContext* pango_ctx) {
    // Gather all text from inline children
    // For Phase 1, we concatenate all text and render as a single PangoLayout
    // This handles wrapping correctly across text runs

    std::string full_text;
    struct TextRun {
        DOMNode* node;
        int start;
        int length;
    };
    std::vector<TextRun> runs;

    std::function<void(LayoutBox*)> gather_text = [&](LayoutBox* child) {
        if (child->type == LayoutBoxType::Text) {
            std::string text = child->text;
            // Collapse whitespace
            int ws = child->white_space;
            if (ws <= 0) {
                LayoutBox* p = child->parent;
                while (p) {
                    if (p->white_space > 0) { ws = p->white_space; break; }
                    p = p->parent;
                }
            }
            if (ws == 0 || ws == 1) {
                std::string collapsed;
                bool in_ws = false;
                for (char c : text) {
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                        if (!in_ws) { collapsed += ' '; in_ws = true; }
                    } else {
                        collapsed += c;
                        in_ws = false;
                    }
                }
                text = collapsed;
            }

            // Ensure whitespace between adjacent text runs
            if (!full_text.empty() && !text.empty() &&
                full_text.back() != ' ' && text.front() != ' ') {
                full_text += ' ';
            }

            int start = (int)full_text.size();
            full_text += text;
            runs.push_back({child->dom_node, start, (int)text.size()});
        } else if (child->type == LayoutBoxType::Inline) {
            for (auto& grandchild : child->children)
                gather_text(grandchild.get());
        } else if (child->type == LayoutBoxType::Replaced || child->type == LayoutBoxType::InlineBlock) {
            // TODO: handle inline replaced elements
        }
    };

    for (auto& child : box->children)
        gather_text(child.get());

    if (full_text.empty()) {
        box->content_rect.h = 0;
        return;
    }

    // Trim leading/trailing whitespace
    while (!full_text.empty() && full_text.front() == ' ') full_text.erase(full_text.begin());
    while (!full_text.empty() && full_text.back() == ' ') full_text.pop_back();

    if (full_text.empty()) {
        box->content_rect.h = 0;
        return;
    }

    // Create PangoLayout for the entire inline content
    PangoLayout* layout = pango_layout_new(pango_ctx);
    PangoFontDescription* fd = pango_font_description_new();

    std::string family = box->font_family;
    if (family.empty()) {
        LayoutBox* p = box->parent;
        while (p) {
            if (!p->font_family.empty()) { family = p->font_family; break; }
            p = p->parent;
        }
    }
    if (family.empty()) family = "sans-serif";
    pango_font_description_set_family(fd, family.c_str());

    int fs = box->font_size > 0 ? box->font_size : 16;
    pango_font_description_set_size(fd, fs * PANGO_SCALE);

    int fw = box->font_weight > 0 ? box->font_weight : 400;
    pango_font_description_set_weight(fd, (PangoWeight)fw);

    if (box->font_style > 0)
        pango_font_description_set_style(fd, (PangoStyle)box->font_style);

    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);

    pango_layout_set_text(layout, full_text.c_str(), -1);

    if (containing_width > 0) {
        pango_layout_set_width(layout, (int)(containing_width * PANGO_SCALE));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    }

    // Text alignment
    int ta = box->text_align;
    if (ta <= 0) {
        LayoutBox* p = box->parent;
        while (p) {
            if (p->dom_node && p->dom_node->text_align_computed >= 0) {
                ta = p->dom_node->text_align_computed;
                break;
            }
            p = p->parent;
        }
    }
    switch (ta) {
        case 1: pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER); break;
        case 2: pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT); break;
        case 3: pango_layout_set_justify(layout, TRUE); break;
        default: pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT); break;
    }

    // Apply line spacing
    float lh = box->line_height_factor;
    if (lh <= 0) lh = 1.2;
    int spacing = (int)((lh - 1.0) * fs * PANGO_SCALE);
    if (spacing > 0) pango_layout_set_spacing(layout, spacing);

    // Measure
    int pw, ph;
    pango_layout_get_pixel_size(layout, &pw, &ph);

    // Store the pango layout on the box for painting
    if (box->pango_layout) g_object_unref(box->pango_layout);
    box->pango_layout = layout;
    box->text = full_text;

    // Build line boxes from PangoLayout lines
    box->line_boxes.clear();
    PangoLayoutIter* iter = pango_layout_get_iter(layout);
    do {
        PangoRectangle ink, logical;
        pango_layout_iter_get_line_extents(iter, &ink, &logical);
        int baseline = pango_layout_iter_get_baseline(iter);

        LineBox lb;
        lb.x = (float)logical.x / PANGO_SCALE;
        lb.y = (float)logical.y / PANGO_SCALE;
        lb.width = (float)logical.width / PANGO_SCALE;
        lb.height = (float)logical.height / PANGO_SCALE;
        lb.baseline = (float)baseline / PANGO_SCALE - lb.y;
        box->line_boxes.push_back(lb);
    } while (pango_layout_iter_next_line(iter));
    pango_layout_iter_free(iter);

    box->content_rect.h = (float)ph;
}

// ---- Flex layout (basic Phase 1 - improved in Phase 4) ----

static void layout_flex(LayoutBox* box, float containing_width, float containing_height,
                         PangoContext* pango_ctx) {
    DOMNode* node = box->dom_node;

    // Resolve content width
    float content_width = -1;
    if (node) {
        content_width = resolve_width(node, containing_width);
        if (content_width >= 0 && node->box_sizing == 1) {
            content_width -= box->padding.horizontal() + box->border.horizontal();
            if (content_width < 0) content_width = 0;
        }
    }
    if (content_width < 0) {
        content_width = containing_width - box->margin.horizontal()
                        - box->padding.horizontal() - box->border.horizontal();
        if (content_width < 0) content_width = 0;
    }
    box->content_rect.w = content_width;

    if (node && node->halign_center) {
        float total_w = content_width + box->padding.horizontal() + box->border.horizontal();
        float leftover = containing_width - total_w - box->margin.left - box->margin.right;
        if (leftover > 0) {
            box->margin.left = leftover / 2;
            box->margin.right = leftover / 2;
        }
    }

    // Determine flex direction
    int dir = node ? node->flex_direction : 0;
    bool is_row = (dir == 0 || dir == 2); // row or row-reverse
    bool is_reverse = (dir == 2 || dir == 3);
    int gap = node ? node->gap : 0;

    // Layout each flex item with content-based sizing for row direction
    float total_main = 0;
    for (auto& child : box->children) {
        if (is_row) {
            // For row flex: determine item width from explicit width or content
            float item_w = -1;
            if (child->dom_node) {
                item_w = resolve_width(child->dom_node, content_width);
                if (item_w >= 0 && child->dom_node->box_sizing == 1) {
                    item_w -= child->padding.horizontal() + child->border.horizontal();
                    if (item_w < 0) item_w = 0;
                }
            }
            if (item_w < 0) {
                // No explicit width: measure intrinsic content width
                float intrinsic = measure_intrinsic_width(child.get(), pango_ctx);
                // intrinsic includes padding+border, we need content width only
                item_w = intrinsic - child->padding.horizontal() - child->border.horizontal();
                if (item_w < 0) item_w = 0;
            }
            // Re-layout with correct width
            child->content_rect.w = item_w;
            layout_box(child.get(), item_w + child->padding.horizontal() + child->border.horizontal(),
                       containing_height, pango_ctx);
        } else {
            layout_box(child.get(), content_width, containing_height, pango_ctx);
        }

        float child_main = is_row
            ? (child->content_rect.w + child->margin.horizontal() +
               child->padding.horizontal() + child->border.horizontal())
            : (child->content_rect.h + child->margin.vertical() +
               child->padding.vertical() + child->border.vertical());
        total_main += child_main;
    }

    int num_gaps = box->children.empty() ? 0 : (int)box->children.size() - 1;
    total_main += num_gaps * gap;

    // Distribute space
    float main_size = is_row ? content_width : containing_height;
    float remaining = main_size - total_main;

    int jc = node ? node->justify_content : 0;
    float main_start = 0;
    float item_gap = (float)gap;

    if (jc == 1) { // flex-end
        main_start = remaining;
    } else if (jc == 2) { // center
        main_start = remaining / 2;
    } else if (jc == 3 && box->children.size() > 1) { // space-between
        item_gap = remaining / ((float)box->children.size() - 1) + gap;
    } else if (jc == 4 && !box->children.empty()) { // space-around
        float sp = remaining / (float)box->children.size();
        main_start = sp / 2;
        item_gap = sp + gap;
    } else if (jc == 5 && !box->children.empty()) { // space-evenly
        float sp = remaining / ((float)box->children.size() + 1);
        main_start = sp;
        item_gap = sp + gap;
    }

    float cursor = main_start;
    float max_cross = 0;

    auto items = std::vector<LayoutBox*>();
    for (auto& child : box->children)
        items.push_back(child.get());
    if (is_reverse)
        std::reverse(items.begin(), items.end());

    for (auto* child : items) {
        float child_main_size = is_row
            ? (child->content_rect.w + child->padding.horizontal() + child->border.horizontal())
            : (child->content_rect.h + child->padding.vertical() + child->border.vertical());
        float child_cross_size = is_row
            ? (child->content_rect.h + child->padding.vertical() + child->border.vertical())
            : (child->content_rect.w + child->padding.horizontal() + child->border.horizontal());

        if (is_row) {
            child->content_rect.x = cursor + child->margin.left + child->border.left + child->padding.left;
            child->content_rect.y = child->margin.top + child->border.top + child->padding.top;
        } else {
            child->content_rect.x = child->margin.left + child->border.left + child->padding.left;
            child->content_rect.y = cursor + child->margin.top + child->border.top + child->padding.top;
        }

        cursor += child_main_size + child->margin.horizontal() * (is_row ? 1 : 0)
                + child->margin.vertical() * (is_row ? 0 : 1) + item_gap;
        if (child_cross_size + child->margin.vertical() * (is_row ? 1 : 0)
            + child->margin.horizontal() * (is_row ? 0 : 1) > max_cross)
            max_cross = child_cross_size + child->margin.vertical() * (is_row ? 1 : 0)
                      + child->margin.horizontal() * (is_row ? 0 : 1);
    }

    // Resolve content height
    float content_height = resolve_height(node, containing_height);
    if (content_height >= 0) {
        if (node && node->box_sizing == 1) {
            content_height -= box->padding.vertical() + box->border.vertical();
            if (content_height < 0) content_height = 0;
        }
        box->content_rect.h = content_height;
    } else {
        box->content_rect.h = is_row ? max_cross : cursor;
    }

    // Align items on cross axis
    int ai = node ? node->align_items : 0;
    float cross_size = is_row ? box->content_rect.h : box->content_rect.w;
    for (auto* child : items) {
        float child_cross = is_row
            ? (child->content_rect.h + child->padding.vertical() + child->border.vertical()
               + child->margin.vertical())
            : (child->content_rect.w + child->padding.horizontal() + child->border.horizontal()
               + child->margin.horizontal());

        if (ai == 0) { // stretch
            if (is_row && child->content_rect.h < cross_size - child->padding.vertical()
                - child->border.vertical() - child->margin.vertical()) {
                // Don't stretch if explicit height
                if (child->dom_node && child->dom_node->height < 0 && child->dom_node->height_pct < 0)
                    child->content_rect.h = cross_size - child->padding.vertical()
                                           - child->border.vertical() - child->margin.vertical();
            }
        } else if (ai == 2) { // flex-end
            float offset = cross_size - child_cross;
            if (is_row) child->content_rect.y += offset;
            else child->content_rect.x += offset;
        } else if (ai == 3) { // center
            float offset = (cross_size - child_cross) / 2;
            if (is_row) child->content_rect.y += offset;
            else child->content_rect.x += offset;
        }
        // flex-start (1) is default - no adjustment needed
    }

    box->scroll_width = box->content_rect.w;
    box->scroll_height = box->content_rect.h;
}

// ---- Top-level layout ----

void perform_layout(LayoutBox* root, float viewport_width, float viewport_height,
                    PangoContext* pango_ctx) {
    if (!root) return;

    // Root box keeps its margins and gets positioned accordingly
    root->content_rect.x = root->margin.left + root->padding.left + root->border.left;
    root->content_rect.y = root->margin.top + root->padding.top + root->border.top;

    // Layout with viewport width minus root margins
    float avail_w = viewport_width - root->margin.horizontal();
    layout_box(root, avail_w, viewport_height, pango_ctx);

    // Compute absolute positions
    root->compute_abs_positions(0, 0);
}

float get_content_height(LayoutBox* root) {
    if (!root) return 0;
    return root->content_rect.h + root->padding.vertical() + root->border.vertical()
           + root->margin.vertical();
}
