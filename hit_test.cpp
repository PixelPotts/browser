// hit_test.cpp - Point-to-element mapping from layout tree
#include "hit_test.h"

static void hit_test_box(LayoutBox* box, float x, float y,
                          float offset_x, float offset_y, HitTestResult& result) {
    if (!box || box->type == LayoutBoxType::None) return;

    float cx = offset_x + box->content_rect.x;
    float cy = offset_y + box->content_rect.y;

    // Check if point is within the border box
    Rect bb = box->border_box();
    bb.x += offset_x;
    bb.y += offset_y;

    if (bb.contains(x, y)) {
        // This box contains the point
        if (box->dom_node && box->dom_node->node_type == DOMNode::ELEMENT)
            result = {box->dom_node, x - bb.x, y - bb.y};
    }

    // Check for overflow clipping
    if (box->overflow == 1) { // hidden
        if (!bb.contains(x, y)) return;
    }

    // Check children (later children paint on top, so check in reverse)
    float child_offset_x = cx - box->scroll_x;
    float child_offset_y = cy - box->scroll_y;

    for (int i = (int)box->children.size() - 1; i >= 0; --i) {
        hit_test_box(box->children[i].get(), x, y, child_offset_x, child_offset_y, result);
    }
}

HitTestResult hit_test(LayoutBox* root, float x, float y) {
    HitTestResult result;
    if (!root) return result;
    hit_test_box(root, x, y, 0, 0, result);
    return result;
}

HitTestResult hit_test_viewport(LayoutBox* root, float vx, float vy,
                                 float scroll_x, float scroll_y) {
    return hit_test(root, vx + scroll_x, vy + scroll_y);
}
