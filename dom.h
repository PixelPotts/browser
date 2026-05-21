#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <algorithm>
#include <array>

// Forward declarations
struct BoxModel;

// ---- DOMNode ----

struct DOMNode {
    uint32_t node_id = 0;
    enum NodeType { ELEMENT = 1, TEXT = 3 } node_type = ELEMENT;

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
    int fs_computed  = 16;      // font-size in px
    double lh_computed = -1.0;  // line-height factor
    std::string color_computed; // text color
    int text_align_computed = -1; // -1=inherit, 0=left,1=center,2=right,3=justify
    std::string href;           // for <a> elements

    // Box model (for block elements)
    int margin[4]  = {0,0,0,0};
    int padding[4] = {0,0,0,0};
    int width      = -1;
    int max_width  = -1;
    int height     = -1;
    int border_width[4] = {0,0,0,0};
    int border_radius   = 0;
    std::string border_color;
    std::string border_style;
    bool halign_center = false;
    enum class Display : uint8_t { Inherit, Block, Inline, None, Flex, InlineBlock } display = Display::Inherit;
    enum class Float   : uint8_t { None, Left, Right } floatdir = Float::None;
    std::string bg_image;
    std::string bg_color;

    // Event listeners
    struct Listener {
        std::string type;
        uint32_t handler_id; // JS function reference id
    };
    std::vector<Listener> listeners;

    // Dirty flag for re-render
    bool dirty = false;
    bool is_body = false;

    void markDirty();

    // Helper: is this a block-level element?
    bool isBlock() const;

    // Get combined text content of all descendants
    std::string getTextContent() const;

    // Set text content (removes all children, adds single text node)
    void setTextContent(const std::string& text, uint32_t& next_id);

    // Get innerHTML as string
    std::string getInnerHTML() const;

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

    // Script collection
    std::vector<std::string> scripts; // inline script contents
    std::vector<std::string> script_srcs; // external script URLs

private:
    // CSS selector matching helpers
    bool selectorMatchesNode(const std::string& selector, DOMNode* node) const;
    void querySelectorHelper(DOMNode* node, const std::string& selector,
                              std::vector<DOMNode*>& results) const;
};

// Simple selector matching (reusable from browser.cpp's sel_matches logic)
bool dom_simple_match(const std::string& tok, DOMNode* node);
bool dom_sel_matches(const std::string& sel, DOMNode* node);
