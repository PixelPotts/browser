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

// ---- Box shadow parsing ----

struct ParsedBoxShadow {
    float offset_x = 0, offset_y = 0, blur = 0, spread = 0;
    CairoColor color;
    bool inset = false;
};

static std::vector<ParsedBoxShadow> parse_box_shadow(const std::string& raw) {
    std::vector<ParsedBoxShadow> shadows;
    if (raw.empty() || raw == "none") return shadows;

    // Split by comma (but not inside parens)
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (char c : raw) {
        if (c == '(') depth++;
        else if (c == ')') depth--;
        if (c == ',' && depth == 0) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);

    for (auto& part : parts) {
        ParsedBoxShadow s;
        s.color = {0, 0, 0, 0.5};

        // Tokenize
        std::vector<std::string> tokens;
        std::string tok;
        int d = 0;
        for (char c : part) {
            if (c == '(') d++;
            else if (c == ')') d--;
            if (c == ' ' && d == 0) {
                if (!tok.empty()) tokens.push_back(tok);
                tok.clear();
            } else {
                tok += c;
            }
        }
        if (!tok.empty()) tokens.push_back(tok);

        // Parse: [inset] offsetX offsetY [blur [spread]] [color]
        std::vector<float> nums;
        bool found_color = false;
        for (auto& t : tokens) {
            if (t == "inset") { s.inset = true; continue; }
            // Try parse as number
            bool is_num = false;
            try {
                size_t pos;
                float v = std::stof(t, &pos);
                // Check for px suffix
                if (pos < t.size() && t[pos] == 'p') pos += 2;
                if (pos >= t.size() - 1 || t.size() == pos) {
                    nums.push_back(v);
                    is_num = true;
                }
            } catch (...) {}
            if (!is_num) {
                // It's a color
                s.color = parse_css_color(t);
                found_color = true;
            }
        }
        if (nums.size() >= 2) {
            s.offset_x = nums[0];
            s.offset_y = nums[1];
            if (nums.size() >= 3) s.blur = nums[2];
            if (nums.size() >= 4) s.spread = nums[3];
        }
        if (!found_color) s.color = {0, 0, 0, 0.3};
        shadows.push_back(s);
    }
    return shadows;
}

// ---- Text shadow parsing ----

struct ParsedTextShadow {
    float offset_x = 0, offset_y = 0, blur = 0;
    CairoColor color;
};

static std::vector<ParsedTextShadow> parse_text_shadow(const std::string& raw) {
    std::vector<ParsedTextShadow> shadows;
    if (raw.empty() || raw == "none") return shadows;

    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (char c : raw) {
        if (c == '(') depth++;
        else if (c == ')') depth--;
        if (c == ',' && depth == 0) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);

    for (auto& part : parts) {
        ParsedTextShadow s;
        s.color = {0, 0, 0, 0.5};

        std::vector<std::string> tokens;
        std::string tok;
        int d = 0;
        for (char c : part) {
            if (c == '(') d++;
            else if (c == ')') d--;
            if (c == ' ' && d == 0) {
                if (!tok.empty()) tokens.push_back(tok);
                tok.clear();
            } else {
                tok += c;
            }
        }
        if (!tok.empty()) tokens.push_back(tok);

        std::vector<float> nums;
        for (auto& t : tokens) {
            bool is_num = false;
            try {
                size_t pos;
                float v = std::stof(t, &pos);
                if (pos < t.size() && t[pos] == 'p') pos += 2;
                if (pos >= t.size() - 1 || t.size() == pos) {
                    nums.push_back(v);
                    is_num = true;
                }
            } catch (...) {}
            if (!is_num) {
                s.color = parse_css_color(t);
            }
        }
        if (nums.size() >= 2) {
            s.offset_x = nums[0];
            s.offset_y = nums[1];
            if (nums.size() >= 3) s.blur = nums[2];
        }
        shadows.push_back(s);
    }
    return shadows;
}

// ---- Display list generation ----

static void paint_box(LayoutBox* box, DisplayList& dl, float offset_x, float offset_y,
                       bool skip_fixed = false) {
    if (!box) return;
    if (box->type == LayoutBoxType::None) return;

    // Skip fixed-position elements in normal paint pass (they're painted separately)
    if (skip_fixed && box->position == 3) return;

    // Visibility: hidden — skip painting but keep space (children still paint if they override)
    bool hidden = (box->visibility == 1);

    DOMNode* node = box->dom_node;
    float cx = offset_x + box->content_rect.x;
    float cy = offset_y + box->content_rect.y;

    // Debug: trace form control painting
    if (node && (node->tag_name == "input" || node->tag_name == "textarea") && box->type != LayoutBoxType::Text) {
        Rect bb = box->border_box();
        fprintf(stderr, "[PAINT-FORM] tag=%s bg='%s' border_box=(%.0f,%.0f,%.0f,%.0f) cx=%.0f cy=%.0f type=%d\n",
            node->tag_name.c_str(), box->bg_color.c_str(),
            bb.x + offset_x, bb.y + offset_y, bb.w, bb.h, cx, cy, (int)box->type);
    }

    if (!hidden) {
        // Box shadow (rendered before background)
        if (!box->box_shadow.empty()) {
            auto shadows = parse_box_shadow(box->box_shadow);
            Rect bb = box->border_box();
            bb.x += offset_x;
            bb.y += offset_y;
            for (auto& s : shadows) {
                PaintCommand cmd;
                cmd.type = PaintCmdType::DrawBoxShadow;
                cmd.shadow_offset_x = s.offset_x;
                cmd.shadow_offset_y = s.offset_y;
                cmd.shadow_blur = s.blur;
                cmd.shadow_spread = s.spread;
                cmd.shadow_color = s.color;
                cmd.shadow_inset = s.inset;
                cmd.shadow_box_rect = bb;
                cmd.shadow_border_radius = node ? node->border_radius : 0;
                cmd.dom_node = node;
                dl.push_back(cmd);
            }
        }

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

        // Background image
        if (!box->bg_image.empty() && node) {
            Rect bb = box->border_box();
            bb.x += offset_x;
            bb.y += offset_y;
            PaintCommand cmd;
            cmd.type = PaintCmdType::DrawBackgroundImage;
            cmd.bg_rect = bb;
            cmd.border_radius = node ? node->border_radius : 0;
            cmd.dom_node = node;
            // bg_surface will be connected externally
            // Store size/position/repeat info from style_props
            auto it_sz = node->style_props.find("background-size");
            if (it_sz != node->style_props.end()) cmd.bg_size_mode = it_sz->second;
            auto it_pos = node->style_props.find("background-position");
            if (it_pos != node->style_props.end()) cmd.bg_position_str = it_pos->second;
            auto it_rep = node->style_props.find("background-repeat");
            if (it_rep != node->style_props.end()) cmd.bg_repeat_mode = it_rep->second;
            else cmd.bg_repeat_mode = "repeat";
            dl.push_back(cmd);
        }

        // List markers for <li> elements
        if (node && node->tag_name == "li") {
            bool ordered = false;
            int index = 1;
            if (node->parent) {
                ordered = (node->parent->tag_name == "ol");
                if (ordered) {
                    index = 0;
                    for (auto& sibling : node->parent->children) {
                        if (sibling->tag_name == "li") index++;
                        if (sibling.get() == node) break;
                    }
                }
            }
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
            cmd.text_x = cx - 20;
            cmd.text_y = cy;
            cmd.dom_node = node;

            PangoLayout* marker_layout = pango_layout_copy(
                box->pango_layout ? box->pango_layout :
                (!box->children.empty() && box->children[0]->pango_layout ? box->children[0]->pango_layout : nullptr));
            if (marker_layout) {
                if (ordered) {
                    std::string num = std::to_string(index) + ".";
                    pango_layout_set_text(marker_layout, num.c_str(), -1);
                } else {
                    pango_layout_set_text(marker_layout, "\xe2\x80\xa2", -1);
                }
                pango_layout_set_width(marker_layout, -1);
                cmd.pango_layout = marker_layout;
                dl.push_back(cmd);
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

    if (!hidden) {
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

            // Text shadow
            std::string ts_raw = box->text_shadow;
            if (ts_raw.empty()) {
                // Inherit from ancestors
                LayoutBox* p = box->parent;
                while (p) {
                    if (!p->text_shadow.empty()) { ts_raw = p->text_shadow; break; }
                    p = p->parent;
                }
            }
            if (!ts_raw.empty() && ts_raw != "none") {
                auto ts_list = parse_text_shadow(ts_raw);
                if (!ts_list.empty()) {
                    cmd.has_text_shadow = true;
                    cmd.ts_offset_x = ts_list[0].offset_x;
                    cmd.ts_offset_y = ts_list[0].offset_y;
                    cmd.ts_blur = ts_list[0].blur;
                    cmd.ts_color = ts_list[0].color;
                }
            }

            dl.push_back(cmd);
        }

        // Replaced elements
        if (box->type == LayoutBoxType::Replaced && box->replaced_surface &&
            box->content_rect.w > 0 && box->content_rect.h > 0) {
            PaintCommand cmd;
            cmd.type = PaintCmdType::DrawImage;
            cmd.surface = box->replaced_surface;
            cmd.dest_rect = {cx, cy, box->content_rect.w, box->content_rect.h};
            cmd.natural_w = box->natural_width;
            cmd.natural_h = box->natural_height;
            cmd.object_fit = node ? node->object_fit : 0;
            cmd.dom_node = node;
            dl.push_back(cmd);
        }
    }

    // Children - sorted by z-index for paint order
    float child_offset_x = cx - box->scroll_x;
    float child_offset_y = cy - box->scroll_y;

    // Collect children indices and sort by z-index
    std::vector<size_t> child_order;
    for (size_t i = 0; i < box->children.size(); i++)
        child_order.push_back(i);

    // Sort: negative z-index first, then source order (z=0), then positive z-index
    std::stable_sort(child_order.begin(), child_order.end(),
        [&](size_t a, size_t b) {
            return box->children[a]->z_index < box->children[b]->z_index;
        });

    for (size_t idx : child_order) {
        paint_box(box->children[idx].get(), dl, child_offset_x, child_offset_y, skip_fixed);
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

// Collect fixed-position elements into a separate display list
static void collect_fixed_boxes(LayoutBox* box, DisplayList& dl) {
    if (!box) return;
    if (box->position == 3) {
        // Paint this fixed element at its absolute position
        paint_box(box, dl, 0, 0, false);
        return; // Don't recurse into children (already painted above)
    }
    for (auto& child : box->children)
        collect_fixed_boxes(child.get(), dl);
}

DisplayList generate_display_list(LayoutBox* root) {
    DisplayList dl;
    if (!root) return dl;
    paint_box(root, dl, 0, 0, true); // skip fixed elements
    return dl;
}

DisplayList generate_fixed_display_list(LayoutBox* root) {
    DisplayList dl;
    if (!root) return dl;
    collect_fixed_boxes(root, dl);
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
                // Text shadow (draw shadow copies first)
                if (cmd.has_text_shadow) {
                    cairo_save(cr);
                    cairo_set_source_rgba(cr, cmd.ts_color.r, cmd.ts_color.g,
                                           cmd.ts_color.b, cmd.ts_color.a);
                    if (cmd.ts_blur > 0) {
                        // Approximate blur by drawing multiple offset copies
                        float blur = cmd.ts_blur;
                        int steps = std::min(5, (int)(blur / 2) + 1);
                        float alpha_per_step = cmd.ts_color.a / (steps * 2.0f + 1.0f);
                        for (int dx = -steps; dx <= steps; dx++) {
                            for (int dy = -steps; dy <= steps; dy++) {
                                float d = sqrtf(dx*dx + dy*dy);
                                if (d > steps) continue;
                                float a = alpha_per_step * (1.0f - d / (steps + 1));
                                cairo_set_source_rgba(cr, cmd.ts_color.r, cmd.ts_color.g,
                                                       cmd.ts_color.b, a);
                                cairo_move_to(cr, cmd.text_x + cmd.ts_offset_x + dx,
                                                  cmd.text_y + cmd.ts_offset_y + dy);
                                pango_cairo_show_layout(cr, cmd.pango_layout);
                            }
                        }
                    } else {
                        cairo_move_to(cr, cmd.text_x + cmd.ts_offset_x,
                                          cmd.text_y + cmd.ts_offset_y);
                        pango_cairo_show_layout(cr, cmd.pango_layout);
                    }
                    cairo_restore(cr);
                }
                // Normal text
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
                if (cmd.dest_rect.w <= 0 || cmd.dest_rect.h <= 0) break;

                float dw = cmd.dest_rect.w, dh = cmd.dest_rect.h;
                float dx = cmd.dest_rect.x, dy = cmd.dest_rect.y;
                float sx = dw / sw, sy = dh / sh;

                if (cmd.object_fit == 1 || cmd.object_fit == 3) {
                    // contain (or scale-down): fit inside, preserve aspect ratio
                    float scale = std::min(sx, sy);
                    if (cmd.object_fit == 3 && scale > 1.0f) scale = 1.0f; // scale-down: don't upscale
                    float rw = sw * scale, rh = sh * scale;
                    dx += (dw - rw) / 2;
                    dy += (dh - rh) / 2;
                    sx = scale;
                    sy = scale;
                    cairo_save(cr);
                    cairo_rectangle(cr, cmd.dest_rect.x, cmd.dest_rect.y, dw, dh);
                    cairo_clip(cr);
                    cairo_translate(cr, dx, dy);
                    cairo_scale(cr, sx, sy);
                    cairo_set_source_surface(cr, cmd.surface, 0, 0);
                    cairo_paint(cr);
                    cairo_restore(cr);
                } else if (cmd.object_fit == 2) {
                    // cover: fill dest, preserve aspect ratio, clip overflow
                    float scale = std::max(sx, sy);
                    float rw = sw * scale, rh = sh * scale;
                    float ox = (dw - rw) / 2;
                    float oy = (dh - rh) / 2;
                    cairo_save(cr);
                    cairo_rectangle(cr, dx, dy, dw, dh);
                    cairo_clip(cr);
                    cairo_translate(cr, dx + ox, dy + oy);
                    cairo_scale(cr, scale, scale);
                    cairo_set_source_surface(cr, cmd.surface, 0, 0);
                    cairo_paint(cr);
                    cairo_restore(cr);
                } else if (cmd.object_fit == 4) {
                    // none: render at natural size, centered, clipped
                    float ox = (dw - sw) / 2;
                    float oy = (dh - sh) / 2;
                    cairo_save(cr);
                    cairo_rectangle(cr, dx, dy, dw, dh);
                    cairo_clip(cr);
                    cairo_translate(cr, dx + ox, dy + oy);
                    cairo_set_source_surface(cr, cmd.surface, 0, 0);
                    cairo_paint(cr);
                    cairo_restore(cr);
                } else {
                    // fill (default): stretch to fill dest
                    cairo_save(cr);
                    cairo_translate(cr, dx, dy);
                    cairo_scale(cr, sx, sy);
                    cairo_set_source_surface(cr, cmd.surface, 0, 0);
                    cairo_paint(cr);
                    cairo_restore(cr);
                }
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

            case PaintCmdType::DrawBoxShadow: {
                const auto& r = cmd.shadow_box_rect;
                float ox = cmd.shadow_offset_x;
                float oy = cmd.shadow_offset_y;
                float blur = cmd.shadow_blur;
                float spread = cmd.shadow_spread;
                const auto& sc = cmd.shadow_color;
                int radius = cmd.shadow_border_radius;

                if (cmd.shadow_inset) {
                    // Inset shadow: draw inside the box
                    cairo_save(cr);
                    // Clip to the box
                    if (radius > 0) {
                        float hr = std::min((float)radius, r.w / 2);
                        float vr = std::min((float)radius, r.h / 2);
                        cairo_new_path(cr);
                        cairo_arc(cr, r.x + hr, r.y + vr, hr, M_PI, 1.5 * M_PI);
                        cairo_arc(cr, r.x + r.w - hr, r.y + vr, hr, 1.5 * M_PI, 2 * M_PI);
                        cairo_arc(cr, r.x + r.w - hr, r.y + r.h - vr, vr, 0, 0.5 * M_PI);
                        cairo_arc(cr, r.x + hr, r.y + r.h - vr, vr, 0.5 * M_PI, M_PI);
                        cairo_close_path(cr);
                    } else {
                        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
                    }
                    cairo_clip(cr);

                    // Draw a larger rectangle offset, creating inset shadow
                    float pad = blur * 2 + spread;
                    cairo_set_source_rgba(cr, sc.r, sc.g, sc.b, sc.a);
                    cairo_rectangle(cr, r.x + ox - pad, r.y + oy - pad,
                                    r.w + pad * 2, r.h + pad * 2);
                    // Cut out the inner area
                    cairo_rectangle(cr, r.x + spread + (ox > 0 ? ox : 0),
                                    r.y + spread + (oy > 0 ? oy : 0),
                                    r.w - spread * 2, r.h - spread * 2);
                    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
                    cairo_fill(cr);
                    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
                    cairo_restore(cr);
                } else {
                    // Outer shadow
                    cairo_save(cr);
                    float sr_x = r.x + ox - spread;
                    float sr_y = r.y + oy - spread;
                    float sr_w = r.w + spread * 2;
                    float sr_h = r.h + spread * 2;

                    if (blur > 0) {
                        // Multi-pass blur approximation
                        int steps = std::min(8, (int)(blur / 2) + 1);
                        float step_alpha = sc.a / (float)(steps * 2 + 1);
                        for (int i = steps; i >= 0; i--) {
                            float expand = blur * i / steps;
                            float a = step_alpha * (steps - i + 1) / steps;
                            cairo_set_source_rgba(cr, sc.r, sc.g, sc.b, a);
                            float ex = sr_x - expand;
                            float ey = sr_y - expand;
                            float ew = sr_w + expand * 2;
                            float eh = sr_h + expand * 2;
                            int er = radius + (int)expand;
                            if (er > 0) {
                                float hr = std::min((float)er, ew / 2);
                                float vr = std::min((float)er, eh / 2);
                                cairo_new_path(cr);
                                cairo_arc(cr, ex + hr, ey + vr, hr, M_PI, 1.5 * M_PI);
                                cairo_arc(cr, ex + ew - hr, ey + vr, hr, 1.5 * M_PI, 2 * M_PI);
                                cairo_arc(cr, ex + ew - hr, ey + eh - vr, vr, 0, 0.5 * M_PI);
                                cairo_arc(cr, ex + hr, ey + eh - vr, vr, 0.5 * M_PI, M_PI);
                                cairo_close_path(cr);
                                cairo_fill(cr);
                            } else {
                                cairo_rectangle(cr, ex, ey, ew, eh);
                                cairo_fill(cr);
                            }
                        }
                    } else {
                        // Sharp shadow
                        cairo_set_source_rgba(cr, sc.r, sc.g, sc.b, sc.a);
                        if (radius > 0) {
                            float hr = std::min((float)radius, sr_w / 2);
                            float vr = std::min((float)radius, sr_h / 2);
                            cairo_new_path(cr);
                            cairo_arc(cr, sr_x + hr, sr_y + vr, hr, M_PI, 1.5 * M_PI);
                            cairo_arc(cr, sr_x + sr_w - hr, sr_y + vr, hr, 1.5 * M_PI, 2 * M_PI);
                            cairo_arc(cr, sr_x + sr_w - hr, sr_y + sr_h - vr, vr, 0, 0.5 * M_PI);
                            cairo_arc(cr, sr_x + hr, sr_y + sr_h - vr, vr, 0.5 * M_PI, M_PI);
                            cairo_close_path(cr);
                            cairo_fill(cr);
                        } else {
                            cairo_rectangle(cr, sr_x, sr_y, sr_w, sr_h);
                            cairo_fill(cr);
                        }
                    }
                    cairo_restore(cr);
                }
                break;
            }

            case PaintCmdType::DrawBackgroundImage: {
                if (!cmd.bg_surface) break;
                const auto& r = cmd.bg_rect;
                int sw = cairo_image_surface_get_width(cmd.bg_surface);
                int sh = cairo_image_surface_get_height(cmd.bg_surface);
                if (sw <= 0 || sh <= 0 || r.w <= 0 || r.h <= 0) break;

                cairo_save(cr);
                // Clip to the element box
                if (cmd.border_radius > 0) {
                    float hr = std::min((float)cmd.border_radius, r.w / 2);
                    float vr = std::min((float)cmd.border_radius, r.h / 2);
                    cairo_new_path(cr);
                    cairo_arc(cr, r.x + hr, r.y + vr, hr, M_PI, 1.5 * M_PI);
                    cairo_arc(cr, r.x + r.w - hr, r.y + vr, hr, 1.5 * M_PI, 2 * M_PI);
                    cairo_arc(cr, r.x + r.w - hr, r.y + r.h - vr, vr, 0, 0.5 * M_PI);
                    cairo_arc(cr, r.x + hr, r.y + r.h - vr, vr, 0.5 * M_PI, M_PI);
                    cairo_close_path(cr);
                    cairo_clip(cr);
                } else {
                    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
                    cairo_clip(cr);
                }

                // Determine draw size
                float draw_w = (float)sw, draw_h = (float)sh;
                if (cmd.bg_size_mode == "cover") {
                    float scale = std::max(r.w / sw, r.h / sh);
                    draw_w = sw * scale;
                    draw_h = sh * scale;
                } else if (cmd.bg_size_mode == "contain") {
                    float scale = std::min(r.w / sw, r.h / sh);
                    draw_w = sw * scale;
                    draw_h = sh * scale;
                } else if (!cmd.bg_size_mode.empty()) {
                    // Try parse "Wpx Hpx" or percentage
                    // Simple: just use natural size (already default)
                }

                // Position (default: top-left; center supported)
                float px = r.x, py = r.y;
                if (cmd.bg_position_str.find("center") != std::string::npos) {
                    px = r.x + (r.w - draw_w) / 2;
                    py = r.y + (r.h - draw_h) / 2;
                }

                // Repeat
                bool repeat_x = (cmd.bg_repeat_mode != "no-repeat" && cmd.bg_repeat_mode != "repeat-y");
                bool repeat_y = (cmd.bg_repeat_mode != "no-repeat" && cmd.bg_repeat_mode != "repeat-x");

                float sx = draw_w / sw, sy = draw_h / sh;

                if (!repeat_x && !repeat_y) {
                    // Single image
                    cairo_save(cr);
                    cairo_translate(cr, px, py);
                    cairo_scale(cr, sx, sy);
                    cairo_set_source_surface(cr, cmd.bg_surface, 0, 0);
                    cairo_paint(cr);
                    cairo_restore(cr);
                } else {
                    // Tiled
                    float start_x = repeat_x ? r.x - fmod(r.x - px, draw_w) - draw_w : px;
                    float start_y = repeat_y ? r.y - fmod(r.y - py, draw_h) - draw_h : py;
                    float end_x = repeat_x ? r.x + r.w : px + draw_w;
                    float end_y = repeat_y ? r.y + r.h : py + draw_h;

                    for (float ty = start_y; ty < end_y; ty += draw_h) {
                        for (float tx = start_x; tx < end_x; tx += draw_w) {
                            cairo_save(cr);
                            cairo_translate(cr, tx, ty);
                            cairo_scale(cr, sx, sy);
                            cairo_set_source_surface(cr, cmd.bg_surface, 0, 0);
                            cairo_paint(cr);
                            cairo_restore(cr);
                        }
                    }
                }
                cairo_restore(cr);
                break;
            }
        }
    }

    cairo_restore(cr);
}
