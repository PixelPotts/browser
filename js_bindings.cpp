#include "js_bindings.h"
#include "js_engine.h"
#include "dom.h"
#include <cstdio>
#include <cstring>

extern "C" {
#include "quickjs.h"
}

// ---- Class IDs ----

static JSClassID js_element_class_id = 0;
static JSClassID js_nodelist_class_id = 0;
static JSClassID js_style_class_id = 0;
static JSClassID js_classlist_class_id = 0;

// Opaque: store node_id (not raw pointer) for safety
struct ElementOpaque {
    uint32_t node_id;
};

struct NodeListOpaque {
    std::vector<uint32_t> node_ids;
};

struct StyleOpaque {
    uint32_t node_id;
};

struct ClassListOpaque {
    uint32_t node_id;
};

// ---- Helpers ----

static DOMNode* get_node_by_id(uint32_t node_id) {
    if (!g_js_engine || !g_js_engine->document) return nullptr;
    auto it = g_js_engine->document->node_map.find(node_id);
    return it != g_js_engine->document->node_map.end() ? it->second : nullptr;
}

DOMNode* js_get_node(JSContext* ctx, JSValueConst val) {
    auto* op = (ElementOpaque*)JS_GetOpaque(val, js_element_class_id);
    if (!op) return nullptr;
    return get_node_by_id(op->node_id);
}

JSValue js_wrap_node(JSContext* ctx, DOMNode* node) {
    if (!node) return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, js_element_class_id);
    auto* op = new ElementOpaque{node->node_id};
    JS_SetOpaque(obj, op);
    return obj;
}

static JSValue js_wrap_nodelist(JSContext* ctx, const std::vector<DOMNode*>& nodes) {
    JSValue obj = JS_NewObjectClass(ctx, js_nodelist_class_id);
    auto* op = new NodeListOpaque;
    for (auto* n : nodes) op->node_ids.push_back(n->node_id);
    JS_SetOpaque(obj, op);
    // Set length property
    JS_SetPropertyStr(ctx, obj, "length", JS_NewInt32(ctx, (int)nodes.size()));
    // Set numeric indices
    for (size_t i = 0; i < nodes.size(); i++) {
        JS_SetPropertyUint32(ctx, obj, (uint32_t)i, js_wrap_node(ctx, nodes[i]));
    }
    return obj;
}

static JSValue js_wrap_style(JSContext* ctx, DOMNode* node) {
    if (!node) return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, js_style_class_id);
    auto* op = new StyleOpaque{node->node_id};
    JS_SetOpaque(obj, op);
    return obj;
}

static JSValue js_wrap_classlist(JSContext* ctx, DOMNode* node) {
    if (!node) return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, js_classlist_class_id);
    auto* op = new ClassListOpaque{node->node_id};
    JS_SetOpaque(obj, op);
    return obj;
}

// ---- Element finalizer ----

static void js_element_finalizer(JSRuntime* rt, JSValue val) {
    auto* op = (ElementOpaque*)JS_GetOpaque(val, js_element_class_id);
    delete op;
}

static void js_nodelist_finalizer(JSRuntime* rt, JSValue val) {
    auto* op = (NodeListOpaque*)JS_GetOpaque(val, js_nodelist_class_id);
    delete op;
}

static void js_style_finalizer(JSRuntime* rt, JSValue val) {
    auto* op = (StyleOpaque*)JS_GetOpaque(val, js_style_class_id);
    delete op;
}

static void js_classlist_finalizer(JSRuntime* rt, JSValue val) {
    auto* op = (ClassListOpaque*)JS_GetOpaque(val, js_classlist_class_id);
    delete op;
}

// ---- Element getters/setters ----

static JSValue js_element_get_id(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node) return JS_UNDEFINED;
    return JS_NewString(ctx, node->id.c_str());
}

static JSValue js_noop_setter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static JSValue js_element_set_id(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    if (s) {
        if (!node->id.empty() && g_js_engine && g_js_engine->document)
            g_js_engine->document->id_map.erase(node->id);
        node->id = s;
        if (!node->id.empty() && g_js_engine && g_js_engine->document)
            g_js_engine->document->id_map[node->id] = node;
        JS_FreeCString(ctx, s);
        node->markDirty();
        if (g_js_engine) g_js_engine->scheduleRerender();
    }
    return JS_UNDEFINED;
}

static JSValue js_element_get_tagName(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node) return JS_UNDEFINED;
    std::string upper = node->tag_name;
    for (auto& c : upper) c = (char)toupper((unsigned char)c);
    return JS_NewString(ctx, upper.c_str());
}

static JSValue js_element_get_nodeName(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node) return JS_UNDEFINED;
    if (node->node_type == DOMNode::TEXT) return JS_NewString(ctx, "#text");
    std::string upper = node->tag_name;
    for (auto& c : upper) c = (char)toupper((unsigned char)c);
    return JS_NewString(ctx, upper.c_str());
}

static JSValue js_element_get_nodeType(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node) return JS_UNDEFINED;
    return JS_NewInt32(ctx, (int)node->node_type);
}

static JSValue js_element_get_textContent(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node) return JS_UNDEFINED;
    return JS_NewString(ctx, node->getTextContent().c_str());
}

static JSValue js_element_set_textContent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || !g_js_engine || !g_js_engine->document || argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    if (s) {
        node->setTextContent(s, g_js_engine->document->next_id);
        JS_FreeCString(ctx, s);
        g_js_engine->scheduleRerender();
    }
    return JS_UNDEFINED;
}

static JSValue js_element_get_innerHTML(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node) return JS_UNDEFINED;
    return JS_NewString(ctx, node->getInnerHTML().c_str());
}

static JSValue js_element_set_innerHTML(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || !g_js_engine || !g_js_engine->document || argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    if (s) {
        Document* doc = g_js_engine->document;
        node->setInnerHTML(s, doc->next_id, doc->node_map, doc->id_map);
        JS_FreeCString(ctx, s);
        g_js_engine->scheduleRerender();
    }
    return JS_UNDEFINED;
}

static JSValue js_element_get_className(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node) return JS_UNDEFINED;
    std::string result;
    for (size_t i = 0; i < node->class_list.size(); i++) {
        if (i > 0) result += " ";
        result += node->class_list[i];
    }
    return JS_NewString(ctx, result.c_str());
}

static JSValue js_element_set_className(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    if (s) {
        node->class_list.clear();
        std::string cls;
        for (const char* p = s; *p; ++p) {
            if (*p == ' ' || *p == '\t') {
                if (!cls.empty()) { node->class_list.push_back(cls); cls.clear(); }
            } else {
                cls += (char)tolower((unsigned char)*p);
            }
        }
        if (!cls.empty()) node->class_list.push_back(cls);
        JS_FreeCString(ctx, s);
        node->markDirty();
        if (g_js_engine) g_js_engine->scheduleRerender();
    }
    return JS_UNDEFINED;
}

static JSValue js_element_get_parentNode(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || !node->parent) return JS_NULL;
    return js_wrap_node(ctx, node->parent);
}

static JSValue js_element_get_children(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node) return JS_NULL;
    std::vector<DOMNode*> elems;
    for (auto& c : node->children)
        if (c->node_type == DOMNode::ELEMENT) elems.push_back(c.get());
    return js_wrap_nodelist(ctx, elems);
}

static JSValue js_element_get_childNodes(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node) return JS_NULL;
    std::vector<DOMNode*> all;
    for (auto& c : node->children) all.push_back(c.get());
    return js_wrap_nodelist(ctx, all);
}

static JSValue js_element_get_firstChild(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || node->children.empty()) return JS_NULL;
    return js_wrap_node(ctx, node->children.front().get());
}

static JSValue js_element_get_lastChild(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || node->children.empty()) return JS_NULL;
    return js_wrap_node(ctx, node->children.back().get());
}

static JSValue js_element_get_nextSibling(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || !node->parent) return JS_NULL;
    auto& siblings = node->parent->children;
    for (size_t i = 0; i < siblings.size(); i++) {
        if (siblings[i].get() == node && i + 1 < siblings.size())
            return js_wrap_node(ctx, siblings[i+1].get());
    }
    return JS_NULL;
}

static JSValue js_element_get_style(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    return js_wrap_style(ctx, node);
}

static JSValue js_element_get_classList(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    return js_wrap_classlist(ctx, node);
}

// ---- Form element properties ----

static JSValue js_element_get_value(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node) return JS_NewString(ctx, "");
    auto it = node->attributes.find("value");
    return JS_NewString(ctx, it != node->attributes.end() ? it->second.c_str() : "");
}

static JSValue js_element_set_value(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    if (s) { node->attributes["value"] = s; JS_FreeCString(ctx, s); }
    node->markDirty();
    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

static JSValue js_element_get_checked(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node) return JS_FALSE;
    return JS_NewBool(ctx, node->attributes.count("checked") > 0);
}

static JSValue js_element_set_checked(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || argc < 1) return JS_UNDEFINED;
    int val = JS_ToBool(ctx, argv[0]);
    if (val) node->attributes["checked"] = "checked";
    else node->attributes.erase("checked");
    node->markDirty();
    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

static JSValue js_element_get_disabled(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node) return JS_FALSE;
    return JS_NewBool(ctx, node->attributes.count("disabled") > 0);
}

static JSValue js_element_set_disabled(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || argc < 1) return JS_UNDEFINED;
    int val = JS_ToBool(ctx, argv[0]);
    if (val) node->attributes["disabled"] = "disabled";
    else node->attributes.erase("disabled");
    node->markDirty();
    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

static JSValue js_element_get_selectedIndex(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node) return JS_NewInt32(ctx, -1);
    auto it = node->attributes.find("selectedindex");
    if (it != node->attributes.end()) {
        try { return JS_NewInt32(ctx, std::stoi(it->second)); } catch(...) {}
    }
    return JS_NewInt32(ctx, 0);
}

static JSValue js_element_set_selectedIndex(JSContext* ctx, JSValueConst this_val,
                                             int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || argc < 1) return JS_UNDEFINED;
    int32_t idx;
    JS_ToInt32(ctx, &idx, argv[0]);
    node->attributes["selectedindex"] = std::to_string(idx);
    // Update value to match selected option
    int i = 0;
    for (auto& c : node->children) {
        if (c->tag_name == "option") {
            if (i == idx) {
                auto vit = c->attributes.find("value");
                node->attributes["value"] = vit != c->attributes.end() ? vit->second : c->getTextContent();
                break;
            }
            ++i;
        }
    }
    node->markDirty();
    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

static JSValue js_element_get_type(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node) return JS_NewString(ctx, "");
    auto it = node->attributes.find("type");
    if (it != node->attributes.end()) return JS_NewString(ctx, it->second.c_str());
    if (node->tag_name == "input") return JS_NewString(ctx, "text");
    if (node->tag_name == "textarea") return JS_NewString(ctx, "textarea");
    if (node->tag_name == "select") return JS_NewString(ctx, "select-one");
    return JS_NewString(ctx, "");
}

// ---- Element methods ----

static JSValue js_element_getAttribute(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || argc < 1) return JS_NULL;
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;
    std::string key(name);
    JS_FreeCString(ctx, name);
    if (key == "id") return JS_NewString(ctx, node->id.c_str());
    if (key == "class" || key == "className") {
        std::string r;
        for (size_t i = 0; i < node->class_list.size(); i++) {
            if (i > 0) r += " ";
            r += node->class_list[i];
        }
        return JS_NewString(ctx, r.c_str());
    }
    auto it = node->attributes.find(key);
    if (it != node->attributes.end()) return JS_NewString(ctx, it->second.c_str());
    return JS_NULL;
}

static JSValue js_element_setAttribute(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || argc < 2) return JS_UNDEFINED;
    const char* name = JS_ToCString(ctx, argv[0]);
    const char* val = JS_ToCString(ctx, argv[1]);
    if (name && val) {
        node->attributes[name] = val;
        if (std::string(name) == "id") {
            node->id = val;
            if (g_js_engine && g_js_engine->document)
                g_js_engine->document->id_map[val] = node;
        }
        node->markDirty();
        if (g_js_engine) g_js_engine->scheduleRerender();
    }
    if (name) JS_FreeCString(ctx, name);
    if (val) JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

static JSValue js_element_removeAttribute(JSContext* ctx, JSValueConst this_val,
                                           int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || argc < 1) return JS_UNDEFINED;
    const char* name = JS_ToCString(ctx, argv[0]);
    if (name) {
        node->attributes.erase(name);
        JS_FreeCString(ctx, name);
        node->markDirty();
        if (g_js_engine) g_js_engine->scheduleRerender();
    }
    return JS_UNDEFINED;
}

static JSValue js_element_appendChild(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    DOMNode* parent = js_get_node(ctx, this_val);
    if (!parent || argc < 1 || !g_js_engine || !g_js_engine->document) return JS_UNDEFINED;
    DOMNode* child = js_get_node(ctx, argv[0]);
    if (!child) return JS_UNDEFINED;

    // Find the shared_ptr for this child
    auto& doc = g_js_engine->document;
    std::shared_ptr<DOMNode> child_ptr;

    // Search in old parent's children
    if (child->parent) {
        for (auto& sp : child->parent->children) {
            if (sp.get() == child) { child_ptr = sp; break; }
        }
    }
    // If not found, search in orphans list (newly created elements)
    if (!child_ptr) {
        for (auto& sp : doc->orphans) {
            if (sp.get() == child) { child_ptr = sp; break; }
        }
    }

    if (child_ptr) {
        doc->appendChild(parent, child_ptr);
    }

    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_element_removeChild(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    DOMNode* parent = js_get_node(ctx, this_val);
    if (!parent || argc < 1 || !g_js_engine || !g_js_engine->document) return JS_UNDEFINED;
    DOMNode* child = js_get_node(ctx, argv[0]);
    if (!child) return JS_UNDEFINED;
    g_js_engine->document->removeChild(parent, child);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_element_insertBefore(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    DOMNode* parent = js_get_node(ctx, this_val);
    if (!parent || argc < 1 || !g_js_engine || !g_js_engine->document) return JS_UNDEFINED;
    DOMNode* newChild = js_get_node(ctx, argv[0]);
    if (!newChild) return JS_UNDEFINED;
    DOMNode* refChild = (argc >= 2 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]))
                        ? js_get_node(ctx, argv[1]) : nullptr;

    auto& doc = g_js_engine->document;
    std::shared_ptr<DOMNode> child_ptr;
    if (newChild->parent) {
        for (auto& sp : newChild->parent->children)
            if (sp.get() == newChild) { child_ptr = sp; break; }
    }
    if (!child_ptr) {
        for (auto& sp : doc->orphans)
            if (sp.get() == newChild) { child_ptr = sp; break; }
    }
    if (child_ptr) {
        doc->insertBefore(parent, child_ptr, refChild);
    }
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_element_replaceChild(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    DOMNode* parent = js_get_node(ctx, this_val);
    if (!parent || argc < 2 || !g_js_engine || !g_js_engine->document) return JS_UNDEFINED;
    DOMNode* newChild = js_get_node(ctx, argv[0]);
    DOMNode* oldChild = js_get_node(ctx, argv[1]);
    if (!newChild || !oldChild) return JS_UNDEFINED;

    auto& doc = g_js_engine->document;
    std::shared_ptr<DOMNode> new_ptr;
    if (newChild->parent) {
        for (auto& sp : newChild->parent->children)
            if (sp.get() == newChild) { new_ptr = sp; break; }
    }
    if (!new_ptr) {
        for (auto& sp : doc->orphans)
            if (sp.get() == newChild) { new_ptr = sp; break; }
    }
    if (new_ptr) {
        doc->insertBefore(parent, new_ptr, oldChild);
        doc->removeChild(parent, oldChild);
    }
    return JS_DupValue(ctx, argv[1]);
}

static JSValue js_element_getElementsByTagName(JSContext* ctx, JSValueConst this_val,
                                                int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || argc < 1) return js_wrap_nodelist(ctx, {});
    const char* tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return js_wrap_nodelist(ctx, {});
    std::string ltag = tag;
    for (auto& c : ltag) c = tolower((unsigned char)c);
    JS_FreeCString(ctx, tag);
    bool match_all = (ltag == "*");
    std::vector<DOMNode*> results;
    std::function<void(DOMNode*)> search = [&](DOMNode* n) {
        if (n != node && n->node_type == DOMNode::ELEMENT &&
            (match_all || n->tag_name == ltag))
            results.push_back(n);
        for (auto& c : n->children) search(c.get());
    };
    for (auto& c : node->children) search(c.get());
    return js_wrap_nodelist(ctx, results);
}

static JSValue js_element_getElementsByClassName(JSContext* ctx, JSValueConst this_val,
                                                  int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || argc < 1) return js_wrap_nodelist(ctx, {});
    const char* cls = JS_ToCString(ctx, argv[0]);
    if (!cls) return js_wrap_nodelist(ctx, {});
    std::string lcls = cls;
    for (auto& c : lcls) c = tolower((unsigned char)c);
    JS_FreeCString(ctx, cls);
    std::vector<DOMNode*> results;
    std::function<void(DOMNode*)> search = [&](DOMNode* n) {
        if (n != node && n->node_type == DOMNode::ELEMENT && n->hasClass(lcls))
            results.push_back(n);
        for (auto& c : n->children) search(c.get());
    };
    for (auto& c : node->children) search(c.get());
    return js_wrap_nodelist(ctx, results);
}

static JSValue js_element_querySelector(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || argc < 1) return JS_NULL;
    const char* sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return JS_NULL;
    // Search within this node's subtree
    std::vector<DOMNode*> results;
    std::function<void(DOMNode*)> search = [&](DOMNode* n) {
        if (n != node && n->node_type == DOMNode::ELEMENT && dom_sel_matches(sel, n)) {
            results.push_back(n);
            return;
        }
        for (auto& c : n->children) search(c.get());
    };
    for (auto& c : node->children) {
        search(c.get());
        if (!results.empty()) break;
    }
    JS_FreeCString(ctx, sel);
    return results.empty() ? JS_NULL : js_wrap_node(ctx, results[0]);
}

static JSValue js_element_querySelectorAll(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || argc < 1) return JS_NULL;
    const char* sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return JS_NULL;
    std::vector<DOMNode*> results;
    std::function<void(DOMNode*)> search = [&](DOMNode* n) {
        if (n != node && n->node_type == DOMNode::ELEMENT && dom_sel_matches(sel, n))
            results.push_back(n);
        for (auto& c : n->children) search(c.get());
    };
    for (auto& c : node->children) search(c.get());
    JS_FreeCString(ctx, sel);
    return js_wrap_nodelist(ctx, results);
}

static JSValue js_element_addEventListener(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv);
static JSValue js_element_removeEventListener(JSContext* ctx, JSValueConst this_val,
                                               int argc, JSValueConst* argv);

// ---- Style proxy (element.style.X) ----

// For style, we use common CSS properties as getter/setter via magic

static std::string camelToKebab(const char* name) {
    std::string result;
    for (const char* p = name; *p; ++p) {
        if (isupper((unsigned char)*p)) {
            result += '-';
            result += (char)tolower((unsigned char)*p);
        } else {
            result += *p;
        }
    }
    return result;
}

// Generic style getter via magic (magic = index into property name table)
static const char* STYLE_PROPS[] = {
    "color", "backgroundColor", "fontSize", "fontWeight", "display",
    "margin", "marginTop", "marginRight", "marginBottom", "marginLeft",
    "padding", "paddingTop", "paddingRight", "paddingBottom", "paddingLeft",
    "width", "height", "maxWidth", "border", "borderRadius",
    "textAlign", "visibility", "opacity", "position", "overflow",
    "cursor", "float", "lineHeight", "textTransform", "fontFamily",
    "boxShadow", "flexDirection", "justifyContent", "alignItems",
    "flexWrap", "gap", "top", "left", "right", "bottom", "zIndex",
    "fontStyle", "letterSpacing", "wordSpacing", "textDecoration",
    "textDecorationColor", "textDecorationStyle", "textDecorationLine",
    "fontVariant", "whiteSpace", "textIndent", "textOverflow",
    "textShadow", "fontStretch",
    nullptr
};

static JSValue js_style_getter_magic(JSContext* ctx, JSValueConst this_val, int magic) {
    auto* op = (StyleOpaque*)JS_GetOpaque(this_val, js_style_class_id);
    if (!op) return JS_UNDEFINED;
    DOMNode* node = get_node_by_id(op->node_id);
    if (!node || !STYLE_PROPS[magic]) return JS_NewString(ctx, "");
    std::string key = camelToKebab(STYLE_PROPS[magic]);
    auto it = node->style_props.find(key);
    if (it != node->style_props.end())
        return JS_NewString(ctx, it->second.c_str());
    return JS_NewString(ctx, "");
}

static JSValue js_style_setter_magic(JSContext* ctx, JSValueConst this_val,
                                      JSValueConst val, int magic) {
    auto* op = (StyleOpaque*)JS_GetOpaque(this_val, js_style_class_id);
    if (!op) return JS_UNDEFINED;
    DOMNode* node = get_node_by_id(op->node_id);
    if (!node || !STYLE_PROPS[magic]) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    if (s) {
        std::string key = camelToKebab(STYLE_PROPS[magic]);
        node->style_props[key] = s;
        JS_FreeCString(ctx, s);
        node->markDirty();
        if (g_js_engine) g_js_engine->scheduleRerender();
    }
    return JS_UNDEFINED;
}

// ---- ClassList methods ----

static JSValue js_classlist_add(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* op = (ClassListOpaque*)JS_GetOpaque(this_val, js_classlist_class_id);
    if (!op) return JS_UNDEFINED;
    DOMNode* node = get_node_by_id(op->node_id);
    if (!node) return JS_UNDEFINED;
    for (int i = 0; i < argc; i++) {
        const char* s = JS_ToCString(ctx, argv[i]);
        if (s) { node->addClass(s); JS_FreeCString(ctx, s); }
    }
    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

static JSValue js_classlist_remove(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* op = (ClassListOpaque*)JS_GetOpaque(this_val, js_classlist_class_id);
    if (!op) return JS_UNDEFINED;
    DOMNode* node = get_node_by_id(op->node_id);
    if (!node) return JS_UNDEFINED;
    for (int i = 0; i < argc; i++) {
        const char* s = JS_ToCString(ctx, argv[i]);
        if (s) { node->removeClass(s); JS_FreeCString(ctx, s); }
    }
    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

static JSValue js_classlist_toggle(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* op = (ClassListOpaque*)JS_GetOpaque(this_val, js_classlist_class_id);
    if (!op || argc < 1) return JS_UNDEFINED;
    DOMNode* node = get_node_by_id(op->node_id);
    if (!node) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_UNDEFINED;
    bool result = node->toggleClass(s);
    JS_FreeCString(ctx, s);
    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_NewBool(ctx, result);
}

static JSValue js_classlist_contains(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* op = (ClassListOpaque*)JS_GetOpaque(this_val, js_classlist_class_id);
    if (!op || argc < 1) return JS_FALSE;
    DOMNode* node = get_node_by_id(op->node_id);
    if (!node) return JS_FALSE;
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_FALSE;
    bool result = node->hasClass(s);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, result);
}

// ---- document object ----

static JSValue js_doc_getElementById(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    if (!g_js_engine || !g_js_engine->document || argc < 1) return JS_NULL;
    const char* id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;
    DOMNode* node = g_js_engine->document->getElementById(id);
    JS_FreeCString(ctx, id);
    return node ? js_wrap_node(ctx, node) : JS_NULL;
}

static JSValue js_doc_querySelector(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    if (!g_js_engine || !g_js_engine->document || argc < 1) return JS_NULL;
    const char* sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return JS_NULL;
    DOMNode* node = g_js_engine->document->querySelector(sel);
    JS_FreeCString(ctx, sel);
    return node ? js_wrap_node(ctx, node) : JS_NULL;
}

static JSValue js_doc_querySelectorAll(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    if (!g_js_engine || !g_js_engine->document || argc < 1) return JS_NULL;
    const char* sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return JS_NULL;
    auto nodes = g_js_engine->document->querySelectorAll(sel);
    JS_FreeCString(ctx, sel);
    return js_wrap_nodelist(ctx, nodes);
}

static JSValue js_doc_createElement(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    if (!g_js_engine || !g_js_engine->document || argc < 1) return JS_NULL;
    const char* tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return JS_NULL;
    auto node = g_js_engine->document->createElement(tag);
    JS_FreeCString(ctx, tag);
    // Store the shared_ptr so it doesn't get freed
    // We attach it as user data on the document
    return js_wrap_node(ctx, node.get());
}

static JSValue js_doc_createTextNode(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    if (!g_js_engine || !g_js_engine->document || argc < 1) return JS_NULL;
    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_NULL;
    auto node = g_js_engine->document->createTextNode(text);
    JS_FreeCString(ctx, text);
    return js_wrap_node(ctx, node.get());
}

static JSValue js_doc_get_body(JSContext* ctx, JSValueConst this_val) {
    if (!g_js_engine || !g_js_engine->document || !g_js_engine->document->body)
        return JS_NULL;
    return js_wrap_node(ctx, g_js_engine->document->body);
}

static JSValue js_doc_get_head(JSContext* ctx, JSValueConst this_val) {
    if (!g_js_engine || !g_js_engine->document || !g_js_engine->document->head)
        return JS_NULL;
    return js_wrap_node(ctx, g_js_engine->document->head);
}

static JSValue js_doc_get_documentElement(JSContext* ctx, JSValueConst this_val) {
    if (!g_js_engine || !g_js_engine->document)
        return JS_NULL;
    return js_wrap_node(ctx, g_js_engine->document->root.get());
}

static JSValue js_doc_get_currentScript(JSContext* ctx, JSValueConst this_val) {
    if (!g_js_engine || !g_js_engine->has_current_script || !g_js_engine->current_script_node)
        return JS_NULL;
    return js_wrap_node(ctx, g_js_engine->current_script_node);
}

// ---- NodeList forEach ----

static JSValue js_nodelist_forEach(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    JSValue len_val = JS_GetPropertyStr(ctx, this_val, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);

    for (int32_t i = 0; i < len; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, this_val, (uint32_t)i);
        JSValue idx = JS_NewInt32(ctx, i);
        JSValue args[3] = {elem, idx, JS_DupValue(ctx, this_val)};
        JSValue ret = JS_Call(ctx, argv[0], JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, elem);
        JS_FreeValue(ctx, idx);
        JS_FreeValue(ctx, args[2]);
    }
    return JS_UNDEFINED;
}

// ---- addEventListener / removeEventListener stubs ----

// These will be connected to the event system in Phase 7
static JSValue js_element_addEventListener(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || argc < 2 || !g_js_engine) return JS_UNDEFINED;
    const char* type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    JSValue func = argv[1];
    if (!JS_IsFunction(ctx, func)) {
        JS_FreeCString(ctx, type);
        return JS_UNDEFINED;
    }

    // Store handler: use a unique ID based on the JS value
    // We'll store the dup'd function value as handler_id (use next_id from document)
    uint32_t handler_id = g_js_engine->document ? g_js_engine->document->next_id++ : 0;
    DOMNode::Listener listener;
    listener.type = type;
    listener.handler_id = handler_id;
    node->listeners.push_back(listener);
    fprintf(stderr, "[DEBUG addEventListener] node_id=%u tag=<%s> id='%s' type='%s' handler=%u total_listeners=%zu addr=%p\n",
            node->node_id, node->tag_name.c_str(), node->id.c_str(), type, handler_id, node->listeners.size(), (void*)node);

    // Store the actual JS function globally so we can call it later
    // Use a property on the global object keyed by handler_id
    JSValue global = JS_GetGlobalObject(ctx);
    std::string key = "__handler_" + std::to_string(handler_id);
    JS_SetPropertyStr(ctx, global, key.c_str(), JS_DupValue(ctx, func));
    JS_FreeValue(ctx, global);

    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

static JSValue js_element_removeEventListener(JSContext* ctx, JSValueConst this_val,
                                               int argc, JSValueConst* argv) {
    DOMNode* node = js_get_node(ctx, this_val);
    if (!node || argc < 2) return JS_UNDEFINED;
    const char* type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;

    // Remove first listener of this type
    for (auto it = node->listeners.begin(); it != node->listeners.end(); ++it) {
        if (it->type == type) {
            // Clean up stored handler
            JSValue global = JS_GetGlobalObject(ctx);
            std::string key = "__handler_" + std::to_string(it->handler_id);
            JSAtom atom = JS_NewAtom(ctx, key.c_str());
            JS_DeleteProperty(ctx, global, atom, 0);
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, global);
            node->listeners.erase(it);
            break;
        }
    }

    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

// ---- Class definitions and prototypes ----

static const JSClassDef js_element_class_def = {
    "Element", js_element_finalizer, nullptr, nullptr, nullptr
};

static const JSClassDef js_nodelist_class_def = {
    "NodeList", js_nodelist_finalizer, nullptr, nullptr, nullptr
};

static const JSClassDef js_style_class_def = {
    "CSSStyleDeclaration", js_style_finalizer, nullptr, nullptr, nullptr
};

static const JSClassDef js_classlist_class_def = {
    "DOMTokenList", js_classlist_finalizer, nullptr, nullptr, nullptr
};

// ---- Initialize bindings ----

void js_bindings_init(JSEngine* engine) {
    JSContext* ctx = engine->ctx;
    JSRuntime* rt = engine->rt;

    // Register class IDs
    JS_NewClassID(&js_element_class_id);
    JS_NewClassID(&js_nodelist_class_id);
    JS_NewClassID(&js_style_class_id);
    JS_NewClassID(&js_classlist_class_id);

    JS_NewClass(rt, js_element_class_id, &js_element_class_def);
    JS_NewClass(rt, js_nodelist_class_id, &js_nodelist_class_def);
    JS_NewClass(rt, js_style_class_id, &js_style_class_def);
    JS_NewClass(rt, js_classlist_class_id, &js_classlist_class_def);

    // Element prototype
    JSValue elem_proto = JS_NewObject(ctx);

    // Getters/setters
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "id"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_id, "get id", 0),
        JS_NewCFunction(ctx, js_element_set_id, "set id", 1),
        JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "tagName"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_tagName, "get tagName", 0),
        JS_NewCFunction(ctx, js_noop_setter, "set tagName", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "nodeName"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_nodeName, "get nodeName", 0),
        JS_NewCFunction(ctx, js_noop_setter, "set nodeName", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "nodeType"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_nodeType, "get nodeType", 0),
        JS_NewCFunction(ctx, js_noop_setter, "set nodeType", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "textContent"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_textContent, "get textContent", 0),
        JS_NewCFunction(ctx, js_element_set_textContent, "set textContent", 1),
        JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "innerHTML"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_innerHTML, "get innerHTML", 0),
        JS_NewCFunction(ctx, js_element_set_innerHTML, "set innerHTML", 1),
        JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "className"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_className, "get className", 0),
        JS_NewCFunction(ctx, js_element_set_className, "set className", 1),
        JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "parentNode"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_parentNode, "get parentNode", 0),
        JS_NewCFunction(ctx, js_noop_setter, "set parentNode", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "parentElement"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_parentNode, "get parentElement", 0),
        JS_NewCFunction(ctx, js_noop_setter, "set parentElement", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "children"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_children, "get children", 0),
        JS_NewCFunction(ctx, js_noop_setter, "set children", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "childNodes"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_childNodes, "get childNodes", 0),
        JS_NewCFunction(ctx, js_noop_setter, "set childNodes", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "firstChild"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_firstChild, "get firstChild", 0),
        JS_NewCFunction(ctx, js_noop_setter, "set firstChild", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "lastChild"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_lastChild, "get lastChild", 0),
        JS_NewCFunction(ctx, js_noop_setter, "set lastChild", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "nextSibling"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_nextSibling, "get nextSibling", 0),
        JS_NewCFunction(ctx, js_noop_setter, "set nextSibling", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "style"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_style, "get style", 0),
        JS_NewCFunction(ctx, js_noop_setter, "set style", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "classList"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_classList, "get classList", 0),
        JS_NewCFunction(ctx, js_noop_setter, "set classList", 1), JS_PROP_CONFIGURABLE);

    // Form properties
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "value"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_value, "get value", 0),
        JS_NewCFunction(ctx, js_element_set_value, "set value", 1),
        JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "checked"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_checked, "get checked", 0),
        JS_NewCFunction(ctx, js_element_set_checked, "set checked", 1),
        JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "disabled"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_disabled, "get disabled", 0),
        JS_NewCFunction(ctx, js_element_set_disabled, "set disabled", 1),
        JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "selectedIndex"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_selectedIndex, "get selectedIndex", 0),
        JS_NewCFunction(ctx, js_element_set_selectedIndex, "set selectedIndex", 1),
        JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, elem_proto,
        JS_NewAtom(ctx, "type"),
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_type, "get type", 0),
        JS_NewCFunction(ctx, js_noop_setter, "set type", 1), JS_PROP_CONFIGURABLE);

    // Methods
    JS_SetPropertyStr(ctx, elem_proto, "getAttribute",
        JS_NewCFunction(ctx, js_element_getAttribute, "getAttribute", 1));
    JS_SetPropertyStr(ctx, elem_proto, "setAttribute",
        JS_NewCFunction(ctx, js_element_setAttribute, "setAttribute", 2));
    JS_SetPropertyStr(ctx, elem_proto, "removeAttribute",
        JS_NewCFunction(ctx, js_element_removeAttribute, "removeAttribute", 1));
    JS_SetPropertyStr(ctx, elem_proto, "appendChild",
        JS_NewCFunction(ctx, js_element_appendChild, "appendChild", 1));
    JS_SetPropertyStr(ctx, elem_proto, "removeChild",
        JS_NewCFunction(ctx, js_element_removeChild, "removeChild", 1));
    JS_SetPropertyStr(ctx, elem_proto, "insertBefore",
        JS_NewCFunction(ctx, js_element_insertBefore, "insertBefore", 2));
    JS_SetPropertyStr(ctx, elem_proto, "replaceChild",
        JS_NewCFunction(ctx, js_element_replaceChild, "replaceChild", 2));
    JS_SetPropertyStr(ctx, elem_proto, "getElementsByTagName",
        JS_NewCFunction(ctx, js_element_getElementsByTagName, "getElementsByTagName", 1));
    JS_SetPropertyStr(ctx, elem_proto, "getElementsByClassName",
        JS_NewCFunction(ctx, js_element_getElementsByClassName, "getElementsByClassName", 1));
    JS_SetPropertyStr(ctx, elem_proto, "querySelector",
        JS_NewCFunction(ctx, js_element_querySelector, "querySelector", 1));
    JS_SetPropertyStr(ctx, elem_proto, "querySelectorAll",
        JS_NewCFunction(ctx, js_element_querySelectorAll, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, elem_proto, "addEventListener",
        JS_NewCFunction(ctx, js_element_addEventListener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, elem_proto, "removeEventListener",
        JS_NewCFunction(ctx, js_element_removeEventListener, "removeEventListener", 2));

    JS_SetClassProto(ctx, js_element_class_id, elem_proto);

    // NodeList prototype
    JSValue nl_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, nl_proto, "forEach",
        JS_NewCFunction(ctx, js_nodelist_forEach, "forEach", 1));
    JS_SetClassProto(ctx, js_nodelist_class_id, nl_proto);

    // Style prototype with magic getters/setters for common CSS properties
    JSValue style_proto = JS_NewObject(ctx);
    for (int i = 0; STYLE_PROPS[i]; i++) {
        JSAtom atom = JS_NewAtom(ctx, STYLE_PROPS[i]);
        JS_DefinePropertyGetSet(ctx, style_proto, atom,
            JS_NewCFunction2(ctx, (JSCFunction*)js_style_getter_magic,
                STYLE_PROPS[i], 0, JS_CFUNC_getter_magic, i),
            JS_NewCFunction2(ctx, (JSCFunction*)js_style_setter_magic,
                STYLE_PROPS[i], 1, JS_CFUNC_setter_magic, i),
            JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, atom);
    }
    JS_SetClassProto(ctx, js_style_class_id, style_proto);

    // ClassList prototype
    JSValue cl_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, cl_proto, "add",
        JS_NewCFunction(ctx, js_classlist_add, "add", 1));
    JS_SetPropertyStr(ctx, cl_proto, "remove",
        JS_NewCFunction(ctx, js_classlist_remove, "remove", 1));
    JS_SetPropertyStr(ctx, cl_proto, "toggle",
        JS_NewCFunction(ctx, js_classlist_toggle, "toggle", 1));
    JS_SetPropertyStr(ctx, cl_proto, "contains",
        JS_NewCFunction(ctx, js_classlist_contains, "contains", 1));
    JS_SetClassProto(ctx, js_classlist_class_id, cl_proto);

    // ---- document object ----
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue doc_obj = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, doc_obj, "getElementById",
        JS_NewCFunction(ctx, js_doc_getElementById, "getElementById", 1));
    JS_SetPropertyStr(ctx, doc_obj, "querySelector",
        JS_NewCFunction(ctx, js_doc_querySelector, "querySelector", 1));
    JS_SetPropertyStr(ctx, doc_obj, "querySelectorAll",
        JS_NewCFunction(ctx, js_doc_querySelectorAll, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, doc_obj, "createElement",
        JS_NewCFunction(ctx, js_doc_createElement, "createElement", 1));
    JS_SetPropertyStr(ctx, doc_obj, "createTextNode",
        JS_NewCFunction(ctx, js_doc_createTextNode, "createTextNode", 1));

    JS_DefinePropertyGetSet(ctx, doc_obj,
        JS_NewAtom(ctx, "body"),
        JS_NewCFunction(ctx, (JSCFunction*)js_doc_get_body, "get body", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, doc_obj,
        JS_NewAtom(ctx, "head"),
        JS_NewCFunction(ctx, (JSCFunction*)js_doc_get_head, "get head", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, doc_obj,
        JS_NewAtom(ctx, "documentElement"),
        JS_NewCFunction(ctx, (JSCFunction*)js_doc_get_documentElement, "get documentElement", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, doc_obj,
        JS_NewAtom(ctx, "currentScript"),
        JS_NewCFunction(ctx, (JSCFunction*)js_doc_get_currentScript, "get currentScript", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);

    JS_SetPropertyStr(ctx, global, "document", doc_obj);

    // window === globalThis
    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));

    JS_FreeValue(ctx, global);
}
