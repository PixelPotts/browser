// paint.cpp - Display list generation and Cairo rendering
#include "paint.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <unordered_map>

// ---- Named CSS colors ----

static const std::unordered_map<std::string, uint32_t>& css_named_colors() {
    static const std::unordered_map<std::string, uint32_t> colors = {
        {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000},
        {"green", 0x008000}, {"blue", 0x0000FF}, {"yellow", 0xFFFF00},
        {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF}, {"gray", 0x808080},
        {"grey", 0x808080}, {"silver", 0xC0C0C0}, {"maroon", 0x800000},
        {"olive", 0x808000}, {"lime", 0x00FF00}, {"aqua", 0x00FFFF},
        {"teal", 0x008080}, {"navy", 0x000080}, {"fuchsia", 0xFF00FF},
        {"purple", 0x800080}, {"orange", 0xFFA500}, {"pink", 0xFFC0CB},
        {"brown", 0xA52A2A}, {"coral", 0xFF7F50}, {"crimson", 0xDC143C},
        {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgray", 0xA9A9A9},
        {"darkgrey", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkmagenta", 0x8B008B},
        {"darkorange", 0xFF8C00}, {"darkred", 0x8B0000}, {"darkviolet", 0x9400D3},
        {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969},
        {"dimgrey", 0x696969}, {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222},
        {"forestgreen", 0x228B22}, {"gold", 0xFFD700}, {"goldenrod", 0xDAA520},
        {"greenyellow", 0xADFF2F}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C},
        {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C},
        {"lavender", 0xE6E6FA}, {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD},
        {"lightblue", 0xADD8E6}, {"lightcoral", 0xF08080}, {"lightcyan", 0xE0FFFF},
        {"lightgray", 0xD3D3D3}, {"lightgrey", 0xD3D3D3}, {"lightgreen", 0x90EE90},
        {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A}, {"lightskyblue", 0x87CEFA},
        {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0},
        {"limegreen", 0x32CD32}, {"linen", 0xFAF0E6}, {"mediumblue", 0x0000CD},
        {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB},
        {"mediumseagreen", 0x3CB371}, {"mediumslateblue", 0x7B68EE},
        {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC},
        {"mediumvioletred", 0xC71585}, {"midnightblue", 0x191970},
        {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1}, {"moccasin", 0xFFE4B5},
        {"navajowhite", 0xFFDEAD}, {"oldlace", 0xFDF5E6}, {"olivedrab", 0x6B8E23},
        {"orangered", 0xFF4500}, {"orchid", 0xDA70D6}, {"palegoldenrod", 0xEEE8AA},
        {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE},
        {"palevioletred", 0xDB7093}, {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9},
        {"peru", 0xCD853F}, {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6},
        {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1}, {"saddlebrown", 0x8B4513},
        {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57},
        {"seashell", 0xFFF5EE}, {"sienna", 0xA0522D}, {"skyblue", 0x87CEEB},
        {"slateblue", 0x6A5ACD}, {"slategray", 0x708090}, {"slategrey", 0x708090},
        {"snow", 0xFFFAFA}, {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4},
        {"tan", 0xD2B48C}, {"thistle", 0xD8BFD8}, {"tomato", 0xFF6347},
        {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE}, {"wheat", 0xF5DEB3},
        {"whitesmoke", 0xF5F5F5}, {"yellowgreen", 0x9ACD32},
        {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aquamarine", 0x7FFFD4},
        {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4},
        {"blanchedalmond", 0xFFEBCD}, {"blueviolet", 0x8A2BE2}, {"burlywood", 0xDEB887},
        {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E},
        {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC},
        {"darkgoldenrod", 0xB8860B}, {"darkkhaki", 0xBDB76B},
        {"darkolivegreen", 0x556B2F}, {"darkorchid", 0x9932CC},
        {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F},
        {"darkslateblue", 0x483D8B}, {"darkslategray", 0x2F4F4F},
        {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1},
        {"floralwhite", 0xFFFAF0}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF},
        {"honeydew", 0xF0FFF0}, {"lavenderblush", 0xFFF0F5},
        {"lightgoldenrodyellow", 0xFAFAD2}, {"lightseagreen", 0x20B2AA},
        {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
        {"mediumaquamarine", 0x66CDAA}, {"transparent", 0x00000000},
    };
    return colors;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

CairoColor parse_css_color(const std::string& color) {
    CairoColor c = {0, 0, 0, 1.0};
    if (color.empty()) return c;

    // Skip leading/trailing whitespace
    std::string s = color;
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();

    if (s == "transparent") return {0, 0, 0, 0};
    if (s == "currentColor" || s == "currentcolor" || s == "inherit") return {0, 0, 0, 1.0};

    // Hex colors
    if (s[0] == '#') {
        if (s.size() == 4) { // #RGB
            c.r = hex_digit(s[1]) / 15.0;
            c.g = hex_digit(s[2]) / 15.0;
            c.b = hex_digit(s[3]) / 15.0;
        } else if (s.size() == 5) { // #RGBA
            c.r = hex_digit(s[1]) / 15.0;
            c.g = hex_digit(s[2]) / 15.0;
            c.b = hex_digit(s[3]) / 15.0;
            c.a = hex_digit(s[4]) / 15.0;
        } else if (s.size() == 7) { // #RRGGBB
            c.r = (hex_digit(s[1]) * 16 + hex_digit(s[2])) / 255.0;
            c.g = (hex_digit(s[3]) * 16 + hex_digit(s[4])) / 255.0;
            c.b = (hex_digit(s[5]) * 16 + hex_digit(s[6])) / 255.0;
        } else if (s.size() == 9) { // #RRGGBBAA
            c.r = (hex_digit(s[1]) * 16 + hex_digit(s[2])) / 255.0;
            c.g = (hex_digit(s[3]) * 16 + hex_digit(s[4])) / 255.0;
            c.b = (hex_digit(s[5]) * 16 + hex_digit(s[6])) / 255.0;
            c.a = (hex_digit(s[7]) * 16 + hex_digit(s[8])) / 255.0;
        }
        return c;
    }

    // rgb()/rgba()
    if (s.substr(0, 4) == "rgba" || s.substr(0, 3) == "rgb") {
        size_t start = s.find('(');
        size_t end = s.find(')');
        if (start != std::string::npos && end != std::string::npos) {
            std::string inner = s.substr(start + 1, end - start - 1);
            // Replace commas and slashes with spaces
            for (char& ch : inner) {
                if (ch == ',' || ch == '/') ch = ' ';
            }
            float vals[4] = {0, 0, 0, 1.0};
            int idx = 0;
            size_t pos = 0;
            while (pos < inner.size() && idx < 4) {
                while (pos < inner.size() && inner[pos] == ' ') ++pos;
                if (pos >= inner.size()) break;
                size_t npos;
                float val = 0;
                try { val = std::stof(inner.substr(pos), &npos); } catch (...) { break; }
                pos += npos;
                // Check for percentage
                if (pos < inner.size() && inner[pos] == '%') {
                    val = val / 100.0f * 255.0f;
                    ++pos;
                }
                vals[idx++] = val;
            }
            c.r = vals[0] / 255.0;
            c.g = vals[1] / 255.0;
            c.b = vals[2] / 255.0;
            if (idx >= 4) c.a = vals[3] > 1.0 ? vals[3] / 255.0 : vals[3];
            else if (s.substr(0, 4) == "rgba" && idx == 4) c.a = vals[3];
        }
        return c;
    }

    // hsl()/hsla()
    if (s.substr(0, 3) == "hsl") {
        size_t start = s.find('(');
        size_t end = s.find(')');
        if (start != std::string::npos && end != std::string::npos) {
            std::string inner = s.substr(start + 1, end - start - 1);
            for (char& ch : inner) {
                if (ch == ',' || ch == '/') ch = ' ';
            }
            float vals[4] = {0, 0, 0, 1.0};
            int idx = 0;
            size_t pos = 0;
            while (pos < inner.size() && idx < 4) {
                while (pos < inner.size() && inner[pos] == ' ') ++pos;
                if (pos >= inner.size()) break;
                size_t npos;
                float val = 0;
                try { val = std::stof(inner.substr(pos), &npos); } catch (...) { break; }
                pos += npos;
                // Skip "deg" or "%"
                while (pos < inner.size() && (inner[pos] == '%' || inner[pos] == 'd' ||
                       inner[pos] == 'e' || inner[pos] == 'g'))
                    ++pos;
                vals[idx++] = val;
            }
            // H is 0-360, S and L are percentages
            float h = fmod(vals[0], 360.0f);
            if (h < 0) h += 360;
            float sat = vals[1] / 100.0f;
            float lit = vals[2] / 100.0f;
            if (idx >= 4) c.a = vals[3] > 1.0 ? vals[3] / 255.0 : vals[3];

            // HSL to RGB conversion
            float C = (1.0f - fabsf(2 * lit - 1)) * sat;
            float X = C * (1.0f - fabsf(fmod(h / 60.0f, 2.0f) - 1.0f));
            float m = lit - C / 2;
            float r1, g1, b1;
            if (h < 60)        { r1 = C; g1 = X; b1 = 0; }
            else if (h < 120)  { r1 = X; g1 = C; b1 = 0; }
            else if (h < 180)  { r1 = 0; g1 = C; b1 = X; }
            else if (h < 240)  { r1 = 0; g1 = X; b1 = C; }
            else if (h < 300)  { r1 = X; g1 = 0; b1 = C; }
            else               { r1 = C; g1 = 0; b1 = X; }
            c.r = r1 + m;
            c.g = g1 + m;
            c.b = b1 + m;
        }
        return c;
    }

    // Named colors
    std::string lower = s;
    for (char& ch : lower) ch = (char)tolower((unsigned char)ch);
    auto& names = css_named_colors();
    auto it = names.find(lower);
    if (it != names.end()) {
        uint32_t v = it->second;
        if (lower == "transparent") {
            c.a = 0;
        } else {
            c.r = ((v >> 16) & 0xFF) / 255.0;
            c.g = ((v >> 8) & 0xFF) / 255.0;
            c.b = (v & 0xFF) / 255.0;
        }
        return c;
    }

    return c; // default black
}

// ---- Display list generation ----

static void paint_box(LayoutBox* box, DisplayList& dl, float offset_x, float offset_y) {
    if (!box) return;
    if (box->type == LayoutBoxType::None) return;

    DOMNode* node = box->dom_node;
    float cx = offset_x + box->content_rect.x;
    float cy = offset_y + box->content_rect.y;

    // Background
    std::string bg = box->bg_color;
    if (!bg.empty() && bg != "transparent") {
        CairoColor bgc = parse_css_color(bg);
        Rect bb = box->border_box();
        bb.x += offset_x;
        bb.y += offset_y;
        PaintCommand cmd;
        cmd.type = PaintCmdType::FillRect;
        cmd.rect = bb;
        cmd.color = bgc;
        cmd.border_radius = node ? node->border_radius : 0;
        cmd.dom_node = node;
        dl.push_back(cmd);
    }

    // List markers for <li> elements
    if (node && node->tag_name == "li") {
        // Determine marker type from parent
        bool ordered = false;
        int index = 1;
        if (node->parent) {
            ordered = (node->parent->tag_name == "ol");
            // Count preceding <li> siblings for numbering
            if (ordered) {
                index = 0;
                for (auto& sibling : node->parent->children) {
                    if (sibling->tag_name == "li") index++;
                    if (sibling.get() == node) break;
                }
            }
        }

        // Resolve text color for the marker
        std::string mc;
        LayoutBox* b = box;
        while (b) {
            if (!b->color.empty()) { mc = b->color; break; }
            b = b->parent;
        }
        if (mc.empty()) mc = "black";
        CairoColor marker_c = parse_css_color(mc);

        PaintCommand cmd;
        cmd.type = PaintCmdType::DrawText;
        cmd.text_color = marker_c;
        cmd.text_x = cx - 20; // position marker to the left
        cmd.text_y = cy;
        cmd.dom_node = node;

        // Create a temporary pango layout for the marker
        PangoLayout* marker_layout = pango_layout_copy(
            box->pango_layout ? box->pango_layout :
            (!box->children.empty() && box->children[0]->pango_layout ? box->children[0]->pango_layout : nullptr));
        if (marker_layout) {
            if (ordered) {
                std::string num = std::to_string(index) + ".";
                pango_layout_set_text(marker_layout, num.c_str(), -1);
            } else {
                pango_layout_set_text(marker_layout, "\xe2\x80\xa2", -1); // bullet •
            }
            pango_layout_set_width(marker_layout, -1); // no wrapping
            cmd.pango_layout = marker_layout;
            dl.push_back(cmd);
            // Note: marker_layout leaked - TODO: track and free
        }
    }

    // Border
    bool has_border = box->border.top > 0 || box->border.right > 0 ||
                      box->border.bottom > 0 || box->border.left > 0;
    if (has_border) {
        std::string bs = box->border_style;
        if (bs.empty()) bs = "solid";
        if (bs != "none" && bs != "hidden") {
            Rect bb = box->border_box();
            bb.x += offset_x;
            bb.y += offset_y;
            CairoColor bc = parse_css_color(
                box->border_color.empty() ? "black" : box->border_color);
            PaintCommand cmd;
            cmd.type = PaintCmdType::DrawBorder;
            cmd.border_rect = bb;
            cmd.border_widths = box->border;
            cmd.border_color_val = bc;
            cmd.border_radius = node ? node->border_radius : 0;
            cmd.dom_node = node;
            dl.push_back(cmd);
        }
    }

    // Opacity
    bool has_opacity = box->opacity < 1.0 && box->opacity >= 0;
    if (has_opacity) {
        PaintCommand cmd;
        cmd.type = PaintCmdType::PushOpacity;
        cmd.opacity = (float)box->opacity;
        dl.push_back(cmd);
    }

    // Clip for overflow
    bool clip = (box->overflow == 1 || box->overflow == 2 || box->overflow == 3);
    if (clip) {
        PaintCommand cmd;
        cmd.type = PaintCmdType::PushClip;
        Rect bb = box->border_box();
        bb.x += offset_x;
        bb.y += offset_y;
        cmd.clip_rect = bb;
        dl.push_back(cmd);
    }

    // Text content (inline formatting context)
    if (box->pango_layout) {
        std::string text_color_str;
        LayoutBox* b = box;
        while (b) {
            if (!b->color.empty()) { text_color_str = b->color; break; }
            b = b->parent;
        }
        if (text_color_str.empty()) text_color_str = "black";

        CairoColor tc = parse_css_color(text_color_str);
        PaintCommand cmd;
        cmd.type = PaintCmdType::DrawText;
        cmd.pango_layout = box->pango_layout;
        cmd.text_x = cx;
        cmd.text_y = cy;
        cmd.text_color = tc;
        cmd.dom_node = node;
        dl.push_back(cmd);
    }

    // Replaced elements
    if (box->type == LayoutBoxType::Replaced && box->replaced_surface) {
        PaintCommand cmd;
        cmd.type = PaintCmdType::DrawImage;
        cmd.surface = box->replaced_surface;
        cmd.dest_rect = {cx, cy, box->content_rect.w, box->content_rect.h};
        cmd.dom_node = node;
        dl.push_back(cmd);
    }

    // Children
    // Apply scroll offset for overflow containers
    float child_offset_x = cx - box->scroll_x;
    float child_offset_y = cy - box->scroll_y;

    for (auto& child : box->children) {
        paint_box(child.get(), dl, child_offset_x, child_offset_y);
    }

    // Pop clip
    if (clip) {
        PaintCommand cmd;
        cmd.type = PaintCmdType::PopClip;
        dl.push_back(cmd);
    }

    // Pop opacity
    if (has_opacity) {
        PaintCommand cmd;
        cmd.type = PaintCmdType::PopOpacity;
        cmd.opacity = (float)box->opacity;
        dl.push_back(cmd);
    }
}

DisplayList generate_display_list(LayoutBox* root) {
    DisplayList dl;
    if (!root) return dl;
    paint_box(root, dl, 0, 0);
    return dl;
}

// ---- Cairo rendering ----

void render_display_list(cairo_t* cr, const DisplayList& dl, float scroll_x, float scroll_y,
                          float viewport_width, float viewport_height) {
    cairo_save(cr);

    // Apply scroll transform
    cairo_translate(cr, -scroll_x, -scroll_y);

    for (const auto& cmd : dl) {
        switch (cmd.type) {
            case PaintCmdType::FillRect: {
                cairo_set_source_rgba(cr, cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);
                if (cmd.border_radius > 0) {
                    float rx = cmd.rect.x, ry = cmd.rect.y;
                    float rw = cmd.rect.w, rh = cmd.rect.h;
                    float hr = std::min((float)cmd.border_radius, rw / 2);
                    float vr = std::min((float)cmd.border_radius, rh / 2);
                    cairo_new_path(cr);
                    cairo_arc(cr, rx + hr, ry + vr, hr, M_PI, 1.5 * M_PI);
                    cairo_arc(cr, rx + rw - hr, ry + vr, hr, 1.5 * M_PI, 2 * M_PI);
                    cairo_arc(cr, rx + rw - hr, ry + rh - vr, vr, 0, 0.5 * M_PI);
                    cairo_arc(cr, rx + hr, ry + rh - vr, vr, 0.5 * M_PI, M_PI);
                    cairo_close_path(cr);
                    cairo_fill(cr);
                } else {
                    cairo_rectangle(cr, cmd.rect.x, cmd.rect.y, cmd.rect.w, cmd.rect.h);
                    cairo_fill(cr);
                }
                break;
            }

            case PaintCmdType::DrawText: {
                if (!cmd.pango_layout) break;
                cairo_set_source_rgba(cr, cmd.text_color.r, cmd.text_color.g,
                                       cmd.text_color.b, cmd.text_color.a);
                cairo_move_to(cr, cmd.text_x, cmd.text_y);
                pango_cairo_show_layout(cr, cmd.pango_layout);
                break;
            }

            case PaintCmdType::DrawBorder: {
                const auto& r = cmd.border_rect;
                const auto& bw = cmd.border_widths;
                const auto& bc = cmd.border_color_val;
                int radius = cmd.border_radius;

                cairo_set_source_rgba(cr, bc.r, bc.g, bc.b, bc.a);

                if (radius > 0) {
                    // Rounded border: draw as stroked rounded rect
                    float avg_w = (bw.top + bw.right + bw.bottom + bw.left) / 4.0f;
                    float hr = std::min((float)radius, r.w / 2);
                    float vr = std::min((float)radius, r.h / 2);

                    float x = r.x + avg_w / 2;
                    float y = r.y + avg_w / 2;
                    float w = r.w - avg_w;
                    float h = r.h - avg_w;

                    cairo_new_path(cr);
                    cairo_arc(cr, x + hr, y + vr, hr, M_PI, 1.5 * M_PI);
                    cairo_arc(cr, x + w - hr, y + vr, hr, 1.5 * M_PI, 2 * M_PI);
                    cairo_arc(cr, x + w - hr, y + h - vr, vr, 0, 0.5 * M_PI);
                    cairo_arc(cr, x + hr, y + h - vr, vr, 0.5 * M_PI, M_PI);
                    cairo_close_path(cr);

                    cairo_set_line_width(cr, avg_w);
                    cairo_stroke(cr);
                } else {
                    // Simple border: draw four rectangles
                    // Top
                    if (bw.top > 0) {
                        cairo_rectangle(cr, r.x, r.y, r.w, bw.top);
                        cairo_fill(cr);
                    }
                    // Bottom
                    if (bw.bottom > 0) {
                        cairo_rectangle(cr, r.x, r.y + r.h - bw.bottom, r.w, bw.bottom);
                        cairo_fill(cr);
                    }
                    // Left
                    if (bw.left > 0) {
                        cairo_rectangle(cr, r.x, r.y + bw.top,
                                        bw.left, r.h - bw.top - bw.bottom);
                        cairo_fill(cr);
                    }
                    // Right
                    if (bw.right > 0) {
                        cairo_rectangle(cr, r.x + r.w - bw.right, r.y + bw.top,
                                        bw.right, r.h - bw.top - bw.bottom);
                        cairo_fill(cr);
                    }
                }
                break;
            }

            case PaintCmdType::DrawImage: {
                if (!cmd.surface) break;
                int sw = cairo_image_surface_get_width(cmd.surface);
                int sh = cairo_image_surface_get_height(cmd.surface);
                if (sw <= 0 || sh <= 0) break;

                cairo_save(cr);
                cairo_translate(cr, cmd.dest_rect.x, cmd.dest_rect.y);
                cairo_scale(cr, cmd.dest_rect.w / sw, cmd.dest_rect.h / sh);
                cairo_set_source_surface(cr, cmd.surface, 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);
                break;
            }

            case PaintCmdType::PushClip: {
                cairo_save(cr);
                cairo_rectangle(cr, cmd.clip_rect.x, cmd.clip_rect.y,
                                cmd.clip_rect.w, cmd.clip_rect.h);
                cairo_clip(cr);
                break;
            }

            case PaintCmdType::PopClip: {
                cairo_restore(cr);
                break;
            }

            case PaintCmdType::PushOpacity: {
                cairo_push_group(cr);
                break;
            }

            case PaintCmdType::PopOpacity: {
                cairo_pop_group_to_source(cr);
                cairo_paint_with_alpha(cr, cmd.opacity);
                break;
            }

            case PaintCmdType::Translate: {
                cairo_save(cr);
                cairo_translate(cr, cmd.tx, cmd.ty);
                break;
            }

            case PaintCmdType::PopTranslate: {
                cairo_restore(cr);
                break;
            }
        }
    }

    cairo_restore(cr);
}
