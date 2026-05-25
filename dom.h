#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <algorithm>
#include <array>
#include <climits>

// Forward declarations
struct BoxModel;

// ---- DOMNode ----

struct DOMNode {
    uint32_t node_id = 0;
    enum NodeType { ELEMENT = 1, TEXT = 3, COMMENT = 8 } node_type = ELEMENT;

    std::string tag_name;       // lowercase, empty for text nodes
    std::string id;             // id attribute
    std::vector<std::string> class_list;
    std::unordered_map<std::string, std::string> attributes;
    std::string text_content;   // for TEXT nodes

    // Style
    std::unordered_map<std::string, std::string> style_props; // inline style
    std::string inline_style_raw;

    // Tree pointers
    DOMNode* parent = nullptr;
    std::vector<std::shared_ptr<DOMNode>> children;

    // Computed style fields (filled during parse/CSS cascade)
    int fw_computed  = -1;      // font-weight (Pango value, -1=inherit)
    int fi_computed  = -1;      // font-style: -1=inherit, 0=normal, 1=italic, 2=oblique
    int fs_computed  = 16;      // font-size in px
    double lh_computed = -1.0;  // line-height factor
    std::string color_computed; // text color
    int text_align_computed = -1; // -1=inherit, 0=left,1=center,2=right,3=justify
    int text_transform = -1;    // -1=inherit, 0=none, 1=uppercase, 2=lowercase, 3=capitalize
    std::string font_family;    // CSS font-family (inherited)
    std::string box_shadow;     // CSS box-shadow raw value
    double opacity = 1.0;       // CSS opacity (0.0–1.0)
    int overflow = -1;          // -1=inherit, 0=visible, 1=hidden, 2=scroll, 3=auto
    std::string href;           // for <a> elements

    // Text decoration & font properties
    int text_decoration = -1;          // -1=inherit, 0=none, bitmask: 1=underline, 2=overline, 4=line-through
    std::string text_decoration_color; // empty=currentColor
    int text_decoration_style = 0;     // 0=solid, 1=double, 2=dotted, 3=dashed, 4=wavy
    int letter_spacing = INT_MIN;      // Pango units, INT_MIN=inherit
    int word_spacing = INT_MIN;        // px, INT_MIN=inherit
    int font_variant = -1;            // -1=inherit, 0=normal, 1=small-caps
    int white_space = -1;             // -1=inherit, 0=normal, 1=nowrap, 2=pre, 3=pre-wrap, 4=pre-line
    int text_indent = INT_MIN;        // px, INT_MIN=inherit
    int text_overflow = 0;            // 0=clip, 1=ellipsis
    int font_stretch = -1;            // -1=inherit, PangoStretch enum
    std::string text_shadow;          // raw CSS value (store only)

    // Flex container properties
    int flex_direction = 0;     // 0=row, 1=column, 2=row-reverse, 3=column-reverse
    int justify_content = 0;    // 0=start, 1=end, 2=center, 3=between, 4=around, 5=evenly
    int align_items = 0;        // 0=stretch, 1=start, 2=end, 3=center
    int flex_wrap = 0;          // 0=nowrap, 1=wrap
    int gap = 0;                // gap in px

    // Flex item properties
    float flex_grow = 0.0f;     // flex-grow factor (default 0)
    float flex_shrink = 1.0f;   // flex-shrink factor (default 1)
    int flex_basis = -1;        // flex-basis in px (-1 = auto)

    // Positioning
    int position = 0;           // 0=static, 1=relative, 2=absolute, 3=fixed
    int pos_top = INT_MIN;      // top offset (INT_MIN = unset)
    int pos_left = INT_MIN;
    int pos_right = INT_MIN;
    int pos_bottom = INT_MIN;
    int z_index = 0;

    // Box model (for block elements)
    int margin[4]  = {0,0,0,0};
    int padding[4] = {0,0,0,0};
    int width      = -1;
    int max_width  = -1;
    int height     = -1;
    int min_width  = -1;
    int min_height = -1;
    int max_height = -1;
    int box_sizing = 0;    // 0=content-box, 1=border-box
    int overflow_x = -1;   // -1=inherit, 0=visible, 1=hidden, 2=scroll, 3=auto
    int overflow_y = -1;
    int width_pct  = -1;   // percentage width (0-100), -1=not percentage
    int height_pct = -1;   // percentage height (0-100), -1=not percentage
    int object_fit = 0;    // 0=fill, 1=contain, 2=cover, 3=scale-down, 4=none
    int border_width[4] = {0,0,0,0};
    int border_radius   = 0;
    std::string border_color;
    std::string border_style;
    bool halign_center = false;
    enum class Display : uint8_t { Inherit, Block, Inline, None, Flex, InlineBlock } display = Display::Inherit;
    enum class Float   : uint8_t { None, Left, Right } floatdir = Float::None;
    std::string bg_image;
    std::string bg_color;

    // Pseudo-element content (::before / ::after)
    std::string before_content; // empty = no ::before
    std::string after_content;  // empty = no ::after

    // Event listeners
    struct Listener {
        std::string type;
        uint32_t handler_id; // JS function reference id
    };
    std::vector<Listener> listeners;

    // Layout geometry (filled by GTK after render)
    int layout_x = 0, layout_y = 0, layout_w = 0, layout_h = 0;

    // Dirty flag for re-render
    bool dirty = false;
    bool is_body = false;

    // Interactive state
    bool is_hovered = false;
    bool is_focused = false;
    bool is_active = false; // mouse-down state

    void markDirty();

    // Helper: is this a block-level element?
    bool isBlock() const;

    // Get combined text content of all descendants
    std::string getTextContent() const;

    // Set text content (removes all children, adds single text node)
    void setTextContent(const std::string& text, uint32_t& next_id);

    // Get innerHTML as string
    std::string getInnerHTML() const;

    // Set innerHTML (parses basic HTML fragment)
    void setInnerHTML(const std::string& html, uint32_t& next_id,
                      std::unordered_map<uint32_t, DOMNode*>& node_map,
                      std::unordered_map<std::string, DOMNode*>& id_map);

    // Clone this node (and optionally children)
    std::shared_ptr<DOMNode> cloneNode(bool deep, uint32_t& next_id,
                                        std::unordered_map<uint32_t, DOMNode*>& node_map) const;

    // Add/remove class
    void addClass(const std::string& cls);
    void removeClass(const std::string& cls);
    bool hasClass(const std::string& cls) const;
    bool toggleClass(const std::string& cls);
};

// ---- Document ----

class Document {
public:
    Document();

    // Tree root
    std::shared_ptr<DOMNode> root;
    DOMNode* body = nullptr;
    DOMNode* head = nullptr;

    // ID maps
    std::unordered_map<uint32_t, DOMNode*> node_map;     // node_id -> node
    std::unordered_map<std::string, DOMNode*> id_map;    // element id attr -> node

    // Next node ID
    uint32_t next_id = 1;

    // Create elements
    std::shared_ptr<DOMNode> createElement(const std::string& tag);
    std::shared_ptr<DOMNode> createTextNode(const std::string& text);

    // Register a node in maps
    void registerNode(DOMNode* node);
    void unregisterNode(DOMNode* node);

    // Query
    DOMNode* getElementById(const std::string& id) const;
    DOMNode* querySelector(const std::string& selector) const;
    std::vector<DOMNode*> querySelectorAll(const std::string& selector) const;

    // Tree mutations
    void appendChild(DOMNode* parent, std::shared_ptr<DOMNode> child);
    void removeChild(DOMNode* parent, DOMNode* child);
    void insertBefore(DOMNode* parent, std::shared_ptr<DOMNode> newChild, DOMNode* refChild);

    // Mutation callback (triggers re-render)
    std::function<void()> on_mutated;

    // Mutation observer hook: (node_id, mutation_type)
    // mutation_type: "childList", "attributes", "characterData"
    std::function<void(uint32_t node_id, const std::string& type)> on_mutation;

    // Orphan nodes: created but not yet appended to tree
    // Keeps shared_ptr alive until appendChild or discard
    std::vector<std::shared_ptr<DOMNode>> orphans;

    // Script collection
    std::vector<std::string> scripts; // inline script contents
    std::vector<std::string> script_srcs; // external script URLs
    std::vector<std::string> script_types; // type attribute for each script_src

    // Script execution order (document order, interleaved)
    // Each entry: {is_external, index} where index into script_srcs or scripts
    std::vector<std::pair<bool, size_t>> script_order;

private:
    // CSS selector matching helpers
    bool selectorMatchesNode(const std::string& selector, DOMNode* node) const;
    void querySelectorHelper(DOMNode* node, const std::string& selector,
                              std::vector<DOMNode*>& results) const;
};

// Simple selector matching (reusable from browser.cpp's sel_matches logic)
bool dom_simple_match(const std::string& tok, DOMNode* node);
bool dom_sel_matches(const std::string& sel, DOMNode* node);

// CSS Specificity
struct Specificity {
    int a = 0, b = 0, c = 0; // (id, class/attr/pseudo-class, type/pseudo-element)
    bool operator<(const Specificity& o) const {
        if (a != o.a) return a < o.a;
        if (b != o.b) return b < o.b;
        return c < o.c;
    }
    bool operator==(const Specificity& o) const { return a == o.a && b == o.b && c == o.c; }
};
Specificity calc_specificity(const std::string& selector);
