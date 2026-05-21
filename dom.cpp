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
        "link","meta","param","source","track","wbr", nullptr
    };
    for (const char** p = voids; *p; ++p)
        if (tag == *p) return true;
    return false;
}

void DOMNode::setInnerHTML(const std::string& html, uint32_t& next_id,
                           std::unordered_map<uint32_t, DOMNode*>& node_map,
                           std::unordered_map<std::string, DOMNode*>& id_map) {
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

            node_map[elem->node_id] = elem.get();
            if (!elem->id.empty()) id_map[elem->id] = elem.get();

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
            }
        }
    }
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

bool dom_simple_match(const std::string& raw, DOMNode* node) {
    if (raw.empty() || raw == "*") return true;
    if (node->node_type != DOMNode::ELEMENT) return false;

    // strip pseudo-class
    std::string tok = raw;
    size_t colon = tok.find(':');
    if (colon != std::string::npos) tok = tok.substr(0, colon);
    if (tok.empty()) return true;

    if (tok[0] == '#') return node->id == tok.substr(1);

    // parse tag.class1.class2
    std::string tag_part;
    std::vector<std::string> req_cls;
    size_t i = 0;
    if (tok[i] != '.') {
        size_t d = tok.find('.');
        tag_part = tok.substr(0, d);
        if (d != std::string::npos) i = d + 1; else i = tok.size();
    } else {
        ++i;
    }
    while (i <= tok.size()) {
        size_t d = tok.find('.', i);
        std::string c = tok.substr(i, d == std::string::npos ? std::string::npos : d - i);
        if (!c.empty()) req_cls.push_back(c);
        if (d == std::string::npos) break;
        i = d + 1;
    }

    if (!tag_part.empty() && node->tag_name != tag_part) return false;
    for (const auto& c : req_cls)
        if (!node->hasClass(c)) return false;
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
