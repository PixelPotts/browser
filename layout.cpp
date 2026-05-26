// layout.cpp - Layout tree construction and block formatting context
#include "layout.h"
#include "paint.h"
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

    // Non-rendered elements
    {
        const auto& t = node->tag_name;
        if (t == "head" || t == "title" || t == "style" || t == "script" ||
            t == "meta" || t == "link" || t == "noscript" || t == "template" ||
            t == "base" || t == "html")
            return LayoutBoxType::None;
    }

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

    // Line break
    if (tag == "br")
        return LayoutBoxType::Inline;

    // Replaced elements
    if (tag == "img" || tag == "canvas" || tag == "video" || tag == "svg" ||
        tag == "iframe" || tag == "object" || tag == "embed")
        return LayoutBoxType::Replaced;

    // Table elements: td/th are inline-like (for table cell wrapping)
    // but we handle them specially in layout_block for <tr> parents

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
    // UA default: links are blue
    if (box->color.empty() && node->tag_name == "a" &&
        node->attributes.count("href") && !node->attributes.at("href").empty()) {
        box->color = "#0000ee";
    }
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
    box->text_decoration = node->text_decoration > 0 ? node->text_decoration : 0;

    // Margins
    box->margin.top = std::max(0, node->margin[0]);
    box->margin.right = std::max(0, node->margin[1]);
    box->margin.bottom = std::max(0, node->margin[2]);
    box->margin.left = std::max(0, node->margin[3]);

    // UA defaults for lists
    const auto& tag = node->tag_name;
    if ((tag == "ul" || tag == "ol") && node->padding[3] <= 0)
        box->padding.left = 40;
    if (tag == "li" && box->margin.bottom == 0)
        box->margin.bottom = 4;

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
        auto hit_it = node->attributes.find("height");
        if (wit != node->attributes.end()) {
            try { box->natural_width = std::stoi(wit->second); } catch (...) {}
        }
        if (hit_it != node->attributes.end()) {
            try { box->natural_height = std::stoi(hit_it->second); } catch (...) {}
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

    // Form controls: synthesize text box from value/placeholder for rendering
    if (box->children.empty() && (tag == "input" || tag == "textarea")) {
        std::string display_text;
        auto val_it = node->attributes.find("value");
        if (val_it != node->attributes.end() && !val_it->second.empty()) {
            display_text = val_it->second;
        } else {
            auto ph_it = node->attributes.find("placeholder");
            if (ph_it != node->attributes.end()) display_text = ph_it->second;
        }
        if (!display_text.empty()) {
            auto text_box = std::make_unique<LayoutBox>();
            text_box->type = LayoutBoxType::Text;
            text_box->dom_node = node;
            text_box->text = display_text;
            text_box->parent = box.get();
            box->children.push_back(std::move(text_box));
        }
    }

    // Block-in-inline: if an Inline element contains Block children, promote to Block
    if (!is_block_level(box->type) && !box->children.empty()) {
        for (auto& child : box->children) {
            if (is_block_level(child->type)) {
                box->type = LayoutBoxType::Block;
                break;
            }
        }
    }

    // If this is a block container with mixed block/inline children,
    // wrap consecutive inline children in anonymous block boxes
    if (is_block_level(box->type) && !box->children.empty()) {
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

    // Text decoration (underline, strikethrough)
    {
        int td = box->text_decoration;
        if (td <= 0 && box->parent) td = box->parent->text_decoration;
        if (td > 0) {
            PangoAttrList* al = pango_layout_get_attributes(layout);
            if (!al) { al = pango_attr_list_new(); pango_layout_set_attributes(layout, al); pango_attr_list_unref(al); al = pango_layout_get_attributes(layout); }
            if (td & 1) pango_attr_list_insert(al, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
            if (td & 4) pango_attr_list_insert(al, pango_attr_strikethrough_new(TRUE));
        }
    }

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

// max-content: minimum width for no overflow (natural/preferred width)
static float measure_intrinsic_width(LayoutBox* box, PangoContext* pango_ctx);

// min-content: narrowest possible width without breaking words
static float measure_min_content_width(LayoutBox* box, PangoContext* pango_ctx);

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

// min-content width: narrowest width by wrapping text at every opportunity
static float measure_min_content_width(LayoutBox* box, PangoContext* pango_ctx) {
    if (!box) return 0;

    // Explicit CSS width wins
    if (box->dom_node) {
        float w = resolve_width(box->dom_node, 0);
        if (w >= 0) {
            if (box->dom_node->box_sizing == 1) return w;
            return w + box->padding.horizontal() + box->border.horizontal();
        }
    }

    float max_word_w = 0;

    bool all_inline = true;
    for (auto& child : box->children) {
        if (is_block_level(child->type) || child->type == LayoutBoxType::Replaced) {
            all_inline = false;
            break;
        }
    }

    if (all_inline && !box->children.empty()) {
        // Find the widest single word
        std::string full_text;
        std::function<void(LayoutBox*)> gather = [&](LayoutBox* child) {
            if (child->type == LayoutBoxType::Text) {
                std::string text = child->text;
                std::string collapsed;
                bool in_ws = false;
                for (char c : text) {
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                        if (!in_ws) { collapsed += ' '; in_ws = true; }
                    } else { collapsed += c; in_ws = false; }
                }
                full_text += collapsed;
            } else if (child->type == LayoutBoxType::Inline) {
                for (auto& gc : child->children) gather(gc.get());
            }
        };
        for (auto& child : box->children) gather(child.get());

        // Measure with WRAP_WORD at width=1 (forces wrapping at every word)
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
            pango_layout_set_width(pl, 1); // force maximum wrapping
            pango_layout_set_wrap(pl, PANGO_WRAP_WORD);
            int pw, ph;
            pango_layout_get_pixel_size(pl, &pw, &ph);
            max_word_w = (float)pw;
            g_object_unref(pl);
        }
    } else {
        for (auto& child : box->children) {
            float cw = measure_min_content_width(child.get(), pango_ctx);
            if (cw > max_word_w) max_word_w = cw;
        }
    }

    return max_word_w + box->padding.horizontal() + box->border.horizontal();
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
                bool w_explicit = false, h_explicit = false;
                if (box->dom_node) {
                    w = resolve_width(box->dom_node, containing_width);
                    h = resolve_height(box->dom_node, containing_height);
                    w_explicit = (w >= 0);
                    h_explicit = (h >= 0);
                }
                if (w < 0) w = (float)box->natural_width;
                if (h < 0) h = (float)box->natural_height;

                // Apply aspect-ratio when only one dimension is explicit
                float ar = box->dom_node ? box->dom_node->aspect_ratio : 0;
                if (ar <= 0 && box->natural_width > 0 && box->natural_height > 0) {
                    ar = (float)box->natural_width / (float)box->natural_height;
                }
                if (ar > 0) {
                    if (w_explicit && !h_explicit) {
                        h = w / ar;
                    } else if (h_explicit && !w_explicit) {
                        w = h * ar;
                    }
                }

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

// Layout table row: lay out td/th children horizontally
static void layout_table_row(LayoutBox* box, float containing_width, float containing_height,
                              PangoContext* pango_ctx) {
    // Count visible cells (skip empty td with no content)
    int num_cells = 0;
    for (auto& child : box->children) {
        if (child->dom_node && (child->dom_node->tag_name == "td" || child->dom_node->tag_name == "th"))
            num_cells++;
        else
            num_cells++; // count non-td children too
    }
    if (num_cells <= 0) num_cells = 1;

    float cell_width = containing_width / num_cells;
    float x_cursor = 0;
    float max_height = 0;

    for (auto& child : box->children) {
        // Check for colspan
        int colspan = 1;
        if (child->dom_node) {
            auto cit = child->dom_node->attributes.find("colspan");
            if (cit != child->dom_node->attributes.end()) {
                try { colspan = std::stoi(cit->second); } catch (...) {}
                if (colspan < 1) colspan = 1;
            }
        }

        float this_cell_width = cell_width * colspan;
        // Override child's width for table cell layout
        child->content_rect.w = this_cell_width - child->padding.horizontal() - child->border.horizontal();
        if (child->content_rect.w < 0) child->content_rect.w = 0;

        layout_box(child.get(), this_cell_width, containing_height, pango_ctx);

        child->content_rect.x = x_cursor + child->margin.left + child->border.left + child->padding.left;
        child->content_rect.y = child->margin.top + child->border.top + child->padding.top;

        float child_h = child->content_rect.h + child->padding.vertical() +
                         child->border.vertical() + child->margin.vertical();
        if (child_h > max_height) max_height = child_h;

        x_cursor += this_cell_width;
    }

    box->content_rect.w = containing_width;
    box->content_rect.h = max_height;
}

static void layout_block(LayoutBox* box, float containing_width, float containing_height,
                          PangoContext* pango_ctx) {
    DOMNode* node = box->dom_node;

    // Table row: lay out cells horizontally
    if (node && node->tag_name == "tr") {
        copy_style(box, node); // ensure margins/padding are set
        layout_table_row(box, containing_width, containing_height, pango_ctx);
        return;
    }

    // Resolve content width
    float content_width = -1;
    if (node) {
        // Check for intrinsic sizing keywords first
        if (node->width_sizing == 1) {
            // min-content
            float mc = measure_min_content_width(box, pango_ctx);
            content_width = mc - box->padding.horizontal() - box->border.horizontal();
            if (content_width < 0) content_width = 0;
        } else if (node->width_sizing == 2) {
            // max-content
            float mc = measure_intrinsic_width(box, pango_ctx);
            content_width = mc - box->padding.horizontal() - box->border.horizontal();
            if (content_width < 0) content_width = 0;
        } else if (node->width_sizing == 3) {
            // fit-content: clamp between min-content and max-content, shrink-to-fit
            float avail = containing_width - box->margin.horizontal()
                          - box->padding.horizontal() - box->border.horizontal();
            float max_c = measure_intrinsic_width(box, pango_ctx)
                          - box->padding.horizontal() - box->border.horizontal();
            float min_c = measure_min_content_width(box, pango_ctx)
                          - box->padding.horizontal() - box->border.horizontal();
            content_width = std::max(min_c, std::min(avail, max_c));
            if (content_width < 0) content_width = 0;
        } else {
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

    // Check if all children are inline (or text or inline-replaced)
    bool all_inline = true;
    bool has_replaced = false;
    for (auto& child : box->children) {
        if (is_block_level(child->type)) {
            all_inline = false;
            break;
        }
        if (child->type == LayoutBoxType::Replaced)
            has_replaced = true;
    }

    if (all_inline && !box->children.empty()) {
        if (has_replaced) {
            // Mixed inline + replaced: wrap consecutive inline runs in anonymous blocks
            // so the BFC can stack them vertically alongside replaced elements
            std::vector<std::unique_ptr<LayoutBox>> new_children;
            std::vector<std::unique_ptr<LayoutBox>> inline_run;

            auto flush_inline = [&]() {
                if (inline_run.empty()) return;
                auto anon = std::make_unique<LayoutBox>();
                anon->type = LayoutBoxType::Anonymous;
                anon->parent = box;
                anon->content_rect.w = content_width;
                // Inherit font properties
                anon->font_family = box->font_family;
                anon->font_size = box->font_size;
                anon->font_weight = box->font_weight;
                anon->font_style = box->font_style;
                anon->color = box->color;
                anon->text_align = box->text_align;
                anon->white_space = box->white_space;
                for (auto& c : inline_run) {
                    c->parent = anon.get();
                    anon->children.push_back(std::move(c));
                }
                inline_run.clear();
                new_children.push_back(std::move(anon));
            };

            for (auto& child : box->children) {
                if (child->type == LayoutBoxType::Replaced) {
                    flush_inline();
                    child->parent = box;
                    new_children.push_back(std::move(child));
                } else {
                    inline_run.push_back(std::move(child));
                }
            }
            flush_inline();
            box->children = std::move(new_children);

            // Now fall through to BFC to stack everything vertically
            all_inline = false;
        }

        if (all_inline) {
            // Pure inline formatting context (no replaced elements)
            layout_inline(box, content_width, pango_ctx);
        }
    }

    if (!all_inline) {
        // Block formatting context: stack children vertically with proper margin collapsing
        float y_cursor = 0;
        float prev_margin_bottom = 0;

        // Parent-child collapsing: if parent has no top border/padding, first child's
        // top margin collapses through parent (we handle by not adding it to y_cursor
        // and instead propagating to parent's margin)
        bool parent_blocks_top_collapse = (box->padding.top > 0 || box->border.top > 0);

        for (size_t ci = 0; ci < box->children.size(); ++ci) {
            auto& child = box->children[ci];
            layout_box(child.get(), content_width, containing_height, pango_ctx);

            float top_margin = child->margin.top;

            // Empty block collapsing: if child has no height, no padding, no border,
            // and no content, its top and bottom margins collapse together
            bool is_empty_block = (child->content_rect.h == 0 &&
                                   child->padding.top == 0 && child->padding.bottom == 0 &&
                                   child->border.top == 0 && child->border.bottom == 0 &&
                                   child->children.empty() && child->line_boxes.empty());
            if (is_empty_block) {
                // Collapse top+bottom margins of empty block into a single margin
                float combined = std::max(top_margin, child->margin.bottom);
                if (ci == 0 && !parent_blocks_top_collapse) {
                    // First child, collapses with parent top margin
                    box->margin.top = std::max(box->margin.top, combined);
                } else {
                    // Collapse with previous sibling's bottom margin
                    prev_margin_bottom = std::max(prev_margin_bottom, combined);
                }
                child->content_rect.x = child->margin.left + child->border.left + child->padding.left;
                child->content_rect.y = y_cursor;
                continue;
            }

            // Parent-child top margin collapsing
            if (ci == 0 && !parent_blocks_top_collapse) {
                // First child's top margin collapses with parent's top margin
                box->margin.top = std::max(box->margin.top, top_margin);
                // Don't add this margin to y_cursor
                float child_x = child->margin.left + child->border.left + child->padding.left;
                float child_y = child->border.top + child->padding.top;
                child->content_rect.x = child_x;
                child->content_rect.y = child_y;
                y_cursor = child_y + child->content_rect.h + child->padding.bottom +
                           child->border.bottom;
                prev_margin_bottom = child->margin.bottom;
                continue;
            }

            // Normal sibling margin collapsing
            float collapsed = std::max(prev_margin_bottom, top_margin);
            float effective_gap;

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

        // Parent-child bottom margin collapsing: if parent has no bottom border/padding
        // and height is auto, last child's bottom margin collapses with parent
        bool parent_blocks_bottom_collapse = (box->padding.bottom > 0 || box->border.bottom > 0);
        float auto_height = y_cursor;
        if (!parent_blocks_bottom_collapse && !box->children.empty()) {
            // Last child's bottom margin collapses with parent's bottom margin
            box->margin.bottom = std::max(box->margin.bottom, prev_margin_bottom);
            // Don't add prev_margin_bottom to height
        } else {
            auto_height += prev_margin_bottom;
        }

        // Resolve content height
        float content_height = resolve_height(node, containing_height);
        if (content_height >= 0) {
            if (node && node->box_sizing == 1) {
                content_height -= box->padding.vertical() + box->border.vertical();
                if (content_height < 0) content_height = 0;
            }
            content_height = clamp_size(content_height, node, false);
            box->content_rect.h = content_height;
        } else if (node && node->aspect_ratio > 0 && box->content_rect.w > 0) {
            // aspect-ratio: compute height from width when height is auto
            box->content_rect.h = box->content_rect.w / node->aspect_ratio;
        } else {
            box->content_rect.h = auto_height;
        }
        // Apply min-height / max-height clamp even when height is auto
        box->content_rect.h = clamp_size(box->content_rect.h, node, false);
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
            // Handle <br> as a newline
            if (child->dom_node && child->dom_node->tag_name == "br") {
                full_text += '\n';
            } else {
                for (auto& grandchild : child->children)
                    gather_text(grandchild.get());
            }
        } else if (child->type == LayoutBoxType::Replaced || child->type == LayoutBoxType::InlineBlock) {
            // Inline replaced elements: insert a newline to separate text
            full_text += '\n';
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

    // Apply per-run text decoration (underline for links, strikethrough, etc.)
    {
        PangoAttrList* al = pango_attr_list_new();
        bool has_attrs = false;
        for (auto& run : runs) {
            if (!run.node || !run.node->parent) continue;
            DOMNode* elem = run.node->parent;
            int td = elem->text_decoration;
            // Check for link underline
            if (td <= 0 && elem->tag_name == "a" &&
                elem->attributes.count("href") && !elem->attributes.at("href").empty())
                td = 1; // underline by default
            if (td > 0) {
                has_attrs = true;
                if (td & 1) {
                    PangoAttribute* attr = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
                    attr->start_index = run.start;
                    attr->end_index = run.start + run.length;
                    pango_attr_list_insert(al, attr);
                }
                if (td & 4) {
                    PangoAttribute* attr = pango_attr_strikethrough_new(TRUE);
                    attr->start_index = run.start;
                    attr->end_index = run.start + run.length;
                    pango_attr_list_insert(al, attr);
                }
            }
            // Apply per-run color for inline elements with explicit color (e.g. links)
            if (!elem->color_computed.empty()) {
                has_attrs = true;
                CairoColor cc = parse_css_color(elem->color_computed);
                PangoAttribute* attr = pango_attr_foreground_new(
                    (guint16)(cc.r * 65535), (guint16)(cc.g * 65535), (guint16)(cc.b * 65535));
                attr->start_index = run.start;
                attr->end_index = run.start + run.length;
                pango_attr_list_insert(al, attr);
            } else if (elem->tag_name == "a" &&
                       elem->attributes.count("href") && !elem->attributes.at("href").empty()) {
                // Default link color
                has_attrs = true;
                PangoAttribute* attr = pango_attr_foreground_new(0, 0, 0xeeee);
                attr->start_index = run.start;
                attr->end_index = run.start + run.length;
                pango_attr_list_insert(al, attr);
            }
        }
        if (has_attrs)
            pango_layout_set_attributes(layout, al);
        pango_attr_list_unref(al);
    }

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

    // Store text run mapping for hit testing
    box->text_runs.clear();
    for (auto& r : runs) {
        box->text_runs.push_back({r.node, r.start, r.length});
    }

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

// ---- Flex layout with flex-grow/shrink/basis ----

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

    // Phase 1: Determine base sizes for each flex item
    struct FlexItem {
        LayoutBox* box;
        float base_size;       // content size from flex-basis or intrinsic
        float flex_grow;
        float flex_shrink;
        float outer_main;      // base_size + margin + padding + border on main axis
    };
    std::vector<FlexItem> flex_items;

    for (auto& child : box->children) {
        FlexItem fi;
        fi.box = child.get();
        fi.flex_grow = child->dom_node ? child->dom_node->flex_grow : 0.0f;
        fi.flex_shrink = child->dom_node ? child->dom_node->flex_shrink : 1.0f;

        float base = -1;
        // Check flex-basis first
        if (child->dom_node && child->dom_node->flex_basis >= 0) {
            base = (float)child->dom_node->flex_basis;
            if (child->dom_node->box_sizing == 1) {
                base -= child->padding.horizontal() + child->border.horizontal();
                if (base < 0) base = 0;
            }
        }
        // If basis is auto, check explicit width/height
        if (base < 0 && child->dom_node) {
            if (is_row) {
                base = resolve_width(child->dom_node, content_width);
                if (base >= 0 && child->dom_node->box_sizing == 1) {
                    base -= child->padding.horizontal() + child->border.horizontal();
                    if (base < 0) base = 0;
                }
            } else {
                base = resolve_height(child->dom_node, containing_height);
                if (base >= 0 && child->dom_node->box_sizing == 1) {
                    base -= child->padding.vertical() + child->border.vertical();
                    if (base < 0) base = 0;
                }
            }
        }
        // If still auto, measure intrinsic size
        if (base < 0) {
            if (is_row) {
                float intrinsic = measure_intrinsic_width(child.get(), pango_ctx);
                base = intrinsic - child->padding.horizontal() - child->border.horizontal();
                if (base < 0) base = 0;
            } else {
                // For column: do initial layout to get content height
                layout_box(child.get(), content_width, containing_height, pango_ctx);
                base = child->content_rect.h;
            }
        }

        fi.base_size = base;
        if (is_row) {
            fi.outer_main = base + child->margin.horizontal() + child->padding.horizontal() + child->border.horizontal();
        } else {
            fi.outer_main = base + child->margin.vertical() + child->padding.vertical() + child->border.vertical();
        }
        flex_items.push_back(fi);
    }

    // Phase 2: Calculate total base main size and distribute space
    int num_gaps = flex_items.empty() ? 0 : (int)flex_items.size() - 1;
    float total_main = 0;
    for (auto& fi : flex_items) total_main += fi.outer_main;
    total_main += num_gaps * gap;

    float main_size = is_row ? content_width : (containing_height > 0 ? containing_height : content_width);
    float remaining = main_size - total_main;

    // Grow or shrink items
    if (remaining > 0) {
        // Distribute extra space via flex-grow
        float total_grow = 0;
        for (auto& fi : flex_items) total_grow += fi.flex_grow;
        if (total_grow > 0) {
            for (auto& fi : flex_items) {
                float grow_share = remaining * (fi.flex_grow / total_grow);
                fi.base_size += grow_share;
                fi.outer_main += grow_share;
            }
            remaining = 0;
        }
    } else if (remaining < 0) {
        // Shrink items via flex-shrink (weighted by base_size * flex_shrink)
        float total_shrink_weighted = 0;
        for (auto& fi : flex_items)
            total_shrink_weighted += fi.flex_shrink * fi.base_size;
        if (total_shrink_weighted > 0) {
            float shrink_amount = -remaining;
            for (auto& fi : flex_items) {
                float weight = fi.flex_shrink * fi.base_size / total_shrink_weighted;
                float shrink = shrink_amount * weight;
                if (shrink > fi.base_size) shrink = fi.base_size; // don't go negative
                fi.base_size -= shrink;
                fi.outer_main -= shrink;
            }
            // Recalculate remaining after shrink
            total_main = 0;
            for (auto& fi : flex_items) total_main += fi.outer_main;
            total_main += num_gaps * gap;
            remaining = main_size - total_main;
        }
    }

    // Phase 3: Layout each item with its resolved size
    for (size_t i = 0; i < flex_items.size(); ++i) {
        auto& fi = flex_items[i];
        auto& child = box->children[i];
        if (is_row) {
            child->content_rect.w = fi.base_size;
            layout_box(child.get(), fi.base_size + child->padding.horizontal() + child->border.horizontal(),
                       containing_height, pango_ctx);
            // Ensure content_rect.w matches resolved size (layout_box may change it)
            child->content_rect.w = fi.base_size;
        } else {
            layout_box(child.get(), content_width, containing_height, pango_ctx);
            child->content_rect.h = fi.base_size;
        }
    }

    // Phase 4: Position items along main axis
    int jc = node ? node->justify_content : 0;
    float main_start = 0;
    float item_gap = (float)gap;

    if (remaining > 0) {
        // Only apply justify-content if there's remaining space (no flex-grow consumed it)
        if (jc == 1) { // flex-end
            main_start = remaining;
        } else if (jc == 2) { // center
            main_start = remaining / 2;
        } else if (jc == 3 && flex_items.size() > 1) { // space-between
            item_gap = remaining / ((float)flex_items.size() - 1) + gap;
        } else if (jc == 4 && !flex_items.empty()) { // space-around
            float sp = remaining / (float)flex_items.size();
            main_start = sp / 2;
            item_gap = sp + gap;
        } else if (jc == 5 && !flex_items.empty()) { // space-evenly
            float sp = remaining / ((float)flex_items.size() + 1);
            main_start = sp;
            item_gap = sp + gap;
        }
    }

    float cursor = main_start;
    float max_cross = 0;

    // Build ordered item list (respecting reverse)
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
    // Apply min-height / max-height clamp
    box->content_rect.h = clamp_size(box->content_rect.h, node, false);

    // Phase 5: Align items on cross axis
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
            } else if (!is_row && child->content_rect.w < cross_size - child->padding.horizontal()
                - child->border.horizontal() - child->margin.horizontal()) {
                if (child->dom_node && child->dom_node->width < 0 && child->dom_node->width_pct < 0)
                    child->content_rect.w = cross_size - child->padding.horizontal()
                                           - child->border.horizontal() - child->margin.horizontal();
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

    // Initial root position (margins may change during layout for margin:auto)
    root->content_rect.x = root->margin.left + root->padding.left + root->border.left;
    root->content_rect.y = root->margin.top + root->padding.top + root->border.top;

    // Layout with viewport width (layout_block will handle margin:auto centering)
    layout_box(root, viewport_width, viewport_height, pango_ctx);

    // Re-set root position after margin:auto may have updated margins
    root->content_rect.x = root->margin.left + root->padding.left + root->border.left;
    root->content_rect.y = root->margin.top + root->padding.top + root->border.top;

    // Compute absolute positions
    root->compute_abs_positions(0, 0);
}

float get_content_height(LayoutBox* root) {
    if (!root) return 0;
    return root->content_rect.h + root->padding.vertical() + root->border.vertical()
           + root->margin.vertical();
}
