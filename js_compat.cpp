#include "js_compat.h"
#include "js_engine.h"
#include "js_bindings.h"
#include "dom.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <random>
#include <sstream>
#include <algorithm>
#include <unordered_map>

extern "C" {
#include "quickjs.h"
}

extern JSEngine* g_js_engine;

// Helper: get node from JS value
static DOMNode* compat_get_node(JSContext* ctx, JSValueConst val) {
    return js_get_node(ctx, val);
}

// Helper: check if node is connected to document tree
static bool node_is_connected(DOMNode* node) {
    if (!node) return false;
    DOMNode* cur = node;
    while (cur->parent) cur = cur->parent;
    // Connected if root is the document root
    if (g_js_engine && g_js_engine->document && g_js_engine->document->root) {
        return cur == g_js_engine->document->root.get();
    }
    return false;
}

// Helper: check if ancestor contains descendant
static bool node_contains(DOMNode* ancestor, DOMNode* descendant) {
    if (!ancestor || !descendant) return false;
    if (ancestor == descendant) return true;
    DOMNode* cur = descendant->parent;
    while (cur) {
        if (cur == ancestor) return true;
        cur = cur->parent;
    }
    return false;
}

// ============================================================
// DOM Methods
// ============================================================

// element.contains(other)
static JSValue js_element_contains(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node) return JS_FALSE;
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) return JS_FALSE;
    DOMNode* other = compat_get_node(ctx, argv[0]);
    return JS_NewBool(ctx, node_contains(node, other));
}

// element.hasAttribute(name)
static JSValue js_element_hasAttribute(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node || argc < 1) return JS_FALSE;
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_FALSE;
    bool has = node->attributes.count(name) > 0;
    // Also check special attributes
    if (!has && strcmp(name, "id") == 0) has = !node->id.empty();
    if (!has && strcmp(name, "class") == 0) has = !node->class_list.empty();
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, has);
}

// element.hasChildNodes()
static JSValue js_element_hasChildNodes(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node) return JS_FALSE;
    return JS_NewBool(ctx, !node->children.empty());
}

// element.toggleAttribute(name, force?)
static JSValue js_element_toggleAttribute(JSContext* ctx, JSValueConst this_val,
                                           int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node || argc < 1) return JS_FALSE;
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_FALSE;

    bool has = node->attributes.count(name) > 0;
    bool force_given = (argc >= 2 && !JS_IsUndefined(argv[1]));
    bool force = false;
    if (force_given) force = JS_ToBool(ctx, argv[1]);

    bool result;
    if (force_given) {
        if (force && !has) {
            node->attributes[name] = "";
            result = true;
        } else if (!force && has) {
            node->attributes.erase(name);
            result = false;
        } else {
            result = force;
        }
    } else {
        if (has) {
            node->attributes.erase(name);
            result = false;
        } else {
            node->attributes[name] = "";
            result = true;
        }
    }
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, result);
}

// element.getAttributeNames()
static JSValue js_element_getAttributeNames(JSContext* ctx, JSValueConst this_val,
                                             int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!node) return arr;
    uint32_t idx = 0;
    if (!node->id.empty()) {
        JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, "id"));
    }
    if (!node->class_list.empty()) {
        JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, "class"));
    }
    for (auto& kv : node->attributes) {
        JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, kv.first.c_str()));
    }
    return arr;
}

// Helper: insert node relative to reference
static void insert_adjacent(DOMNode* elem, const std::string& position,
                           std::shared_ptr<DOMNode> newNode) {
    if (!elem || !newNode || !g_js_engine || !g_js_engine->document) return;
    Document* doc = g_js_engine->document;

    if (position == "beforebegin") {
        if (elem->parent) doc->insertBefore(elem->parent, newNode, elem);
    } else if (position == "afterbegin") {
        DOMNode* first = elem->children.empty() ? nullptr : elem->children[0].get();
        doc->insertBefore(elem, newNode, first);
    } else if (position == "beforeend") {
        doc->appendChild(elem, newNode);
    } else if (position == "afterend") {
        if (elem->parent) {
            // Find next sibling
            DOMNode* next = nullptr;
            auto& siblings = elem->parent->children;
            for (size_t i = 0; i < siblings.size(); i++) {
                if (siblings[i].get() == elem && i + 1 < siblings.size()) {
                    next = siblings[i + 1].get();
                    break;
                }
            }
            doc->insertBefore(elem->parent, newNode, next);
        }
    }
}

// element.insertAdjacentHTML(position, html)
static JSValue js_element_insertAdjacentHTML(JSContext* ctx, JSValueConst this_val,
                                              int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node || argc < 2 || !g_js_engine || !g_js_engine->document) return JS_UNDEFINED;

    const char* pos = JS_ToCString(ctx, argv[0]);
    const char* html = JS_ToCString(ctx, argv[1]);
    if (!pos || !html) {
        if (pos) JS_FreeCString(ctx, pos);
        if (html) JS_FreeCString(ctx, html);
        return JS_UNDEFINED;
    }

    Document* doc = g_js_engine->document;
    // Parse HTML into a temporary container
    auto container = doc->createElement("div");
    container->setInnerHTML(html, doc->next_id, doc->node_map, doc->id_map);

    std::string position(pos);
    JS_FreeCString(ctx, pos);
    JS_FreeCString(ctx, html);

    // Move children from container to target position
    std::vector<std::shared_ptr<DOMNode>> children_copy;
    for (auto& child : container->children) {
        children_copy.push_back(child);
    }
    container->children.clear();

    for (auto& child : children_copy) {
        child->parent = nullptr;
        insert_adjacent(node, position, child);
    }

    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

// element.insertAdjacentElement(position, element)
static JSValue js_element_insertAdjacentElement(JSContext* ctx, JSValueConst this_val,
                                                 int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node || argc < 2 || !g_js_engine || !g_js_engine->document) return JS_NULL;

    const char* pos = JS_ToCString(ctx, argv[0]);
    DOMNode* newElem = compat_get_node(ctx, argv[1]);
    if (!pos || !newElem) {
        if (pos) JS_FreeCString(ctx, pos);
        return JS_NULL;
    }

    std::string position(pos);
    JS_FreeCString(ctx, pos);

    // Find the shared_ptr for newElem
    Document* doc = g_js_engine->document;
    std::shared_ptr<DOMNode> shared;

    // Remove from current parent if any
    if (newElem->parent) {
        auto& siblings = newElem->parent->children;
        for (auto it = siblings.begin(); it != siblings.end(); ++it) {
            if (it->get() == newElem) {
                shared = *it;
                siblings.erase(it);
                break;
            }
        }
        newElem->parent = nullptr;
    } else {
        // Check orphans
        for (auto it = doc->orphans.begin(); it != doc->orphans.end(); ++it) {
            if (it->get() == newElem) {
                shared = *it;
                break;
            }
        }
    }

    if (!shared) return JS_NULL;
    insert_adjacent(node, position, shared);
    if (g_js_engine) g_js_engine->scheduleRerender();
    return js_wrap_node(ctx, newElem);
}

// element.insertAdjacentText(position, text)
static JSValue js_element_insertAdjacentText(JSContext* ctx, JSValueConst this_val,
                                              int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node || argc < 2 || !g_js_engine || !g_js_engine->document) return JS_UNDEFINED;

    const char* pos = JS_ToCString(ctx, argv[0]);
    const char* text = JS_ToCString(ctx, argv[1]);
    if (!pos || !text) {
        if (pos) JS_FreeCString(ctx, pos);
        if (text) JS_FreeCString(ctx, text);
        return JS_UNDEFINED;
    }

    Document* doc = g_js_engine->document;
    auto textNode = doc->createTextNode(text);
    std::string position(pos);
    JS_FreeCString(ctx, pos);
    JS_FreeCString(ctx, text);

    insert_adjacent(node, position, textNode);
    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

// element.append(...nodes) - accepts strings and nodes
static JSValue js_element_append(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node || !g_js_engine || !g_js_engine->document) return JS_UNDEFINED;
    Document* doc = g_js_engine->document;

    for (int i = 0; i < argc; i++) {
        if (JS_IsString(argv[i])) {
            const char* text = JS_ToCString(ctx, argv[i]);
            if (text) {
                auto tn = doc->createTextNode(text);
                doc->appendChild(node, tn);
                JS_FreeCString(ctx, text);
            }
        } else {
            DOMNode* child = compat_get_node(ctx, argv[i]);
            if (child) {
                // Remove from current parent
                if (child->parent) {
                    auto& siblings = child->parent->children;
                    std::shared_ptr<DOMNode> shared;
                    for (auto it = siblings.begin(); it != siblings.end(); ++it) {
                        if (it->get() == child) { shared = *it; siblings.erase(it); break; }
                    }
                    child->parent = nullptr;
                    if (shared) doc->appendChild(node, shared);
                } else {
                    // Check orphans
                    for (auto& o : doc->orphans) {
                        if (o.get() == child) { doc->appendChild(node, o); break; }
                    }
                }
            }
        }
    }
    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

// element.prepend(...nodes)
static JSValue js_element_prepend(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node || !g_js_engine || !g_js_engine->document) return JS_UNDEFINED;
    Document* doc = g_js_engine->document;

    DOMNode* firstChild = node->children.empty() ? nullptr : node->children[0].get();

    for (int i = 0; i < argc; i++) {
        if (JS_IsString(argv[i])) {
            const char* text = JS_ToCString(ctx, argv[i]);
            if (text) {
                auto tn = doc->createTextNode(text);
                doc->insertBefore(node, tn, firstChild);
                JS_FreeCString(ctx, text);
            }
        } else {
            DOMNode* child = compat_get_node(ctx, argv[i]);
            if (child) {
                std::shared_ptr<DOMNode> shared;
                if (child->parent) {
                    auto& siblings = child->parent->children;
                    for (auto it = siblings.begin(); it != siblings.end(); ++it) {
                        if (it->get() == child) { shared = *it; siblings.erase(it); break; }
                    }
                    child->parent = nullptr;
                } else {
                    for (auto& o : doc->orphans) {
                        if (o.get() == child) { shared = o; break; }
                    }
                }
                if (shared) doc->insertBefore(node, shared, firstChild);
            }
        }
    }
    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

// element.before(...nodes)
static JSValue js_element_before(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node || !node->parent || !g_js_engine || !g_js_engine->document) return JS_UNDEFINED;
    Document* doc = g_js_engine->document;

    for (int i = 0; i < argc; i++) {
        if (JS_IsString(argv[i])) {
            const char* text = JS_ToCString(ctx, argv[i]);
            if (text) {
                auto tn = doc->createTextNode(text);
                doc->insertBefore(node->parent, tn, node);
                JS_FreeCString(ctx, text);
            }
        } else {
            DOMNode* child = compat_get_node(ctx, argv[i]);
            if (child) {
                std::shared_ptr<DOMNode> shared;
                if (child->parent) {
                    auto& siblings = child->parent->children;
                    for (auto it = siblings.begin(); it != siblings.end(); ++it) {
                        if (it->get() == child) { shared = *it; siblings.erase(it); break; }
                    }
                    child->parent = nullptr;
                } else {
                    for (auto& o : doc->orphans) {
                        if (o.get() == child) { shared = o; break; }
                    }
                }
                if (shared) doc->insertBefore(node->parent, shared, node);
            }
        }
    }
    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

// element.after(...nodes)
static JSValue js_element_after(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node || !node->parent || !g_js_engine || !g_js_engine->document) return JS_UNDEFINED;
    Document* doc = g_js_engine->document;

    // Find next sibling
    DOMNode* nextSibling = nullptr;
    auto& siblings = node->parent->children;
    for (size_t i = 0; i < siblings.size(); i++) {
        if (siblings[i].get() == node && i + 1 < siblings.size()) {
            nextSibling = siblings[i + 1].get();
            break;
        }
    }

    for (int i = 0; i < argc; i++) {
        if (JS_IsString(argv[i])) {
            const char* text = JS_ToCString(ctx, argv[i]);
            if (text) {
                auto tn = doc->createTextNode(text);
                doc->insertBefore(node->parent, tn, nextSibling);
                JS_FreeCString(ctx, text);
            }
        } else {
            DOMNode* child = compat_get_node(ctx, argv[i]);
            if (child) {
                std::shared_ptr<DOMNode> shared;
                if (child->parent) {
                    auto& siblings2 = child->parent->children;
                    for (auto it = siblings2.begin(); it != siblings2.end(); ++it) {
                        if (it->get() == child) { shared = *it; siblings2.erase(it); break; }
                    }
                    child->parent = nullptr;
                } else {
                    for (auto& o : doc->orphans) {
                        if (o.get() == child) { shared = o; break; }
                    }
                }
                if (shared) doc->insertBefore(node->parent, shared, nextSibling);
            }
        }
    }
    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

// element.replaceWith(...nodes)
static JSValue js_element_replaceWith(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node || !node->parent || !g_js_engine || !g_js_engine->document) return JS_UNDEFINED;
    Document* doc = g_js_engine->document;
    DOMNode* parent = node->parent;

    // Find position in parent
    DOMNode* nextSibling = nullptr;
    auto& siblings = parent->children;
    for (size_t i = 0; i < siblings.size(); i++) {
        if (siblings[i].get() == node) {
            if (i + 1 < siblings.size()) nextSibling = siblings[i + 1].get();
            break;
        }
    }

    // Remove self
    doc->removeChild(parent, node);

    // Insert new nodes
    for (int i = 0; i < argc; i++) {
        if (JS_IsString(argv[i])) {
            const char* text = JS_ToCString(ctx, argv[i]);
            if (text) {
                auto tn = doc->createTextNode(text);
                doc->insertBefore(parent, tn, nextSibling);
                JS_FreeCString(ctx, text);
            }
        } else {
            DOMNode* child = compat_get_node(ctx, argv[i]);
            if (child) {
                std::shared_ptr<DOMNode> shared;
                if (child->parent) {
                    auto& siblings2 = child->parent->children;
                    for (auto it = siblings2.begin(); it != siblings2.end(); ++it) {
                        if (it->get() == child) { shared = *it; siblings2.erase(it); break; }
                    }
                    child->parent = nullptr;
                } else {
                    for (auto& o : doc->orphans) {
                        if (o.get() == child) { shared = o; break; }
                    }
                }
                if (shared) doc->insertBefore(parent, shared, nextSibling);
            }
        }
    }
    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

// element.outerHTML getter
static JSValue js_element_get_outerHTML(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node) return JS_NewString(ctx, "");

    if (node->node_type == DOMNode::TEXT) {
        return JS_NewString(ctx, node->text_content.c_str());
    }

    std::string result = "<" + node->tag_name;
    if (!node->id.empty()) result += " id=\"" + node->id + "\"";
    if (!node->class_list.empty()) {
        result += " class=\"";
        for (size_t i = 0; i < node->class_list.size(); i++) {
            if (i > 0) result += " ";
            result += node->class_list[i];
        }
        result += "\"";
    }
    for (auto& kv : node->attributes) {
        if (kv.first == "id" || kv.first == "class") continue;
        result += " " + kv.first + "=\"" + kv.second + "\"";
    }
    result += ">";
    result += node->getInnerHTML();
    result += "</" + node->tag_name + ">";
    return JS_NewString(ctx, result.c_str());
}

// element.outerHTML setter
static JSValue js_element_set_outerHTML(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node || !node->parent || argc < 1 || !g_js_engine || !g_js_engine->document) return JS_UNDEFINED;

    const char* html = JS_ToCString(ctx, argv[0]);
    if (!html) return JS_UNDEFINED;

    Document* doc = g_js_engine->document;
    DOMNode* parent = node->parent;

    // Parse new HTML
    auto container = doc->createElement("div");
    container->setInnerHTML(html, doc->next_id, doc->node_map, doc->id_map);
    JS_FreeCString(ctx, html);

    // Find position
    DOMNode* nextSibling = nullptr;
    auto& siblings = parent->children;
    for (size_t i = 0; i < siblings.size(); i++) {
        if (siblings[i].get() == node) {
            if (i + 1 < siblings.size()) nextSibling = siblings[i + 1].get();
            break;
        }
    }

    // Remove original
    doc->removeChild(parent, node);

    // Insert parsed nodes
    std::vector<std::shared_ptr<DOMNode>> new_children;
    for (auto& child : container->children) new_children.push_back(child);
    container->children.clear();

    for (auto& child : new_children) {
        child->parent = nullptr;
        doc->insertBefore(parent, child, nextSibling);
    }

    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

// element.isConnected getter
static JSValue js_element_get_isConnected(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = compat_get_node(ctx, this_val);
    return JS_NewBool(ctx, node_is_connected(node));
}

// element.childElementCount getter
static JSValue js_element_get_childElementCount(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node) return JS_NewInt32(ctx, 0);
    int count = 0;
    for (auto& child : node->children) {
        if (child->node_type == DOMNode::ELEMENT) count++;
    }
    return JS_NewInt32(ctx, count);
}

// element.firstElementChild getter
static JSValue js_element_get_firstElementChild(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node) return JS_NULL;
    for (auto& child : node->children) {
        if (child->node_type == DOMNode::ELEMENT) return js_wrap_node(ctx, child.get());
    }
    return JS_NULL;
}

// element.lastElementChild getter
static JSValue js_element_get_lastElementChild(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node) return JS_NULL;
    for (int i = (int)node->children.size() - 1; i >= 0; i--) {
        if (node->children[i]->node_type == DOMNode::ELEMENT)
            return js_wrap_node(ctx, node->children[i].get());
    }
    return JS_NULL;
}

// element.focus() / blur() / click()
static JSValue js_element_focus(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (node) node->is_focused = true;
    return JS_UNDEFINED;
}

static JSValue js_element_blur(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (node) node->is_focused = false;
    return JS_UNDEFINED;
}

static JSValue js_element_click(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (node && g_js_engine) {
        g_js_engine->dispatchEvent(node->node_id, "click", 0, 0);
    }
    return JS_UNDEFINED;
}

// element.scrollIntoView()
static JSValue js_element_scrollIntoView(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    // No-op for now but doesn't throw
    return JS_UNDEFINED;
}

// element.innerText getter
static JSValue js_element_get_innerText(JSContext* ctx, JSValueConst this_val) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node) return JS_NewString(ctx, "");
    // innerText is like textContent but respects display:none and adds newlines for blocks
    return JS_NewString(ctx, node->getTextContent().c_str());
}

// element.innerText setter
static JSValue js_element_set_innerText(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node || argc < 1 || !g_js_engine || !g_js_engine->document) return JS_UNDEFINED;
    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_UNDEFINED;
    Document* doc = g_js_engine->document;
    node->setTextContent(text, doc->next_id);
    JS_FreeCString(ctx, text);
    node->markDirty();
    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

// element.normalize()
static JSValue js_element_normalize(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node) return JS_UNDEFINED;
    // Merge adjacent text nodes, remove empty ones
    for (int i = (int)node->children.size() - 1; i >= 0; i--) {
        auto& child = node->children[i];
        if (child->node_type == DOMNode::TEXT) {
            if (child->text_content.empty()) {
                node->children.erase(node->children.begin() + i);
            } else if (i > 0 && node->children[i-1]->node_type == DOMNode::TEXT) {
                node->children[i-1]->text_content += child->text_content;
                node->children.erase(node->children.begin() + i);
            }
        }
    }
    return JS_UNDEFINED;
}

// element.replaceChildren(...nodes)
static JSValue js_element_replaceChildren(JSContext* ctx, JSValueConst this_val,
                                           int argc, JSValueConst* argv) {
    DOMNode* node = compat_get_node(ctx, this_val);
    if (!node || !g_js_engine || !g_js_engine->document) return JS_UNDEFINED;
    Document* doc = g_js_engine->document;

    // Remove all existing children
    while (!node->children.empty()) {
        doc->removeChild(node, node->children.back().get());
    }

    // Append new nodes (reuse append logic)
    for (int i = 0; i < argc; i++) {
        if (JS_IsString(argv[i])) {
            const char* text = JS_ToCString(ctx, argv[i]);
            if (text) {
                auto tn = doc->createTextNode(text);
                doc->appendChild(node, tn);
                JS_FreeCString(ctx, text);
            }
        } else {
            DOMNode* child = compat_get_node(ctx, argv[i]);
            if (child) {
                std::shared_ptr<DOMNode> shared;
                if (child->parent) {
                    auto& siblings = child->parent->children;
                    for (auto it = siblings.begin(); it != siblings.end(); ++it) {
                        if (it->get() == child) { shared = *it; siblings.erase(it); break; }
                    }
                    child->parent = nullptr;
                } else {
                    for (auto& o : doc->orphans) {
                        if (o.get() == child) { shared = o; break; }
                    }
                }
                if (shared) doc->appendChild(node, shared);
            }
        }
    }
    if (g_js_engine) g_js_engine->scheduleRerender();
    return JS_UNDEFINED;
}

// element.closest(selector) - already exists but verify
// element.matches(selector) - already exists

// document.createDocumentFragment()
static JSValue js_doc_createDocumentFragment(JSContext* ctx, JSValueConst this_val,
                                              int argc, JSValueConst* argv) {
    if (!g_js_engine || !g_js_engine->document) return JS_NULL;
    Document* doc = g_js_engine->document;
    auto frag = doc->createElement("__fragment__");
    frag->node_type = DOMNode::ELEMENT;
    return js_wrap_node(ctx, frag.get());
}

// document.createComment(data)
static JSValue js_doc_createComment(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    if (!g_js_engine || !g_js_engine->document) return JS_NULL;
    Document* doc = g_js_engine->document;
    std::string data;
    if (argc >= 1) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (s) { data = s; JS_FreeCString(ctx, s); }
    }
    auto node = std::make_shared<DOMNode>();
    node->node_id = doc->next_id++;
    node->node_type = DOMNode::COMMENT;
    node->text_content = data;
    doc->node_map[node->node_id] = node.get();
    doc->orphans.push_back(node);
    return js_wrap_node(ctx, node.get());
}

// document.title getter/setter
static JSValue js_doc_get_title(JSContext* ctx, JSValueConst this_val) {
    if (!g_js_engine || !g_js_engine->document) return JS_NewString(ctx, "");
    Document* doc = g_js_engine->document;
    // Find <title> element
    if (doc->head) {
        for (auto& child : doc->head->children) {
            if (child->tag_name == "title") {
                return JS_NewString(ctx, child->getTextContent().c_str());
            }
        }
    }
    return JS_NewString(ctx, "");
}

static JSValue js_doc_set_title(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    if (!g_js_engine || !g_js_engine->document || argc < 1) return JS_UNDEFINED;
    const char* title = JS_ToCString(ctx, argv[0]);
    if (!title) return JS_UNDEFINED;
    Document* doc = g_js_engine->document;
    // Find or create <title> element
    DOMNode* titleNode = nullptr;
    if (doc->head) {
        for (auto& child : doc->head->children) {
            if (child->tag_name == "title") { titleNode = child.get(); break; }
        }
    }
    if (titleNode) {
        titleNode->setTextContent(title, doc->next_id);
    }
    JS_FreeCString(ctx, title);
    return JS_UNDEFINED;
}

// document.readyState getter
static JSValue js_doc_get_readyState(JSContext* ctx, JSValueConst this_val) {
    return JS_NewString(ctx, "complete");
}

// document.URL / document.documentURI getter
static JSValue js_doc_get_URL(JSContext* ctx, JSValueConst this_val) {
    if (g_js_engine) return JS_NewString(ctx, g_js_engine->page_url.c_str());
    return JS_NewString(ctx, "");
}

// document.compatMode
static JSValue js_doc_get_compatMode(JSContext* ctx, JSValueConst this_val) {
    return JS_NewString(ctx, "CSS1Compat");
}

// document.characterSet
static JSValue js_doc_get_characterSet(JSContext* ctx, JSValueConst this_val) {
    return JS_NewString(ctx, "UTF-8");
}

// document.contentType
static JSValue js_doc_get_contentType(JSContext* ctx, JSValueConst this_val) {
    return JS_NewString(ctx, "text/html");
}

// ============================================================
// Web APIs
// ============================================================

// structuredClone(value) - deep clone using JSON (simplified)
static JSValue js_structuredClone(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    // Use JSON round-trip for structured clone (handles objects, arrays, primitives)
    JSValue json_str = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json_str)) return JS_EXCEPTION;
    if (JS_IsUndefined(json_str)) return JS_UNDEFINED;
    const char* str = JS_ToCString(ctx, json_str);
    JS_FreeValue(ctx, json_str);
    if (!str) return JS_UNDEFINED;
    JSValue result = JS_ParseJSON(ctx, str, strlen(str), "<structuredClone>");
    JS_FreeCString(ctx, str);
    return result;
}

// queueMicrotask(callback)
static JSValue js_queueMicrotask(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    // QuickJS: enqueue as a resolved promise .then()
    JSValue promise = JS_NewPromiseCapability(ctx, nullptr);
    if (JS_IsException(promise)) {
        // Fallback: just call it via setTimeout 0
        if (g_js_engine) {
            JSValue func = JS_DupValue(ctx, argv[0]);
            g_js_engine->setTimeout(func, 0);
        }
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx, promise);
    // Simpler approach: use Promise.resolve().then(callback)
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue prom_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    JSValue resolve_fn = JS_GetPropertyStr(ctx, prom_ctor, "resolve");
    JSValue resolved = JS_Call(ctx, resolve_fn, prom_ctor, 0, nullptr);
    JSValue then_fn = JS_GetPropertyStr(ctx, resolved, "then");
    JSValue result = JS_Call(ctx, then_fn, resolved, 1, argv);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, then_fn);
    JS_FreeValue(ctx, resolved);
    JS_FreeValue(ctx, resolve_fn);
    JS_FreeValue(ctx, prom_ctor);
    JS_FreeValue(ctx, global);
    return JS_UNDEFINED;
}

// crypto.randomUUID()
static JSValue js_crypto_randomUUID(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t a = dist(gen);
    uint64_t b = dist(gen);

    // Format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    char uuid[37];
    snprintf(uuid, sizeof(uuid),
        "%08x-%04x-4%03x-%x%03x-%012llx",
        (uint32_t)(a >> 32),
        (uint16_t)(a >> 16),
        (uint16_t)(a & 0xFFF),
        (unsigned)(8 | ((b >> 60) & 3)),
        (uint16_t)((b >> 48) & 0xFFF),
        (unsigned long long)(b & 0xFFFFFFFFFFFFULL));
    return JS_NewString(ctx, uuid);
}

// crypto.getRandomValues(typedArray)
static JSValue js_crypto_getRandomValues(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;

    size_t byte_length = 0;
    size_t byte_offset = 0;
    uint8_t* buf = JS_GetArrayBuffer(ctx, &byte_length, argv[0]);

    if (!buf) {
        // It's a TypedArray - get the underlying buffer
        JSValue ab = JS_GetPropertyStr(ctx, argv[0], "buffer");
        JSValue bo = JS_GetPropertyStr(ctx, argv[0], "byteOffset");
        JSValue bl = JS_GetPropertyStr(ctx, argv[0], "byteLength");

        int32_t offset_val = 0, length_val = 0;
        JS_ToInt32(ctx, &offset_val, bo);
        JS_ToInt32(ctx, &length_val, bl);
        byte_offset = offset_val;
        byte_length = length_val;

        size_t ab_size = 0;
        buf = JS_GetArrayBuffer(ctx, &ab_size, ab);

        JS_FreeValue(ctx, ab);
        JS_FreeValue(ctx, bo);
        JS_FreeValue(ctx, bl);
    }

    if (buf && byte_length > 0) {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(0, 255);
        for (size_t i = 0; i < byte_length; i++) {
            buf[byte_offset + i] = (uint8_t)dist(gen);
        }
    }

    return JS_DupValue(ctx, argv[0]);
}

// TextEncoder
static JSValue js_textencoder_constructor(JSContext* ctx, JSValueConst new_target,
                                           int argc, JSValueConst* argv) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "encoding", JS_NewString(ctx, "utf-8"));

    // encode(string) -> Uint8Array
    JS_SetPropertyStr(ctx, obj, "encode",
        JS_NewCFunction(ctx, [](JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
            const char* str = (argc >= 1) ? JS_ToCString(c, argv[0]) : nullptr;
            size_t len = str ? strlen(str) : 0;
            JSValue ab = JS_NewArrayBufferCopy(c, (const uint8_t*)(str ? str : ""), len);
            if (str) JS_FreeCString(c, str);
            JSValue global = JS_GetGlobalObject(c);
            JSValue uint8_ctor = JS_GetPropertyStr(c, global, "Uint8Array");
            JSValue result = JS_CallConstructor(c, uint8_ctor, 1, &ab);
            JS_FreeValue(c, uint8_ctor);
            JS_FreeValue(c, global);
            JS_FreeValue(c, ab);
            return result;
        }, "encode", 1));

    // encodeInto(string, uint8array) -> {read, written}
    JS_SetPropertyStr(ctx, obj, "encodeInto",
        JS_NewCFunction(ctx, [](JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
            JSValue result = JS_NewObject(c);
            JS_SetPropertyStr(c, result, "read", JS_NewInt32(c, 0));
            JS_SetPropertyStr(c, result, "written", JS_NewInt32(c, 0));
            if (argc < 2) return result;
            const char* str = JS_ToCString(c, argv[0]);
            if (!str) return result;
            size_t str_len = strlen(str);

            // Get dest buffer
            JSValue bl = JS_GetPropertyStr(c, argv[1], "byteLength");
            int32_t dest_len = 0;
            JS_ToInt32(c, &dest_len, bl);
            JS_FreeValue(c, bl);

            size_t written = std::min(str_len, (size_t)dest_len);
            // Write bytes
            for (size_t i = 0; i < written; i++) {
                JS_SetPropertyUint32(c, argv[1], i, JS_NewInt32(c, (uint8_t)str[i]));
            }

            JS_SetPropertyStr(c, result, "read", JS_NewInt32(c, (int)written));
            JS_SetPropertyStr(c, result, "written", JS_NewInt32(c, (int)written));
            JS_FreeCString(c, str);
            return result;
        }, "encodeInto", 2));

    return obj;
}

// TextDecoder
static JSValue js_textdecoder_constructor(JSContext* ctx, JSValueConst new_target,
                                           int argc, JSValueConst* argv) {
    JSValue obj = JS_NewObject(ctx);
    std::string encoding = "utf-8";
    if (argc >= 1) {
        const char* enc = JS_ToCString(ctx, argv[0]);
        if (enc) { encoding = enc; JS_FreeCString(ctx, enc); }
    }
    JS_SetPropertyStr(ctx, obj, "encoding", JS_NewString(ctx, encoding.c_str()));
    JS_SetPropertyStr(ctx, obj, "fatal", JS_FALSE);
    JS_SetPropertyStr(ctx, obj, "ignoreBOM", JS_FALSE);

    // decode(buffer) -> string
    JS_SetPropertyStr(ctx, obj, "decode",
        JS_NewCFunction(ctx, [](JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
            if (argc < 1) return JS_NewString(c, "");

            // Try ArrayBuffer first
            size_t buf_len = 0;
            uint8_t* buf = JS_GetArrayBuffer(c, &buf_len, argv[0]);

            if (!buf) {
                // TypedArray - read bytes
                JSValue len_val = JS_GetPropertyStr(c, argv[0], "length");
                int32_t len = 0;
                JS_ToInt32(c, &len, len_val);
                JS_FreeValue(c, len_val);

                std::string result;
                result.reserve(len);
                for (int i = 0; i < len; i++) {
                    JSValue byte_val = JS_GetPropertyUint32(c, argv[0], i);
                    int32_t byte = 0;
                    JS_ToInt32(c, &byte, byte_val);
                    JS_FreeValue(c, byte_val);
                    result += (char)(byte & 0xFF);
                }
                return JS_NewStringLen(c, result.data(), result.size());
            }

            return JS_NewStringLen(c, (const char*)buf, buf_len);
        }, "decode", 1));

    return obj;
}

// requestIdleCallback / cancelIdleCallback
static JSValue js_requestIdleCallback(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    // Implement as setTimeout(fn, 0) with a deadline object
    if (!g_js_engine) return JS_NewInt32(ctx, 0);

    // Wrap callback to pass IdleDeadline
    JSValue func = JS_DupValue(ctx, argv[0]);
    uint32_t id = g_js_engine->setTimeout(func, 1);
    return JS_NewInt32(ctx, (int32_t)id);
}

static JSValue js_cancelIdleCallback(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    if (argc < 1 || !g_js_engine) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    g_js_engine->clearTimer((uint32_t)id);
    return JS_UNDEFINED;
}

// ============================================================
// classList extras
// ============================================================

// We need to add replace, forEach, length, value, item to the classList
// These will be added via the init function

// ============================================================
// NodeList extras: item, entries, keys, values, Symbol.iterator
// ============================================================

// ============================================================
// DOMRect / DOMPoint constructors
// ============================================================

static JSValue js_domrect_constructor(JSContext* ctx, JSValueConst new_target,
                                       int argc, JSValueConst* argv) {
    JSValue obj = JS_NewObject(ctx);
    double x = 0, y = 0, w = 0, h = 0;
    if (argc >= 1) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &y, argv[1]);
    if (argc >= 3) JS_ToFloat64(ctx, &w, argv[2]);
    if (argc >= 4) JS_ToFloat64(ctx, &h, argv[3]);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, obj, "width", JS_NewFloat64(ctx, w));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewFloat64(ctx, h));
    JS_SetPropertyStr(ctx, obj, "top", JS_NewFloat64(ctx, std::min(y, y + h)));
    JS_SetPropertyStr(ctx, obj, "right", JS_NewFloat64(ctx, std::max(x, x + w)));
    JS_SetPropertyStr(ctx, obj, "bottom", JS_NewFloat64(ctx, std::max(y, y + h)));
    JS_SetPropertyStr(ctx, obj, "left", JS_NewFloat64(ctx, std::min(x, x + w)));
    JS_SetPropertyStr(ctx, obj, "toJSON",
        JS_NewCFunction(ctx, [](JSContext* c, JSValueConst tv, int, JSValueConst*) -> JSValue {
            JSValue r = JS_NewObject(c);
            const char* props[] = {"x", "y", "width", "height", "top", "right", "bottom", "left"};
            for (auto p : props) {
                JS_SetPropertyStr(c, r, p, JS_GetPropertyStr(c, tv, p));
            }
            return r;
        }, "toJSON", 0));
    return obj;
}

static JSValue js_dompoint_constructor(JSContext* ctx, JSValueConst new_target,
                                        int argc, JSValueConst* argv) {
    JSValue obj = JS_NewObject(ctx);
    double x = 0, y = 0, z = 0, w = 1;
    if (argc >= 1) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &y, argv[1]);
    if (argc >= 3) JS_ToFloat64(ctx, &z, argv[2]);
    if (argc >= 4) JS_ToFloat64(ctx, &w, argv[3]);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, z));
    JS_SetPropertyStr(ctx, obj, "w", JS_NewFloat64(ctx, w));
    return obj;
}

// ============================================================
// Main init function
// ============================================================

void js_compat_init(JSEngine* engine) {
    if (!engine || !engine->ctx) return;
    JSContext* ctx = engine->ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    // Get document object
    JSValue doc_obj = JS_GetPropertyStr(ctx, global, "document");

    // Get element prototype - we need to access it through the class system
    // We'll add methods to document and use eval for element prototype extension

    // --- document methods ---
    JS_SetPropertyStr(ctx, doc_obj, "createDocumentFragment",
        JS_NewCFunction(ctx, js_doc_createDocumentFragment, "createDocumentFragment", 0));
    JS_SetPropertyStr(ctx, doc_obj, "createComment",
        JS_NewCFunction(ctx, js_doc_createComment, "createComment", 1));

    // document.title
    JS_DefinePropertyGetSet(ctx, doc_obj,
        JS_NewAtom(ctx, "title"),
        JS_NewCFunction(ctx, (JSCFunction*)js_doc_get_title, "get title", 0),
        JS_NewCFunction(ctx, js_doc_set_title, "set title", 1),
        JS_PROP_CONFIGURABLE);

    // document.readyState
    JS_DefinePropertyGetSet(ctx, doc_obj,
        JS_NewAtom(ctx, "readyState"),
        JS_NewCFunction(ctx, (JSCFunction*)js_doc_get_readyState, "get readyState", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);

    // document.URL / documentURI
    JS_DefinePropertyGetSet(ctx, doc_obj,
        JS_NewAtom(ctx, "URL"),
        JS_NewCFunction(ctx, (JSCFunction*)js_doc_get_URL, "get URL", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, doc_obj,
        JS_NewAtom(ctx, "documentURI"),
        JS_NewCFunction(ctx, (JSCFunction*)js_doc_get_URL, "get documentURI", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);

    // document.compatMode
    JS_DefinePropertyGetSet(ctx, doc_obj,
        JS_NewAtom(ctx, "compatMode"),
        JS_NewCFunction(ctx, (JSCFunction*)js_doc_get_compatMode, "get compatMode", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);

    // document.characterSet / charset
    JS_DefinePropertyGetSet(ctx, doc_obj,
        JS_NewAtom(ctx, "characterSet"),
        JS_NewCFunction(ctx, (JSCFunction*)js_doc_get_characterSet, "get characterSet", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyGetSet(ctx, doc_obj,
        JS_NewAtom(ctx, "charset"),
        JS_NewCFunction(ctx, (JSCFunction*)js_doc_get_characterSet, "get charset", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);

    // document.contentType
    JS_DefinePropertyGetSet(ctx, doc_obj,
        JS_NewAtom(ctx, "contentType"),
        JS_NewCFunction(ctx, (JSCFunction*)js_doc_get_contentType, "get contentType", 0),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);

    // document.nodeType (9 for DOCUMENT_NODE)
    JS_SetPropertyStr(ctx, doc_obj, "nodeType", JS_NewInt32(ctx, 9));
    JS_SetPropertyStr(ctx, doc_obj, "nodeName", JS_NewString(ctx, "#document"));

    JS_FreeValue(ctx, doc_obj);

    // --- Global Web APIs ---

    // structuredClone
    JS_SetPropertyStr(ctx, global, "structuredClone",
        JS_NewCFunction(ctx, js_structuredClone, "structuredClone", 1));

    // queueMicrotask
    JS_SetPropertyStr(ctx, global, "queueMicrotask",
        JS_NewCFunction(ctx, js_queueMicrotask, "queueMicrotask", 1));

    // crypto object
    JSValue crypto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, crypto, "randomUUID",
        JS_NewCFunction(ctx, js_crypto_randomUUID, "randomUUID", 0));
    JS_SetPropertyStr(ctx, crypto, "getRandomValues",
        JS_NewCFunction(ctx, js_crypto_getRandomValues, "getRandomValues", 1));
    // subtle stub
    JSValue subtle = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, subtle, "digest",
        JS_NewCFunction(ctx, [](JSContext* c, JSValueConst, int, JSValueConst*) -> JSValue {
            JSValue resolving[2];
            JSValue promise = JS_NewPromiseCapability(c, resolving);
            JSValue ab = JS_NewArrayBufferCopy(c, (const uint8_t*)"", 0);
            JS_Call(c, resolving[0], JS_UNDEFINED, 1, &ab);
            JS_FreeValue(c, ab);
            JS_FreeValue(c, resolving[0]);
            JS_FreeValue(c, resolving[1]);
            return promise;
        }, "digest", 2));
    JS_SetPropertyStr(ctx, crypto, "subtle", subtle);
    JS_SetPropertyStr(ctx, global, "crypto", crypto);

    // TextEncoder / TextDecoder
    JS_SetPropertyStr(ctx, global, "TextEncoder",
        JS_NewCFunction2(ctx, js_textencoder_constructor, "TextEncoder", 0, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, global, "TextDecoder",
        JS_NewCFunction2(ctx, js_textdecoder_constructor, "TextDecoder", 1, JS_CFUNC_constructor, 0));

    // requestIdleCallback / cancelIdleCallback
    JS_SetPropertyStr(ctx, global, "requestIdleCallback",
        JS_NewCFunction(ctx, js_requestIdleCallback, "requestIdleCallback", 1));
    JS_SetPropertyStr(ctx, global, "cancelIdleCallback",
        JS_NewCFunction(ctx, js_cancelIdleCallback, "cancelIdleCallback", 1));

    // DOMRect / DOMRectReadOnly
    JS_SetPropertyStr(ctx, global, "DOMRect",
        JS_NewCFunction2(ctx, js_domrect_constructor, "DOMRect", 4, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, global, "DOMRectReadOnly",
        JS_NewCFunction2(ctx, js_domrect_constructor, "DOMRectReadOnly", 4, JS_CFUNC_constructor, 0));

    // DOMPoint / DOMPointReadOnly
    JS_SetPropertyStr(ctx, global, "DOMPoint",
        JS_NewCFunction2(ctx, js_dompoint_constructor, "DOMPoint", 4, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, global, "DOMPointReadOnly",
        JS_NewCFunction2(ctx, js_dompoint_constructor, "DOMPointReadOnly", 4, JS_CFUNC_constructor, 0));

    // --- Add element methods via JS prototype patching ---
    // This is the cleanest way to add methods to existing element wrappers
    const char* compat_js = R"JS(
(function() {
    // We need to intercept element creation to add these methods
    // Instead, use Object.defineProperty on the prototype chain

    // Store original createElement to get element prototype
    var _origCreate = document.createElement;
    var _tmpEl = _origCreate.call(document, 'div');
    var elemProto = Object.getPrototypeOf(_tmpEl);

    // Now add all missing methods to elemProto
    if (!elemProto.contains) {
        elemProto.contains = function(other) { return this.__contains(other); };
    }
    if (!elemProto.hasAttribute) {
        elemProto.hasAttribute = function(n) { return this.__hasAttribute(n); };
    }
    if (!elemProto.hasChildNodes) {
        elemProto.hasChildNodes = function() { return this.__hasChildNodes(); };
    }
    if (!elemProto.toggleAttribute) {
        elemProto.toggleAttribute = function(n, f) { return this.__toggleAttribute(n, f); };
    }
    if (!elemProto.getAttributeNames) {
        elemProto.getAttributeNames = function() { return this.__getAttributeNames(); };
    }
    if (!elemProto.insertAdjacentHTML) {
        elemProto.insertAdjacentHTML = function(p, h) { return this.__insertAdjacentHTML(p, h); };
    }
    if (!elemProto.insertAdjacentElement) {
        elemProto.insertAdjacentElement = function(p, e) { return this.__insertAdjacentElement(p, e); };
    }
    if (!elemProto.insertAdjacentText) {
        elemProto.insertAdjacentText = function(p, t) { return this.__insertAdjacentText(p, t); };
    }
    if (!elemProto.append) {
        elemProto.append = function() { return this.__append.apply(this, arguments); };
    }
    if (!elemProto.prepend) {
        elemProto.prepend = function() { return this.__prepend.apply(this, arguments); };
    }
    if (!elemProto.before) {
        elemProto.before = function() { return this.__before.apply(this, arguments); };
    }
    if (!elemProto.after) {
        elemProto.after = function() { return this.__after.apply(this, arguments); };
    }
    if (!elemProto.replaceWith) {
        elemProto.replaceWith = function() { return this.__replaceWith.apply(this, arguments); };
    }
    if (!elemProto.replaceChildren) {
        elemProto.replaceChildren = function() { return this.__replaceChildren.apply(this, arguments); };
    }
    if (!elemProto.focus) {
        elemProto.focus = function() { return this.__focus(); };
    }
    if (!elemProto.blur) {
        elemProto.blur = function() { return this.__blur(); };
    }
    if (!elemProto.click) {
        elemProto.click = function() { return this.__click(); };
    }
    if (!elemProto.scrollIntoView) {
        elemProto.scrollIntoView = function() {};
    }
    if (!elemProto.normalize) {
        elemProto.normalize = function() { return this.__normalize(); };
    }

    // Getters
    if (!Object.getOwnPropertyDescriptor(elemProto, 'isConnected')) {
        Object.defineProperty(elemProto, 'isConnected', {
            get: function() { return this.__get_isConnected(); },
            configurable: true
        });
    }
    if (!Object.getOwnPropertyDescriptor(elemProto, 'childElementCount')) {
        Object.defineProperty(elemProto, 'childElementCount', {
            get: function() { return this.__get_childElementCount(); },
            configurable: true
        });
    }
    if (!Object.getOwnPropertyDescriptor(elemProto, 'firstElementChild')) {
        Object.defineProperty(elemProto, 'firstElementChild', {
            get: function() { return this.__get_firstElementChild(); },
            configurable: true
        });
    }
    if (!Object.getOwnPropertyDescriptor(elemProto, 'lastElementChild')) {
        Object.defineProperty(elemProto, 'lastElementChild', {
            get: function() { return this.__get_lastElementChild(); },
            configurable: true
        });
    }
    if (!Object.getOwnPropertyDescriptor(elemProto, 'outerHTML')) {
        Object.defineProperty(elemProto, 'outerHTML', {
            get: function() { return this.__get_outerHTML(); },
            set: function(v) { return this.__set_outerHTML(v); },
            configurable: true
        });
    }
    if (!Object.getOwnPropertyDescriptor(elemProto, 'innerText')) {
        Object.defineProperty(elemProto, 'innerText', {
            get: function() { return this.__get_innerText(); },
            set: function(v) { return this.__set_innerText(v); },
            configurable: true
        });
    }

    // getClientRects
    if (!elemProto.getClientRects) {
        elemProto.getClientRects = function() {
            var rect = this.getBoundingClientRect();
            var list = [rect];
            list.item = function(i) { return list[i] || null; };
            return list;
        };
    }

    // compareDocumentPosition
    if (!elemProto.compareDocumentPosition) {
        elemProto.compareDocumentPosition = function(other) {
            if (this === other) return 0;
            if (this.contains(other)) return 20; // CONTAINED_BY | FOLLOWING
            if (other && other.contains && other.contains(this)) return 10; // CONTAINS | PRECEDING
            return 4; // FOLLOWING (simplified)
        };
    }

    // isSameNode
    if (!elemProto.isSameNode) {
        elemProto.isSameNode = function(other) { return this === other; };
    }

    // isEqualNode (simplified - checks tag, attributes, children count)
    if (!elemProto.isEqualNode) {
        elemProto.isEqualNode = function(other) {
            if (!other) return false;
            if (this.nodeType !== other.nodeType) return false;
            if (this.nodeName !== other.nodeName) return false;
            if (this.textContent !== other.textContent) return false;
            return true;
        };
    }

    // getRootNode
    if (!elemProto.getRootNode) {
        elemProto.getRootNode = function() {
            var node = this;
            while (node.parentNode) node = node.parentNode;
            return node;
        };
    }

    // classList enhancements
    var cl = _tmpEl.classList;
    if (cl) {
        var clProto = Object.getPrototypeOf(cl);
        if (clProto && !clProto.replace) {
            clProto.replace = function(oldClass, newClass) {
                if (!this.contains(oldClass)) return false;
                this.remove(oldClass);
                this.add(newClass);
                return true;
            };
        }
        if (clProto && !clProto.forEach) {
            clProto.forEach = function(cb, thisArg) {
                var str = this.toString ? this.toString() : '';
                var classes = str.split(/\s+/).filter(Boolean);
                classes.forEach(function(c, i) { cb.call(thisArg, c, i, this); }, this);
            };
        }
        if (clProto && !clProto.entries) {
            clProto.entries = function() {
                var str = this.toString ? this.toString() : '';
                var classes = str.split(/\s+/).filter(Boolean);
                return classes.entries();
            };
        }
        if (clProto && !clProto.keys) {
            clProto.keys = function() {
                var str = this.toString ? this.toString() : '';
                var classes = str.split(/\s+/).filter(Boolean);
                return classes.keys();
            };
        }
        if (clProto && !clProto.values) {
            clProto.values = function() {
                var str = this.toString ? this.toString() : '';
                var classes = str.split(/\s+/).filter(Boolean);
                return classes.values();
            };
        }
    }

    // NodeList enhancements - add item() and iterators
    var nl = document.querySelectorAll('*');
    if (nl) {
        var nlProto = Object.getPrototypeOf(nl);
        if (nlProto && !nlProto.item) {
            nlProto.item = function(i) { return this[i] || null; };
        }
        if (nlProto && !nlProto.entries) {
            nlProto.entries = function() {
                var arr = [];
                for (var i = 0; i < this.length; i++) arr.push([i, this[i]]);
                return arr[Symbol.iterator]();
            };
        }
        if (nlProto && !nlProto.keys) {
            nlProto.keys = function() {
                var arr = [];
                for (var i = 0; i < this.length; i++) arr.push(i);
                return arr[Symbol.iterator]();
            };
        }
        if (nlProto && !nlProto.values) {
            nlProto.values = function() {
                var arr = [];
                for (var i = 0; i < this.length; i++) arr.push(this[i]);
                return arr[Symbol.iterator]();
            };
        }
        if (nlProto && !nlProto[Symbol.iterator]) {
            nlProto[Symbol.iterator] = function() {
                var i = 0, self = this;
                return { next: function() {
                    return i < self.length ? { value: self[i++], done: false } : { done: true };
                }};
            };
        }
    }

    // document.createTreeWalker (basic implementation)
    if (!document.createTreeWalker) {
        document.createTreeWalker = function(root, whatToShow, filter) {
            whatToShow = whatToShow || 0xFFFFFFFF;
            var currentNode = root;
            var walker = {
                root: root,
                whatToShow: whatToShow,
                filter: filter || null,
                currentNode: currentNode,
                nextNode: function() {
                    // Simple DFS traversal
                    var node = this.currentNode;
                    if (node.firstChild) {
                        this.currentNode = node.firstChild;
                        return this.currentNode;
                    }
                    while (node) {
                        if (node.nextSibling) {
                            this.currentNode = node.nextSibling;
                            return this.currentNode;
                        }
                        node = node.parentNode;
                        if (node === this.root) return null;
                    }
                    return null;
                },
                previousNode: function() {
                    var node = this.currentNode;
                    if (node === this.root) return null;
                    if (node.previousSibling) {
                        node = node.previousSibling;
                        while (node.lastChild) node = node.lastChild;
                        this.currentNode = node;
                        return this.currentNode;
                    }
                    if (node.parentNode && node.parentNode !== this.root) {
                        this.currentNode = node.parentNode;
                        return this.currentNode;
                    }
                    return null;
                },
                firstChild: function() {
                    var child = this.currentNode.firstChild;
                    if (child) { this.currentNode = child; return child; }
                    return null;
                },
                lastChild: function() {
                    var child = this.currentNode.lastChild;
                    if (child) { this.currentNode = child; return child; }
                    return null;
                },
                nextSibling: function() {
                    var sib = this.currentNode.nextSibling;
                    if (sib) { this.currentNode = sib; return sib; }
                    return null;
                },
                previousSibling: function() {
                    var sib = this.currentNode.previousSibling;
                    if (sib) { this.currentNode = sib; return sib; }
                    return null;
                },
                parentNode: function() {
                    if (this.currentNode === this.root) return null;
                    var p = this.currentNode.parentNode;
                    if (p) { this.currentNode = p; return p; }
                    return null;
                }
            };
            return walker;
        };
    }

    // NodeFilter constants
    if (typeof NodeFilter === 'undefined') {
        globalThis.NodeFilter = {
            SHOW_ALL: 0xFFFFFFFF,
            SHOW_ELEMENT: 1,
            SHOW_TEXT: 4,
            SHOW_COMMENT: 128,
            SHOW_DOCUMENT: 256,
            FILTER_ACCEPT: 1,
            FILTER_REJECT: 2,
            FILTER_SKIP: 3
        };
    }

    // document.createRange (basic)
    if (!document.createRange) {
        document.createRange = function() {
            return {
                startContainer: null, startOffset: 0,
                endContainer: null, endOffset: 0,
                collapsed: true, commonAncestorContainer: null,
                setStart: function(node, offset) { this.startContainer = node; this.startOffset = offset; this.collapsed = false; },
                setEnd: function(node, offset) { this.endContainer = node; this.endOffset = offset; },
                selectNode: function(node) { this.startContainer = node.parentNode; this.endContainer = node.parentNode; },
                selectNodeContents: function(node) { this.startContainer = node; this.endContainer = node; },
                collapse: function(toStart) { this.collapsed = true; },
                cloneRange: function() { return Object.assign({}, this); },
                deleteContents: function() {},
                extractContents: function() { return document.createDocumentFragment(); },
                cloneContents: function() { return document.createDocumentFragment(); },
                insertNode: function(node) {},
                surroundContents: function(node) {},
                getBoundingClientRect: function() { return new DOMRect(0,0,0,0); },
                getClientRects: function() { return []; },
                toString: function() { return ''; }
            };
        };
    }

    // window.getSelection enhancement
    window.getSelection = function() {
        return {
            anchorNode: null, anchorOffset: 0,
            focusNode: null, focusOffset: 0,
            isCollapsed: true, rangeCount: 0, type: 'None',
            addRange: function() {},
            removeAllRanges: function() {},
            getRangeAt: function() { return document.createRange(); },
            collapse: function() {},
            extend: function() {},
            selectAllChildren: function() {},
            toString: function() { return ''; },
            containsNode: function() { return false; }
        };
    };

    // Intl stub (if not present from QuickJS)
    if (typeof Intl === 'undefined') {
        globalThis.Intl = {};
    }
    if (!Intl.NumberFormat) {
        Intl.NumberFormat = function(locale, options) {
            this.format = function(num) { return String(num); };
            this.resolvedOptions = function() { return {}; };
        };
    }
    if (!Intl.DateTimeFormat) {
        Intl.DateTimeFormat = function(locale, options) {
            this.format = function(date) { return date.toString(); };
            this.resolvedOptions = function() { return {}; };
        };
    }

    // Error.captureStackTrace (V8 compat)
    if (!Error.captureStackTrace) {
        Error.captureStackTrace = function(target, ctor) {
            target.stack = (new Error()).stack;
        };
    }

})();
)JS";

    // Now register the native methods that JS wrappers will call
    // We do this by adding __methods to the element prototype via the bindings
    // First, evaluate the JS code to set up prototype chains

    // Before running JS, register native functions on elements
    // We'll add them with __ prefix so the JS can wrap them

    // The approach: we need to add to elem_proto which is the class prototype
    // We can't easily get it from here, so we'll use a different approach:
    // Register global functions that take (this, args) and the JS wraps them

    // Actually, the cleanest approach: add C functions to a global __compat object
    // then have JS install them on the prototype

    JSValue compat = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, compat, "contains",
        JS_NewCFunction(ctx, js_element_contains, "contains", 1));
    JS_SetPropertyStr(ctx, compat, "hasAttribute",
        JS_NewCFunction(ctx, js_element_hasAttribute, "hasAttribute", 1));
    JS_SetPropertyStr(ctx, compat, "hasChildNodes",
        JS_NewCFunction(ctx, js_element_hasChildNodes, "hasChildNodes", 0));
    JS_SetPropertyStr(ctx, compat, "toggleAttribute",
        JS_NewCFunction(ctx, js_element_toggleAttribute, "toggleAttribute", 2));
    JS_SetPropertyStr(ctx, compat, "getAttributeNames",
        JS_NewCFunction(ctx, js_element_getAttributeNames, "getAttributeNames", 0));
    JS_SetPropertyStr(ctx, compat, "insertAdjacentHTML",
        JS_NewCFunction(ctx, js_element_insertAdjacentHTML, "insertAdjacentHTML", 2));
    JS_SetPropertyStr(ctx, compat, "insertAdjacentElement",
        JS_NewCFunction(ctx, js_element_insertAdjacentElement, "insertAdjacentElement", 2));
    JS_SetPropertyStr(ctx, compat, "insertAdjacentText",
        JS_NewCFunction(ctx, js_element_insertAdjacentText, "insertAdjacentText", 2));
    JS_SetPropertyStr(ctx, compat, "append",
        JS_NewCFunction(ctx, js_element_append, "append", 0));
    JS_SetPropertyStr(ctx, compat, "prepend",
        JS_NewCFunction(ctx, js_element_prepend, "prepend", 0));
    JS_SetPropertyStr(ctx, compat, "before",
        JS_NewCFunction(ctx, js_element_before, "before", 0));
    JS_SetPropertyStr(ctx, compat, "after",
        JS_NewCFunction(ctx, js_element_after, "after", 0));
    JS_SetPropertyStr(ctx, compat, "replaceWith",
        JS_NewCFunction(ctx, js_element_replaceWith, "replaceWith", 0));
    JS_SetPropertyStr(ctx, compat, "replaceChildren",
        JS_NewCFunction(ctx, js_element_replaceChildren, "replaceChildren", 0));
    JS_SetPropertyStr(ctx, compat, "focus",
        JS_NewCFunction(ctx, js_element_focus, "focus", 0));
    JS_SetPropertyStr(ctx, compat, "blur",
        JS_NewCFunction(ctx, js_element_blur, "blur", 0));
    JS_SetPropertyStr(ctx, compat, "click",
        JS_NewCFunction(ctx, js_element_click, "click", 0));
    JS_SetPropertyStr(ctx, compat, "scrollIntoView",
        JS_NewCFunction(ctx, js_element_scrollIntoView, "scrollIntoView", 0));
    JS_SetPropertyStr(ctx, compat, "normalize",
        JS_NewCFunction(ctx, js_element_normalize, "normalize", 0));
    JS_SetPropertyStr(ctx, compat, "get_outerHTML",
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_outerHTML, "get_outerHTML", 0));
    JS_SetPropertyStr(ctx, compat, "set_outerHTML",
        JS_NewCFunction(ctx, js_element_set_outerHTML, "set_outerHTML", 1));
    JS_SetPropertyStr(ctx, compat, "get_isConnected",
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_isConnected, "get_isConnected", 0));
    JS_SetPropertyStr(ctx, compat, "get_childElementCount",
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_childElementCount, "get_childElementCount", 0));
    JS_SetPropertyStr(ctx, compat, "get_firstElementChild",
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_firstElementChild, "get_firstElementChild", 0));
    JS_SetPropertyStr(ctx, compat, "get_lastElementChild",
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_lastElementChild, "get_lastElementChild", 0));
    JS_SetPropertyStr(ctx, compat, "get_innerText",
        JS_NewCFunction(ctx, (JSCFunction*)js_element_get_innerText, "get_innerText", 0));
    JS_SetPropertyStr(ctx, compat, "set_innerText",
        JS_NewCFunction(ctx, js_element_set_innerText, "set_innerText", 1));

    JS_SetPropertyStr(ctx, global, "__compat", compat);

    // Now run JS that installs these on the element prototype using proper `this` binding
    const char* install_js = R"JS(
(function() {
    var C = __compat;
    var _tmpEl = document.createElement('div');
    var elemProto = Object.getPrototypeOf(_tmpEl);

    // Methods (bind 'this' correctly by using .call)
    elemProto.contains = function(o) { return C.contains.call(this, o); };
    elemProto.hasAttribute = function(n) { return C.hasAttribute.call(this, n); };
    elemProto.hasChildNodes = function() { return C.hasChildNodes.call(this); };
    elemProto.toggleAttribute = function(n, f) { return C.toggleAttribute.call(this, n, f); };
    elemProto.getAttributeNames = function() { return C.getAttributeNames.call(this); };
    elemProto.insertAdjacentHTML = function(p, h) { return C.insertAdjacentHTML.call(this, p, h); };
    elemProto.insertAdjacentElement = function(p, e) { return C.insertAdjacentElement.call(this, p, e); };
    elemProto.insertAdjacentText = function(p, t) { return C.insertAdjacentText.call(this, p, t); };
    elemProto.append = function() { return C.append.apply(this, arguments); };
    elemProto.prepend = function() { return C.prepend.apply(this, arguments); };
    elemProto.before = function() { return C.before.apply(this, arguments); };
    elemProto.after = function() { return C.after.apply(this, arguments); };
    elemProto.replaceWith = function() { return C.replaceWith.apply(this, arguments); };
    elemProto.replaceChildren = function() { return C.replaceChildren.apply(this, arguments); };
    elemProto.focus = function() { return C.focus.call(this); };
    elemProto.blur = function() { return C.blur.call(this); };
    elemProto.click = function() { return C.click.call(this); };
    elemProto.scrollIntoView = function() { return C.scrollIntoView.call(this); };
    elemProto.normalize = function() { return C.normalize.call(this); };

    // Getters/setters
    Object.defineProperty(elemProto, 'outerHTML', {
        get: function() { return C.get_outerHTML.call(this); },
        set: function(v) { return C.set_outerHTML.call(this, v); },
        configurable: true
    });
    Object.defineProperty(elemProto, 'isConnected', {
        get: function() { return C.get_isConnected.call(this); },
        configurable: true
    });
    Object.defineProperty(elemProto, 'childElementCount', {
        get: function() { return C.get_childElementCount.call(this); },
        configurable: true
    });
    Object.defineProperty(elemProto, 'firstElementChild', {
        get: function() { return C.get_firstElementChild.call(this); },
        configurable: true
    });
    Object.defineProperty(elemProto, 'lastElementChild', {
        get: function() { return C.get_lastElementChild.call(this); },
        configurable: true
    });
    Object.defineProperty(elemProto, 'innerText', {
        get: function() { return C.get_innerText.call(this); },
        set: function(v) { return C.set_innerText.call(this, v); },
        configurable: true
    });

    // getClientRects
    elemProto.getClientRects = function() {
        var rect = this.getBoundingClientRect();
        var list = [rect];
        list.item = function(i) { return list[i] || null; };
        return list;
    };

    // compareDocumentPosition
    elemProto.compareDocumentPosition = function(other) {
        if (this === other) return 0;
        if (this.contains(other)) return 20;
        if (other && other.contains && other.contains(this)) return 10;
        return 4;
    };

    elemProto.isSameNode = function(other) { return this === other; };
    elemProto.isEqualNode = function(other) {
        if (!other) return false;
        return this.nodeType === other.nodeType && this.nodeName === other.nodeName &&
               this.textContent === other.textContent;
    };
    elemProto.getRootNode = function() {
        var n = this; while (n.parentNode) n = n.parentNode; return n;
    };

    // Delete __compat from global
    delete globalThis.__compat;
})();
)JS";

    engine->eval(install_js, "<compat-install>");

    // Run the main compat JS
    engine->eval(compat_js, "<compat>");

    JS_FreeValue(ctx, global);
}
