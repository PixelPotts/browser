#include "dom.h"
#include <sstream>
#include <cctype>
#include <set>

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

// Encode a Unicode code point as UTF-8
static std::string utf8_encode(uint32_t cp) {
    std::string r;
    if (cp < 0x80) {
        r += (char)cp;
    } else if (cp < 0x800) {
        r += (char)(0xC0 | (cp >> 6));
        r += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        r += (char)(0xE0 | (cp >> 12));
        r += (char)(0x80 | ((cp >> 6) & 0x3F));
        r += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        r += (char)(0xF0 | (cp >> 18));
        r += (char)(0x80 | ((cp >> 12) & 0x3F));
        r += (char)(0x80 | ((cp >> 6) & 0x3F));
        r += (char)(0x80 | (cp & 0x3F));
    }
    return r;
}

// Decode HTML character references in text
static std::string decode_html_entities(const std::string& s) {
    static const std::unordered_map<std::string, uint32_t> named = {
        {"amp", 0x26}, {"lt", 0x3C}, {"gt", 0x3E}, {"quot", 0x22}, {"apos", 0x27},
        {"nbsp", 0xA0}, {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122},
        {"laquo", 0xAB}, {"raquo", 0xBB}, {"bull", 0x2022}, {"hellip", 0x2026},
        {"mdash", 0x2014}, {"ndash", 0x2013}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
        {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"euro", 0x20AC}, {"pound", 0xA3},
        {"yen", 0xA5}, {"cent", 0xA2}, {"sect", 0xA7}, {"para", 0xB6},
        {"deg", 0xB0}, {"plusmn", 0xB1}, {"micro", 0xB5}, {"frac14", 0xBC},
        {"frac12", 0xBD}, {"frac34", 0xBE}, {"times", 0xD7}, {"divide", 0xF7},
        {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3}, {"delta", 0x3B4},
        {"pi", 0x3C0}, {"sigma", 0x3C3}, {"omega", 0x3C9},
        {"larr", 0x2190}, {"uarr", 0x2191}, {"rarr", 0x2192}, {"darr", 0x2193},
        {"hearts", 0x2665}, {"diams", 0x2666}, {"clubs", 0x2663}, {"spades", 0x2660},
        {"ImaginaryI", 0x2148}, {"notinva", 0x2209}, {"Kopf", 0x1D542},
        {"lang", 0x27E8}, {"rang", 0x27E9}, {"lang", 0x27E8}, {"rang", 0x27E9},
        {"notin", 0x2209}, {"isin", 0x2208}, {"exist", 0x2203}, {"forall", 0x2200},
        {"empty", 0x2205}, {"nabla", 0x2207}, {"prod", 0x220F}, {"sum", 0x2211},
        {"infin", 0x221E}, {"radic", 0x221A}, {"prop", 0x221D},
    };
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] != '&') { r += s[i++]; continue; }
        size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 32) { r += s[i++]; continue; }
        std::string e = s.substr(i + 1, semi - i - 1);
        if (!e.empty() && e[0] == '#') {
            try {
                uint32_t cp = (e.size() > 1 && (e[1] == 'x' || e[1] == 'X'))
                    ? (uint32_t)std::stoul(e.substr(2), nullptr, 16)
                    : (uint32_t)std::stoul(e.substr(1));
                r += utf8_encode(cp);
            } catch (...) { r += s[i++]; continue; }
        } else {
            auto it = named.find(e);
            if (it != named.end()) r += utf8_encode(it->second);
            else { r += s[i++]; continue; }
        }
        i = semi + 1;
    }
    return r;
}

// HTML5 tree construction helpers
static bool is_formatting_element(const std::string& tag) {
    return tag == "a" || tag == "b" || tag == "big" || tag == "code" ||
           tag == "em" || tag == "font" || tag == "i" || tag == "nobr" ||
           tag == "s" || tag == "small" || tag == "strike" || tag == "strong" ||
           tag == "tt" || tag == "u";
}

static bool is_special_element(const std::string& tag) {
    return tag == "address" || tag == "applet" || tag == "area" || tag == "article" ||
           tag == "aside" || tag == "base" || tag == "basefont" || tag == "bgsound" ||
           tag == "blockquote" || tag == "body" || tag == "br" || tag == "button" ||
           tag == "caption" || tag == "center" || tag == "col" || tag == "colgroup" ||
           tag == "dd" || tag == "details" || tag == "dir" || tag == "div" ||
           tag == "dl" || tag == "dt" || tag == "embed" || tag == "fieldset" ||
           tag == "figcaption" || tag == "figure" || tag == "footer" || tag == "form" ||
           tag == "frame" || tag == "frameset" || tag == "h1" || tag == "h2" ||
           tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6" ||
           tag == "head" || tag == "header" || tag == "hgroup" || tag == "hr" ||
           tag == "html" || tag == "iframe" || tag == "img" || tag == "input" ||
           tag == "li" || tag == "link" || tag == "listing" || tag == "main" ||
           tag == "marquee" || tag == "menu" || tag == "meta" || tag == "nav" ||
           tag == "noembed" || tag == "noframes" || tag == "noscript" || tag == "object" ||
           tag == "ol" || tag == "p" || tag == "param" || tag == "pre" ||
           tag == "script" || tag == "section" || tag == "select" || tag == "source" ||
           tag == "style" || tag == "summary" || tag == "table" || tag == "tbody" ||
           tag == "td" || tag == "template" || tag == "textarea" || tag == "tfoot" ||
           tag == "th" || tag == "thead" || tag == "title" || tag == "tr" ||
           tag == "track" || tag == "ul" || tag == "wbr" || tag == "xmp";
}

static bool is_table_child_element(const std::string& tag) {
    return tag == "caption" || tag == "colgroup" || tag == "col" ||
           tag == "thead" || tag == "tfoot" || tag == "tbody" ||
           tag == "tr" || tag == "th" || tag == "td" ||
           tag == "script" || tag == "template" || tag == "style";
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

    if (html.empty()) {
        if (tag_name == "html") {
            auto head = std::make_shared<DOMNode>();
            head->node_id = next_id++;
            head->node_type = ELEMENT;
            head->tag_name = "head";
            head->parent = this;
            node_map[head->node_id] = head.get();
            children.push_back(head);
            auto body = std::make_shared<DOMNode>();
            body->node_id = next_id++;
            body->node_type = ELEMENT;
            body->tag_name = "body";
            body->parent = this;
            node_map[body->node_id] = body.get();
            children.push_back(body);
        }
        markDirty(); return;
    }

    // HTML5 tree construction state
    std::vector<DOMNode*> open_elements;
    std::vector<DOMNode*> active_formatting;
    std::unordered_map<DOMNode*, std::shared_ptr<DOMNode>> ptr_map;

    DOMNode* cur = this;
    open_elements.push_back(this);

    // Helper: find node in a vector, return index or -1
    auto find_in = [](const std::vector<DOMNode*>& v, DOMNode* n) -> int {
        for (int i = (int)v.size() - 1; i >= 0; i--)
            if (v[i] == n) return i;
        return -1;
    };

    // Helper: detach node from its parent's children vector
    auto detach_from_parent = [&](DOMNode* node) {
        if (!node->parent) return;
        auto& siblings = node->parent->children;
        for (auto it = siblings.begin(); it != siblings.end(); ++it) {
            if (it->get() == node) { siblings.erase(it); break; }
        }
        node->parent = nullptr;
    };

    // Helper: get or create shared_ptr for a node
    auto get_sp = [&](DOMNode* node) -> std::shared_ptr<DOMNode> {
        auto it = ptr_map.find(node);
        if (it != ptr_map.end()) return it->second;
        return nullptr;
    };

    // Helper: append child (with shared_ptr) to parent
    auto append_child = [&](DOMNode* child, DOMNode* parent) {
        auto sp = get_sp(child);
        if (!sp) return;
        detach_from_parent(child);
        child->parent = parent;
        parent->children.push_back(sp);
    };

    // Helper: create a clone of an element (for adoption agency)
    auto clone_elem = [&](DOMNode* orig) -> std::shared_ptr<DOMNode> {
        auto elem = std::make_shared<DOMNode>();
        elem->node_id = next_id++;
        elem->node_type = DOMNode::ELEMENT;
        elem->tag_name = orig->tag_name;
        elem->attributes = orig->attributes;
        elem->fw_computed = orig->fw_computed;
        elem->fi_computed = orig->fi_computed;
        elem->fs_computed = orig->fs_computed;
        node_map[elem->node_id] = elem.get();
        ptr_map[elem.get()] = elem;
        return elem;
    };

    // Helper: insert element before table for foster parenting
    auto foster_parent_elem = [&](std::shared_ptr<DOMNode>& elem) {
        DOMNode* table = nullptr;
        for (int i = (int)open_elements.size() - 1; i >= 0; i--) {
            if (open_elements[i]->tag_name == "table") { table = open_elements[i]; break; }
        }
        if (!table || !table->parent) {
            elem->parent = cur;
            cur->children.push_back(elem);
            return;
        }
        DOMNode* tp = table->parent;
        auto& sibs = tp->children;
        auto it = std::find_if(sibs.begin(), sibs.end(),
                               [table](const auto& sp) { return sp.get() == table; });
        elem->parent = tp;
        if (it != sibs.end()) sibs.insert(it, elem);
        else sibs.push_back(elem);
    };

    // Check if currently inside a table element in the open elements stack
    auto in_table_scope = [&]() -> bool {
        for (int i = (int)open_elements.size() - 1; i > 0; i--) {
            const auto& t = open_elements[i]->tag_name;
            if (t == "table") return true;
            if (t == "td" || t == "th" || t == "caption" || t == "template") return false;
        }
        return false;
    };

    // Adoption agency algorithm
    auto run_adoption_agency = [&](const std::string& subject) {
        // Quick check: if current node has the tag and is NOT in active formatting, just pop
        if (!open_elements.empty() && open_elements.back()->tag_name == subject) {
            if (find_in(active_formatting, open_elements.back()) < 0) {
                open_elements.pop_back();
                cur = open_elements.empty() ? this : open_elements.back();
                return;
            }
        }

        for (int outer = 0; outer < 8; outer++) {
            // Find formatting element (last in active_formatting with matching tag)
            DOMNode* fmt_elem = nullptr;
            int fmt_idx = -1;
            for (int i = (int)active_formatting.size() - 1; i >= 0; i--) {
                if (active_formatting[i] && active_formatting[i]->tag_name == subject) {
                    fmt_elem = active_formatting[i]; fmt_idx = i; break;
                }
            }
            if (!fmt_elem) {
                // "Any other end tag": walk up and pop if found
                for (int i = (int)open_elements.size() - 1; i > 0; i--) {
                    if (open_elements[i]->tag_name == subject) {
                        open_elements.erase(open_elements.begin() + i);
                        cur = open_elements.empty() ? this : open_elements.back();
                        return;
                    }
                    if (is_special_element(open_elements[i]->tag_name)) break;
                }
                return;
            }

            int stack_idx = find_in(open_elements, fmt_elem);
            if (stack_idx < 0) {
                active_formatting.erase(active_formatting.begin() + fmt_idx);
                return;
            }

            // Find furthest block (first special element after fmt_elem in stack)
            DOMNode* furthest_block = nullptr;
            int furthest_idx = -1;
            for (int i = stack_idx + 1; i < (int)open_elements.size(); i++) {
                if (is_special_element(open_elements[i]->tag_name)) {
                    furthest_block = open_elements[i]; furthest_idx = i; break;
                }
            }

            if (!furthest_block) {
                // Pop everything including fmt_elem
                while ((int)open_elements.size() > stack_idx)
                    open_elements.pop_back();
                active_formatting.erase(active_formatting.begin() + fmt_idx);
                cur = open_elements.empty() ? this : open_elements.back();
                return;
            }

            DOMNode* common_ancestor = (stack_idx > 0) ? open_elements[stack_idx - 1] : this;
            int bookmark = fmt_idx;

            DOMNode* last_node = furthest_block;
            int node_idx = furthest_idx;

            for (int inner = 1; inner <= 3; inner++) {
                node_idx--;
                if (node_idx <= 0) break;
                DOMNode* node = open_elements[node_idx];

                if (node == fmt_elem) break;

                int node_fmt_idx = find_in(active_formatting, node);
                if (node_fmt_idx < 0) {
                    // Remove from stack, adjust indices
                    open_elements.erase(open_elements.begin() + node_idx);
                    furthest_idx--;
                    continue;
                }

                // Create new element clone
                auto new_elem = clone_elem(node);
                active_formatting[node_fmt_idx] = new_elem.get();
                open_elements[node_idx] = new_elem.get();
                node = new_elem.get();

                if (last_node == furthest_block)
                    bookmark = node_fmt_idx + 1;

                // Append last_node to node
                append_child(last_node, node);
                last_node = node;
            }

            // Insert last_node into common ancestor
            append_child(last_node, common_ancestor);

            // Create new element for fmt_elem's token
            auto new_fmt = clone_elem(fmt_elem);

            // Move all children of furthest_block to new_fmt
            while (!furthest_block->children.empty()) {
                auto child_sp = furthest_block->children[0];
                furthest_block->children.erase(furthest_block->children.begin());
                child_sp->parent = new_fmt.get();
                new_fmt->children.push_back(child_sp);
            }

            // Append new_fmt to furthest_block
            new_fmt->parent = furthest_block;
            furthest_block->children.push_back(new_fmt);

            // Update formatting list
            active_formatting.erase(active_formatting.begin() + fmt_idx);
            int ins_pos = bookmark;
            if (fmt_idx < bookmark) ins_pos--;
            if (ins_pos < 0) ins_pos = 0;
            if (ins_pos > (int)active_formatting.size()) ins_pos = active_formatting.size();
            active_formatting.insert(active_formatting.begin() + ins_pos, new_fmt.get());

            // Update stack: remove fmt_elem, insert new_fmt after furthest_block
            int fmt_si = find_in(open_elements, fmt_elem);
            if (fmt_si >= 0) {
                open_elements.erase(open_elements.begin() + fmt_si);
                // Recalculate furthest_block position after removal
                int fb_si = find_in(open_elements, furthest_block);
                if (fb_si >= 0 && fb_si + 1 <= (int)open_elements.size())
                    open_elements.insert(open_elements.begin() + fb_si + 1, new_fmt.get());
            }

            cur = open_elements.empty() ? this : open_elements.back();
        }
    };

    // Main tokenizer + tree construction loop
    size_t i = 0;
    size_t len = html.size();

    while (i < len) {
        if (html[i] == '<') {
            i++;
            if (i >= len) break;

            // Comment: <!-- ... -->
            if (i + 2 < len && html[i] == '!' && html[i+1] == '-' && html[i+2] == '-') {
                i += 3;
                size_t end = html.find("-->", i);
                std::string ctext;
                if (end != std::string::npos) { ctext = html.substr(i, end - i); i = end + 3; }
                else { ctext = html.substr(i); i = len; }
                auto cn = std::make_shared<DOMNode>();
                cn->node_id = next_id++;
                cn->node_type = COMMENT;
                cn->tag_name = "";
                cn->text_content = ctext;
                cn->parent = cur;
                node_map[cn->node_id] = cn.get();
                ptr_map[cn.get()] = cn;
                cur->children.push_back(cn);
                continue;
            }

            // DOCTYPE: <!DOCTYPE ...> → skip entirely
            if (i + 7 < len && str_lower(html.substr(i, 8)) == "!doctype") {
                while (i < len && html[i] != '>') i++;
                if (i < len) i++;
                continue;
            }

            // CDATA: <![CDATA[...]]> → comment node
            if (i + 6 < len && html.substr(i, 7) == "![CDATA") {
                size_t start = i + 1;
                size_t end = html.find(">", i);
                std::string content;
                if (end != std::string::npos) { content = html.substr(start, end - start); i = end + 1; }
                else { content = html.substr(start); i = len; }
                auto cn = std::make_shared<DOMNode>();
                cn->node_id = next_id++;
                cn->node_type = COMMENT;
                cn->tag_name = "";
                cn->text_content = content;
                cn->parent = cur;
                node_map[cn->node_id] = cn.get();
                ptr_map[cn.get()] = cn;
                cur->children.push_back(cn);
                continue;
            }

            // Processing instruction: <?...> → comment node
            if (html[i] == '?') {
                size_t start = i;
                size_t end = html.find(">", i);
                std::string content;
                if (end != std::string::npos) { content = html.substr(start, end - start); i = end + 1; }
                else { content = html.substr(start); i = len; }
                auto cn = std::make_shared<DOMNode>();
                cn->node_id = next_id++;
                cn->node_type = COMMENT;
                cn->tag_name = "";
                cn->text_content = content;
                cn->parent = cur;
                node_map[cn->node_id] = cn.get();
                ptr_map[cn.get()] = cn;
                cur->children.push_back(cn);
                continue;
            }

            // Closing tag
            if (html[i] == '/') {
                i++;
                size_t ns = i;
                while (i < len && html[i] != '>') i++;
                std::string close_tag = str_lower(html.substr(ns, i - ns));
                while (!close_tag.empty() && isspace((unsigned char)close_tag.back()))
                    close_tag.pop_back();
                if (i < len) i++;
                fprintf(stderr, "[DEBUG setInnerHTML]   close </%s> cur now=<%s>\n",
                        close_tag.c_str(), cur->tag_name.c_str());

                if (is_formatting_element(close_tag)) {
                    // Run adoption agency algorithm
                    run_adoption_agency(close_tag);
                } else if (close_tag == "form" && in_table_scope()) {
                    // Ignore </form> in table context
                } else {
                    // Walk up stack to find matching element, pop everything above it
                    for (int si = (int)open_elements.size() - 1; si > 0; si--) {
                        if (open_elements[si]->tag_name == close_tag) {
                            open_elements.erase(open_elements.begin() + si, open_elements.end());
                            cur = open_elements.empty() ? this : open_elements.back();
                            break;
                        }
                    }
                }
                continue;
            }

            // Opening tag
            size_t ts = i;
            while (i < len && html[i] != '>' && !isspace((unsigned char)html[i])) i++;
            std::string otag = str_lower(html.substr(ts, i - ts));

            // Collect full tag body until >
            while (i < len && html[i] != '>') i++;
            std::string tag_body = html.substr(ts, i - ts);
            bool self_close = (!tag_body.empty() && tag_body.back() == '/');
            if (self_close) tag_body.pop_back();
            if (i < len) i++;

            auto elem = std::make_shared<DOMNode>();
            elem->node_id = next_id++;
            elem->node_type = ELEMENT;
            elem->tag_name = otag;
            elem->parent = cur;
            parse_tag_attrs(tag_body, elem.get());

            if (is_bold_tag(otag)) {
                elem->fw_computed = 700;
                fprintf(stderr, "[DEBUG setInnerHTML]   <%s> -> fw_computed=700 (bold)\n", otag.c_str());
            }
            if (is_italic_tag(otag)) {
                elem->fi_computed = 2;
                fprintf(stderr, "[DEBUG setInnerHTML]   <%s> -> fi_computed=2 (italic)\n", otag.c_str());
            }
            int hsz = heading_font_size(otag);
            if (hsz > 0) elem->fs_computed = hsz;

            node_map[elem->node_id] = elem.get();
            if (!elem->id.empty()) id_map[elem->id] = elem.get();
            ptr_map[elem.get()] = elem;

            fprintf(stderr, "[DEBUG setInnerHTML]   open <%s> id=%u fw=%d fs=%d parent=<%s>\n",
                    otag.c_str(), elem->node_id, elem->fw_computed, elem->fs_computed,
                    cur->tag_name.c_str());

            // Foster parenting: if in table context and element is not table-compatible
            if (cur->tag_name == "table" && in_table_scope()) {
                if (otag == "form") {
                    // Add form to table, don't push to stack
                    elem->parent = cur;
                    cur->children.push_back(elem);
                    continue;
                }
                if (otag == "input") {
                    auto type_it = elem->attributes.find("type");
                    bool is_hidden = (type_it != elem->attributes.end() &&
                                      str_lower(type_it->second) == "hidden");
                    if (is_hidden) {
                        // Hidden input stays in table
                        elem->parent = cur;
                        cur->children.push_back(elem);
                        continue;
                    }
                    // Non-hidden input: foster parent
                    foster_parent_elem(elem);
                    continue;
                }
                if (!is_table_child_element(otag) && otag != "table") {
                    // Foster parent this element
                    foster_parent_elem(elem);
                    if (!self_close && !is_void_element(otag)) {
                        open_elements.push_back(elem.get());
                        cur = elem.get();
                    }
                    continue;
                }
            }

            // Implicit closing: certain elements close an open <p>
            static const char* p_closers[] = {
                "address", "article", "aside", "blockquote", "details", "dialog",
                "dd", "div", "dl", "dt", "fieldset", "figcaption", "figure",
                "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6",
                "header", "hgroup", "hr", "li", "main", "menu", "nav",
                "ol", "p", "pre", "section", "summary", "table", "ul",
                nullptr
            };
            if (cur->tag_name == "p") {
                bool closes_p = false;
                for (int ci = 0; p_closers[ci]; ci++) {
                    if (otag == p_closers[ci]) { closes_p = true; break; }
                }
                if (closes_p) {
                    // Pop <p> from stack
                    int p_idx = find_in(open_elements, cur);
                    if (p_idx > 0) {
                        open_elements.erase(open_elements.begin() + p_idx, open_elements.end());
                        cur = open_elements.empty() ? this : open_elements.back();
                    } else if (cur->parent) {
                        cur = cur->parent;
                    }
                    elem->parent = cur;
                }
            }

            // Table: auto-wrap <col> in <colgroup>
            if (otag == "col" && cur->tag_name == "table") {
                auto colgroup = std::make_shared<DOMNode>();
                colgroup->node_id = next_id++;
                colgroup->node_type = ELEMENT;
                colgroup->tag_name = "colgroup";
                colgroup->parent = cur;
                node_map[colgroup->node_id] = colgroup.get();
                ptr_map[colgroup.get()] = colgroup;
                cur->children.push_back(colgroup);
                elem->parent = colgroup.get();
                colgroup->children.push_back(elem);
                continue;
            }

            cur->children.push_back(elem);

            if (!self_close && !is_void_element(otag)) {
                // Raw text elements: content until matching close tag is not parsed
                if (otag == "textarea" || otag == "style" || otag == "script" ||
                    otag == "title" || otag == "xmp") {
                    std::string close_pat = "</" + otag;
                    size_t end = std::string::npos;
                    for (size_t s = i; s + close_pat.size() <= len; s++) {
                        bool match = true;
                        for (size_t j = 0; j < close_pat.size(); j++) {
                            if (tolower((unsigned char)html[s+j]) != close_pat[j]) {
                                match = false; break;
                            }
                        }
                        if (match && (s + close_pat.size() >= len ||
                                      html[s + close_pat.size()] == '>' ||
                                      isspace((unsigned char)html[s + close_pat.size()]))) {
                            end = s; break;
                        }
                    }
                    std::string raw;
                    if (end != std::string::npos) {
                        raw = html.substr(i, end - i);
                        i = end + close_pat.size();
                        while (i < len && html[i] != '>') i++;
                        if (i < len) i++;
                    } else { raw = html.substr(i); i = len; }
                    if (!raw.empty()) {
                        auto tn = std::make_shared<DOMNode>();
                        tn->node_id = next_id++;
                        tn->node_type = TEXT;
                        tn->text_content = raw;
                        tn->parent = elem.get();
                        node_map[tn->node_id] = tn.get();
                        ptr_map[tn.get()] = tn;
                        elem->children.push_back(tn);
                    }
                } else {
                    open_elements.push_back(elem.get());
                    cur = elem.get();

                    // If formatting element, add to active formatting list
                    if (is_formatting_element(otag)) {
                        active_formatting.push_back(elem.get());
                    }
                }
            }
        } else {
            // Text content - decode HTML entities
            size_t ts = i;
            while (i < len && html[i] != '<') i++;
            std::string raw_text = html.substr(ts, i - ts);
            // CR normalization: \r\n → \n, lone \r → \n
            std::string norm_text;
            norm_text.reserve(raw_text.size());
            for (size_t k = 0; k < raw_text.size(); k++) {
                if (raw_text[k] == '\r') {
                    norm_text += '\n';
                    if (k + 1 < raw_text.size() && raw_text[k+1] == '\n') k++;
                } else {
                    norm_text += raw_text[k];
                }
            }
            std::string text = decode_html_entities(norm_text);
            if (!text.empty()) {
                auto tn = std::make_shared<DOMNode>();
                tn->node_id = next_id++;
                tn->node_type = TEXT;
                tn->text_content = text;
                tn->parent = cur;
                node_map[tn->node_id] = tn.get();
                ptr_map[tn.get()] = tn;
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
        } else if (child->node_type == COMMENT) {
            result += "<!--" + child->text_content + "-->";
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
    int op_type = 0; // 0=has, 1==, 2=^=, 3=$=, 4=*=, 5=~=, 6=|=
    for (size_t i = 0; i < expr.size(); ++i) {
        if (expr[i] == '=' && i > 0 && expr[i-1] == '^') { op_pos = i-1; op_type = 2; break; }
        if (expr[i] == '=' && i > 0 && expr[i-1] == '$') { op_pos = i-1; op_type = 3; break; }
        if (expr[i] == '=' && i > 0 && expr[i-1] == '*') { op_pos = i-1; op_type = 4; break; }
        if (expr[i] == '=' && i > 0 && expr[i-1] == '~') { op_pos = i-1; op_type = 5; break; }
        if (expr[i] == '=' && i > 0 && expr[i-1] == '|') { op_pos = i-1; op_type = 6; break; }
        if (expr[i] == '=' && (i == 0 || (expr[i-1] != '^' && expr[i-1] != '$' && expr[i-1] != '*' && expr[i-1] != '~' && expr[i-1] != '|')))
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

    // Check for case-sensitivity flag: i or s at end after space
    bool case_insensitive = false;
    if (val_part.size() >= 2) {
        char last = val_part.back();
        if ((last == 'i' || last == 'I' || last == 's' || last == 'S') &&
            val_part.size() >= 3 && isspace((unsigned char)val_part[val_part.size()-2])) {
            case_insensitive = (last == 'i' || last == 'I');
            val_part.pop_back();
            while (!val_part.empty() && isspace((unsigned char)val_part.back())) val_part.pop_back();
        }
    }

    // strip quotes
    if (val_part.size() >= 2 && (val_part.front() == '"' || val_part.front() == '\''))
        val_part = val_part.substr(1, val_part.size() - 2);

    auto it = node->attributes.find(attr);
    if (it == node->attributes.end()) return false;
    std::string v = it->second;
    std::string vp = val_part;
    if (case_insensitive) { v = str_lower(v); vp = str_lower(vp); }

    switch (op_type) {
        case 1: return v == vp; // exact
        case 2: return v.size() >= vp.size() && v.substr(0, vp.size()) == vp; // ^=
        case 3: return v.size() >= vp.size() && v.substr(v.size() - vp.size()) == vp; // $=
        case 4: return v.find(vp) != std::string::npos; // *=
        case 5: { // ~= word match
            std::string word;
            for (char c : v) {
                if (isspace((unsigned char)c)) {
                    if (word == vp) return true;
                    word.clear();
                } else word += c;
            }
            return word == vp;
        }
        case 6: // |= dash-separated prefix
            return v == vp || (v.size() > vp.size() && v.substr(0, vp.size()) == vp && v[vp.size()] == '-');
    }
    return false;
}

// Parse An+B notation: "odd", "even", "3", "2n+1", "-n+3", "n", "3n", "-2n-1", etc.
// Returns {a, b} where the formula is An+B
static std::pair<int,int> parse_anb(const std::string& raw) {
    std::string s;
    for (char c : raw) if (!isspace((unsigned char)c)) s += c;
    if (s == "odd") return {2, 1};
    if (s == "even") return {2, 0};
    size_t npos = s.find('n');
    if (npos == std::string::npos) {
        // Just a number B
        try { return {0, std::stoi(s)}; } catch (...) { return {0, 0}; }
    }
    // Parse A
    int a = 1;
    if (npos == 0) a = 1;
    else if (npos == 1 && s[0] == '-') a = -1;
    else if (npos == 1 && s[0] == '+') a = 1;
    else { try { a = std::stoi(s.substr(0, npos)); } catch (...) { a = 0; } }
    // Parse B
    int b = 0;
    if (npos + 1 < s.size()) {
        try { b = std::stoi(s.substr(npos + 1)); } catch (...) { b = 0; }
    }
    return {a, b};
}

// Check if 1-based index matches An+B formula
static bool matches_anb(int a, int b, int index) {
    if (a == 0) return index == b;
    // index = a*n + b, for some integer n >= 0
    // n = (index - b) / a, must be integer >= 0
    int diff = index - b;
    if (diff == 0) return true;
    if ((diff < 0 && a > 0) || (diff > 0 && a < 0)) return false;
    return diff % a == 0 && diff / a >= 0;
}

// Count element's 1-based index among siblings (element children only)
static int element_index(DOMNode* node) {
    if (!node->parent) return 1;
    int idx = 0;
    for (auto& c : node->parent->children) {
        if (c->node_type == DOMNode::ELEMENT) ++idx;
        if (c.get() == node) return idx;
    }
    return 1;
}

// Count element's 1-based index from end among siblings
static int element_index_last(DOMNode* node) {
    if (!node->parent) return 1;
    int total = 0;
    for (auto& c : node->parent->children)
        if (c->node_type == DOMNode::ELEMENT) ++total;
    return total - element_index(node) + 1;
}

// Count element's 1-based index among same-tag siblings
static int element_type_index(DOMNode* node) {
    if (!node->parent) return 1;
    int idx = 0;
    for (auto& c : node->parent->children) {
        if (c->node_type == DOMNode::ELEMENT && c->tag_name == node->tag_name) ++idx;
        if (c.get() == node) return idx;
    }
    return 1;
}

// Count element's 1-based index from end among same-tag siblings
static int element_type_index_last(DOMNode* node) {
    if (!node->parent) return 1;
    int total = 0;
    for (auto& c : node->parent->children)
        if (c->node_type == DOMNode::ELEMENT && c->tag_name == node->tag_name) ++total;
    return total - element_type_index(node) + 1;
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
            if (dom_sel_matches(inner, node)) return false;
        } else if (ps.substr(0, 3) == "is(" || ps.substr(0, 6) == "where(") {
            bool is_where = (ps[0] == 'w');
            size_t start = is_where ? 6 : 3;
            std::string inner = ps.substr(start, ps.size() - start - 1);
            if (!dom_sel_matches(inner, node)) return false;
        } else if (ps.substr(0, 4) == "has(") {
            std::string inner = ps.substr(4, ps.size() - 5);
            // :has(> .x) means direct child, :has(.x) means descendant
            bool found = false;
            // Check if inner starts with '>' (direct child combinator)
            std::string trimmed = inner;
            size_t ts = 0;
            while (ts < trimmed.size() && trimmed[ts] == ' ') ++ts;
            if (ts < trimmed.size() && trimmed[ts] == '>') {
                // Direct child: match inner selector against children
                std::string child_sel = trimmed.substr(ts + 1);
                while (!child_sel.empty() && child_sel[0] == ' ') child_sel.erase(child_sel.begin());
                for (auto& c : node->children) {
                    if (c->node_type == DOMNode::ELEMENT && dom_sel_matches(child_sel, c.get())) {
                        found = true; break;
                    }
                }
            } else {
                // Descendant: search all descendants
                std::function<bool(DOMNode*)> search = [&](DOMNode* n) -> bool {
                    for (auto& c : n->children) {
                        if (c->node_type == DOMNode::ELEMENT) {
                            if (dom_sel_matches(inner, c.get())) return true;
                            if (search(c.get())) return true;
                        }
                    }
                    return false;
                };
                found = search(node);
            }
            if (!found) return false;
        } else if (ps == "required") {
            if (node->attributes.find("required") == node->attributes.end()) return false;
        } else if (ps == "optional") {
            static const std::set<std::string> form_tags = {"input","select","textarea"};
            if (form_tags.find(node->tag_name) == form_tags.end()) return false;
            if (node->attributes.find("required") != node->attributes.end()) return false;
        } else if (ps == "read-only") {
            // contentEditable=false elements are read-only
            auto ce = node->attributes.find("contenteditable");
            if (ce != node->attributes.end() && ce->second == "false") {
                // read-only
            } else if (ce != node->attributes.end() && (ce->second == "true" || ce->second == "")) {
                return false; // contentEditable=true is read-write, not read-only
            } else {
                // For form elements: readonly or disabled = read-only
                static const std::set<std::string> form_tags = {"input","select","textarea"};
                if (form_tags.find(node->tag_name) != form_tags.end()) {
                    if (node->attributes.find("readonly") == node->attributes.end() &&
                        node->attributes.find("disabled") == node->attributes.end()) return false;
                } else {
                    // Non-form, non-contentEditable elements are read-only by default
                    // but only if not inheriting contentEditable from parent
                    // For now, non-form elements without contentEditable are read-only
                }
            }
        } else if (ps == "read-write") {
            auto ce = node->attributes.find("contenteditable");
            if (ce != node->attributes.end() && (ce->second == "true" || ce->second == "")) {
                // contentEditable=true is read-write
            } else {
                static const std::set<std::string> form_tags = {"input","select","textarea"};
                if (form_tags.find(node->tag_name) == form_tags.end()) return false;
                if (node->attributes.find("readonly") != node->attributes.end()) return false;
                if (node->attributes.find("disabled") != node->attributes.end()) return false;
            }
        } else if (ps == "in-range") {
            auto mn = node->attributes.find("min");
            auto mx = node->attributes.find("max");
            auto vl = node->attributes.find("value");
            if (mn == node->attributes.end() && mx == node->attributes.end()) return false;
            if (vl == node->attributes.end()) return false;
            double v = 0; try { v = std::stod(vl->second); } catch(...) { return false; }
            if (mn != node->attributes.end()) { double lo = 0; try { lo = std::stod(mn->second); } catch(...) {} if (v < lo) return false; }
            if (mx != node->attributes.end()) { double hi = 0; try { hi = std::stod(mx->second); } catch(...) {} if (v > hi) return false; }
        } else if (ps == "out-of-range") {
            auto mn = node->attributes.find("min");
            auto mx = node->attributes.find("max");
            auto vl = node->attributes.find("value");
            if (mn == node->attributes.end() && mx == node->attributes.end()) return false;
            if (vl == node->attributes.end()) return false;
            double v = 0; try { v = std::stod(vl->second); } catch(...) { return false; }
            bool in_range = true;
            if (mn != node->attributes.end()) { double lo = 0; try { lo = std::stod(mn->second); } catch(...) {} if (v < lo) in_range = false; }
            if (mx != node->attributes.end()) { double hi = 0; try { hi = std::stod(mx->second); } catch(...) {} if (v > hi) in_range = false; }
            if (in_range) return false;
        } else if (ps == "valid") {
            static const std::set<std::string> form_tags = {"input","select","textarea","button","output","fieldset"};
            if (form_tags.find(node->tag_name) == form_tags.end()) return false;
            // Invalid if has custom validity error
            if (node->attributes.find("data-custom-validity") != node->attributes.end()) return false;
            // Invalid if required but empty value
            if (node->attributes.find("required") != node->attributes.end()) {
                auto vl = node->attributes.find("value");
                if (vl == node->attributes.end() || vl->second.empty()) return false;
            }
        } else if (ps == "invalid") {
            static const std::set<std::string> form_tags = {"input","select","textarea","button","output","fieldset"};
            if (form_tags.find(node->tag_name) == form_tags.end()) return false;
            bool is_invalid = false;
            if (node->attributes.find("data-custom-validity") != node->attributes.end()) is_invalid = true;
            if (!is_invalid && node->attributes.find("required") != node->attributes.end()) {
                auto vl = node->attributes.find("value");
                if (vl == node->attributes.end() || vl->second.empty()) is_invalid = true;
            }
            if (!is_invalid) return false;
        } else if (ps == "checked") {
            if (node->attributes.find("checked") == node->attributes.end()) return false;
        } else if (ps == "disabled") {
            if (node->attributes.find("disabled") == node->attributes.end()) return false;
        } else if (ps == "enabled") {
            if (node->attributes.find("disabled") != node->attributes.end()) return false;
        } else if (ps == "root") {
            if (node->tag_name != "html") return false;
            if (node->parent && node->parent->parent) return false;
        } else if (ps == "empty") {
            bool has_content = false;
            for (auto& c : node->children) {
                if (c->node_type == DOMNode::ELEMENT) { has_content = true; break; }
                if (c->node_type == DOMNode::TEXT && !c->text_content.empty()) { has_content = true; break; }
            }
            if (has_content) return false;
        } else if (ps == "only-child") {
            if (!node->parent) return false;
            int count = 0;
            for (auto& c : node->parent->children)
                if (c->node_type == DOMNode::ELEMENT) ++count;
            if (count != 1) return false;
        } else if (ps == "first-of-type") {
            if (!node->parent) return false;
            for (auto& c : node->parent->children) {
                if (c->node_type == DOMNode::ELEMENT && c->tag_name == node->tag_name) {
                    if (c.get() != node) return false;
                    break;
                }
            }
        } else if (ps == "last-of-type") {
            if (!node->parent) return false;
            bool found = false;
            for (int j = (int)node->parent->children.size() - 1; j >= 0; --j) {
                auto& c = node->parent->children[j];
                if (c->node_type == DOMNode::ELEMENT && c->tag_name == node->tag_name) {
                    if (c.get() != node) return false;
                    found = true; break;
                }
            }
            if (!found) return false;
        } else if (ps == "only-of-type") {
            if (!node->parent) return false;
            int count = 0;
            for (auto& c : node->parent->children)
                if (c->node_type == DOMNode::ELEMENT && c->tag_name == node->tag_name) ++count;
            if (count != 1) return false;
        } else if (ps.substr(0, 10) == "nth-child(") {
            std::string arg = ps.substr(10, ps.size() - 11);
            auto [a, b] = parse_anb(arg);
            if (!matches_anb(a, b, element_index(node))) return false;
        } else if (ps.substr(0, 15) == "nth-last-child(") {
            std::string arg = ps.substr(15, ps.size() - 16);
            auto [a, b] = parse_anb(arg);
            if (!matches_anb(a, b, element_index_last(node))) return false;
        } else if (ps.substr(0, 13) == "nth-of-type(") {
            std::string arg = ps.substr(13, ps.size() - 14);
            auto [a, b] = parse_anb(arg);
            if (!matches_anb(a, b, element_type_index(node))) return false;
        } else if (ps.substr(0, 18) == "nth-last-of-type(") {
            std::string arg = ps.substr(18, ps.size() - 19);
            auto [a, b] = parse_anb(arg);
            if (!matches_anb(a, b, element_type_index_last(node))) return false;
        }
        // other pseudo-classes: silently ignore (hover, focus, etc.)
    }

    return true;
}

// Match a single (non-comma-separated) complex selector against node
static bool dom_sel_matches_single(const std::string& sel, DOMNode* node) {
    if (sel.empty()) return false;

    // Tokenize: selectors and combinators
    // Combinators: ' ' (descendant), '>' (child), '+' (adjacent sibling), '~' (general sibling)
    enum Comb { DESC, CHILD, ADJ_SIB, GEN_SIB };
    struct Token { std::string selector; Comb combinator; };
    std::vector<Token> tokens;
    size_t i = 0, n = sel.size();
    while (i < n) {
        while (i < n && sel[i] == ' ') ++i;
        if (i >= n) break;
        // Check for combinator
        if (!tokens.empty() && (sel[i] == '>' || sel[i] == '+' || sel[i] == '~')) {
            Comb c = (sel[i] == '>') ? CHILD : (sel[i] == '+') ? ADJ_SIB : GEN_SIB;
            tokens.back().combinator = c;
            ++i;
            continue;
        }
        // Read selector part — skip inside brackets and parentheses
        size_t j = i;
        while (j < n && sel[j] != ' ' && sel[j] != '>' && sel[j] != '+' && sel[j] != '~') {
            if (sel[j] == '[') {
                int depth = 1; ++j;
                while (j < n && depth > 0) { if (sel[j]=='[') ++depth; else if (sel[j]==']') --depth; ++j; }
            } else if (sel[j] == '(') {
                int depth = 1; ++j;
                while (j < n && depth > 0) { if (sel[j]=='(') ++depth; else if (sel[j]==')') --depth; ++j; }
            } else {
                ++j;
            }
        }
        if (j > i) {
            tokens.push_back({sel.substr(i, j - i), DESC});
        }
        i = j;
    }
    if (tokens.empty()) return false;

    // Last token must match the node itself
    if (!dom_simple_match(tokens.back().selector, node)) return false;
    if (tokens.size() == 1) return true;

    // Match right-to-left using combinators
    int ti = (int)tokens.size() - 2;
    DOMNode* cur = node;

    while (ti >= 0 && cur) {
        Comb comb = tokens[ti].combinator;

        if (comb == DESC) {
            DOMNode* p = cur->parent;
            bool found = false;
            while (p) {
                if (dom_simple_match(tokens[ti].selector, p)) {
                    cur = p;
                    found = true;
                    break;
                }
                p = p->parent;
            }
            if (!found) return false;
        } else if (comb == CHILD) {
            DOMNode* p = cur->parent;
            if (!p || !dom_simple_match(tokens[ti].selector, p)) return false;
            cur = p;
        } else if (comb == ADJ_SIB) {
            if (!cur->parent) return false;
            auto& siblings = cur->parent->children;
            DOMNode* prev = nullptr;
            for (size_t si = 0; si < siblings.size(); si++) {
                if (siblings[si].get() == cur) break;
                if (siblings[si]->node_type == DOMNode::ELEMENT)
                    prev = siblings[si].get();
            }
            if (!prev || !dom_simple_match(tokens[ti].selector, prev)) return false;
            cur = prev;
        } else if (comb == GEN_SIB) {
            if (!cur->parent) return false;
            auto& siblings = cur->parent->children;
            bool found = false;
            for (size_t si = 0; si < siblings.size(); si++) {
                if (siblings[si].get() == cur) break;
                if (siblings[si]->node_type == DOMNode::ELEMENT &&
                    dom_simple_match(tokens[ti].selector, siblings[si].get())) {
                    cur = siblings[si].get();
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
        --ti;
    }
    return ti < 0;
}

// Split comma-separated selector list, respecting brackets and parens
static std::vector<std::string> split_selector_list(const std::string& sel) {
    std::vector<std::string> parts;
    size_t i = 0, n = sel.size(), start = 0;
    int bracket_depth = 0, paren_depth = 0;
    while (i < n) {
        if (sel[i] == '[') ++bracket_depth;
        else if (sel[i] == ']') --bracket_depth;
        else if (sel[i] == '(') ++paren_depth;
        else if (sel[i] == ')') --paren_depth;
        else if (sel[i] == ',' && bracket_depth == 0 && paren_depth == 0) {
            std::string part = sel.substr(start, i - start);
            // trim
            size_t a = 0, b = part.size();
            while (a < b && isspace((unsigned char)part[a])) ++a;
            while (b > a && isspace((unsigned char)part[b-1])) --b;
            if (a < b) parts.push_back(part.substr(a, b - a));
            start = i + 1;
        }
        ++i;
    }
    // last part
    std::string part = sel.substr(start);
    size_t a = 0, b = part.size();
    while (a < b && isspace((unsigned char)part[a])) ++a;
    while (b > a && isspace((unsigned char)part[b-1])) --b;
    if (a < b) parts.push_back(part.substr(a, b - a));
    return parts;
}

bool dom_sel_matches(const std::string& sel, DOMNode* node) {
    if (sel.empty()) return false;
    // Handle comma-separated selector lists
    auto parts = split_selector_list(sel);
    for (const auto& part : parts) {
        if (dom_sel_matches_single(part, node)) return true;
    }
    return false;
}

// Calculate specificity of a single (non-comma) selector
static Specificity calc_specificity_single(const std::string& sel) {
    Specificity spec;
    size_t i = 0, n = sel.size();
    while (i < n) {
        // Skip whitespace and combinators
        if (sel[i] == ' ' || sel[i] == '>' || sel[i] == '+' || sel[i] == '~') { ++i; continue; }
        if (sel[i] == '#') {
            spec.a++; ++i;
            while (i < n && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' && sel[i] != ':' &&
                   sel[i] != ' ' && sel[i] != '>' && sel[i] != '+' && sel[i] != '~') ++i;
        } else if (sel[i] == '.') {
            spec.b++; ++i;
            while (i < n && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' && sel[i] != ':' &&
                   sel[i] != ' ' && sel[i] != '>' && sel[i] != '+' && sel[i] != '~') ++i;
        } else if (sel[i] == '[') {
            spec.b++; ++i;
            int depth = 1;
            while (i < n && depth > 0) { if (sel[i]=='[') ++depth; else if (sel[i]==']') --depth; ++i; }
        } else if (sel[i] == ':') {
            ++i;
            bool is_double = (i < n && sel[i] == ':');
            if (is_double) ++i;
            // Read pseudo name
            size_t ps = i;
            while (i < n && sel[i] != '(' && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' &&
                   sel[i] != ':' && sel[i] != ' ' && sel[i] != '>' && sel[i] != '+' && sel[i] != '~') ++i;
            std::string pname = sel.substr(ps, i - ps);
            std::string parg;
            if (i < n && sel[i] == '(') {
                ++i; int d = 1; size_t as = i;
                while (i < n && d > 0) { if (sel[i]=='(') ++d; else if (sel[i]==')') --d; ++i; }
                parg = sel.substr(as, i - 1 - as);
            }
            if (is_double) {
                spec.c++; // pseudo-element
            } else if (pname == "where") {
                // :where() has zero specificity
            } else if (pname == "not" || pname == "is" || pname == "has") {
                // Specificity = most specific argument
                if (!parg.empty()) {
                    auto parts = split_selector_list(parg);
                    Specificity best;
                    for (auto& p : parts) {
                        Specificity s = calc_specificity_single(p);
                        if (best < s) best = s;
                    }
                    spec.a += best.a; spec.b += best.b; spec.c += best.c;
                }
            } else {
                spec.b++; // regular pseudo-class
            }
        } else if (sel[i] == '*') {
            ++i; // universal selector: zero specificity
        } else {
            // tag name
            spec.c++;
            while (i < n && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' && sel[i] != ':' &&
                   sel[i] != ' ' && sel[i] != '>' && sel[i] != '+' && sel[i] != '~') ++i;
        }
    }
    return spec;
}

Specificity calc_specificity(const std::string& selector) {
    // For comma-separated lists, return the highest specificity
    auto parts = split_selector_list(selector);
    Specificity best;
    for (auto& p : parts) {
        Specificity s = calc_specificity_single(p);
        if (best < s) best = s;
    }
    return best;
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
    if (on_mutation) on_mutation(parent->node_id, "childList");
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
            if (on_mutation) on_mutation(parent->node_id, "childList");
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
            if (on_mutation) on_mutation(parent->node_id, "childList");
            return;
        }
    }
    // refChild not found, append
    siblings.push_back(newChild);
    registerNode(newChild.get());
    parent->markDirty();
    if (on_mutated) on_mutated();
    if (on_mutation) on_mutation(parent->node_id, "childList");
}
