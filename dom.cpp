#include "dom.h"
#include <sstream>
#include <cctype>

// ---- DOMNode ----

void DOMNode::markDirty() {
    dirty = true;
    DOMNode* p = parent;
    while (p) {
        p->dirty = true;
        p = p->parent;
    }
}

static const char* BLOCK_TAGS[] = {
    "div","section","article","main","header","footer","nav","aside",
    "p","ul","ol","li","h1","h2","h3","h4","h5","h6",
    "blockquote","form","figure","figcaption","details","summary",
    "body","html",nullptr
};

bool DOMNode::isBlock() const {
    if (node_type != ELEMENT) return false;
    if (floatdir != Float::None) return true;
    if (display == Display::Block || display == Display::Flex ||
        display == Display::InlineBlock) return true;
    if (display == Display::Inline) return false;
    for (int i = 0; BLOCK_TAGS[i]; ++i)
        if (tag_name == BLOCK_TAGS[i]) return true;
    return false;
}

std::string DOMNode::getTextContent() const {
    if (node_type == TEXT) return text_content;
    std::string result;
    for (auto& child : children)
        result += child->getTextContent();
    return result;
}

void DOMNode::setTextContent(const std::string& text, uint32_t& next_id) {
    children.clear();
    auto tn = std::make_shared<DOMNode>();
    tn->node_id = next_id++;
    tn->node_type = TEXT;
    tn->text_content = text;
    tn->parent = this;
    children.push_back(tn);
    markDirty();
}

// Helper: lowercase a string
static std::string str_lower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = tolower((unsigned char)c);
    return r;
}

// Helper: parse attributes from an opening tag string like `div id="x" class="y"`
static void parse_tag_attrs(const std::string& tag_body, DOMNode* node) {
    size_t i = 0;
    // skip tag name
    while (i < tag_body.size() && !isspace((unsigned char)tag_body[i])) i++;
    while (i < tag_body.size()) {
        while (i < tag_body.size() && isspace((unsigned char)tag_body[i])) i++;
        if (i >= tag_body.size()) break;
        // attribute name
        size_t name_start = i;
        while (i < tag_body.size() && tag_body[i] != '=' && !isspace((unsigned char)tag_body[i])) i++;
        std::string name = str_lower(tag_body.substr(name_start, i - name_start));
        while (i < tag_body.size() && isspace((unsigned char)tag_body[i])) i++;
        std::string value;
        if (i < tag_body.size() && tag_body[i] == '=') {
            i++; // skip =
            while (i < tag_body.size() && isspace((unsigned char)tag_body[i])) i++;
            if (i < tag_body.size() && (tag_body[i] == '"' || tag_body[i] == '\'')) {
                char q = tag_body[i++];
                size_t vs = i;
                while (i < tag_body.size() && tag_body[i] != q) i++;
                value = tag_body.substr(vs, i - vs);
                if (i < tag_body.size()) i++;
            } else {
                size_t vs = i;
                while (i < tag_body.size() && !isspace((unsigned char)tag_body[i])) i++;
                value = tag_body.substr(vs, i - vs);
            }
        }
        if (!name.empty()) {
            node->attributes[name] = value;
            if (name == "id") node->id = value;
            else if (name == "class") {
                node->class_list.clear();
                std::string cls;
                for (char c : value) {
                    if (isspace((unsigned char)c)) {
                        if (!cls.empty()) { node->class_list.push_back(cls); cls.clear(); }
                    } else cls += c;
                }
                if (!cls.empty()) node->class_list.push_back(cls);
            }
        }
    }
}

// Void/self-closing elements
static bool is_void_element(const std::string& tag) {
    static const char* voids[] = {
        "area","base","br","col","embed","hr","img","input",
        "link","meta","param","source","track","wbr",
        "path","circle","ellipse","line","polyline","polygon",
        "rect","use","stop", nullptr
    };
    for (const char** p = voids; *p; ++p)
        if (tag == *p) return true;
    return false;
}

// UA bold tags (must match browser.cpp BOLD_TAGS)
static bool is_bold_tag(const std::string& tag) {
    return tag == "b" || tag == "strong" || tag == "h1" || tag == "h2" ||
           tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6";
}

// UA italic tags
static bool is_italic_tag(const std::string& tag) {
    return tag == "i" || tag == "em" || tag == "cite" || tag == "dfn" || tag == "var";
}

// UA heading font sizes (in px)
static int heading_font_size(const std::string& tag) {
    if (tag == "h1") return 32;
    if (tag == "h2") return 24;
    if (tag == "h3") return 19;
    if (tag == "h4") return 16;
    if (tag == "h5") return 13;
    if (tag == "h6") return 11;
    return 0;
}

void DOMNode::setInnerHTML(const std::string& html, uint32_t& next_id,
                           std::unordered_map<uint32_t, DOMNode*>& node_map,
                           std::unordered_map<std::string, DOMNode*>& id_map) {
    fprintf(stderr, "[DEBUG setInnerHTML] on <%s id='%s'> html='%.80s%s'\n",
            tag_name.c_str(), id.c_str(), html.c_str(), html.size() > 80 ? "..." : "");

    // Unregister old children from maps
    std::function<void(DOMNode*)> unreg = [&](DOMNode* n) {
        node_map.erase(n->node_id);
        if (!n->id.empty()) id_map.erase(n->id);
        for (auto& c : n->children) unreg(c.get());
    };
    for (auto& c : children) unreg(c.get());
    children.clear();

    if (html.empty()) { markDirty(); return; }

    // Simple HTML fragment parser
    DOMNode* cur = this;
    size_t i = 0;
    size_t len = html.size();

    while (i < len) {
        if (html[i] == '<') {
            size_t tag_start = i;
            i++;
            if (i >= len) break;

            // Closing tag
            if (html[i] == '/') {
                i++;
                size_t ns = i;
                while (i < len && html[i] != '>') i++;
                std::string close_tag = str_lower(html.substr(ns, i - ns));
                // Trim whitespace
                while (!close_tag.empty() && isspace((unsigned char)close_tag.back()))
                    close_tag.pop_back();
                if (i < len) i++; // skip >
                fprintf(stderr, "[DEBUG setInnerHTML]   close </%s> cur now=<%s>\n",
                        close_tag.c_str(), cur->tag_name.c_str());
                // Walk up to find matching open tag
                DOMNode* p = cur;
                while (p && p != this && p->tag_name != close_tag)
                    p = p->parent;
                if (p && p != this)
                    cur = p->parent ? p->parent : this;
                continue;
            }

            // Opening tag
            size_t ts = i;
            while (i < len && html[i] != '>' && !isspace((unsigned char)html[i])) i++;
            std::string tag_name = str_lower(html.substr(ts, i - ts));

            // Collect full tag body until >
            while (i < len && html[i] != '>') i++;
            std::string tag_body = html.substr(ts, i - ts);
            bool self_close = (!tag_body.empty() && tag_body.back() == '/');
            if (self_close) tag_body.pop_back();
            if (i < len) i++; // skip >

            auto elem = std::make_shared<DOMNode>();
            elem->node_id = next_id++;
            elem->node_type = ELEMENT;
            elem->tag_name = tag_name;
            elem->parent = cur;
            parse_tag_attrs(tag_body, elem.get());

            // Apply UA defaults for bold/italic/heading tags
            if (is_bold_tag(tag_name)) {
                elem->fw_computed = 700; // PANGO_WEIGHT_BOLD
                fprintf(stderr, "[DEBUG setInnerHTML]   <%s> -> fw_computed=700 (bold)\n", tag_name.c_str());
            }
            if (is_italic_tag(tag_name)) {
                elem->fi_computed = 2; // PANGO_STYLE_ITALIC
                fprintf(stderr, "[DEBUG setInnerHTML]   <%s> -> fi_computed=2 (italic)\n", tag_name.c_str());
            }
            int hsz = heading_font_size(tag_name);
            if (hsz > 0) elem->fs_computed = hsz;

            node_map[elem->node_id] = elem.get();
            if (!elem->id.empty()) id_map[elem->id] = elem.get();

            fprintf(stderr, "[DEBUG setInnerHTML]   open <%s> id=%u fw=%d fs=%d parent=<%s>\n",
                    tag_name.c_str(), elem->node_id, elem->fw_computed, elem->fs_computed,
                    cur->tag_name.c_str());

            cur->children.push_back(elem);

            if (!self_close && !is_void_element(tag_name)) {
                cur = elem.get();
            }
        } else {
            // Text content
            size_t ts = i;
            while (i < len && html[i] != '<') i++;
            std::string text = html.substr(ts, i - ts);
            if (!text.empty()) {
                auto tn = std::make_shared<DOMNode>();
                tn->node_id = next_id++;
                tn->node_type = TEXT;
                tn->text_content = text;
                tn->parent = cur;
                node_map[tn->node_id] = tn.get();
                cur->children.push_back(tn);
                fprintf(stderr, "[DEBUG setInnerHTML]   text '%s' parent=<%s> parent_fw=%d\n",
                        text.c_str(), cur->tag_name.c_str(), cur->fw_computed);
            }
        }
    }

    // Debug: dump resulting child tree
    std::function<void(DOMNode*, int)> dump = [&](DOMNode* n, int depth) {
        std::string indent(depth * 2, ' ');
        if (n->node_type == DOMNode::TEXT)
            fprintf(stderr, "[DEBUG setInnerHTML] %s TEXT: '%s'\n", indent.c_str(), n->text_content.c_str());
        else
            fprintf(stderr, "[DEBUG setInnerHTML] %s <%s> fw=%d fs=%d children=%zu\n",
                    indent.c_str(), n->tag_name.c_str(), n->fw_computed, n->fs_computed, n->children.size());
        for (auto& c : n->children) dump(c.get(), depth + 1);
    };
    fprintf(stderr, "[DEBUG setInnerHTML] resulting tree for <%s id='%s'>:\n", tag_name.c_str(), id.c_str());
    for (auto& c : children) dump(c.get(), 1);

    markDirty();
}

std::string DOMNode::getInnerHTML() const {
    std::string result;
    for (auto& child : children) {
        if (child->node_type == TEXT) {
            result += child->text_content;
        } else {
            result += "<" + child->tag_name;
            if (!child->id.empty())
                result += " id=\"" + child->id + "\"";
            if (!child->class_list.empty()) {
                result += " class=\"";
                for (size_t i = 0; i < child->class_list.size(); ++i) {
                    if (i > 0) result += " ";
                    result += child->class_list[i];
                }
                result += "\"";
            }
            for (auto& [k, v] : child->attributes) {
                if (k != "id" && k != "class")
                    result += " " + k + "=\"" + v + "\"";
            }
            result += ">";
            result += child->getInnerHTML();
            result += "</" + child->tag_name + ">";
        }
    }
    return result;
}

void DOMNode::addClass(const std::string& cls) {
    if (!hasClass(cls)) {
        class_list.push_back(cls);
        markDirty();
    }
}

void DOMNode::removeClass(const std::string& cls) {
    auto it = std::find(class_list.begin(), class_list.end(), cls);
    if (it != class_list.end()) {
        class_list.erase(it);
        markDirty();
    }
}

bool DOMNode::hasClass(const std::string& cls) const {
    return std::find(class_list.begin(), class_list.end(), cls) != class_list.end();
}

bool DOMNode::toggleClass(const std::string& cls) {
    if (hasClass(cls)) {
        removeClass(cls);
        return false;
    }
    addClass(cls);
    return true;
}

// ---- cloneNode ----

std::shared_ptr<DOMNode> DOMNode::cloneNode(bool deep, uint32_t& next_id,
                                              std::unordered_map<uint32_t, DOMNode*>& node_map) const {
    auto clone = std::make_shared<DOMNode>();
    clone->node_id = next_id++;
    clone->node_type = node_type;
    clone->tag_name = tag_name;
    // Don't copy id — clones shouldn't duplicate IDs
    clone->class_list = class_list;
    clone->attributes = attributes;
    clone->attributes.erase("id"); // remove id from attributes too
    clone->text_content = text_content;
    clone->style_props = style_props;
    clone->inline_style_raw = inline_style_raw;
    // Copy computed style fields
    clone->fw_computed = fw_computed;
    clone->fi_computed = fi_computed;
    clone->fs_computed = fs_computed;
    clone->lh_computed = lh_computed;
    clone->color_computed = color_computed;
    clone->text_align_computed = text_align_computed;
    clone->text_transform = text_transform;
    clone->font_family = font_family;
    clone->box_shadow = box_shadow;
    clone->opacity = opacity;
    clone->overflow = overflow;
    clone->href = href;
    clone->text_decoration = text_decoration;
    clone->text_decoration_color = text_decoration_color;
    clone->text_decoration_style = text_decoration_style;
    clone->letter_spacing = letter_spacing;
    clone->word_spacing = word_spacing;
    clone->font_variant = font_variant;
    clone->white_space = white_space;
    clone->text_indent = text_indent;
    clone->text_overflow = text_overflow;
    clone->font_stretch = font_stretch;
    clone->text_shadow = text_shadow;
    clone->flex_direction = flex_direction;
    clone->justify_content = justify_content;
    clone->align_items = align_items;
    clone->flex_wrap = flex_wrap;
    clone->gap = gap;
    clone->position = position;
    clone->pos_top = pos_top;
    clone->pos_left = pos_left;
    clone->pos_right = pos_right;
    clone->pos_bottom = pos_bottom;
    clone->z_index = z_index;
    for (int i = 0; i < 4; i++) {
        clone->margin[i] = margin[i];
        clone->padding[i] = padding[i];
        clone->border_width[i] = border_width[i];
    }
    clone->width = width;
    clone->max_width = max_width;
    clone->height = height;
    clone->border_radius = border_radius;
    clone->border_color = border_color;
    clone->border_style = border_style;
    clone->halign_center = halign_center;
    clone->display = display;
    clone->floatdir = floatdir;
    clone->bg_image = bg_image;
    clone->bg_color = bg_color;

    node_map[clone->node_id] = clone.get();

    if (deep) {
        for (const auto& child : children) {
            auto child_clone = child->cloneNode(true, next_id, node_map);
            child_clone->parent = clone.get();
            clone->children.push_back(child_clone);
        }
    }
    return clone;
}

// ---- Document ----

Document::Document() {
    root = std::make_shared<DOMNode>();
    root->node_id = next_id++;
    root->node_type = DOMNode::ELEMENT;
    root->tag_name = "html";
    node_map[root->node_id] = root.get();
}

std::shared_ptr<DOMNode> Document::createElement(const std::string& tag) {
    auto node = std::make_shared<DOMNode>();
    node->node_id = next_id++;
    node->node_type = DOMNode::ELEMENT;
    // lowercase the tag
    node->tag_name = tag;
    for (auto& c : node->tag_name)
        c = (char)std::tolower((unsigned char)c);
    registerNode(node.get());
    orphans.push_back(node);  // keep alive until appendChild
    return node;
}

std::shared_ptr<DOMNode> Document::createTextNode(const std::string& text) {
    auto node = std::make_shared<DOMNode>();
    node->node_id = next_id++;
    node->node_type = DOMNode::TEXT;
    node->text_content = text;
    registerNode(node.get());
    orphans.push_back(node);  // keep alive until appendChild
    return node;
}

void Document::registerNode(DOMNode* node) {
    node_map[node->node_id] = node;
    if (!node->id.empty())
        id_map[node->id] = node;
}

void Document::unregisterNode(DOMNode* node) {
    node_map.erase(node->node_id);
    if (!node->id.empty())
        id_map.erase(node->id);
    for (auto& child : node->children)
        unregisterNode(child.get());
}

DOMNode* Document::getElementById(const std::string& id) const {
    auto it = id_map.find(id);
    return it != id_map.end() ? it->second : nullptr;
}

// ---- CSS selector matching for DOM tree ----

// Check a single attribute selector condition against a node
static bool check_attr_selector(const std::string& expr, DOMNode* node) {
    // expr is contents inside [...], e.g. "attr", "attr=val", "attr^=val", etc.
    // Find operator position
    size_t op_pos = std::string::npos;
    int op_type = 0; // 0=has, 1==, 2=^=, 3=$=, 4=*=, 5=~=
    for (size_t i = 0; i < expr.size(); ++i) {
        if (expr[i] == '=' && i > 0 && expr[i-1] == '^') { op_pos = i-1; op_type = 2; break; }
        if (expr[i] == '=' && i > 0 && expr[i-1] == '$') { op_pos = i-1; op_type = 3; break; }
        if (expr[i] == '=' && i > 0 && expr[i-1] == '*') { op_pos = i-1; op_type = 4; break; }
        if (expr[i] == '=' && i > 0 && expr[i-1] == '~') { op_pos = i-1; op_type = 5; break; }
        if (expr[i] == '=' && (i == 0 || (expr[i-1] != '^' && expr[i-1] != '$' && expr[i-1] != '*' && expr[i-1] != '~')))
            { op_pos = i; op_type = 1; break; }
    }
    if (op_pos == std::string::npos) {
        // [attr] — has attribute
        std::string attr = expr;
        while (!attr.empty() && isspace((unsigned char)attr.back())) attr.pop_back();
        while (!attr.empty() && isspace((unsigned char)attr.front())) attr.erase(attr.begin());
        return node->attributes.count(attr) > 0;
    }
    std::string attr = expr.substr(0, op_pos);
    while (!attr.empty() && isspace((unsigned char)attr.back())) attr.pop_back();
    std::string val_part = expr.substr(op_type == 1 ? op_pos + 1 : op_pos + 2);
    while (!val_part.empty() && isspace((unsigned char)val_part.front())) val_part.erase(val_part.begin());
    while (!val_part.empty() && isspace((unsigned char)val_part.back())) val_part.pop_back();
    // strip quotes
    if (val_part.size() >= 2 && (val_part.front() == '"' || val_part.front() == '\''))
        val_part = val_part.substr(1, val_part.size() - 2);

    auto it = node->attributes.find(attr);
    if (it == node->attributes.end()) return false;
    const std::string& v = it->second;

    switch (op_type) {
        case 1: return v == val_part; // exact
        case 2: return v.size() >= val_part.size() && v.substr(0, val_part.size()) == val_part; // ^=
        case 3: return v.size() >= val_part.size() && v.substr(v.size() - val_part.size()) == val_part; // $=
        case 4: return v.find(val_part) != std::string::npos; // *=
        case 5: { // ~= word match
            std::string word;
            for (char c : v) {
                if (isspace((unsigned char)c)) {
                    if (word == val_part) return true;
                    word.clear();
                } else word += c;
            }
            return word == val_part;
        }
    }
    return false;
}

bool dom_simple_match(const std::string& raw, DOMNode* node) {
    if (raw.empty() || raw == "*") return true;
    if (node->node_type != DOMNode::ELEMENT) return false;

    // Extract and check attribute selectors and pseudo-classes
    std::string base;
    std::vector<std::string> attr_sels;
    std::vector<std::string> pseudos;
    size_t i = 0, n = raw.size();
    while (i < n) {
        if (raw[i] == '[') {
            ++i; size_t start = i;
            int depth = 1;
            while (i < n && depth > 0) { if (raw[i]=='[') ++depth; else if (raw[i]==']') --depth; ++i; }
            attr_sels.push_back(raw.substr(start, i - 1 - start));
        } else if (raw[i] == ':') {
            ++i;
            size_t start = i;
            // Collect pseudo name
            while (i < n && raw[i] != '(' && raw[i] != '.' && raw[i] != '#' && raw[i] != '[' && raw[i] != ':')
                ++i;
            std::string pname = raw.substr(start, i - start);
            std::string parg;
            if (i < n && raw[i] == '(') {
                ++i; int d = 1; size_t as = i;
                while (i < n && d > 0) { if (raw[i]=='(') ++d; else if (raw[i]==')') --d; ++i; }
                parg = raw.substr(as, i - 1 - as);
            }
            pseudos.push_back(pname + (parg.empty() ? "" : "(" + parg + ")"));
        } else {
            base += raw[i++];
        }
    }

    // Match base selector (tag#id.class1.class2)
    if (!base.empty() && base != "*") {
        if (base[0] == '#') {
            if (node->id != base.substr(1)) return false;
        } else {
            std::string tag_part;
            std::vector<std::string> req_cls;
            size_t bi = 0;
            // check for #id in base
            size_t hash = base.find('#');
            std::string base_no_id = base;
            if (hash != std::string::npos) {
                std::string id_part;
                size_t end_id = hash + 1;
                while (end_id < base.size() && base[end_id] != '.') ++end_id;
                id_part = base.substr(hash + 1, end_id - hash - 1);
                if (node->id != id_part) return false;
                base_no_id = base.substr(0, hash) + base.substr(end_id);
            }
            if (!base_no_id.empty() && base_no_id[0] != '.') {
                size_t d = base_no_id.find('.');
                tag_part = base_no_id.substr(0, d);
                if (d != std::string::npos) bi = d + 1; else bi = base_no_id.size();
            } else if (!base_no_id.empty()) { bi = 1; }
            while (bi <= base_no_id.size()) {
                size_t d = base_no_id.find('.', bi);
                std::string c = base_no_id.substr(bi, d == std::string::npos ? std::string::npos : d - bi);
                if (!c.empty()) req_cls.push_back(c);
                if (d == std::string::npos) break;
                bi = d + 1;
            }
            if (!tag_part.empty() && node->tag_name != tag_part) return false;
            for (const auto& c : req_cls)
                if (!node->hasClass(c)) return false;
        }
    }

    // Check attribute selectors
    for (const auto& as : attr_sels)
        if (!check_attr_selector(as, node)) return false;

    // Check pseudo-classes
    for (const auto& ps : pseudos) {
        if (ps == "first-child") {
            if (!node->parent) return false;
            bool is_first = true;
            for (auto& c : node->parent->children) {
                if (c->node_type == DOMNode::ELEMENT) { is_first = (c.get() == node); break; }
            }
            if (!is_first) return false;
        } else if (ps == "last-child") {
            if (!node->parent) return false;
            bool is_last = true;
            for (int j = (int)node->parent->children.size() - 1; j >= 0; --j) {
                if (node->parent->children[j]->node_type == DOMNode::ELEMENT) {
                    is_last = (node->parent->children[j].get() == node); break;
                }
            }
            if (!is_last) return false;
        } else if (ps.substr(0, 4) == "not(") {
            std::string inner = ps.substr(4, ps.size() - 5);
            if (dom_simple_match(inner, node)) return false; // :not matches if inner doesn't
        }
        // other pseudo-classes: silently ignore (hover, focus, etc.)
    }

    return true;
}

bool dom_sel_matches(const std::string& sel, DOMNode* node) {
    if (sel.empty()) return false;

    // Split selector into tokens
    std::vector<std::string> parts;
    size_t i = 0, n = sel.size();
    while (i < n) {
        while (i < n && sel[i] == ' ') ++i;
        size_t j = i;
        while (j < n && sel[j] != ' ') ++j;
        if (j > i) {
            std::string p = sel.substr(i, j - i);
            if (p != ">") parts.push_back(p);
        }
        i = j;
    }
    if (parts.empty()) return false;

    // Last part must match current node
    if (!dom_simple_match(parts.back(), node)) return false;
    if (parts.size() == 1) return true;

    // Remaining parts match ancestors (right-to-left greedy)
    int pi = (int)parts.size() - 2;
    DOMNode* ancestor = node->parent;
    while (pi >= 0 && ancestor) {
        if (dom_simple_match(parts[pi], ancestor)) --pi;
        ancestor = ancestor->parent;
    }
    return pi < 0;
}

DOMNode* Document::querySelector(const std::string& selector) const {
    std::vector<DOMNode*> results;
    querySelectorHelper(root.get(), selector, results);
    return results.empty() ? nullptr : results[0];
}

std::vector<DOMNode*> Document::querySelectorAll(const std::string& selector) const {
    std::vector<DOMNode*> results;
    querySelectorHelper(root.get(), selector, results);
    return results;
}

void Document::querySelectorHelper(DOMNode* node, const std::string& selector,
                                    std::vector<DOMNode*>& results) const {
    if (node->node_type == DOMNode::ELEMENT && dom_sel_matches(selector, node))
        results.push_back(node);
    for (auto& child : node->children)
        querySelectorHelper(child.get(), selector, results);
}

void Document::appendChild(DOMNode* parent, std::shared_ptr<DOMNode> child) {
    // Remove from old parent if any
    if (child->parent) {
        auto& siblings = child->parent->children;
        siblings.erase(
            std::remove_if(siblings.begin(), siblings.end(),
                [&](const std::shared_ptr<DOMNode>& n) { return n.get() == child.get(); }),
            siblings.end());
    }
    // Remove from orphans list (now owned by parent's children vector)
    orphans.erase(
        std::remove_if(orphans.begin(), orphans.end(),
            [&](const std::shared_ptr<DOMNode>& n) { return n.get() == child.get(); }),
        orphans.end());
    child->parent = parent;
    parent->children.push_back(child);
    registerNode(child.get());
    parent->markDirty();
    if (on_mutated) on_mutated();
}

void Document::removeChild(DOMNode* parent, DOMNode* child) {
    auto& siblings = parent->children;
    for (auto it = siblings.begin(); it != siblings.end(); ++it) {
        if (it->get() == child) {
            unregisterNode(child);
            child->parent = nullptr;
            siblings.erase(it);
            parent->markDirty();
            if (on_mutated) on_mutated();
            return;
        }
    }
}

void Document::insertBefore(DOMNode* parent, std::shared_ptr<DOMNode> newChild, DOMNode* refChild) {
    if (!refChild) {
        appendChild(parent, newChild);
        return;
    }
    // Remove from old parent
    if (newChild->parent) {
        auto& siblings = newChild->parent->children;
        siblings.erase(
            std::remove_if(siblings.begin(), siblings.end(),
                [&](const std::shared_ptr<DOMNode>& n) { return n.get() == newChild.get(); }),
            siblings.end());
    }
    newChild->parent = parent;
    auto& siblings = parent->children;
    for (auto it = siblings.begin(); it != siblings.end(); ++it) {
        if (it->get() == refChild) {
            siblings.insert(it, newChild);
            registerNode(newChild.get());
            parent->markDirty();
            if (on_mutated) on_mutated();
            return;
        }
    }
    // refChild not found, append
    siblings.push_back(newChild);
    registerNode(newChild.get());
    parent->markDirty();
    if (on_mutated) on_mutated();
}
