// bare-bones browser: text + images in document order
#include <gtk/gtk.h>
#include <curl/curl.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <functional>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <memory>
#include "dom.h"
#include "js_engine.h"
#include "js_event.h"
#include <unordered_map>

// ---- Canvas state (shared with js_bindings.cpp) ----
struct CanvasState {
    cairo_surface_t* surface = nullptr;
    GtkWidget* drawing_area = nullptr;
    int width = 300, height = 150;
};
extern std::unordered_map<uint32_t, CanvasState> g_canvas_map;

static gboolean draw_canvas(GtkWidget* w, cairo_t* cr, gpointer data) {
    uint32_t node_id = GPOINTER_TO_UINT(data);
    auto it = g_canvas_map.find(node_id);
    if (it == g_canvas_map.end() || !it->second.surface) return FALSE;
    cairo_set_source_surface(cr, it->second.surface, 0, 0);
    cairo_paint(cr);
    return FALSE;
}

// ---- curl ----

struct Buf { std::string data; };
static size_t write_cb(char* p, size_t s, size_t n, void* ud) {
    static_cast<Buf*>(ud)->data.append(p, s*n); return s*n;
}
static bool fetch(const std::string& url, Buf& out) {
    CURL* c = curl_easy_init(); if (!c) return false;
    curl_easy_setopt(c, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &out);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT,      "Mozilla/5.0");
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        15L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    CURLcode rc = curl_easy_perform(c); curl_easy_cleanup(c);
    return rc == CURLE_OK;
}

// ---- URL ----

static std::string normalize_url(const std::string& r) {
    if (r.size()>=12 && r.substr(0,12)=="view-source:") return r;
    if (r.size()>=7  && r.substr(0,7)=="file://")  return r;
    if (r.size()>=7  && r.substr(0,7)=="http://")  return r;
    if (r.size()>=8  && r.substr(0,8)=="https://") return r;
    return "https://" + r;
}
static std::string origin_of(const std::string& url) {
    auto p = url.find("://"); if (p==std::string::npos) return url;
    auto q = url.find('/', p+3);
    return q!=std::string::npos ? url.substr(0,q) : url;
}
static std::string resolve(const std::string& base, const std::string& href) {
    if (href.empty() || href[0]=='#') return "";
    if (href.size()>=4 && href.substr(0,4)=="http") return href;
    if (href.size()>=2 && href.substr(0,2)=="//")   return "https:"+href;
    if (href[0]=='/')                                return origin_of(base)+href;
    auto last = base.rfind('/');
    return (last!=std::string::npos ? base.substr(0,last+1) : base+"/") + href;
}

// ---- HTML helpers ----

static char lc(char c) { return (char)::tolower((unsigned char)c); }
static std::string tolower_s(std::string s) { for (auto& c:s) c=lc(c); return s; }

static size_t find_ci(const std::string& h, const char* needle, size_t pos=0) {
    size_t nl = std::strlen(needle);
    for (size_t i=pos; i+nl<=h.size(); ++i) {
        bool ok=true;
        for (size_t j=0; j<nl; ++j) if (lc(h[i+j])!=lc(needle[j])) { ok=false; break; }
        if (ok) return i;
    }
    return std::string::npos;
}

static std::string decode_entities(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (size_t i=0; i<s.size(); ) {
        if (s[i]!='&') { r+=s[i++]; continue; }
        size_t semi = s.find(';', i+1);
        if (semi==std::string::npos || semi-i>12) { r+=s[i++]; continue; }
        std::string e = s.substr(i+1, semi-i-1);
        if      (e=="amp")  r+='&';
        else if (e=="lt")   r+='<';
        else if (e=="gt")   r+='>';
        else if (e=="quot") r+='"';
        else if (e=="apos") r+='\'';
        else if (e=="nbsp") r+=' ';
        else if (!e.empty() && e[0]=='#') {
            try {
                int code = (e.size()>1 && e[1]=='x') ? std::stoi(e.substr(2),nullptr,16)
                                                      : std::stoi(e.substr(1));
                r += (code>0 && code<128) ? (char)code : '?';
            } catch (...) { r+=s[i++]; continue; }
        } else { r+=s[i++]; continue; }
        i = semi+1;
    }
    return r;
}

static std::string collapse_ws(const std::string& s) {
    std::string r; bool sp=true;
    for (char c : s) {
        if (std::isspace((unsigned char)c)) { if (!sp) r+=' '; sp=true; }
        else { r+=c; sp=false; }
    }
    if (!r.empty() && r.back()==' ') r.pop_back();
    return r;
}

static std::string extract_attr(const std::string& tag, const char* name) {
    size_t p = find_ci(tag, name);
    if (p==std::string::npos) return "";
    p += std::strlen(name);
    while (p<tag.size() && tag[p]==' ') ++p;
    if (p>=tag.size() || tag[p]!='=') return "";
    ++p;
    while (p<tag.size() && tag[p]==' ') ++p;
    if (p>=tag.size()) return "";
    char q = tag[p];
    if (q=='"' || q=='\'') {
        size_t end = tag.find(q, p+1);
        return end==std::string::npos ? tag.substr(p+1) : tag.substr(p+1, end-p-1);
    }
    size_t end=p;
    while (end<tag.size() && !std::isspace((unsigned char)tag[end]) && tag[end]!='>') ++end;
    return tag.substr(p, end-p);
}

static std::unordered_map<std::string, std::string> extract_all_attrs(const std::string& tag) {
    std::unordered_map<std::string, std::string> attrs;
    size_t i = 0;
    // skip tag name: <tagname ...
    while (i < tag.size() && tag[i] != ' ' && tag[i] != '\t' && tag[i] != '>' && tag[i] != '/') i++;
    while (i < tag.size()) {
        // skip whitespace
        while (i < tag.size() && (tag[i] == ' ' || tag[i] == '\t' || tag[i] == '\n' || tag[i] == '\r')) i++;
        if (i >= tag.size() || tag[i] == '>' || tag[i] == '/') break;
        // read attribute name
        size_t name_start = i;
        while (i < tag.size() && tag[i] != '=' && tag[i] != ' ' && tag[i] != '>' && tag[i] != '/') i++;
        std::string name = tag.substr(name_start, i - name_start);
        for (auto& c : name) c = (char)tolower((unsigned char)c);
        // skip whitespace around =
        while (i < tag.size() && tag[i] == ' ') i++;
        if (i >= tag.size() || tag[i] != '=') {
            if (!name.empty()) attrs[name] = "";
            continue;
        }
        i++; // skip '='
        while (i < tag.size() && tag[i] == ' ') i++;
        if (i >= tag.size()) break;
        std::string value;
        char q = tag[i];
        if (q == '"' || q == '\'') {
            i++;
            size_t end = tag.find(q, i);
            if (end == std::string::npos) { value = tag.substr(i); i = tag.size(); }
            else { value = tag.substr(i, end - i); i = end + 1; }
        } else {
            size_t start = i;
            while (i < tag.size() && !std::isspace((unsigned char)tag[i]) && tag[i] != '>') i++;
            value = tag.substr(start, i - start);
        }
        if (!name.empty()) attrs[name] = value;
    }
    return attrs;
}

// ---- CSS ----

static int fw_value(std::string v) {
    v = collapse_ws(tolower_s(v));
    if (v=="bold")    return PANGO_WEIGHT_BOLD;
    if (v=="normal")  return PANGO_WEIGHT_NORMAL;
    if (v=="bolder")  return PANGO_WEIGHT_ULTRABOLD;
    if (v=="lighter") return PANGO_WEIGHT_LIGHT;
    try {
        int n = std::stoi(v);
        if (n<=100) return PANGO_WEIGHT_THIN;
        if (n<=200) return PANGO_WEIGHT_ULTRALIGHT;
        if (n<=300) return PANGO_WEIGHT_LIGHT;
        if (n<=400) return PANGO_WEIGHT_NORMAL;
        if (n<=500) return PANGO_WEIGHT_MEDIUM;
        if (n<=600) return PANGO_WEIGHT_SEMIBOLD;
        if (n<=700) return PANGO_WEIGHT_BOLD;
        if (n<=800) return PANGO_WEIGHT_ULTRABOLD;
        return PANGO_WEIGHT_HEAVY;
    } catch (...) { return -1; }
}

// extract a single CSS property value from a declaration block
static std::string prop_val(const std::string& decls, const char* prop) {
    size_t plen = std::strlen(prop), p = 0;
    while (true) {
        p = find_ci(decls, prop, p);
        if (p==std::string::npos) return "";
        if (p>0 && decls[p-1]!=';' && !std::isspace((unsigned char)decls[p-1]))
            { p+=plen; continue; }
        p += plen;
        while (p<decls.size() && decls[p]==' ') ++p;
        if (p>=decls.size() || decls[p]!=':') continue;
        ++p;
        while (p<decls.size() && decls[p]==' ') ++p;
        size_t end = decls.find(';', p);
        std::string val = end==std::string::npos ? decls.substr(p) : decls.substr(p, end-p);
        while (!val.empty() && std::isspace((unsigned char)val.back())) val.pop_back();
        return val;
    }
}

static int fw_from_decls(const std::string& d) { return fw_value(prop_val(d, "font-weight")); }

// font-size string → pixels (parent_px used for em/% resolution)
static int parse_fs(const std::string& raw, int parent_px) {
    std::string v = collapse_ws(tolower_s(raw));
    if (v=="xx-small") return 9;   if (v=="x-small")  return 10;
    if (v=="small")    return 13;  if (v=="medium")    return 16;
    if (v=="large")    return 18;  if (v=="x-large")   return 24;
    if (v=="xx-large") return 32;  if (v=="smaller")   return (int)(parent_px*0.833);
    if (v=="larger")   return (int)(parent_px*1.2);
    try {
        size_t pos; double n = std::stod(v, &pos); std::string u = v.substr(pos);
        if (u=="px")  return (int)n;
        if (u=="pt")  return (int)(n*4.0/3.0);
        if (u=="em")  return (int)(n*parent_px);
        if (u=="rem") return (int)(n*16.0);
        if (u=="%")   return (int)(n/100.0*parent_px);
    } catch (...) {}
    return -1;
}

// line-height string → multiplier factor (-1 = normal/unset)
static double parse_lh(const std::string& raw, int fs_px) {
    std::string v = collapse_ws(tolower_s(raw));
    if (v=="normal" || v.empty()) return -1.0;
    try {
        size_t pos; double n = std::stod(v, &pos); std::string u = v.substr(pos);
        if (u.empty() || u=="em") return n;
        if (u=="%")               return n/100.0;
        if (u=="px" && fs_px>0)   return n/fs_px;
    } catch (...) {}
    return -1.0;
}

// parse a CSS length value → pixels (best-effort; 'auto' → 0)
static int parse_px_val(const std::string& raw) {
    std::string v = collapse_ws(tolower_s(raw));
    if (v.empty() || v=="auto") return 0;
    try {
        size_t pos; double n = std::stod(v, &pos); std::string u = v.substr(pos);
        if (u==""||u=="px") return (int)n;
        if (u=="pt")        return (int)(n*4.0/3.0);
        if (u=="em")        return (int)(n*16); // approximate
        if (u=="rem")       return (int)(n*16);
    } catch (...) {}
    return 0;
}

static std::array<int,4> parse_box_shorthand(const std::string& raw) {
    std::array<int,4> v={0,0,0,0};
    std::vector<std::string> parts;
    size_t i=0, n=raw.size();
    while (i<n) {
        while (i<n && raw[i]==' ') ++i;
        size_t j=i; while (j<n && raw[j]!=' ') ++j;
        if (j>i) parts.push_back(raw.substr(i,j-i));
        i=j;
    }
    if (parts.empty()) return v;
    auto p=[](const std::string& s){ return parse_px_val(s); };
    if (parts.size()==1) v={p(parts[0]),p(parts[0]),p(parts[0]),p(parts[0])};
    else if(parts.size()==2) v={p(parts[0]),p(parts[1]),p(parts[0]),p(parts[1])};
    else if(parts.size()==3) v={p(parts[0]),p(parts[1]),p(parts[2]),p(parts[1])};
    else { v={p(parts[0]),p(parts[1]),p(parts[2]),p(parts[3])}; }
    return v; // [top, right, bottom, left]
}

// Strip CSS quotes and normalize font-family for Pango
static std::string css_font_to_pango(const std::string& s) {
    std::string result;
    bool in_quote = false;
    char qch = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (!in_quote && (c == '"' || c == '\'')) { in_quote = true; qch = c; continue; }
        if (in_quote && c == qch) { in_quote = false; continue; }
        result += c;
    }
    return result;
}

// Parse text-decoration-line value into bitmask
static int parse_td_line(const std::string& s) {
    if (s == "none") return 0;
    int mask = 0;
    if (s.find("underline") != std::string::npos) mask |= 1;
    if (s.find("overline") != std::string::npos) mask |= 2;
    if (s.find("line-through") != std::string::npos) mask |= 4;
    return mask;
}

// Parse text-decoration shorthand: line || style || color
static void parse_text_decoration(const std::string& s, DOMNode* elem) {
    if (s == "none") { elem->text_decoration = 0; return; }
    // Check for line keywords
    int mask = parse_td_line(s);
    if (mask > 0) elem->text_decoration = mask;
    else elem->text_decoration = 0;
    // Check for style keywords in the shorthand
    if (s.find("dotted") != std::string::npos) elem->text_decoration_style = 2;
    else if (s.find("dashed") != std::string::npos) elem->text_decoration_style = 3;
    else if (s.find("double") != std::string::npos) elem->text_decoration_style = 1;
    else if (s.find("wavy") != std::string::npos) elem->text_decoration_style = 4;
}

// Parse text-decoration-style string to int
static int parse_td_style(const std::string& s) {
    if (s == "solid") return 0;
    if (s == "double") return 1;
    if (s == "dotted") return 2;
    if (s == "dashed") return 3;
    if (s == "wavy") return 4;
    return 0;
}

// Parse white-space value to int
static int parse_white_space(const std::string& s) {
    if (s == "normal") return 0;
    if (s == "nowrap") return 1;
    if (s == "pre") return 2;
    if (s == "pre-wrap") return 3;
    if (s == "pre-line") return 4;
    return -1;
}

// Parse font-stretch value to PangoStretch enum
static int parse_font_stretch(const std::string& s) {
    if (s == "ultra-condensed") return PANGO_STRETCH_ULTRA_CONDENSED;
    if (s == "extra-condensed") return PANGO_STRETCH_EXTRA_CONDENSED;
    if (s == "condensed") return PANGO_STRETCH_CONDENSED;
    if (s == "semi-condensed") return PANGO_STRETCH_SEMI_CONDENSED;
    if (s == "normal") return PANGO_STRETCH_NORMAL;
    if (s == "semi-expanded") return PANGO_STRETCH_SEMI_EXPANDED;
    if (s == "expanded") return PANGO_STRETCH_EXPANDED;
    if (s == "extra-expanded") return PANGO_STRETCH_EXTRA_EXPANDED;
    if (s == "ultra-expanded") return PANGO_STRETCH_ULTRA_EXPANDED;
    return -1;
}

// Parse font shorthand: font: [style] [variant] [weight] [stretch] size[/line-height] family
static void parse_font_shorthand(const std::string& val, DOMNode* elem, int parent_fs) {
    std::string v = collapse_ws(val);
    if (v.empty()) return;
    // Split into tokens
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < v.size()) {
        while (i < v.size() && v[i] == ' ') ++i;
        size_t j = i;
        // If we hit a quote, find matching end
        if (j < v.size() && (v[j] == '"' || v[j] == '\'')) {
            char q = v[j]; ++j;
            while (j < v.size() && v[j] != q) ++j;
            if (j < v.size()) ++j;
        } else {
            while (j < v.size() && v[j] != ' ') ++j;
        }
        if (j > i) tokens.push_back(v.substr(i, j - i));
        i = j;
    }
    if (tokens.empty()) return;

    // Last token(s) are family - find the size token first
    // Size is the first token that starts with a digit or is a keyword size
    int size_idx = -1;
    for (int ti = 0; ti < (int)tokens.size(); ++ti) {
        std::string tl = tolower_s(tokens[ti]);
        // Remove /line-height part for checking
        std::string check = tl;
        auto slash = check.find('/');
        if (slash != std::string::npos) check = check.substr(0, slash);
        int px = parse_fs(check, parent_fs);
        if (px > 0) { size_idx = ti; break; }
    }
    if (size_idx < 0) return; // can't parse without size

    // Tokens before size_idx are style/variant/weight/stretch
    for (int ti = 0; ti < size_idx; ++ti) {
        std::string tl = tolower_s(tokens[ti]);
        if (tl == "italic") elem->fi_computed = PANGO_STYLE_ITALIC;
        else if (tl == "oblique") elem->fi_computed = PANGO_STYLE_OBLIQUE;
        else if (tl == "small-caps") elem->font_variant = 1;
        else {
            int w = fw_value(tl);
            if (w != -1) elem->fw_computed = w;
            int st = parse_font_stretch(tl);
            if (st >= 0) elem->font_stretch = st;
        }
    }

    // Parse size[/line-height]
    std::string size_tok = tokens[size_idx];
    auto slash = size_tok.find('/');
    if (slash != std::string::npos) {
        std::string lh_str = size_tok.substr(slash + 1);
        size_tok = size_tok.substr(0, slash);
        double lh = parse_lh(lh_str, elem->fs_computed);
        if (lh >= 0) elem->lh_computed = lh;
    }
    int px = parse_fs(size_tok, parent_fs);
    if (px > 0) elem->fs_computed = px;

    // Everything after size is the family
    if (size_idx + 1 < (int)tokens.size()) {
        std::string fam;
        for (int ti = size_idx + 1; ti < (int)tokens.size(); ++ti) {
            if (!fam.empty()) fam += ' ';
            fam += tokens[ti];
        }
        // Remove trailing commas and clean for Pango
        elem->font_family = css_font_to_pango(fam);
    }
}

struct CSSRule { std::string sel; int fw=-1; std::string fs_raw, lh_raw, decls, src_url; };

struct ParsedBorder { int width=0; std::string style, color; };
static ParsedBorder parse_border_shorthand(const std::string& raw) {
    ParsedBorder b;
    std::vector<std::string> parts;
    size_t i=0, n=raw.size();
    while (i<n) {
        while (i<n && raw[i]==' ') ++i;
        size_t j=i; while (j<n && raw[j]!=' ') ++j;
        if (j>i) parts.push_back(raw.substr(i,j-i));
        i=j;
    }
    static const char* STYLES[]={"solid","dashed","dotted","double","groove","ridge","inset","outset","none",nullptr};
    std::string color_acc;
    for (const auto& p : parts) {
        std::string pl = tolower_s(p);
        bool is_style=false;
        for (int si=0; STYLES[si]; ++si) if (pl==STYLES[si]) { b.style=pl; is_style=true; break; }
        if (is_style) continue;
        if (b.width==0) {
            if (pl=="thin")  { b.width=1; continue; }
            if (pl=="medium"){ b.width=3; continue; }
            if (pl=="thick") { b.width=5; continue; }
            int w=parse_px_val(p); if (w>0) { b.width=w; continue; }
        }
        if (!color_acc.empty()) color_acc+=' ';
        color_acc+=p;
    }
    if (!color_acc.empty()) b.color=color_acc;
    return b;
}

static std::vector<CSSRule> parse_css(const std::string& css) {
    std::vector<CSSRule> rules;
    size_t i=0, n=css.size();
    while (i<n) {
        while (i<n && std::isspace((unsigned char)css[i])) ++i;
        // skip /* comments */
        if (i+1<n && css[i]=='/' && css[i+1]=='*') {
            size_t e = css.find("*/", i+2);
            i = e==std::string::npos ? n : e+2; continue;
        }
        // skip @rules (find matching braces)
        if (i<n && css[i]=='@') {
            size_t ob = css.find('{', i);
            if (ob==std::string::npos) { i=n; break; }
            int depth=1; i=ob+1;
            while (i<n && depth>0) { if(css[i]=='{')++depth; else if(css[i]=='}')--depth; ++i; }
            continue;
        }
        size_t ss=i;
        while (i<n && css[i]!='{') ++i;
        if (i>=n) break;
        std::string sels_str = css.substr(ss, i-ss);
        ++i; // skip '{'
        size_t ds=i; int depth=1;
        while (i<n && depth>0) { if(css[i]=='{')++depth; else if(css[i]=='}')--depth; ++i; }
        std::string decls = css.substr(ds, i-1-ds);
        int fw = fw_from_decls(decls);
        std::string fs_raw = prop_val(decls, "font-size");
        std::string lh_raw = prop_val(decls, "line-height");
        bool has_box = !prop_val(decls,"margin").empty() || !prop_val(decls,"padding").empty()
                    || !prop_val(decls,"margin-top").empty() || !prop_val(decls,"padding-top").empty()
                    || !prop_val(decls,"width").empty()      || !prop_val(decls,"max-width").empty()
                    || !prop_val(decls,"height").empty()
                    || !prop_val(decls,"display").empty()    || !prop_val(decls,"float").empty()
                    || !prop_val(decls,"background-color").empty()
                    || !prop_val(decls,"background-image").empty()
                    || !prop_val(decls,"background").empty()
                    || !prop_val(decls,"border").empty()     || !prop_val(decls,"border-radius").empty()
                    || !prop_val(decls,"color").empty()      || !prop_val(decls,"text-align").empty()
                    || !prop_val(decls,"text-transform").empty() || !prop_val(decls,"font-family").empty()
                    || !prop_val(decls,"box-shadow").empty() || !prop_val(decls,"opacity").empty()
                    || !prop_val(decls,"overflow").empty()
                    || !prop_val(decls,"flex-direction").empty() || !prop_val(decls,"justify-content").empty()
                    || !prop_val(decls,"align-items").empty() || !prop_val(decls,"flex-wrap").empty()
                    || !prop_val(decls,"gap").empty()
                    || !prop_val(decls,"position").empty()
                    || !prop_val(decls,"top").empty() || !prop_val(decls,"left").empty()
                    || !prop_val(decls,"right").empty() || !prop_val(decls,"bottom").empty()
                    || !prop_val(decls,"z-index").empty()
                    || !prop_val(decls,"background-repeat").empty()
                    || !prop_val(decls,"background-size").empty()
                    || !prop_val(decls,"background-position").empty()
                    || !prop_val(decls,"font-style").empty()
                    || !prop_val(decls,"text-decoration").empty()
                    || !prop_val(decls,"text-decoration-line").empty()
                    || !prop_val(decls,"text-decoration-color").empty()
                    || !prop_val(decls,"text-decoration-style").empty()
                    || !prop_val(decls,"letter-spacing").empty()
                    || !prop_val(decls,"word-spacing").empty()
                    || !prop_val(decls,"font-variant").empty()
                    || !prop_val(decls,"white-space").empty()
                    || !prop_val(decls,"text-indent").empty()
                    || !prop_val(decls,"text-overflow").empty()
                    || !prop_val(decls,"font-stretch").empty()
                    || !prop_val(decls,"text-shadow").empty()
                    || !prop_val(decls,"font").empty();
        if (fw==-1 && fs_raw.empty() && lh_raw.empty() && !has_box) continue;
        // split comma-separated selectors
        size_t j=0;
        while (j<=sels_str.size()) {
            size_t comma = sels_str.find(',', j);
            std::string sel = collapse_ws(tolower_s(
                comma==std::string::npos ? sels_str.substr(j) : sels_str.substr(j, comma-j)));
            if (!sel.empty()) rules.push_back({sel, fw, fs_raw, lh_raw, decls});
            if (comma==std::string::npos) break;
            j = comma+1;
        }
    }
    return rules;
}

// collect CSS from all <style> blocks in the document
static std::vector<CSSRule> extract_css(const std::string& html) {
    std::vector<CSSRule> all;
    size_t i=0;
    while (true) {
        size_t start = find_ci(html, "<style", i);
        if (start==std::string::npos) break;
        size_t nx = start+6;
        if (nx<html.size() && html[nx]!='>' && !std::isspace((unsigned char)html[nx]))
            { i=nx; continue; } // not a <style> tag
        size_t gt = html.find('>', start);
        if (gt==std::string::npos) break;
        size_t end = find_ci(html, "</style>", gt+1);
        std::string css = html.substr(gt+1, end==std::string::npos ? html.size()-gt-1 : end-gt-1);
        for (auto& r : parse_css(css)) all.push_back(r);
        i = end==std::string::npos ? html.size() : end+8;
    }
    return all;
}

// ---- Box model ----

struct BoxModel {
    int margin[4]  = {0,0,0,0}; // top right bottom left
    int padding[4] = {0,0,0,0};
    int width      = -1;
    int max_width  = -1;
    int height     = -1;
    int border_width[4] = {0,0,0,0}; // top right bottom left
    int border_radius   = 0;
    std::string border_color;
    std::string border_style; // solid, dashed, etc.
    bool halign_center = false;
    enum class Display : uint8_t { Inherit, Block, Inline, None, Flex, InlineBlock } display = Display::Inherit;
    enum class Float   : uint8_t { None, Left, Right }                               floatdir = Float::None;
    std::string bg_image; // resolved URL
    std::string bg_color; // raw CSS color value
    std::string box_shadow; // raw CSS box-shadow value
    double opacity = 1.0;   // CSS opacity
    int overflow = -1;      // -1=inherit, 0=visible, 1=hidden, 2=scroll, 3=auto
    std::string bg_repeat;  // background-repeat (default "repeat")
    std::string bg_size;    // background-size (e.g. "contain", "cover", "100px 50px")
    std::string bg_position; // background-position (e.g. "0px 0px", "center", "-32px 0px")
};

static bool is_block_element(const std::string& t) {
    static const char* B[]={"div","section","article","main","header","footer","nav","aside",
                             "p","ul","ol","li","h1","h2","h3","h4","h5","h6",
                             "blockquote","form","figure","figcaption","details","summary",
                             "body","html",nullptr};
    for (int i=0; B[i]; ++i) if (t==B[i]) return true;
    return false;
}

static void apply_box(const std::string& decls, BoxModel& bm) {
    auto m = prop_val(decls, "margin");
    if (!m.empty()) {
        auto v = parse_box_shorthand(m);
        for (int i=0; i<4; ++i) bm.margin[i] = v[i];
        if (tolower_s(m).find("auto") != std::string::npos) bm.halign_center = true;
    }
    { auto s=prop_val(decls,"margin-top");    if(!s.empty()) bm.margin[0]=parse_px_val(s); }
    { auto s=prop_val(decls,"margin-right");  if(!s.empty()) bm.margin[1]=parse_px_val(s); }
    { auto s=prop_val(decls,"margin-bottom"); if(!s.empty()) bm.margin[2]=parse_px_val(s); }
    { auto s=prop_val(decls,"margin-left");   if(!s.empty()) { bm.margin[3]=parse_px_val(s); if(tolower_s(s)=="auto") bm.halign_center=true; } }
    auto p = prop_val(decls, "padding");
    if (!p.empty()) { auto v=parse_box_shorthand(p); for(int i=0;i<4;++i) bm.padding[i]=v[i]; }
    { auto s=prop_val(decls,"padding-top");    if(!s.empty()) bm.padding[0]=parse_px_val(s); }
    { auto s=prop_val(decls,"padding-right");  if(!s.empty()) bm.padding[1]=parse_px_val(s); }
    { auto s=prop_val(decls,"padding-bottom"); if(!s.empty()) bm.padding[2]=parse_px_val(s); }
    { auto s=prop_val(decls,"padding-left");   if(!s.empty()) bm.padding[3]=parse_px_val(s); }
    { auto s=prop_val(decls,"width");     auto sl=tolower_s(s);
      if(!s.empty()&&sl!="auto"&&sl!="100%") bm.width=parse_px_val(s); }
    { auto s=prop_val(decls,"max-width"); if(!s.empty()) bm.max_width=parse_px_val(s); }
    { auto s=prop_val(decls,"height");    auto sl=tolower_s(s);
      if(!s.empty()&&sl!="auto") bm.height=parse_px_val(s); }
    { auto s=tolower_s(prop_val(decls,"display"));
      if      (s=="none")                       bm.display=BoxModel::Display::None;
      else if (s=="flex"||s=="inline-flex")     bm.display=BoxModel::Display::Flex;
      else if (s=="inline-block")               bm.display=BoxModel::Display::InlineBlock;
      else if (s=="inline")                     bm.display=BoxModel::Display::Inline;
      else if (s=="block")                      bm.display=BoxModel::Display::Block; }
    { auto s=tolower_s(prop_val(decls,"float"));
      if      (s=="left")  bm.floatdir=BoxModel::Float::Left;
      else if (s=="right") bm.floatdir=BoxModel::Float::Right;
      else if (s=="none")  bm.floatdir=BoxModel::Float::None; }
    { auto s = prop_val(decls,"background-color");
      if (s.empty()) {
          // background shorthand: grab color if no url()
          auto bg = prop_val(decls,"background");
          if (bg.find("url(")==std::string::npos && !bg.empty()) s = bg;
      }
      if (!s.empty()) bm.bg_color = collapse_ws(s); }
    {
        std::string s = prop_val(decls,"background-image");
        if (s.empty()) s = prop_val(decls,"background");
        size_t p = s.find("url(");
        if (p != std::string::npos) {
            size_t q = s.find(')', p+4);
            if (q != std::string::npos) {
                std::string u = s.substr(p+4, q-p-4);
                while (!u.empty()&&(u.front()=='"'||u.front()=='\''||u.front()==' ')) u.erase(u.begin());
                while (!u.empty()&&(u.back() =='"'||u.back() =='\''||u.back() ==' ')) u.pop_back();
                if (!u.empty()) bm.bg_image = u;
            }
        }
    }
    // border shorthand
    { auto s=prop_val(decls,"border"); if (!s.empty()) {
          auto b=parse_border_shorthand(s);
          if (b.width>0) for(int i=0;i<4;++i) bm.border_width[i]=b.width;
          if (!b.style.empty()) bm.border_style=b.style;
          if (!b.color.empty()) bm.border_color=b.color;
    }}
    // individual border sides
    { const char* sides[4]={"border-top","border-right","border-bottom","border-left"};
      for (int i=0;i<4;++i) {
          auto s=prop_val(decls,sides[i]); if (s.empty()) continue;
          auto b=parse_border_shorthand(s);
          if (b.width>0) bm.border_width[i]=b.width;
          if (!b.style.empty()) bm.border_style=b.style;
          if (!b.color.empty()) bm.border_color=b.color;
    }}
    { auto s=prop_val(decls,"border-color"); if (!s.empty()) bm.border_color=collapse_ws(s); }
    { auto s=prop_val(decls,"border-width"); if (!s.empty()) {
          auto v=parse_box_shorthand(s); for(int i=0;i<4;++i) if(v[i]>0) bm.border_width[i]=v[i];
    }}
    { auto s=tolower_s(prop_val(decls,"border-style")); if (!s.empty()) bm.border_style=s; }
    { auto s=prop_val(decls,"border-radius"); if (!s.empty()) {
          size_t sl=s.find('/');
          bm.border_radius=parse_px_val(sl==std::string::npos ? s : s.substr(0,sl));
    }}
    { auto s=prop_val(decls,"box-shadow"); if (!s.empty()) bm.box_shadow=s; }
    { auto s=prop_val(decls,"opacity"); if (!s.empty()) {
          try { bm.opacity=std::stod(s); } catch(...){} } }
    { auto s=tolower_s(prop_val(decls,"overflow"));
      if      (s=="visible") bm.overflow=0;
      else if (s=="hidden")  bm.overflow=1;
      else if (s=="scroll")  bm.overflow=2;
      else if (s=="auto")    bm.overflow=3; }
    { auto s=tolower_s(prop_val(decls,"background-repeat"));
      if (!s.empty()) bm.bg_repeat=s; }
    { auto s=tolower_s(prop_val(decls,"background-size"));
      if (!s.empty()) bm.bg_size=s; }
    { auto s=prop_val(decls,"background-position");
      if (!s.empty()) { bm.bg_position=s; fprintf(stderr,"[DEBUG apply_box] bg_position='%s'\n", s.c_str()); } }
}

// ---- Element stack ----

static std::vector<std::string> split_classes(const std::string& s) {
    std::vector<std::string> v; std::string cur;
    for (char c : s) {
        if (c==' '||c=='\t') { if (!cur.empty()) { v.push_back(cur); cur.clear(); } }
        else cur += lc(c);
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

static bool is_void(const std::string& t) {
    static const char* V[]={"area","base","br","col","embed","hr","img",
                             "input","link","meta","param","source","track","wbr",
                             "path","circle","ellipse","line","polyline","polygon",
                             "rect","use","stop",nullptr};
    for (int i=0; V[i]; ++i) if (t==V[i]) return true;
    return false;
}

static const char* BOLD_TAGS[]={"b","strong","h1","h2","h3","h4","h5","h6",nullptr};

struct StackEntry {
    std::string tag; std::vector<std::string> cls; std::string id;
    int    fw_own       = -1;
    int    fs_computed  = 16;
    double lh_own       = -1.0;
    std::string color_own;
    int    text_align_own = -1; // -1=inherit, 0=left, 1=center, 2=right, 3=justify
    BoxModel box;
    std::string href; // non-empty for <a> elements
};

// Strip attribute selectors [attr...] and pseudo-classes from a simple selector,
// returning the base selector part (tag.class#id).
// Sets has_pseudo_element=true if selector targets ::before, ::after, etc.
static std::string strip_selector_extras(const std::string& raw, bool& has_pseudo_element) {
    std::string result;
    has_pseudo_element = false;
    size_t i = 0, n = raw.size();
    while (i < n) {
        if (raw[i] == '[') {
            int depth = 1; ++i;
            while (i < n && depth > 0) { if (raw[i]=='[') ++depth; else if (raw[i]==']') --depth; ++i; }
        } else if (raw[i] == ':') {
            ++i;
            bool is_double = (i < n && raw[i] == ':');
            if (is_double) ++i; // ::pseudo-element
            // Extract pseudo name
            size_t ps = i;
            while (i < n && raw[i] != '.' && raw[i] != '#' && raw[i] != '[' && raw[i] != ':' && raw[i] != '(') ++i;
            std::string pname = raw.substr(ps, i - ps);
            // Skip function args if present
            if (i < n && raw[i] == '(') { int d=1; ++i; while(i<n&&d>0){if(raw[i]=='(')++d;else if(raw[i]==')')--d;++i;} }
            // Check for pseudo-elements (target generated content, not the element itself)
            if (is_double || pname == "before" || pname == "after" ||
                pname == "first-line" || pname == "first-letter" || pname == "placeholder")
                has_pseudo_element = true;
        } else {
            result += raw[i++];
        }
    }
    return result;
}

// Match a single simple selector token (tag, .class, #id, tag.class, .a.b, with optional :pseudo and [attr])
static bool simple_match(const std::string& raw, const StackEntry& e) {
    if (raw.empty() || raw=="*") return true;
    bool has_pseudo_element = false;
    std::string tok = strip_selector_extras(raw, has_pseudo_element);
    // Pseudo-elements (::before, ::after, etc.) target generated content,
    // not the element itself — never match the actual element.
    if (has_pseudo_element) return false;
    // If raw had content but stripping [attr] and :pseudo left nothing,
    // the selector is purely attribute/pseudo-based (e.g. "[hidden]").
    // We can't evaluate these, so conservatively return false.
    if (tok.empty()) return false;
    if (tok[0]=='#') return e.id == tok.substr(1);
    // parse tag part and class parts from e.g. "div.foo.bar" or ".foo.bar" or "div"
    std::string tag_part;
    std::vector<std::string> req_cls;
    size_t i = 0;
    if (tok[i] != '.') {
        size_t d = tok.find('.'); tag_part = tok.substr(0, d);
        if (d != std::string::npos) i = d + 1; else i = tok.size();
    } else { ++i; }
    while (i <= tok.size()) {
        size_t d = tok.find('.', i);
        std::string c = tok.substr(i, d==std::string::npos ? std::string::npos : d-i);
        if (!c.empty()) req_cls.push_back(c);
        if (d==std::string::npos) break; i = d+1;
    }
    if (!tag_part.empty() && e.tag != tag_part) return false;
    for (const auto& c : req_cls)
        if (std::find(e.cls.begin(),e.cls.end(),c)==e.cls.end()) return false;
    return true;
}

// Match a full selector (handles descendant/child combinators) against ancestors + current element
static bool sel_matches(const std::string& sel,
                         const std::vector<StackEntry>& ancestors,
                         const StackEntry& cur) {
    if (sel.empty()) return false;
    // Split selector into tokens, discarding '>' combinator tokens
    std::vector<std::string> parts;
    size_t i = 0, n = sel.size();
    while (i < n) {
        while (i<n && sel[i]==' ') ++i;
        size_t j = i; while (j<n && sel[j]!=' ') ++j;
        if (j>i) { std::string p=sel.substr(i,j-i); if (p!=">") parts.push_back(p); }
        i = j;
    }
    if (parts.empty()) return false;
    if (!simple_match(parts.back(), cur)) return false;
    if (parts.size()==1) return true;
    // greedily match remaining parts against ancestors (right-to-left)
    int pi = (int)parts.size()-2, ai = (int)ancestors.size()-1;
    while (pi>=0 && ai>=0) { if (simple_match(parts[pi],ancestors[ai])) --pi; --ai; }
    return pi < 0;
}

static int    compute_fw(const std::vector<StackEntry>& s) {
    int fw=PANGO_WEIGHT_NORMAL; for(auto&e:s) if(e.fw_own!=-1) fw=e.fw_own; return fw;
}
static double compute_lh(const std::vector<StackEntry>& s) {
    double lh=-1.0; for(auto&e:s) if(e.lh_own>=0) lh=e.lh_own; return lh;
}
static std::string compute_href(const std::vector<StackEntry>& s) {
    for (int i=(int)s.size()-1; i>=0; --i) if (!s[i].href.empty()) return s[i].href;
    return "";
}
static std::string compute_color(const std::vector<StackEntry>& s) {
    for (int i=(int)s.size()-1; i>=0; --i) if (!s[i].color_own.empty()) return s[i].color_own;
    return "";
}
static int compute_text_align(const std::vector<StackEntry>& s) {
    for (int i=(int)s.size()-1; i>=0; --i) if (s[i].text_align_own>=0) return s[i].text_align_own;
    return 0;
}

// ---- Element ----

struct Element {
    enum {TEXT,IMAGE,DIV_OPEN,DIV_CLOSE} type;
    std::string content;
    int fw=PANGO_WEIGHT_NORMAL; int fs_px=16; double lh=-1.0;
    std::string color;   // text color (TEXT only)
    int text_align = 0;  // 0=left,1=center,2=right,3=justify (TEXT only)
    BoxModel box; // DIV_OPEN only
    std::string href; // non-empty for link text
    bool is_body = false; // DIV_OPEN only
};

static std::vector<CSSRule> fetch_linked_css(const std::string& html, const std::string& base) {
    std::vector<CSSRule> all;
    size_t i = 0;
    while (true) {
        size_t p = find_ci(html, "<link", i);
        if (p == std::string::npos) break;
        size_t gt = html.find('>', p);
        if (gt == std::string::npos) break;
        std::string tag = html.substr(p+1, gt-p-1);
        i = gt+1;
        if (tolower_s(extract_attr(tag,"rel")) != "stylesheet") continue;
        std::string href = extract_attr(tag,"href");
        if (href.empty()) continue;
        std::string url = resolve(base, href);
        if (url.empty()) continue;
        Buf buf;
        if (!fetch(url, buf)) continue;
        for (auto& r : parse_css(buf.data)) { r.src_url = url; all.push_back(std::move(r)); }
    }
    return all;
}

static std::vector<Element> parse_elements(const std::string& html, const std::string& base) {
    auto css = extract_css(html);
    for (auto& r : fetch_linked_css(html, base)) css.push_back(std::move(r));
    std::vector<Element> elems;
    std::vector<StackEntry> stack;
    std::string acc;
    auto flush = [&]() {
        std::string t = collapse_ws(decode_entities(acc));
        if (!t.empty()) {
            int fs = stack.empty() ? 16 : stack.back().fs_computed;
            Element el; el.type=Element::TEXT; el.content=t;
            el.fw=compute_fw(stack); el.fs_px=fs; el.lh=compute_lh(stack);
            el.href=compute_href(stack);
            el.color=compute_color(stack);
            el.text_align=compute_text_align(stack);
            elems.push_back(std::move(el));
        }
        acc.clear();
    };

    enum { NORM, SCRIPT_SKIP, STYLE_SKIP, COMMENT } state = NORM;
    int skip_depth = 0; // for display:none subtrees
    size_t i=0, n=html.size();

    while (i<n) {
        if (state==COMMENT) {
            size_t p = html.find("-->", i);
            i = p==std::string::npos ? n : p+3;
            state=NORM; continue;
        }
        if (state==SCRIPT_SKIP) {
            size_t p = find_ci(html, "</script>", i);
            i = p==std::string::npos ? n : p+9;
            state=NORM; continue;
        }
        if (state==STYLE_SKIP) {
            size_t p = find_ci(html, "</style>", i);
            i = p==std::string::npos ? n : p+8;
            state=NORM; continue;
        }
        if (html[i]!='<') { if (!skip_depth) acc+=html[i]; ++i; continue; }
        flush(); ++i;
        if (i>=n) break;
        if (i+2<n && html[i]=='!' && html[i+1]=='-' && html[i+2]=='-') {
            state=COMMENT; i+=3; continue;
        }
        // find tag end, respecting quoted attributes
        size_t ts=i; bool inq=false; char qc=0;
        while (i<n) {
            if (!inq && html[i]=='>') break;
            if (inq && html[i]==qc) inq=false;
            else if (!inq && (html[i]=='"'||html[i]=='\'')) { inq=true; qc=html[i]; }
            ++i;
        }
        std::string tag = html.substr(ts, i-ts);
        if (i<n) ++i;
        if (tag.empty()) continue;

        bool closing = (tag[0]=='/');
        size_t ks=closing?1:0, k=ks;
        while (k<tag.size() && !std::isspace((unsigned char)tag[k]) && tag[k]!='>') ++k;
        std::string tname = tolower_s(tag.substr(ks, k-ks));

        // handle display:none skip region
        if (skip_depth > 0) {
            bool self_closing = (!tag.empty() && tag.back() == '/');
            if (!closing && !self_closing && !is_void(tname)) skip_depth++;
            else if (closing) { if (--skip_depth == 0) acc.clear(); }
            continue;
        }

        if (!closing && tname=="script") { state=SCRIPT_SKIP; continue; }
        if (!closing && tname=="style")  { state=STYLE_SKIP;  continue; }
        if (!tname.empty() && tname[0] == '!') continue; // skip <!DOCTYPE> etc.

        if (!closing && tname=="img") {
            std::string src = extract_attr(tag, "src");
            if (!src.empty()) {
                std::string u = resolve(base, src);
                if (!u.empty()) elems.push_back({Element::IMAGE, u});
            }
            continue;
        }

        if (!closing && !is_void(tname)) {
            int parent_fs = stack.empty() ? 16 : stack.back().fs_computed;
            StackEntry e;
            e.tag = tname;
            e.cls = split_classes(extract_attr(tag, "class"));
            e.id  = tolower_s(extract_attr(tag, "id"));
            e.fs_computed = parent_fs;
            // UA defaults
            for (int bi=0; BOLD_TAGS[bi]; ++bi)
                if (tname==BOLD_TAGS[bi]) e.fw_own=PANGO_WEIGHT_BOLD;
            // CSS rules
            std::string bg_img_src; // track src_url of the rule that set bg_image
            for (const auto& r : css) {
                if (!sel_matches(r.sel, stack, e)) continue;
                if (tname == "body") {
                    FILE* fl = fopen("/tmp/browser_debug.log","a");
                    if (fl) { fprintf(fl, "body CSS match: sel='%s'\n", r.sel.c_str()); fclose(fl); }
                }
                if (r.fw!=-1) e.fw_own = r.fw;
                if (!r.fs_raw.empty()) { int px=parse_fs(r.fs_raw,parent_fs); if(px>0) e.fs_computed=px; }
                if (!r.lh_raw.empty()) { double f=parse_lh(r.lh_raw,e.fs_computed); if(f>=0) e.lh_own=f; }
                std::string prev_bg = e.box.bg_image;
                apply_box(r.decls, e.box);
                if (e.box.bg_image != prev_bg) bg_img_src = r.src_url;
                { auto s=prop_val(r.decls,"color"); if(!s.empty()) e.color_own=collapse_ws(s); }
                { auto s=tolower_s(prop_val(r.decls,"text-align"));
                  if      (s=="center")  e.text_align_own=1;
                  else if (s=="right")   e.text_align_own=2;
                  else if (s=="justify") e.text_align_own=3;
                  else if (s=="left")    e.text_align_own=0; }
            }
            // anchor href
            if (tname=="a") {
                std::string h = extract_attr(tag, "href");
                if (!h.empty()) e.href = resolve(base, h);
            }
            // inline style
            std::string ist = extract_attr(tag, "style");
            { int v=fw_value(prop_val(ist,"font-weight"));    if(v!=-1)   e.fw_own=v; }
            { int px=parse_fs(prop_val(ist,"font-size"),parent_fs); if(px>0) e.fs_computed=px; }
            { double f=parse_lh(prop_val(ist,"line-height"),e.fs_computed); if(f>=0) e.lh_own=f; }
            apply_box(ist, e.box);
            { auto s=prop_val(ist,"color"); if(!s.empty()) e.color_own=collapse_ws(s); }
            { auto s=tolower_s(prop_val(ist,"text-align"));
              if      (s=="center")  e.text_align_own=1;
              else if (s=="right")   e.text_align_own=2;
              else if (s=="justify") e.text_align_own=3;
              else if (s=="left")    e.text_align_own=0; }

            // resolve bg_image: use CSS file URL if it came from a linked sheet
            if (!e.box.bg_image.empty()) {
                const std::string& res_base = !bg_img_src.empty() ? bg_img_src : base;
                e.box.bg_image = resolve(res_base, e.box.bg_image);
            }

            // display:none — skip this subtree entirely
            if (e.box.display == BoxModel::Display::None) {
                skip_depth = 1; acc.clear(); continue;
            }

            // decide if this element creates a block container
            bool floated = e.box.floatdir != BoxModel::Float::None;
            bool emits_block = floated
                || e.box.display == BoxModel::Display::Flex
                || e.box.display == BoxModel::Display::InlineBlock
                || e.box.display == BoxModel::Display::Block
                || (is_block_element(tname) && e.box.display != BoxModel::Display::Inline);

            stack.push_back(e);
            if (emits_block) {
                Element d; d.type=Element::DIV_OPEN; d.box=e.box;
                if (tname == "body") d.is_body = true;
                elems.push_back(d);
            }
        } else if (closing) {
            for (int j=(int)stack.size()-1; j>=0; --j) {
                if (stack[j].tag==tname) {
                    bool floated = stack[j].box.floatdir != BoxModel::Float::None;
                    bool had_block = floated
                        || stack[j].box.display == BoxModel::Display::Flex
                        || stack[j].box.display == BoxModel::Display::InlineBlock
                        || stack[j].box.display == BoxModel::Display::Block
                        || (is_block_element(tname) && stack[j].box.display != BoxModel::Display::Inline);
                    if (had_block) {
                        Element d; d.type=Element::DIV_CLOSE;
                        elems.push_back(d);
                    }
                    stack.erase(stack.begin()+j, stack.end());
                    break;
                }
            }
        }
    }
    flush();
    return elems;
}

// ---- parse HTML to DOM tree ----

static std::shared_ptr<Document> parse_html_to_dom(const std::string& html, const std::string& base) {
    auto css = extract_css(html);
    for (auto& r : fetch_linked_css(html, base)) css.push_back(std::move(r));

    auto doc = std::make_shared<Document>();
    // Root is <html>
    DOMNode* cur_parent = doc->root.get();

    // Create head and body under root
    auto head_node = doc->createElement("head");
    doc->appendChild(cur_parent, head_node);
    doc->head = head_node.get();

    auto body_node = doc->createElement("body");
    doc->appendChild(cur_parent, body_node);
    doc->body = body_node.get();

    // Stack for tracking open elements (for CSS matching)
    struct ParseEntry {
        DOMNode* node;
        std::string tag;
        std::vector<std::string> cls;
        std::string id;
    };
    std::vector<ParseEntry> stack;
    // Start inside body
    stack.push_back({doc->body, "body", {}, ""});
    cur_parent = doc->body;

    std::string acc;
    auto flush = [&]() {
        std::string t = collapse_ws(decode_entities(acc));
        if (!t.empty()) {
            auto tn = doc->createTextNode(t);

            // Compute style from stack
            int fs = 16;
            if (!stack.empty()) {
                // Walk the parent chain to find font-size
                DOMNode* p = cur_parent;
                while (p) { fs = p->fs_computed; break; }
            }
            tn->fs_computed = fs;

            // Inherit font-weight
            int fw = PANGO_WEIGHT_NORMAL;
            for (auto& e : stack) if (e.node->fw_computed != -1) fw = e.node->fw_computed;
            tn->fw_computed = fw;

            // Inherit line-height
            double lh = -1.0;
            for (auto& e : stack) if (e.node->lh_computed >= 0) lh = e.node->lh_computed;
            tn->lh_computed = lh;

            // Inherit color
            for (int i = (int)stack.size()-1; i >= 0; --i)
                if (!stack[i].node->color_computed.empty()) { tn->color_computed = stack[i].node->color_computed; break; }

            // Inherit text-align
            for (int i = (int)stack.size()-1; i >= 0; --i)
                if (stack[i].node->text_align_computed >= 0) { tn->text_align_computed = stack[i].node->text_align_computed; break; }

            // Inherit href
            for (int i = (int)stack.size()-1; i >= 0; --i)
                if (!stack[i].node->href.empty()) { tn->href = stack[i].node->href; break; }

            cur_parent->children.push_back(tn);
            tn->parent = cur_parent;
            doc->registerNode(tn.get());
        }
        acc.clear();
    };

    // Adapter: build StackEntry from ParseEntry for sel_matches compatibility
    auto make_stack_entries = [&]() -> std::vector<StackEntry> {
        std::vector<StackEntry> entries;
        for (auto& pe : stack) {
            StackEntry se;
            se.tag = pe.tag;
            se.cls = pe.cls;
            se.id = pe.id;
            entries.push_back(se);
        }
        return entries;
    };

    enum { NORM, SCRIPT_CAP, STYLE_SKIP, NOSCRIPT_SKIP, COMMENT } state = NORM;
    std::string script_content;
    std::string script_src;
    int skip_depth = 0;
    size_t i = 0, n = html.size();

    while (i < n) {
        if (state == COMMENT) {
            size_t p = html.find("-->", i);
            i = p == std::string::npos ? n : p + 3;
            state = NORM; continue;
        }
        if (state == SCRIPT_CAP) {
            size_t p = find_ci(html, "</script>", i);
            if (p == std::string::npos) {
                script_content += html.substr(i);
                i = n;
            } else {
                script_content += html.substr(i, p - i);
                i = p + 9;
            }
            if (!script_src.empty()) {
                doc->script_srcs.push_back(script_src);
            } else if (!script_content.empty()) {
                doc->scripts.push_back(script_content);
            }
            script_content.clear();
            script_src.clear();
            state = NORM; continue;
        }
        if (state == STYLE_SKIP) {
            size_t p = find_ci(html, "</style>", i);
            i = p == std::string::npos ? n : p + 8;
            state = NORM; continue;
        }
        if (state == NOSCRIPT_SKIP) {
            size_t p = find_ci(html, "</noscript>", i);
            i = p == std::string::npos ? n : p + 11;
            state = NORM; continue;
        }
        if (html[i] != '<') { if (!skip_depth) acc += html[i]; ++i; continue; }
        flush(); ++i;
        if (i >= n) break;
        if (i + 2 < n && html[i] == '!' && html[i+1] == '-' && html[i+2] == '-') {
            state = COMMENT; i += 3; continue;
        }
        // find tag end
        size_t ts = i; bool inq = false; char qc = 0;
        while (i < n) {
            if (!inq && html[i] == '>') break;
            if (inq && html[i] == qc) inq = false;
            else if (!inq && (html[i] == '"' || html[i] == '\'')) { inq = true; qc = html[i]; }
            ++i;
        }
        std::string tag = html.substr(ts, i - ts);
        if (i < n) ++i;
        if (tag.empty()) continue;

        bool closing = (tag[0] == '/');
        size_t ks = closing ? 1 : 0, k = ks;
        while (k < tag.size() && !std::isspace((unsigned char)tag[k]) && tag[k] != '>') ++k;
        std::string tname = tolower_s(tag.substr(ks, k - ks));

        // handle display:none skip region
        if (skip_depth > 0) {
            bool self_closing = (!tag.empty() && tag.back() == '/');
            if (!closing && !self_closing && !is_void(tname)) skip_depth++;
            else if (closing) { if (--skip_depth == 0) acc.clear(); }
            continue;
        }

        if (!closing && tname == "script") {
            script_src = extract_attr(tag, "src");
            if (!script_src.empty()) script_src = resolve(base, script_src);
            state = SCRIPT_CAP; continue;
        }
        if (!closing && tname == "style") { state = STYLE_SKIP; continue; }
        if (!closing && tname == "noscript") { state = NOSCRIPT_SKIP; continue; }
        if (closing && tname == "noscript") continue;
        // Skip <!DOCTYPE>, <!-- -->, and other !-prefixed declarations
        if (!tname.empty() && tname[0] == '!') continue;

        if (!closing && tname == "img") {
            std::string src = extract_attr(tag, "src");
            if (!src.empty()) {
                std::string u = resolve(base, src);
                if (!u.empty()) {
                    auto img_node = doc->createElement("img");
                    img_node->attributes["src"] = u;
                    cur_parent->children.push_back(img_node);
                    img_node->parent = cur_parent;
                    doc->registerNode(img_node.get());
                }
            }
            continue;
        }

        // <input> — void element, but we need to create a DOMNode for it
        if (!closing && tname == "input") {
            auto elem = doc->createElement("input");
            elem->attributes = extract_all_attrs(tag);
            auto id_it = elem->attributes.find("id");
            elem->id = id_it != elem->attributes.end() ? id_it->second : "";
            auto cls_it = elem->attributes.find("class");
            elem->class_list = cls_it != elem->attributes.end() ? split_classes(cls_it->second) : std::vector<std::string>{};
            cur_parent->children.push_back(elem);
            elem->parent = cur_parent;
            doc->registerNode(elem.get());
            if (!elem->id.empty()) doc->id_map[elem->id] = elem.get();
            continue;
        }

        // SVG void elements — need DOM nodes for the SVG renderer
        if (!closing && (tname=="path"||tname=="circle"||tname=="ellipse"||
            tname=="line"||tname=="polyline"||tname=="polygon"||
            tname=="rect"||tname=="use"||tname=="stop")) {
            auto elem = doc->createElement(tname);
            elem->attributes = extract_all_attrs(tag);
            auto id_it = elem->attributes.find("id");
            elem->id = id_it != elem->attributes.end() ? id_it->second : "";
            auto cls_it = elem->attributes.find("class");
            elem->class_list = cls_it != elem->attributes.end() ? split_classes(cls_it->second) : std::vector<std::string>{};
            cur_parent->children.push_back(elem);
            elem->parent = cur_parent;
            doc->registerNode(elem.get());
            if (!elem->id.empty()) doc->id_map[elem->id] = elem.get();
            continue;
        }

        if (!closing && !is_void(tname)) {
            int parent_fs = cur_parent ? cur_parent->fs_computed : 16;
            auto elem = doc->createElement(tname);
            elem->attributes = extract_all_attrs(tag);
            // Use properly parsed attributes for class/id
            auto cls_it = elem->attributes.find("class");
            elem->class_list = cls_it != elem->attributes.end() ? split_classes(cls_it->second) : std::vector<std::string>{};
            auto id_it = elem->attributes.find("id");
            elem->id = id_it != elem->attributes.end() ? id_it->second : "";
            elem->fs_computed = parent_fs;

            // UA defaults for bold tags
            for (int bi = 0; BOLD_TAGS[bi]; ++bi)
                if (tname == BOLD_TAGS[bi]) elem->fw_computed = PANGO_WEIGHT_BOLD;

            // UA defaults for italic tags
            if (tname == "i" || tname == "em" || tname == "cite" || tname == "dfn" || tname == "var")
                elem->fi_computed = PANGO_STYLE_ITALIC;

            // UA font-size defaults for headings
            if (tname == "h1") elem->fs_computed = 32;
            else if (tname == "h2") elem->fs_computed = 24;
            else if (tname == "h3") elem->fs_computed = 19;
            else if (tname == "h4") elem->fs_computed = 16;
            else if (tname == "h5") elem->fs_computed = 13;
            else if (tname == "h6") elem->fs_computed = 11;

            // UA defaults for strikethrough tags
            if (tname == "s" || tname == "del" || tname == "strike")
                elem->text_decoration = 4; // line-through
            // UA defaults for underline tags
            if (tname == "u" || tname == "ins")
                elem->text_decoration = 1; // underline
            // UA defaults for monospace tags
            if (tname == "code" || tname == "kbd" || tname == "samp" || tname == "tt")
                elem->font_family = "monospace";
            // UA defaults for <pre>
            if (tname == "pre") { elem->font_family = "monospace"; elem->white_space = 2; }
            // UA defaults for size tags
            if (tname == "small") elem->fs_computed = (int)(parent_fs * 0.83);
            if (tname == "sub" || tname == "sup") elem->fs_computed = (int)(parent_fs * 0.83);
            if (tname == "big") elem->fs_computed = (int)(parent_fs * 1.17);
            // UA defaults for <abbr> - dotted underline
            if (tname == "abbr") { elem->text_decoration = 1; elem->text_decoration_style = 2; }

            // Build StackEntry for CSS matching
            StackEntry se;
            se.tag = tname;
            se.cls = elem->class_list;
            se.id = elem->id;
            auto ancestors = make_stack_entries();

            // CSS rules
            std::string bg_img_src;
            BoxModel bm;
            for (const auto& r : css) {
                if (!sel_matches(r.sel, ancestors, se)) continue;
                if (r.fw != -1) elem->fw_computed = r.fw;
                if (!r.fs_raw.empty()) {
                    int px = parse_fs(r.fs_raw, parent_fs);
                    if (px > 0) elem->fs_computed = px;
                }
                if (!r.lh_raw.empty()) {
                    double f = parse_lh(r.lh_raw, elem->fs_computed);
                    if (f >= 0) elem->lh_computed = f;
                }
                std::string prev_bg = bm.bg_image;
                apply_box(r.decls, bm);
                if (bm.bg_image != prev_bg) bg_img_src = r.src_url;
                { auto s = prop_val(r.decls, "color"); if (!s.empty()) elem->color_computed = collapse_ws(s); }
                { auto s = tolower_s(prop_val(r.decls, "text-align"));
                  if      (s == "center")  elem->text_align_computed = 1;
                  else if (s == "right")   elem->text_align_computed = 2;
                  else if (s == "justify") elem->text_align_computed = 3;
                  else if (s == "left")    elem->text_align_computed = 0; }
                { auto s = tolower_s(prop_val(r.decls, "text-transform"));
                  if      (s == "uppercase")  elem->text_transform = 1;
                  else if (s == "lowercase")  elem->text_transform = 2;
                  else if (s == "capitalize") elem->text_transform = 3;
                  else if (s == "none")       elem->text_transform = 0; }
                { auto s = prop_val(r.decls, "font-family");
                  if (!s.empty()) elem->font_family = s; }
                { auto s = prop_val(r.decls, "box-shadow");
                  if (!s.empty()) elem->box_shadow = s; }
                { auto s = prop_val(r.decls, "opacity");
                  if (!s.empty()) { try { elem->opacity = std::stod(s); } catch(...){} } }
                { auto s = tolower_s(prop_val(r.decls, "overflow"));
                  if      (s == "visible") elem->overflow = 0;
                  else if (s == "hidden")  elem->overflow = 1;
                  else if (s == "scroll")  elem->overflow = 2;
                  else if (s == "auto")    elem->overflow = 3; }
                { auto s = tolower_s(prop_val(r.decls, "flex-direction"));
                  if      (s == "row")            elem->flex_direction = 0;
                  else if (s == "column")         elem->flex_direction = 1;
                  else if (s == "row-reverse")    elem->flex_direction = 2;
                  else if (s == "column-reverse") elem->flex_direction = 3; }
                { auto s = tolower_s(prop_val(r.decls, "justify-content"));
                  if      (s == "flex-start" || s == "start") elem->justify_content = 0;
                  else if (s == "flex-end" || s == "end")     elem->justify_content = 1;
                  else if (s == "center")                     elem->justify_content = 2;
                  else if (s == "space-between")              elem->justify_content = 3;
                  else if (s == "space-around")               elem->justify_content = 4;
                  else if (s == "space-evenly")               elem->justify_content = 5; }
                { auto s = tolower_s(prop_val(r.decls, "align-items"));
                  if      (s == "stretch")                    elem->align_items = 0;
                  else if (s == "flex-start" || s == "start") elem->align_items = 1;
                  else if (s == "flex-end" || s == "end")     elem->align_items = 2;
                  else if (s == "center")                     elem->align_items = 3; }
                { auto s = tolower_s(prop_val(r.decls, "flex-wrap"));
                  if      (s == "nowrap") elem->flex_wrap = 0;
                  else if (s == "wrap")   elem->flex_wrap = 1; }
                { auto s = prop_val(r.decls, "gap");
                  if (!s.empty()) { int px = parse_px_val(s); if (px > 0) elem->gap = px; } }
                { auto s = tolower_s(prop_val(r.decls, "position"));
                  if      (s == "static")   elem->position = 0;
                  else if (s == "relative") elem->position = 1;
                  else if (s == "absolute") elem->position = 2;
                  else if (s == "fixed")    elem->position = 3; }
                { auto s = prop_val(r.decls, "top");
                  if (!s.empty() && tolower_s(s) != "auto") elem->pos_top = parse_px_val(s); }
                { auto s = prop_val(r.decls, "left");
                  if (!s.empty() && tolower_s(s) != "auto") elem->pos_left = parse_px_val(s); }
                { auto s = prop_val(r.decls, "right");
                  if (!s.empty() && tolower_s(s) != "auto") elem->pos_right = parse_px_val(s); }
                { auto s = prop_val(r.decls, "bottom");
                  if (!s.empty() && tolower_s(s) != "auto") elem->pos_bottom = parse_px_val(s); }
                { auto s = prop_val(r.decls, "z-index");
                  if (!s.empty()) { try { elem->z_index = std::stoi(s); } catch(...){} } }
                // font-style from CSS rules (BUG FIX: was only parsed from inline styles)
                { std::string fs = tolower_s(prop_val(r.decls, "font-style"));
                  if (fs == "italic") elem->fi_computed = PANGO_STYLE_ITALIC;
                  else if (fs == "oblique") elem->fi_computed = PANGO_STYLE_OBLIQUE;
                  else if (fs == "normal") elem->fi_computed = PANGO_STYLE_NORMAL; }
                // text-decoration shorthand
                { auto s = tolower_s(prop_val(r.decls, "text-decoration"));
                  if (!s.empty()) parse_text_decoration(s, elem.get()); }
                // text-decoration-line
                { auto s = tolower_s(prop_val(r.decls, "text-decoration-line"));
                  if (!s.empty()) elem->text_decoration = parse_td_line(s); }
                // text-decoration-color
                { auto s = prop_val(r.decls, "text-decoration-color");
                  if (!s.empty()) elem->text_decoration_color = s; }
                // text-decoration-style
                { auto s = tolower_s(prop_val(r.decls, "text-decoration-style"));
                  if (!s.empty()) elem->text_decoration_style = parse_td_style(s); }
                // letter-spacing
                { auto s = prop_val(r.decls, "letter-spacing");
                  if (tolower_s(s) == "normal") elem->letter_spacing = 0;
                  else if (!s.empty()) { int px = parse_px_val(s); elem->letter_spacing = px * PANGO_SCALE; } }
                // word-spacing
                { auto s = prop_val(r.decls, "word-spacing");
                  if (tolower_s(s) == "normal") elem->word_spacing = 0;
                  else if (!s.empty()) elem->word_spacing = parse_px_val(s); }
                // font-variant
                { auto s = tolower_s(prop_val(r.decls, "font-variant"));
                  if (s == "small-caps") elem->font_variant = 1;
                  else if (s == "normal") elem->font_variant = 0; }
                // white-space
                { auto s = tolower_s(prop_val(r.decls, "white-space"));
                  if (!s.empty()) { int ws = parse_white_space(s); if (ws >= 0) elem->white_space = ws; } }
                // text-indent
                { auto s = prop_val(r.decls, "text-indent");
                  if (!s.empty()) elem->text_indent = parse_px_val(s); }
                // text-overflow
                { auto s = tolower_s(prop_val(r.decls, "text-overflow"));
                  if (s == "ellipsis") elem->text_overflow = 1;
                  else if (s == "clip") elem->text_overflow = 0; }
                // font-stretch
                { auto s = tolower_s(prop_val(r.decls, "font-stretch"));
                  if (!s.empty()) { int st = parse_font_stretch(s); if (st >= 0) elem->font_stretch = st; } }
                // text-shadow (store only)
                { auto s = prop_val(r.decls, "text-shadow");
                  if (!s.empty()) elem->text_shadow = s; }
                // font shorthand
                { auto s = prop_val(r.decls, "font");
                  if (!s.empty()) parse_font_shorthand(s, elem.get(), parent_fs); }
                // font-family: clean for Pango
                if (!elem->font_family.empty())
                    elem->font_family = css_font_to_pango(elem->font_family);
            }

            // anchor href + default underline
            if (tname == "a") {
                auto href_it = elem->attributes.find("href");
                if (href_it != elem->attributes.end() && !href_it->second.empty()) {
                    elem->href = resolve(base, href_it->second);
                    if (elem->text_decoration < 0) elem->text_decoration = 1; // default underline
                }
            }
            // UA default for <mark> - yellow background
            if (tname == "mark") bm.bg_color = "yellow";

            // inline style
            auto style_it = elem->attributes.find("style");
            std::string ist = style_it != elem->attributes.end() ? style_it->second : "";
            elem->inline_style_raw = ist;
            { int v = fw_value(prop_val(ist, "font-weight")); if (v != -1) elem->fw_computed = v; }
            { std::string fs = prop_val(ist, "font-style");
              if (fs == "italic") elem->fi_computed = PANGO_STYLE_ITALIC;
              else if (fs == "oblique") elem->fi_computed = PANGO_STYLE_OBLIQUE;
              else if (fs == "normal") elem->fi_computed = PANGO_STYLE_NORMAL; }
            { int px = parse_fs(prop_val(ist, "font-size"), parent_fs); if (px > 0) elem->fs_computed = px; }
            { double f = parse_lh(prop_val(ist, "line-height"), elem->fs_computed); if (f >= 0) elem->lh_computed = f; }
            apply_box(ist, bm);
            { auto s = prop_val(ist, "color"); if (!s.empty()) elem->color_computed = collapse_ws(s); }
            { auto s = tolower_s(prop_val(ist, "text-align"));
              if      (s == "center")  elem->text_align_computed = 1;
              else if (s == "right")   elem->text_align_computed = 2;
              else if (s == "justify") elem->text_align_computed = 3;
              else if (s == "left")    elem->text_align_computed = 0; }
            { auto s = tolower_s(prop_val(ist, "text-transform"));
              if      (s == "uppercase")  elem->text_transform = 1;
              else if (s == "lowercase")  elem->text_transform = 2;
              else if (s == "capitalize") elem->text_transform = 3;
              else if (s == "none")       elem->text_transform = 0; }
            { auto s = prop_val(ist, "font-family");
              if (!s.empty()) elem->font_family = s; }
            { auto s = prop_val(ist, "box-shadow");
              if (!s.empty()) elem->box_shadow = s; }
            { auto s = prop_val(ist, "opacity");
              if (!s.empty()) { try { elem->opacity = std::stod(s); } catch(...){} } }
            { auto s = tolower_s(prop_val(ist, "overflow"));
              if      (s == "visible") elem->overflow = 0;
              else if (s == "hidden")  elem->overflow = 1;
              else if (s == "scroll")  elem->overflow = 2;
              else if (s == "auto")    elem->overflow = 3; }
            { auto s = tolower_s(prop_val(ist, "flex-direction"));
              if      (s == "row")            elem->flex_direction = 0;
              else if (s == "column")         elem->flex_direction = 1;
              else if (s == "row-reverse")    elem->flex_direction = 2;
              else if (s == "column-reverse") elem->flex_direction = 3; }
            { auto s = tolower_s(prop_val(ist, "justify-content"));
              if      (s == "flex-start" || s == "start") elem->justify_content = 0;
              else if (s == "flex-end" || s == "end")     elem->justify_content = 1;
              else if (s == "center")                     elem->justify_content = 2;
              else if (s == "space-between")              elem->justify_content = 3;
              else if (s == "space-around")               elem->justify_content = 4;
              else if (s == "space-evenly")               elem->justify_content = 5; }
            { auto s = tolower_s(prop_val(ist, "align-items"));
              if      (s == "stretch")                    elem->align_items = 0;
              else if (s == "flex-start" || s == "start") elem->align_items = 1;
              else if (s == "flex-end" || s == "end")     elem->align_items = 2;
              else if (s == "center")                     elem->align_items = 3; }
            { auto s = tolower_s(prop_val(ist, "flex-wrap"));
              if      (s == "nowrap") elem->flex_wrap = 0;
              else if (s == "wrap")   elem->flex_wrap = 1; }
            { auto s = prop_val(ist, "gap");
              if (!s.empty()) { int px = parse_px_val(s); if (px > 0) elem->gap = px; } }
            { auto s = tolower_s(prop_val(ist, "position"));
              if      (s == "static")   elem->position = 0;
              else if (s == "relative") elem->position = 1;
              else if (s == "absolute") elem->position = 2;
              else if (s == "fixed")    elem->position = 3; }
            { auto s = prop_val(ist, "top");
              if (!s.empty() && tolower_s(s) != "auto") elem->pos_top = parse_px_val(s); }
            { auto s = prop_val(ist, "left");
              if (!s.empty() && tolower_s(s) != "auto") elem->pos_left = parse_px_val(s); }
            { auto s = prop_val(ist, "right");
              if (!s.empty() && tolower_s(s) != "auto") elem->pos_right = parse_px_val(s); }
            { auto s = prop_val(ist, "bottom");
              if (!s.empty() && tolower_s(s) != "auto") elem->pos_bottom = parse_px_val(s); }
            { auto s = prop_val(ist, "z-index");
              if (!s.empty()) { try { elem->z_index = std::stoi(s); } catch(...){} } }
            // text-decoration shorthand (inline)
            { auto s = tolower_s(prop_val(ist, "text-decoration"));
              if (!s.empty()) parse_text_decoration(s, elem.get()); }
            { auto s = tolower_s(prop_val(ist, "text-decoration-line"));
              if (!s.empty()) elem->text_decoration = parse_td_line(s); }
            { auto s = prop_val(ist, "text-decoration-color");
              if (!s.empty()) elem->text_decoration_color = s; }
            { auto s = tolower_s(prop_val(ist, "text-decoration-style"));
              if (!s.empty()) elem->text_decoration_style = parse_td_style(s); }
            // letter-spacing (inline)
            { auto s = prop_val(ist, "letter-spacing");
              if (tolower_s(s) == "normal") elem->letter_spacing = 0;
              else if (!s.empty()) { int px = parse_px_val(s); elem->letter_spacing = px * PANGO_SCALE; } }
            // word-spacing (inline)
            { auto s = prop_val(ist, "word-spacing");
              if (tolower_s(s) == "normal") elem->word_spacing = 0;
              else if (!s.empty()) elem->word_spacing = parse_px_val(s); }
            // font-variant (inline)
            { auto s = tolower_s(prop_val(ist, "font-variant"));
              if (s == "small-caps") elem->font_variant = 1;
              else if (s == "normal") elem->font_variant = 0; }
            // white-space (inline)
            { auto s = tolower_s(prop_val(ist, "white-space"));
              if (!s.empty()) { int ws = parse_white_space(s); if (ws >= 0) elem->white_space = ws; } }
            // text-indent (inline)
            { auto s = prop_val(ist, "text-indent");
              if (!s.empty()) elem->text_indent = parse_px_val(s); }
            // text-overflow (inline)
            { auto s = tolower_s(prop_val(ist, "text-overflow"));
              if (s == "ellipsis") elem->text_overflow = 1;
              else if (s == "clip") elem->text_overflow = 0; }
            // font-stretch (inline)
            { auto s = tolower_s(prop_val(ist, "font-stretch"));
              if (!s.empty()) { int st = parse_font_stretch(s); if (st >= 0) elem->font_stretch = st; } }
            // text-shadow (inline, store only)
            { auto s = prop_val(ist, "text-shadow");
              if (!s.empty()) elem->text_shadow = s; }
            // font shorthand (inline)
            { auto s = prop_val(ist, "font");
              if (!s.empty()) parse_font_shorthand(s, elem.get(), parent_fs); }
            // Clean font-family for Pango
            if (!elem->font_family.empty())
                elem->font_family = css_font_to_pango(elem->font_family);

            // Resolve bg_image
            if (!bm.bg_image.empty()) {
                const std::string& res_base = !bg_img_src.empty() ? bg_img_src : base;
                bm.bg_image = resolve(res_base, bm.bg_image);
            }

            // Copy BoxModel fields to DOMNode
            for (int j = 0; j < 4; ++j) elem->margin[j] = bm.margin[j];
            for (int j = 0; j < 4; ++j) elem->padding[j] = bm.padding[j];
            elem->width = bm.width;
            elem->max_width = bm.max_width;
            elem->height = bm.height;
            for (int j = 0; j < 4; ++j) elem->border_width[j] = bm.border_width[j];
            elem->border_radius = bm.border_radius;
            elem->border_color = bm.border_color;
            elem->border_style = bm.border_style;
            elem->halign_center = bm.halign_center;
            elem->display = static_cast<DOMNode::Display>(static_cast<int>(bm.display));
            elem->floatdir = static_cast<DOMNode::Float>(static_cast<int>(bm.floatdir));
            elem->bg_image = bm.bg_image;
            elem->bg_color = bm.bg_color;
            if (!bm.bg_repeat.empty()) elem->style_props["background-repeat"] = bm.bg_repeat;
            if (!bm.bg_size.empty()) elem->style_props["background-size"] = bm.bg_size;
            if (!bm.bg_position.empty()) {
                elem->style_props["background-position"] = bm.bg_position;
                fprintf(stderr,"[DEBUG cascade] <%s> bg_position='%s'\n", tname.c_str(), bm.bg_position.c_str());
            }
            if (!bm.box_shadow.empty()) elem->box_shadow = bm.box_shadow;
            if (bm.opacity < 1.0) elem->opacity = bm.opacity;
            if (bm.overflow >= 0) elem->overflow = bm.overflow;

            // display:none - skip subtree
            if (elem->display == DOMNode::Display::None) {
                skip_depth = 1; acc.clear(); continue;
            }

            // body special
            if (tname == "body") {
                elem->is_body = true;
                // Copy box model to the existing body node
                doc->body->fw_computed = elem->fw_computed;
                doc->body->fs_computed = elem->fs_computed;
                doc->body->lh_computed = elem->lh_computed;
                doc->body->color_computed = elem->color_computed;
                doc->body->text_align_computed = elem->text_align_computed;
                for (int j = 0; j < 4; ++j) doc->body->margin[j] = elem->margin[j];
                for (int j = 0; j < 4; ++j) doc->body->padding[j] = elem->padding[j];
                doc->body->width = elem->width;
                doc->body->max_width = elem->max_width;
                doc->body->height = elem->height;
                for (int j = 0; j < 4; ++j) doc->body->border_width[j] = elem->border_width[j];
                doc->body->border_radius = elem->border_radius;
                doc->body->border_color = elem->border_color;
                doc->body->border_style = elem->border_style;
                doc->body->halign_center = elem->halign_center;
                doc->body->display = elem->display;
                doc->body->floatdir = elem->floatdir;
                doc->body->bg_image = elem->bg_image;
                doc->body->bg_color = elem->bg_color;
                { auto it = elem->style_props.find("background-repeat");
                  if (it != elem->style_props.end()) doc->body->style_props["background-repeat"] = it->second; }
                { auto it = elem->style_props.find("background-size");
                  if (it != elem->style_props.end()) doc->body->style_props["background-size"] = it->second; }
                { auto it = elem->style_props.find("background-position");
                  if (it != elem->style_props.end()) doc->body->style_props["background-position"] = it->second; }
                doc->body->text_transform = elem->text_transform;
                doc->body->font_family = elem->font_family;
                doc->body->box_shadow = elem->box_shadow;
                doc->body->opacity = elem->opacity;
                doc->body->overflow = elem->overflow;
                doc->body->flex_direction = elem->flex_direction;
                doc->body->justify_content = elem->justify_content;
                doc->body->align_items = elem->align_items;
                doc->body->flex_wrap = elem->flex_wrap;
                doc->body->gap = elem->gap;
                doc->body->position = elem->position;
                doc->body->pos_top = elem->pos_top;
                doc->body->pos_left = elem->pos_left;
                doc->body->pos_right = elem->pos_right;
                doc->body->pos_bottom = elem->pos_bottom;
                doc->body->z_index = elem->z_index;
                doc->body->is_body = true;
                doc->body->inline_style_raw = elem->inline_style_raw;
                if (!elem->id.empty()) {
                    doc->body->id = elem->id;
                    doc->id_map[elem->id] = doc->body;
                }
                doc->body->class_list = elem->class_list;
                stack.push_back({doc->body, tname, elem->class_list, elem->id});
                // cur_parent stays as body
                continue;
            }

            // Register ID
            if (!elem->id.empty())
                doc->id_map[elem->id] = elem.get();

            // Add to tree
            cur_parent->children.push_back(elem);
            elem->parent = cur_parent;
            doc->registerNode(elem.get());

            stack.push_back({elem.get(), tname, elem->class_list, elem->id});
            cur_parent = elem.get();

        } else if (closing) {
            // Find matching open tag
            for (int j = (int)stack.size() - 1; j >= 0; --j) {
                if (stack[j].tag == tname) {
                    // Pop back to that level
                    cur_parent = (j > 0) ? stack[j-1].node : doc->root.get();
                    stack.erase(stack.begin() + j, stack.end());
                    break;
                }
            }
        }
    }
    flush();

    return doc;
}

// ---- idle trampoline (allows capturing lambdas via g_idle_add) ----

static gboolean idle_trampoline(gpointer data) {
    auto* fn = static_cast<std::function<void()>*>(data);
    (*fn)(); delete fn;
    return G_SOURCE_REMOVE;
}
static void idle_add(std::function<void()> fn) {
    g_idle_add(idle_trampoline, new std::function<void()>(std::move(fn)));
}

// ---- TabState (per-tab page state) ----

struct TabState {
    // Per-page navigation
    std::string current_url;
    std::vector<std::string> back_stack;
    std::vector<std::string> fwd_stack;

    // Page content
    std::shared_ptr<Document> document;
    JSEngine* js_engine = nullptr;
    std::string page_source;
    std::string title = "New Tab";

    // Async cancellation
    std::mutex mu;
    int generation = 0;

    // Per-tab widget tree
    GtkWidget* paned = nullptr;          // horizontal pane: content | inspector
    GtkWidget* scroll = nullptr;         // scrolled window
    GtkWidget* viewport = nullptr;       // viewport inside scroll
    GtkWidget* content_box = nullptr;    // main content container

    // Node-to-widget map for getBoundingClientRect / offset*
    std::unordered_map<uint32_t, GtkWidget*> node_widget_map;
    uint32_t focused_node_id = 0;
    gulong body_draw_signal = 0;

    // Inspector panel
    GtkWidget* inspector_box = nullptr;
    GtkWidget* inspector_notebook = nullptr;
    GtkWidget* console_text_view = nullptr;
    GtkWidget* elements_text_view = nullptr;
    bool inspector_visible = false;
    int inspector_width = 500;

    ~TabState() {
        if (js_engine) { delete js_engine; js_engine = nullptr; }
    }
};

struct ClosedTabInfo {
    std::string url;
    std::vector<std::string> back_stack;
    std::vector<std::string> fwd_stack;
};

// ---- AppState (browser-level container) ----

struct AppState {
    GtkWidget* window;
    GtkWidget* address_bar;
    GtkWidget* btn_back;
    GtkWidget* btn_fwd;

    // Tab management
    std::vector<std::shared_ptr<TabState>> tabs;
    int active_tab_idx = 0;
    std::vector<ClosedTabInfo> closed_tabs;

    // Tab bar (Cairo-drawn)
    GtkWidget* tab_bar_area = nullptr;

    // Content stack (GtkStack for tab switching)
    GtkWidget* content_stack = nullptr;

    // Tab bar interaction state
    int tab_bar_hover = -1;
    int tab_bar_close_hover = -1;
    bool tab_bar_dragging = false;
    int tab_bar_drag_idx = -1;
    double tab_bar_drag_x = 0;
    double tab_bar_drag_start_x = 0;

    // Convenience: current tab pointer (always valid when tabs non-empty)
    TabState* ct = nullptr;
    TabState* current_tab() { return tabs.empty() ? nullptr : tabs[active_tab_idx].get(); }
    void sync_ct() { ct = current_tab(); }
};

// ---- geometry helper for js_bindings (getBoundingClientRect / offset*) ----

void js_get_node_geometry(TabState* tab, uint32_t node_id, int& x, int& y, int& w, int& h) {
    x = y = w = h = 0;
    if (!tab) return;
    auto it = tab->node_widget_map.find(node_id);
    if (it == tab->node_widget_map.end()) return;
    GtkWidget* widget = it->second;
    if (!widget || !GTK_IS_WIDGET(widget) || !gtk_widget_get_realized(widget)) return;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    w = alloc.width;
    h = alloc.height;
    // Translate to viewport-relative coordinates
    if (tab->viewport && GTK_IS_WIDGET(tab->viewport) && gtk_widget_get_realized(tab->viewport)) {
        int vx = 0, vy = 0;
        gtk_widget_translate_coordinates(widget, tab->viewport, 0, 0, &vx, &vy);
        x = vx;
        y = vy;
    } else {
        x = alloc.x;
        y = alloc.y;
    }
}

// ---- block container builder (main thread only) ----

static bool has_css_var(const std::string& s) {
    return s.find("var(") != std::string::npos;
}

static GtkWidget* make_block(const BoxModel& box, GtkWidget* parent,
                              GtkOrientation orient = GTK_ORIENTATION_VERTICAL,
                              bool to_end = false) {
    GtkWidget* outer = gtk_box_new(orient, 0);
    {
        std::string props;
        if (!box.bg_color.empty() && !has_css_var(box.bg_color))
            props += "background-color: " + box.bg_color + "; ";
        if (box.border_radius > 0)
            props += "border-radius: " + std::to_string(box.border_radius) + "px; ";
        bool has_border = box.border_width[0]||box.border_width[1]||box.border_width[2]||box.border_width[3];
        if (has_border) {
            std::string bstyle = box.border_style.empty() ? "solid" : box.border_style;
            std::string bcolor = box.border_color.empty() ? "currentColor" : box.border_color;
            if (!has_css_var(bcolor)) {
                bool uniform = (box.border_width[0]==box.border_width[1] &&
                                box.border_width[1]==box.border_width[2] &&
                                box.border_width[2]==box.border_width[3]);
                if (uniform)
                    props += "border: " + std::to_string(box.border_width[0]) + "px " + bstyle + " " + bcolor + "; ";
                else {
                    static const char* sides[4] = {"top","right","bottom","left"};
                    for (int i=0;i<4;++i) if (box.border_width[i]>0)
                        props += "border-" + std::string(sides[i]) + ": "
                               + std::to_string(box.border_width[i]) + "px " + bstyle + " " + bcolor + "; ";
                }
            }
        }
        if (!box.box_shadow.empty() && !has_css_var(box.box_shadow))
            props += "box-shadow: " + box.box_shadow + "; ";
        if (!props.empty()) {
            GtkCssProvider* cp = gtk_css_provider_new();
            gtk_css_provider_load_from_data(cp, ("box { " + props + "}").c_str(), -1, nullptr);
            gtk_style_context_add_provider(gtk_widget_get_style_context(outer),
                GTK_STYLE_PROVIDER(cp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(cp);
        }
    }
    if (box.opacity < 1.0)
        gtk_widget_set_opacity(outer, box.opacity);
    gtk_widget_set_margin_top(outer,    std::max(0, box.margin[0]));
    gtk_widget_set_margin_end(outer,    std::max(0, box.margin[1]));
    gtk_widget_set_margin_bottom(outer, std::max(0, box.margin[2]));
    gtk_widget_set_margin_start(outer,  std::max(0, box.margin[3]));

    int eff_w = box.width > 0 ? box.width : box.max_width;
    int eff_h = box.height > 0 ? box.height : -1;
    if (eff_w > 0) {
        // size_request sets the minimum; GTK can still grow the widget if children
        // have a larger natural width.  CSS max-width caps the natural width so the
        // parent container never allocates more than eff_w pixels to this block.
        gtk_widget_set_size_request(outer, eff_w, eff_h);
        {
            GtkCssProvider* cp = gtk_css_provider_new();
            std::string w_css = "* { min-width: " + std::to_string(eff_w) + "px; }";
            gtk_css_provider_load_from_data(cp, w_css.c_str(), -1, nullptr);
            gtk_style_context_add_provider(gtk_widget_get_style_context(outer),
                GTK_STYLE_PROVIDER(cp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(cp);
        }
        gtk_widget_set_hexpand(outer, FALSE);
        gtk_widget_set_halign(outer, box.halign_center ? GTK_ALIGN_CENTER : GTK_ALIGN_START);
    } else {
        if (eff_h > 0) gtk_widget_set_size_request(outer, -1, eff_h);
        gtk_widget_set_hexpand(outer, orient == GTK_ORIENTATION_VERTICAL);
        if (box.halign_center) gtk_widget_set_halign(outer, GTK_ALIGN_CENTER);
    }

    if (to_end) gtk_box_pack_end(GTK_BOX(parent), outer, FALSE, FALSE, 0);
    else        gtk_box_pack_start(GTK_BOX(parent), outer, FALSE, FALSE, 0);
    gtk_widget_show(outer);

    // overflow handling: wrap content in scroll window
    if (box.overflow == 1 || box.overflow == 2 || box.overflow == 3) {
        GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        if (box.overflow == 1) // hidden: clip, no scrollbars
            gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                GTK_POLICY_NEVER, GTK_POLICY_NEVER);
        else // scroll/auto: show scrollbars as needed
            gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        GtkWidget* content = gtk_box_new(orient, 0);
        gtk_widget_set_hexpand(content, TRUE);
        gtk_container_add(GTK_CONTAINER(scroll), content);
        gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);
        gtk_widget_show(scroll);
        gtk_widget_show(content);
        // inner padding inside overflow container
        bool has_pad = box.padding[0]||box.padding[1]||box.padding[2]||box.padding[3];
        if (has_pad) {
            gtk_widget_set_margin_top(content,    box.padding[0]);
            gtk_widget_set_margin_end(content,    box.padding[1]);
            gtk_widget_set_margin_bottom(content, box.padding[2]);
            gtk_widget_set_margin_start(content,  box.padding[3]);
        }
        return content;
    }

    // inner box simulates padding
    bool has_pad = box.padding[0]||box.padding[1]||box.padding[2]||box.padding[3];
    if (has_pad) {
        GtkWidget* inner = gtk_box_new(orient, 0);
        gtk_widget_set_margin_top(inner,    box.padding[0]);
        gtk_widget_set_margin_end(inner,    box.padding[1]);
        gtk_widget_set_margin_bottom(inner, box.padding[2]);
        gtk_widget_set_margin_start(inner,  box.padding[3]);
        gtk_widget_set_hexpand(inner, TRUE);
        gtk_box_pack_start(GTK_BOX(outer), inner, TRUE, TRUE, 0);
        gtk_widget_show(inner);
        return inner;
    }
    return outer;
}

// ---- SVG rendering ----

struct SvgStyle {
    std::string fill = "black";
    std::string stroke;
    double stroke_width = 1.0;
    double opacity = 1.0;
    double fill_opacity = 1.0;
    double stroke_opacity = 1.0;
    std::string font_family = "sans-serif";
    double font_size = 16.0;
    std::string text_anchor = "start";
    int font_weight = 400;
};

struct SvgPathCmd {
    char cmd;
    std::vector<double> args;
};

struct SvgTransform {
    double a=1, b=0, c=0, d=1, e=0, f=0;
    void multiply(const SvgTransform& o) {
        double na=a*o.a+c*o.b, nb=b*o.a+d*o.b;
        double nc=a*o.c+c*o.d, nd=b*o.c+d*o.d;
        double ne=a*o.e+c*o.f+e, nf=b*o.e+d*o.f+f;
        a=na; b=nb; c=nc; d=nd; e=ne; f=nf;
    }
    cairo_matrix_t as_matrix() const {
        cairo_matrix_t m; cairo_matrix_init(&m, a, b, c, d, e, f); return m;
    }
};

struct SvgViewBox {
    double min_x=0, min_y=0, width=0, height=0;
    bool valid = false;
};

static bool svg_parse_color(const std::string& s, double& r, double& g, double& b, double& a) {
    if (s.empty() || s == "none") return false;
    GdkRGBA rgba = {0,0,0,1};
    if (gdk_rgba_parse(&rgba, s.c_str())) {
        r = rgba.red; g = rgba.green; b = rgba.blue; a = rgba.alpha;
        return true;
    }
    return false;
}

static double svg_parse_double(const std::string& s, size_t& pos) {
    while (pos < s.size() && (s[pos]==' '||s[pos]==','||s[pos]=='\t'||s[pos]=='\n'||s[pos]=='\r')) ++pos;
    if (pos >= s.size()) return 0;
    size_t start = pos;
    if (s[pos]=='-'||s[pos]=='+') ++pos;
    while (pos < s.size() && ((s[pos]>='0'&&s[pos]<='9')||s[pos]=='.')) ++pos;
    if (pos < s.size() && (s[pos]=='e'||s[pos]=='E')) {
        ++pos;
        if (pos < s.size() && (s[pos]=='-'||s[pos]=='+')) ++pos;
        while (pos < s.size() && s[pos]>='0'&&s[pos]<='9') ++pos;
    }
    if (pos == start) return 0;
    try { return std::stod(s.substr(start, pos-start)); } catch (...) { return 0; }
}

static SvgViewBox svg_parse_viewbox(const std::string& s) {
    SvgViewBox vb;
    if (s.empty()) return vb;
    size_t pos = 0;
    vb.min_x = svg_parse_double(s, pos);
    vb.min_y = svg_parse_double(s, pos);
    vb.width = svg_parse_double(s, pos);
    vb.height = svg_parse_double(s, pos);
    vb.valid = (vb.width > 0 && vb.height > 0);
    return vb;
}

static SvgTransform svg_parse_transform(const std::string& s) {
    SvgTransform result;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i]==' '||s[i]==',')) ++i;
        size_t ns = i;
        while (i < s.size() && s[i] != '(') ++i;
        if (i >= s.size()) break;
        std::string fn = s.substr(ns, i - ns);
        while (!fn.empty() && fn.back()==' ') fn.pop_back();
        ++i; // skip (
        std::vector<double> args;
        while (i < s.size() && s[i] != ')') {
            args.push_back(svg_parse_double(s, i));
        }
        if (i < s.size()) ++i; // skip )

        SvgTransform t;
        if (fn == "translate" && args.size() >= 1) {
            t.e = args[0]; t.f = args.size()>=2 ? args[1] : 0;
        } else if (fn == "scale" && args.size() >= 1) {
            t.a = args[0]; t.d = args.size()>=2 ? args[1] : args[0];
        } else if (fn == "rotate" && args.size() >= 1) {
            double rad = args[0] * M_PI / 180.0;
            double co = cos(rad), si = sin(rad);
            if (args.size() >= 3) {
                SvgTransform t1; t1.e = args[1]; t1.f = args[2];
                SvgTransform r; r.a = co; r.b = si; r.c = -si; r.d = co;
                SvgTransform t2; t2.e = -args[1]; t2.f = -args[2];
                t1.multiply(r); t1.multiply(t2);
                t = t1;
            } else {
                t.a = co; t.b = si; t.c = -si; t.d = co;
            }
        } else if (fn == "skewX" && args.size() >= 1) {
            t.c = tan(args[0] * M_PI / 180.0);
        } else if (fn == "skewY" && args.size() >= 1) {
            t.b = tan(args[0] * M_PI / 180.0);
        } else if (fn == "matrix" && args.size() >= 6) {
            t.a=args[0]; t.b=args[1]; t.c=args[2]; t.d=args[3]; t.e=args[4]; t.f=args[5];
        }
        result.multiply(t);
    }
    return result;
}

static SvgStyle svg_resolve_style(DOMNode* node) {
    SvgStyle style;
    std::vector<DOMNode*> chain;
    DOMNode* n = node;
    while (n) {
        chain.push_back(n);
        if (n->tag_name == "svg") break;
        n = n->parent;
    }
    for (int i = (int)chain.size()-1; i >= 0; --i) {
        DOMNode* cur = chain[i];
        auto get = [&](const char* attr) -> std::string {
            auto it = cur->attributes.find(attr);
            if (it != cur->attributes.end()) return it->second;
            auto sit = cur->attributes.find("style");
            if (sit != cur->attributes.end()) {
                std::string v = prop_val(sit->second, attr);
                if (!v.empty()) return v;
            }
            return "";
        };
        std::string v;
        v = get("fill"); if (!v.empty()) style.fill = v;
        v = get("stroke"); if (!v.empty()) style.stroke = v;
        v = get("stroke-width"); if (!v.empty()) { try { style.stroke_width = std::stod(v); } catch (...) {} }
        v = get("opacity"); if (!v.empty()) { try { style.opacity = std::stod(v); } catch (...) {} }
        v = get("fill-opacity"); if (!v.empty()) { try { style.fill_opacity = std::stod(v); } catch (...) {} }
        v = get("stroke-opacity"); if (!v.empty()) { try { style.stroke_opacity = std::stod(v); } catch (...) {} }
        v = get("font-family"); if (!v.empty()) style.font_family = v;
        v = get("font-size"); if (!v.empty()) { try { style.font_size = std::stod(v); } catch (...) {} }
        v = get("text-anchor"); if (!v.empty()) style.text_anchor = v;
        v = get("font-weight"); if (!v.empty()) {
            if (v == "bold") style.font_weight = 700;
            else { try { style.font_weight = std::stoi(v); } catch (...) {} }
        }
    }
    return style;
}

static std::vector<SvgPathCmd> svg_parse_path_d(const std::string& d) {
    std::vector<SvgPathCmd> cmds;
    size_t i = 0, n = d.size();
    char cur_cmd = 'M';
    while (i < n) {
        while (i < n && (d[i]==' '||d[i]==','||d[i]=='\t'||d[i]=='\n'||d[i]=='\r')) ++i;
        if (i >= n) break;

        char c = d[i];
        if ((c>='A'&&c<='Z') || (c>='a'&&c<='z')) {
            cur_cmd = c; ++i;
        }
        if (cur_cmd == 'Z' || cur_cmd == 'z') {
            cmds.push_back({cur_cmd, {}});
            continue;
        }

        std::vector<double> args;
        while (i < n) {
            while (i < n && (d[i]==' '||d[i]==','||d[i]=='\t'||d[i]=='\n'||d[i]=='\r')) ++i;
            if (i >= n) break;
            char ch = d[i];
            if ((ch>='A'&&ch<='Z') || (ch>='a'&&ch<='z')) break;
            // Arc flag parsing: single digit 0/1 without separator
            if ((cur_cmd=='A'||cur_cmd=='a') && args.size() >= 3 &&
                (args.size() % 7 == 3 || args.size() % 7 == 4)) {
                if (ch == '0' || ch == '1') { args.push_back(ch - '0'); ++i; continue; }
            }
            args.push_back(svg_parse_double(d, i));
        }

        int nargs = 0;
        switch (cur_cmd) {
            case 'M': case 'm': case 'L': case 'l': case 'T': case 't': nargs=2; break;
            case 'H': case 'h': case 'V': case 'v': nargs=1; break;
            case 'C': case 'c': nargs=6; break;
            case 'S': case 's': case 'Q': case 'q': nargs=4; break;
            case 'A': case 'a': nargs=7; break;
        }
        if (nargs == 0) {
            cmds.push_back({cur_cmd, args});
        } else {
            for (size_t j = 0; j + (size_t)nargs <= args.size(); j += nargs) {
                char emit = cur_cmd;
                if (j > 0 && cur_cmd == 'M') emit = 'L';
                if (j > 0 && cur_cmd == 'm') emit = 'l';
                cmds.push_back({emit, std::vector<double>(args.begin()+j, args.begin()+j+nargs)});
            }
        }
    }
    return cmds;
}

static void svg_arc_to_cairo(cairo_t* cr, double x1, double y1,
                              double rx, double ry, double phi_deg,
                              int large_arc, int sweep, double x2, double y2) {
    if (rx == 0 || ry == 0) { cairo_line_to(cr, x2, y2); return; }
    rx = fabs(rx); ry = fabs(ry);
    double phi = phi_deg * M_PI / 180.0;
    double cp = cos(phi), sp = sin(phi);
    double dx = (x1-x2)/2.0, dy = (y1-y2)/2.0;
    double x1p = cp*dx + sp*dy, y1p = -sp*dx + cp*dy;
    double lam = (x1p*x1p)/(rx*rx) + (y1p*y1p)/(ry*ry);
    if (lam > 1) { double sl = sqrt(lam); rx *= sl; ry *= sl; }
    double num = rx*rx*ry*ry - rx*rx*y1p*y1p - ry*ry*x1p*x1p;
    double den = rx*rx*y1p*y1p + ry*ry*x1p*x1p;
    double sq = (den > 0) ? sqrt(fabs(num/den)) : 0;
    if (large_arc == sweep) sq = -sq;
    double cxp = sq*rx*y1p/ry, cyp = -sq*ry*x1p/rx;
    double cx = cp*cxp - sp*cyp + (x1+x2)/2.0;
    double cy = sp*cxp + cp*cyp + (y1+y2)/2.0;
    auto angle = [](double ux, double uy, double vx, double vy) -> double {
        double n = sqrt(ux*ux+uy*uy) * sqrt(vx*vx+vy*vy);
        if (n == 0) return 0;
        double c = (ux*vx+uy*vy)/n;
        c = std::max(-1.0, std::min(1.0, c));
        double a = acos(c);
        if (ux*vy - uy*vx < 0) a = -a;
        return a;
    };
    double theta1 = angle(1, 0, (x1p-cxp)/rx, (y1p-cyp)/ry);
    double dtheta = angle((x1p-cxp)/rx, (y1p-cyp)/ry, (-x1p-cxp)/rx, (-y1p-cyp)/ry);
    if (!sweep && dtheta > 0) dtheta -= 2*M_PI;
    if (sweep && dtheta < 0) dtheta += 2*M_PI;
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, phi);
    cairo_scale(cr, rx, ry);
    if (dtheta > 0) cairo_arc(cr, 0, 0, 1, theta1, theta1+dtheta);
    else            cairo_arc_negative(cr, 0, 0, 1, theta1, theta1+dtheta);
    cairo_restore(cr);
}

static void svg_render_path_cmds(cairo_t* cr, const std::vector<SvgPathCmd>& cmds) {
    double cx=0, cy=0, sx=0, sy=0, lcx=0, lcy=0;
    char last_cmd = 0;
    for (const auto& cmd : cmds) {
        switch (cmd.cmd) {
        case 'M': if (cmd.args.size()>=2) { cx=cmd.args[0]; cy=cmd.args[1]; cairo_move_to(cr,cx,cy); sx=cx; sy=cy; } break;
        case 'm': if (cmd.args.size()>=2) { cx+=cmd.args[0]; cy+=cmd.args[1]; cairo_move_to(cr,cx,cy); sx=cx; sy=cy; } break;
        case 'L': if (cmd.args.size()>=2) { cx=cmd.args[0]; cy=cmd.args[1]; cairo_line_to(cr,cx,cy); } break;
        case 'l': if (cmd.args.size()>=2) { cx+=cmd.args[0]; cy+=cmd.args[1]; cairo_line_to(cr,cx,cy); } break;
        case 'H': if (cmd.args.size()>=1) { cx=cmd.args[0]; cairo_line_to(cr,cx,cy); } break;
        case 'h': if (cmd.args.size()>=1) { cx+=cmd.args[0]; cairo_line_to(cr,cx,cy); } break;
        case 'V': if (cmd.args.size()>=1) { cy=cmd.args[0]; cairo_line_to(cr,cx,cy); } break;
        case 'v': if (cmd.args.size()>=1) { cy+=cmd.args[0]; cairo_line_to(cr,cx,cy); } break;
        case 'C': if (cmd.args.size()>=6) {
            lcx=cmd.args[2]; lcy=cmd.args[3];
            cairo_curve_to(cr, cmd.args[0],cmd.args[1], lcx,lcy, cmd.args[4],cmd.args[5]);
            cx=cmd.args[4]; cy=cmd.args[5];
        } break;
        case 'c': if (cmd.args.size()>=6) {
            double x1=cx+cmd.args[0],y1=cy+cmd.args[1]; lcx=cx+cmd.args[2]; lcy=cy+cmd.args[3];
            double x=cx+cmd.args[4],y=cy+cmd.args[5];
            cairo_curve_to(cr, x1,y1, lcx,lcy, x,y); cx=x; cy=y;
        } break;
        case 'S': if (cmd.args.size()>=4) {
            double rx1=cx,ry1=cy;
            if (last_cmd=='C'||last_cmd=='c'||last_cmd=='S'||last_cmd=='s') { rx1=2*cx-lcx; ry1=2*cy-lcy; }
            lcx=cmd.args[0]; lcy=cmd.args[1];
            cairo_curve_to(cr, rx1,ry1, lcx,lcy, cmd.args[2],cmd.args[3]);
            cx=cmd.args[2]; cy=cmd.args[3];
        } break;
        case 's': if (cmd.args.size()>=4) {
            double rx1=cx,ry1=cy;
            if (last_cmd=='C'||last_cmd=='c'||last_cmd=='S'||last_cmd=='s') { rx1=2*cx-lcx; ry1=2*cy-lcy; }
            lcx=cx+cmd.args[0]; lcy=cy+cmd.args[1];
            double x=cx+cmd.args[2],y=cy+cmd.args[3];
            cairo_curve_to(cr, rx1,ry1, lcx,lcy, x,y); cx=x; cy=y;
        } break;
        case 'Q': if (cmd.args.size()>=4) {
            lcx=cmd.args[0]; lcy=cmd.args[1];
            double cp1x=cx+2.0/3.0*(lcx-cx), cp1y=cy+2.0/3.0*(lcy-cy);
            double cp2x=cmd.args[2]+2.0/3.0*(lcx-cmd.args[2]), cp2y=cmd.args[3]+2.0/3.0*(lcy-cmd.args[3]);
            cairo_curve_to(cr, cp1x,cp1y, cp2x,cp2y, cmd.args[2],cmd.args[3]);
            cx=cmd.args[2]; cy=cmd.args[3];
        } break;
        case 'q': if (cmd.args.size()>=4) {
            lcx=cx+cmd.args[0]; lcy=cy+cmd.args[1];
            double x=cx+cmd.args[2],y=cy+cmd.args[3];
            double cp1x=cx+2.0/3.0*(lcx-cx), cp1y=cy+2.0/3.0*(lcy-cy);
            double cp2x=x+2.0/3.0*(lcx-x), cp2y=y+2.0/3.0*(lcy-y);
            cairo_curve_to(cr, cp1x,cp1y, cp2x,cp2y, x,y); cx=x; cy=y;
        } break;
        case 'T': if (cmd.args.size()>=2) {
            if (last_cmd=='Q'||last_cmd=='q'||last_cmd=='T'||last_cmd=='t') { lcx=2*cx-lcx; lcy=2*cy-lcy; }
            else { lcx=cx; lcy=cy; }
            double cp1x=cx+2.0/3.0*(lcx-cx), cp1y=cy+2.0/3.0*(lcy-cy);
            double cp2x=cmd.args[0]+2.0/3.0*(lcx-cmd.args[0]), cp2y=cmd.args[1]+2.0/3.0*(lcy-cmd.args[1]);
            cairo_curve_to(cr, cp1x,cp1y, cp2x,cp2y, cmd.args[0],cmd.args[1]);
            cx=cmd.args[0]; cy=cmd.args[1];
        } break;
        case 't': if (cmd.args.size()>=2) {
            if (last_cmd=='Q'||last_cmd=='q'||last_cmd=='T'||last_cmd=='t') { lcx=2*cx-lcx; lcy=2*cy-lcy; }
            else { lcx=cx; lcy=cy; }
            double x=cx+cmd.args[0],y=cy+cmd.args[1];
            double cp1x=cx+2.0/3.0*(lcx-cx), cp1y=cy+2.0/3.0*(lcy-cy);
            double cp2x=x+2.0/3.0*(lcx-x), cp2y=y+2.0/3.0*(lcy-y);
            cairo_curve_to(cr, cp1x,cp1y, cp2x,cp2y, x,y); cx=x; cy=y;
        } break;
        case 'A': if (cmd.args.size()>=7) {
            svg_arc_to_cairo(cr, cx,cy, cmd.args[0],cmd.args[1], cmd.args[2],
                             (int)cmd.args[3],(int)cmd.args[4], cmd.args[5],cmd.args[6]);
            cx=cmd.args[5]; cy=cmd.args[6];
        } break;
        case 'a': if (cmd.args.size()>=7) {
            double x=cx+cmd.args[5],y=cy+cmd.args[6];
            svg_arc_to_cairo(cr, cx,cy, cmd.args[0],cmd.args[1], cmd.args[2],
                             (int)cmd.args[3],(int)cmd.args[4], x,y);
            cx=x; cy=y;
        } break;
        case 'Z': case 'z': cairo_close_path(cr); cx=sx; cy=sy; break;
        }
        last_cmd = cmd.cmd;
    }
}

static void svg_apply_style(cairo_t* cr, const SvgStyle& style) {
    double fr, fg, fb, fa;
    bool has_fill = (style.fill != "none" && svg_parse_color(style.fill, fr, fg, fb, fa));
    double sr, sg, sb, sa;
    bool has_stroke = (!style.stroke.empty() && style.stroke != "none" &&
                       svg_parse_color(style.stroke, sr, sg, sb, sa));
    if (has_fill) {
        cairo_set_source_rgba(cr, fr, fg, fb, fa * style.fill_opacity * style.opacity);
        if (has_stroke) cairo_fill_preserve(cr);
        else cairo_fill(cr);
    }
    if (has_stroke) {
        cairo_set_source_rgba(cr, sr, sg, sb, sa * style.stroke_opacity * style.opacity);
        cairo_set_line_width(cr, style.stroke_width);
        cairo_stroke(cr);
    }
    if (!has_fill && !has_stroke) cairo_new_path(cr);
}

static void svg_render_element(cairo_t* cr, DOMNode* node) {
    if (node->node_type != DOMNode::ELEMENT) return;
    const std::string& tag = node->tag_name;
    if (tag=="defs"||tag=="title"||tag=="desc"||tag=="metadata"||tag=="style") return;

    SvgStyle style = svg_resolve_style(node);

    auto tr_it = node->attributes.find("transform");
    bool has_transform = (tr_it != node->attributes.end() && !tr_it->second.empty());
    if (has_transform) {
        cairo_save(cr);
        SvgTransform t = svg_parse_transform(tr_it->second);
        cairo_matrix_t m = t.as_matrix();
        cairo_transform(cr, &m);
    }

    auto ga = [&](const char* name) -> double {
        auto it = node->attributes.find(name);
        if (it == node->attributes.end()) return 0;
        try { return std::stod(it->second); } catch (...) { return 0; }
    };

    if (tag == "g") {
        for (auto& child : node->children)
            svg_render_element(cr, child.get());
    } else if (tag == "rect") {
        double x=ga("x"), y=ga("y"), w=ga("width"), h=ga("height");
        double rx=ga("rx"), ry=ga("ry");
        if (rx==0 && ry>0) rx=ry; if (ry==0 && rx>0) ry=rx;
        if (rx > w/2) rx = w/2; if (ry > h/2) ry = h/2;
        if (rx > 0 && ry > 0) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, x+w-rx, y+ry,   rx, -M_PI/2, 0);
            cairo_arc(cr, x+w-rx, y+h-ry, rx, 0,        M_PI/2);
            cairo_arc(cr, x+rx,   y+h-ry, rx, M_PI/2,   M_PI);
            cairo_arc(cr, x+rx,   y+ry,   rx, M_PI,      3*M_PI/2);
            cairo_close_path(cr);
        } else {
            cairo_rectangle(cr, x, y, w, h);
        }
        svg_apply_style(cr, style);
    } else if (tag == "circle") {
        double ccx=ga("cx"), ccy=ga("cy"), r=ga("r");
        if (r > 0) { cairo_arc(cr, ccx, ccy, r, 0, 2*M_PI); svg_apply_style(cr, style); }
    } else if (tag == "ellipse") {
        double ecx=ga("cx"), ecy=ga("cy"), erx=ga("rx"), ery=ga("ry");
        if (erx > 0 && ery > 0) {
            cairo_save(cr);
            cairo_translate(cr, ecx, ecy);
            cairo_scale(cr, erx, ery);
            cairo_arc(cr, 0, 0, 1, 0, 2*M_PI);
            cairo_restore(cr);
            svg_apply_style(cr, style);
        }
    } else if (tag == "line") {
        cairo_move_to(cr, ga("x1"), ga("y1"));
        cairo_line_to(cr, ga("x2"), ga("y2"));
        double sr, sg, sb, sa;
        if (!style.stroke.empty() && style.stroke != "none" &&
            svg_parse_color(style.stroke, sr, sg, sb, sa)) {
            cairo_set_source_rgba(cr, sr, sg, sb, sa * style.stroke_opacity * style.opacity);
            cairo_set_line_width(cr, style.stroke_width);
            cairo_stroke(cr);
        } else cairo_new_path(cr);
    } else if (tag == "polyline" || tag == "polygon") {
        auto pts_it = node->attributes.find("points");
        if (pts_it != node->attributes.end()) {
            std::vector<double> pts;
            size_t pos = 0;
            while (pos < pts_it->second.size())
                pts.push_back(svg_parse_double(pts_it->second, pos));
            if (pts.size() >= 4) {
                cairo_move_to(cr, pts[0], pts[1]);
                for (size_t j = 2; j+1 < pts.size(); j += 2)
                    cairo_line_to(cr, pts[j], pts[j+1]);
                if (tag == "polygon") cairo_close_path(cr);
                svg_apply_style(cr, style);
            }
        }
    } else if (tag == "path") {
        auto d_it = node->attributes.find("d");
        if (d_it != node->attributes.end()) {
            auto cmds = svg_parse_path_d(d_it->second);
            svg_render_path_cmds(cr, cmds);
            svg_apply_style(cr, style);
        }
    } else if (tag == "text") {
        std::string text = node->getTextContent();
        if (!text.empty()) {
            double tx=ga("x"), ty=ga("y");
            cairo_select_font_face(cr, style.font_family.c_str(),
                CAIRO_FONT_SLANT_NORMAL,
                style.font_weight >= 700 ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, style.font_size);
            if (style.text_anchor == "middle" || style.text_anchor == "end") {
                cairo_text_extents_t ext;
                cairo_text_extents(cr, text.c_str(), &ext);
                if (style.text_anchor == "middle") tx -= ext.width / 2.0;
                else tx -= ext.width;
            }
            cairo_move_to(cr, tx, ty);
            double fr, fg, fb, fa;
            if (style.fill != "none" && svg_parse_color(style.fill, fr, fg, fb, fa)) {
                cairo_set_source_rgba(cr, fr, fg, fb, fa * style.fill_opacity * style.opacity);
                cairo_show_text(cr, text.c_str());
            }
        }
    }

    if (has_transform) cairo_restore(cr);
}

static gboolean draw_svg(GtkWidget* w, cairo_t* cr, gpointer data) {
    DOMNode* svg_node = static_cast<DOMNode*>(data);
    if (!svg_node) return FALSE;

    int widget_w = gtk_widget_get_allocated_width(w);
    int widget_h = gtk_widget_get_allocated_height(w);

    SvgViewBox vb;
    auto vb_it = svg_node->attributes.find("viewbox");
    if (vb_it != svg_node->attributes.end())
        vb = svg_parse_viewbox(vb_it->second);

    cairo_save(cr);
    if (vb.valid) {
        double sx = (double)widget_w / vb.width;
        double sy = (double)widget_h / vb.height;
        double scale = std::min(sx, sy);
        double tx = (widget_w - vb.width * scale) / 2.0;
        double ty = (widget_h - vb.height * scale) / 2.0;
        cairo_translate(cr, tx, ty);
        cairo_scale(cr, scale, scale);
        cairo_translate(cr, -vb.min_x, -vb.min_y);
    }

    for (auto& child : svg_node->children)
        svg_render_element(cr, child.get());

    cairo_restore(cr);
    return FALSE;
}

// ---- page fetch ----

static void navigate(AppState* st, const std::string& raw); // forward decl

static gboolean draw_bg(GtkWidget* w, cairo_t* cr, gpointer) {
    GdkPixbuf* pb    = (GdkPixbuf*)g_object_get_data(G_OBJECT(w), "bg_pb");
    const char* bgc  = (const char*)g_object_get_data(G_OBJECT(w), "bg_color_str");
    const char* rep  = (const char*)g_object_get_data(G_OBJECT(w), "bg_repeat");
    const char* bsz  = (const char*)g_object_get_data(G_OBJECT(w), "bg_size");
    const char* bpos = (const char*)g_object_get_data(G_OBJECT(w), "bg_position");
    {
        FILE* f = fopen("/tmp/browser_debug.log","a");
        if (f) { fprintf(f,"draw_bg called: pb=%s bgc=%s rep=%s bsz=%s bpos=%s\n",
                 pb?"ok":"null", bgc?bgc:"none", rep?rep:"(default)", bsz?bsz:"(default)", bpos?bpos:"(default)"); fclose(f); }
    }
    // fill background color first (so it shows through transparent parts of image)
    if (bgc && bgc[0]) {
        GdkRGBA rgba = {1,1,1,1};
        gdk_rgba_parse(&rgba, bgc);
        cairo_set_source_rgba(cr, rgba.red, rgba.green, rgba.blue, rgba.alpha);
        cairo_paint(cr);
    }
    if (!pb) return FALSE;
    int pw = gdk_pixbuf_get_width(pb);
    int ph = gdk_pixbuf_get_height(pb);
    if (pw<=0||ph<=0) return FALSE;

    bool no_repeat = rep && (strstr(rep, "no-repeat") != nullptr);
    bool is_contain = bsz && (strcmp(bsz, "contain") == 0);
    bool is_cover   = bsz && (strcmp(bsz, "cover") == 0);

    // Parse background-position: "Xpx Ypx" or "X% Y%" or keywords
    double pos_x = 0.0, pos_y = 0.0;
    if (bpos && bpos[0]) {
        int ww = gtk_widget_get_allocated_width(w);
        int wh = gtk_widget_get_allocated_height(w);
        // Try parsing "Xpx Ypx" or "X Y"
        double vx = 0, vy = 0;
        char* end = nullptr;
        vx = strtod(bpos, &end);
        if (end && end != bpos) {
            // skip "px" or whitespace
            while (*end && (*end == 'p' || *end == 'x' || *end == ' ' || *end == ',')) end++;
            if (*end) {
                char* end2 = nullptr;
                vy = strtod(end, &end2);
            }
            // Check for percentage
            const char* pct = strchr(bpos, '%');
            if (pct) {
                pos_x = vx / 100.0 * (ww - pw);
                // find second value
                const char* sp = strchr(bpos, ' ');
                if (sp) {
                    double vy2 = strtod(sp + 1, nullptr);
                    pos_y = vy2 / 100.0 * (wh - ph);
                }
            } else {
                pos_x = vx;
                pos_y = vy;
            }
        } else {
            // keyword parsing
            std::string p(bpos);
            if (p.find("center") != std::string::npos) { pos_x = (ww - pw) / 2.0; pos_y = (wh - ph) / 2.0; }
            if (p.find("right") != std::string::npos) pos_x = ww - pw;
            if (p.find("bottom") != std::string::npos) pos_y = wh - ph;
        }
    }

    cairo_save(cr);

    if (is_contain || is_cover) {
        int ww = gtk_widget_get_allocated_width(w);
        int wh = gtk_widget_get_allocated_height(w);
        if (ww <= 0 || wh <= 0) { cairo_restore(cr); return FALSE; }
        double sx = (double)ww / pw;
        double sy = (double)wh / ph;
        double scale = is_contain ? std::min(sx, sy) : std::max(sx, sy);
        double dw = pw * scale;
        double dh = ph * scale;
        double dx = (ww - dw) / 2.0 + pos_x;
        double dy = (wh - dh) / 2.0 + pos_y;
        GdkPixbuf* scaled = gdk_pixbuf_scale_simple(pb, (int)dw, (int)dh, GDK_INTERP_BILINEAR);
        if (scaled) {
            gdk_cairo_set_source_pixbuf(cr, scaled, dx, dy);
            cairo_paint(cr);
            g_object_unref(scaled);
        }
    } else if (no_repeat) {
        gdk_cairo_set_source_pixbuf(cr, pb, pos_x, pos_y);
        cairo_paint(cr);
    } else {
        // Tile with position offset
        cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
        cairo_t* tc = cairo_create(surf);
        gdk_cairo_set_source_pixbuf(tc, pb, 0, 0);
        cairo_paint(tc);
        cairo_destroy(tc);
        cairo_pattern_t* pat = cairo_pattern_create_for_surface(surf);
        cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
        cairo_matrix_t mat;
        cairo_matrix_init_translate(&mat, -pos_x, -pos_y);
        cairo_pattern_set_matrix(pat, &mat);
        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);
        cairo_surface_destroy(surf);
    }

    cairo_restore(cr);
    return FALSE;
}

// ---- Inspector panel helpers ----

static void inspector_update_elements(AppState* st) {
    auto* tab = st->ct;
    if (!tab || !tab->elements_text_view) return;
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->elements_text_view));
    gtk_text_buffer_set_text(buf, tab->page_source.c_str(), (gint)tab->page_source.size());
}

static void inspector_append_console_entry(AppState* st, const ConsoleEntry& entry) {
    auto* tab = st->ct;
    if (!tab || !tab->console_text_view) return;
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->console_text_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);

    // Pick tag based on level
    const char* tag_name = nullptr;
    const char* prefix = "";
    switch (entry.level) {
        case ConsoleLevel::ERROR: tag_name = "error"; prefix = "\u2718 "; break;
        case ConsoleLevel::WARN:  tag_name = "warn";  prefix = "\u26A0 "; break;
        case ConsoleLevel::INFO:  tag_name = "info";  prefix = "\u2139 "; break;
        case ConsoleLevel::LOG:   tag_name = "log";   break;
    }

    // Build the line
    std::string line;
    if (prefix[0]) line += prefix;
    line += entry.message;
    if (!entry.source.empty()) line += "  (" + entry.source + ")";
    line += "\n";

    if (tag_name) {
        gtk_text_buffer_insert_with_tags_by_name(buf, &end, line.c_str(), -1, tag_name, nullptr);
    } else {
        gtk_text_buffer_insert(buf, &end, line.c_str(), -1);
    }

    // Auto-scroll to bottom
    gtk_text_buffer_get_end_iter(buf, &end);
    GtkTextMark* mark = gtk_text_buffer_get_mark(buf, "end_mark");
    if (!mark) {
        mark = gtk_text_buffer_create_mark(buf, "end_mark", &end, FALSE);
    } else {
        gtk_text_buffer_move_mark(buf, mark, &end);
    }
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(tab->console_text_view), mark);
}

static void inspector_refresh_console(AppState* st) {
    auto* tab = st->ct;
    if (!tab || !tab->console_text_view) return;
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->console_text_view));
    gtk_text_buffer_set_text(buf, "", 0);
    if (tab->js_engine) {
        for (const auto& entry : tab->js_engine->console_log) {
            inspector_append_console_entry(st, entry);
        }
    }
}

static void inspector_create_panel(AppState* st) {
    auto* tab = st->ct;
    // Create the inspector container
    tab->inspector_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(tab->inspector_box, 200, -1); // minimum width

    // Apply dark background style
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "notebook, notebook tab, notebook header { background: #1e1e1e; color: #ccc; }"
        "notebook tab:checked { background: #2d2d2d; color: #fff; }"
        "textview, textview text { background-color: #1e1e1e; color: #d4d4d4; "
        "  font-family: monospace; font-size: 9pt; }", -1, nullptr);
    gtk_style_context_add_provider(gtk_widget_get_style_context(tab->inspector_box),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Create notebook (tabs)
    tab->inspector_notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(tab->inspector_notebook), GTK_POS_TOP);
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(tab->inspector_notebook),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // ---- Console tab ----
    GtkWidget* console_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(console_scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    tab->console_text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tab->console_text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tab->console_text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tab->console_text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tab->console_text_view), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tab->console_text_view), 4);

    // Apply style to console text view
    gtk_style_context_add_provider(gtk_widget_get_style_context(tab->console_text_view),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Create text tags for different log levels
    GtkTextBuffer* cbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->console_text_view));
    gtk_text_buffer_create_tag(cbuf, "error", "foreground", "#f44747", nullptr);
    gtk_text_buffer_create_tag(cbuf, "warn", "foreground", "#cca700", nullptr);
    gtk_text_buffer_create_tag(cbuf, "info", "foreground", "#3794ff", nullptr);
    gtk_text_buffer_create_tag(cbuf, "log", "foreground", "#d4d4d4", nullptr);

    gtk_container_add(GTK_CONTAINER(console_scroll), tab->console_text_view);
    gtk_notebook_append_page(GTK_NOTEBOOK(tab->inspector_notebook),
        console_scroll, gtk_label_new("Console"));

    // ---- Elements tab ----
    GtkWidget* elements_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(elements_scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    tab->elements_text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tab->elements_text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tab->elements_text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tab->elements_text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tab->elements_text_view), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tab->elements_text_view), 4);

    // Apply style to elements text view
    gtk_style_context_add_provider(gtk_widget_get_style_context(tab->elements_text_view),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    gtk_container_add(GTK_CONTAINER(elements_scroll), tab->elements_text_view);
    gtk_notebook_append_page(GTK_NOTEBOOK(tab->inspector_notebook),
        elements_scroll, gtk_label_new("Elements"));

    g_object_unref(css);

    gtk_box_pack_start(GTK_BOX(tab->inspector_box), tab->inspector_notebook, TRUE, TRUE, 0);
}

static void inspector_show(AppState* st) {
    auto* tab = st->ct;
    if (!tab) return;
    if (tab->inspector_visible) return;
    if (!tab->inspector_box) inspector_create_panel(st);

    // Add inspector to right side of paned
    gtk_paned_pack2(GTK_PANED(tab->paned), tab->inspector_box, FALSE, FALSE);

    // Set the divider position (window width - inspector width)
    int win_w;
    gtk_window_get_size(GTK_WINDOW(st->window), &win_w, nullptr);
    gtk_paned_set_position(GTK_PANED(tab->paned), win_w - tab->inspector_width);

    gtk_widget_show_all(tab->inspector_box);
    tab->inspector_visible = true;

    // Populate content
    inspector_update_elements(st);
    inspector_refresh_console(st);

    // Wire up console entry callback
    if (tab->js_engine) {
        tab->js_engine->on_console_entry = [st]() {
            auto* tab = st->ct;
            if (tab && tab->js_engine && !tab->js_engine->console_log.empty()) {
                inspector_append_console_entry(st, tab->js_engine->console_log.back());
            }
        };
    }
}

static void inspector_hide(AppState* st) {
    auto* tab = st->ct;
    if (!tab || !tab->inspector_visible) return;

    // Save current width
    int win_w;
    gtk_window_get_size(GTK_WINDOW(st->window), &win_w, nullptr);
    int pos = gtk_paned_get_position(GTK_PANED(tab->paned));
    tab->inspector_width = win_w - pos;
    if (tab->inspector_width < 200) tab->inspector_width = 200;

    // Remove from paned (but don't destroy)
    g_object_ref(tab->inspector_box);
    gtk_container_remove(GTK_CONTAINER(tab->paned), tab->inspector_box);

    // Disconnect console callback
    if (tab->js_engine) tab->js_engine->on_console_entry = nullptr;

    tab->inspector_visible = false;
}

static void inspector_toggle(AppState* st) {
    auto* tab = st->ct;
    if (!tab) return;
    if (tab->inspector_visible) inspector_hide(st);
    else inspector_show(st);
}

static void on_inspect(GtkMenuItem*, gpointer d) {
    auto* st = static_cast<AppState*>(d);
    inspector_toggle(st);
}
static gboolean on_content_click(GtkWidget*, GdkEventButton* ev, gpointer d) {
    if (ev->button!=3 || ev->type!=GDK_BUTTON_PRESS) return FALSE;
    auto* st = static_cast<AppState*>(d);
    GtkWidget* menu = gtk_menu_new();
    GtkWidget* item = gtk_menu_item_new_with_label("Inspect");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    g_signal_connect(item, "activate", G_CALLBACK(on_inspect), st);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)ev);
    return TRUE;
}

// ---- Forward declarations for tab lifecycle ----
static void navigate(AppState* st, const std::string& raw);
static void load_url(AppState* st, const std::string& url);

// ---- Tab bar constants ----
static const int TAB_BAR_HEIGHT = 38;
static const int TAB_MIN_WIDTH = 60;
static const int TAB_MAX_WIDTH = 240;
static const int TAB_CLOSE_SIZE = 14;
static const int TAB_PADDING = 8;
static const int NEW_TAB_BTN_WIDTH = 28;
static const int WINDOW_CTRL_WIDTH = 46;
static const int WINDOW_CTRL_COUNT = 3; // minimize, maximize, close

static int compute_tab_width(int n_tabs, int bar_width) {
    int avail = bar_width - NEW_TAB_BTN_WIDTH - WINDOW_CTRL_WIDTH * WINDOW_CTRL_COUNT - 8;
    if (n_tabs <= 0) return TAB_MAX_WIDTH;
    int w = avail / n_tabs;
    if (w < TAB_MIN_WIDTH) w = TAB_MIN_WIDTH;
    if (w > TAB_MAX_WIDTH) w = TAB_MAX_WIDTH;
    return w;
}

// ---- Tab bar drawing (Cairo) ----

static gboolean draw_tab_bar(GtkWidget* widget, cairo_t* cr, gpointer data) {
    auto* st = static_cast<AppState*>(data);
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    int n = (int)st->tabs.size();
    int tw = compute_tab_width(n, width);

    // Background
    cairo_set_source_rgb(cr, 0.22, 0.22, 0.22); // #383838
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    // Draw each tab
    for (int i = 0; i < n; i++) {
        double x = i * tw;
        bool active = (i == st->active_tab_idx);
        bool hover = (i == st->tab_bar_hover);

        // Tab background
        if (active)
            cairo_set_source_rgb(cr, 0.30, 0.30, 0.30); // #4d4d4d
        else if (hover)
            cairo_set_source_rgb(cr, 0.26, 0.26, 0.26); // #424242
        else
            cairo_set_source_rgb(cr, 0.22, 0.22, 0.22); // #383838

        // Rounded top corners
        double r = 6;
        double y0 = active ? 2 : 4;
        double y1 = height;
        cairo_new_path(cr);
        cairo_move_to(cr, x, y1);
        cairo_line_to(cr, x, y0 + r);
        cairo_arc(cr, x + r, y0 + r, r, M_PI, 1.5 * M_PI);
        cairo_line_to(cr, x + tw - r, y0);
        cairo_arc(cr, x + tw - r, y0 + r, r, 1.5 * M_PI, 2 * M_PI);
        cairo_line_to(cr, x + tw, y1);
        cairo_close_path(cr);
        cairo_fill(cr);

        // Active tab bottom highlight
        if (active) {
            cairo_set_source_rgb(cr, 0.40, 0.40, 0.40);
            cairo_rectangle(cr, x + 1, height - 2, tw - 2, 2);
            cairo_fill(cr);
        }

        // Tab title
        cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12);

        std::string title = st->tabs[i]->title;
        int max_chars = (tw - TAB_PADDING * 2 - TAB_CLOSE_SIZE - 8) / 7;
        if (max_chars < 3) max_chars = 3;
        if ((int)title.size() > max_chars) title = title.substr(0, max_chars - 1) + "\xe2\x80\xa6";

        cairo_move_to(cr, x + TAB_PADDING, y0 + (height - y0) / 2 + 4);
        cairo_show_text(cr, title.c_str());

        // Close button (X)
        double cx = x + tw - TAB_PADDING - TAB_CLOSE_SIZE / 2;
        double cy = y0 + (height - y0) / 2;
        bool close_hover = (i == st->tab_bar_close_hover);

        if (close_hover) {
            cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
            cairo_arc(cr, cx, cy, TAB_CLOSE_SIZE / 2 + 2, 0, 2 * M_PI);
            cairo_fill(cr);
        }

        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_set_line_width(cr, 1.5);
        double cs = 4;
        cairo_move_to(cr, cx - cs, cy - cs);
        cairo_line_to(cr, cx + cs, cy + cs);
        cairo_move_to(cr, cx + cs, cy - cs);
        cairo_line_to(cr, cx - cs, cy + cs);
        cairo_stroke(cr);
    }

    // New tab (+) button
    double nx = n * tw + 4;
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_set_font_size(cr, 18);
    cairo_move_to(cr, nx + 6, height / 2 + 6);
    cairo_show_text(cr, "+");

    // Window controls (minimize, maximize, close) at right
    double wc_x = width - WINDOW_CTRL_WIDTH * WINDOW_CTRL_COUNT;
    const char* ctrl_labels[] = {"\xe2\x80\x93", "\xe2\x96\xa1", "\xc3\x97"}; // –, □, ×
    for (int i = 0; i < WINDOW_CTRL_COUNT; i++) {
        double bx = wc_x + i * WINDOW_CTRL_WIDTH;
        // Hover highlight
        if (i == 2) { // Close button
            cairo_set_source_rgb(cr, 0.90, 0.18, 0.18);
        } else {
            cairo_set_source_rgb(cr, 0.22, 0.22, 0.22);
        }
        // Only highlight on hover would need more state, keep simple
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, bx + WINDOW_CTRL_WIDTH / 2 - 4, height / 2 + 5);
        cairo_show_text(cr, ctrl_labels[i]);
    }

    return FALSE;
}

// ---- Tab bar hit testing ----

enum TabBarHit { HIT_NONE, HIT_TAB, HIT_CLOSE, HIT_NEW, HIT_MINIMIZE, HIT_MAXIMIZE, HIT_CLOSE_WINDOW, HIT_EMPTY };

struct TabBarHitResult {
    TabBarHit type = HIT_NONE;
    int tab_idx = -1;
};

static TabBarHitResult hit_test_tab_bar(AppState* st, double x, double y) {
    int width = gtk_widget_get_allocated_width(st->tab_bar_area);
    int n = (int)st->tabs.size();
    int tw = compute_tab_width(n, width);
    TabBarHitResult r;

    // Window controls
    double wc_x = width - WINDOW_CTRL_WIDTH * WINDOW_CTRL_COUNT;
    if (x >= wc_x) {
        int ctrl = (int)((x - wc_x) / WINDOW_CTRL_WIDTH);
        if (ctrl == 0) { r.type = HIT_MINIMIZE; return r; }
        if (ctrl == 1) { r.type = HIT_MAXIMIZE; return r; }
        if (ctrl == 2) { r.type = HIT_CLOSE_WINDOW; return r; }
    }

    // New tab button
    double nx = n * tw + 4;
    if (x >= nx && x < nx + NEW_TAB_BTN_WIDTH) { r.type = HIT_NEW; return r; }

    // Tab area
    if (x < n * tw) {
        int idx = (int)(x / tw);
        if (idx >= 0 && idx < n) {
            // Check close button
            double cx = idx * tw + tw - TAB_PADDING - TAB_CLOSE_SIZE / 2;
            double cy = TAB_BAR_HEIGHT / 2;
            if (fabs(x - cx) < TAB_CLOSE_SIZE && fabs(y - cy) < TAB_CLOSE_SIZE) {
                r.type = HIT_CLOSE;
                r.tab_idx = idx;
                return r;
            }
            r.type = HIT_TAB;
            r.tab_idx = idx;
            return r;
        }
    }

    r.type = HIT_EMPTY;
    return r;
}

// ---- Tab lifecycle ----

static void create_tab_widgets(AppState* st, std::shared_ptr<TabState> tab) {
    // Horizontal paned: left = scroll+content, right = inspector
    tab->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    tab->scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab->scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_paned_pack1(GTK_PANED(tab->paned), tab->scroll, TRUE, FALSE);

    tab->viewport = gtk_viewport_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(tab->scroll), tab->viewport);
    gtk_widget_add_events(tab->viewport, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(tab->viewport, "button-press-event", G_CALLBACK(on_content_click), st);

    tab->content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(tab->content_box, TRUE);
    gtk_widget_set_vexpand(tab->content_box, TRUE);
    gtk_container_add(GTK_CONTAINER(tab->viewport), tab->content_box);

    // Add to content stack
    gtk_stack_add_named(GTK_STACK(st->content_stack), tab->paned,
        ("tab_" + std::to_string((uintptr_t)tab.get())).c_str());
    gtk_widget_show_all(tab->paned);
}

static void update_nav_buttons(AppState* st);

static void switch_to_tab(AppState* st, int idx) {
    if (idx < 0 || idx >= (int)st->tabs.size()) return;
    st->active_tab_idx = idx;
    st->sync_ct();
    auto* tab = st->ct;

    // Switch visible child in stack
    gtk_stack_set_visible_child(GTK_STACK(st->content_stack), tab->paned);

    // Update address bar and nav buttons
    gtk_entry_set_text(GTK_ENTRY(st->address_bar), tab->current_url.c_str());
    update_nav_buttons(st);
    gtk_window_set_title(GTK_WINDOW(st->window), tab->title.c_str());

    // Swap global JS engine pointer
    extern JSEngine* g_js_engine;
    g_js_engine = tab->js_engine;

    // Redraw tab bar
    if (st->tab_bar_area) gtk_widget_queue_draw(st->tab_bar_area);
}

static void new_tab(AppState* st, const std::string& url = "file:///mnt/1tb-ssd/random/browser/api_test.html") {
    auto tab = std::make_shared<TabState>();
    st->tabs.push_back(tab);
    create_tab_widgets(st, tab);
    st->active_tab_idx = (int)st->tabs.size() - 1;
    st->sync_ct();

    if (st->tab_bar_area) gtk_widget_queue_draw(st->tab_bar_area);
    switch_to_tab(st, st->active_tab_idx);
    navigate(st, url);
}

static void close_tab(AppState* st, int idx) {
    if (idx < 0 || idx >= (int)st->tabs.size()) return;
    if (st->tabs.size() <= 1) {
        // Last tab: close window
        gtk_main_quit();
        return;
    }

    auto& tab = st->tabs[idx];

    // Save to closed tabs
    st->closed_tabs.push_back({tab->current_url, tab->back_stack, tab->fwd_stack});

    // Destroy JS engine
    if (tab->js_engine) {
        extern JSEngine* g_js_engine;
        if (g_js_engine == tab->js_engine) g_js_engine = nullptr;
        delete tab->js_engine;
        tab->js_engine = nullptr;
    }

    // Remove widget from stack
    gtk_widget_destroy(tab->paned);
    tab->paned = nullptr;

    // Remove from vector
    st->tabs.erase(st->tabs.begin() + idx);

    // Adjust active tab
    if (st->active_tab_idx >= (int)st->tabs.size())
        st->active_tab_idx = (int)st->tabs.size() - 1;
    if (st->active_tab_idx < 0) st->active_tab_idx = 0;

    st->sync_ct();
    switch_to_tab(st, st->active_tab_idx);
}

static void reopen_closed_tab(AppState* st) {
    if (st->closed_tabs.empty()) return;
    auto info = st->closed_tabs.back();
    st->closed_tabs.pop_back();

    auto tab = std::make_shared<TabState>();
    tab->back_stack = info.back_stack;
    tab->fwd_stack = info.fwd_stack;
    st->tabs.push_back(tab);
    create_tab_widgets(st, tab);
    st->active_tab_idx = (int)st->tabs.size() - 1;
    st->sync_ct();

    if (st->tab_bar_area) gtk_widget_queue_draw(st->tab_bar_area);
    switch_to_tab(st, st->active_tab_idx);
    navigate(st, info.url);
}

// ---- Tab bar event handlers ----

static gboolean tab_bar_press(GtkWidget*, GdkEventButton* ev, gpointer data) {
    auto* st = static_cast<AppState*>(data);
    auto hit = hit_test_tab_bar(st, ev->x, ev->y);

    if (ev->button == 1) {
        switch (hit.type) {
            case HIT_TAB:
                st->tab_bar_dragging = true;
                st->tab_bar_drag_idx = hit.tab_idx;
                st->tab_bar_drag_x = ev->x;
                st->tab_bar_drag_start_x = ev->x;
                switch_to_tab(st, hit.tab_idx);
                break;
            case HIT_CLOSE:
                close_tab(st, hit.tab_idx);
                break;
            case HIT_NEW:
                new_tab(st);
                break;
            case HIT_MINIMIZE:
                gtk_window_iconify(GTK_WINDOW(st->window));
                break;
            case HIT_MAXIMIZE:
                if (gtk_window_is_maximized(GTK_WINDOW(st->window)))
                    gtk_window_unmaximize(GTK_WINDOW(st->window));
                else
                    gtk_window_maximize(GTK_WINDOW(st->window));
                break;
            case HIT_CLOSE_WINDOW:
                gtk_main_quit();
                break;
            case HIT_EMPTY:
                // Start window drag
                gtk_window_begin_move_drag(GTK_WINDOW(st->window),
                    ev->button, (int)ev->x_root, (int)ev->y_root, ev->time);
                break;
            default: break;
        }
    } else if (ev->button == 2) {
        // Middle click: close tab
        if (hit.type == HIT_TAB) close_tab(st, hit.tab_idx);
    } else if (ev->button == 3) {
        // Right click: context menu
        if (hit.type == HIT_TAB || hit.type == HIT_EMPTY) {
            GtkWidget* menu = gtk_menu_new();
            GtkWidget* item_new = gtk_menu_item_new_with_label("New Tab");
            g_signal_connect(item_new, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer d) {
                new_tab(static_cast<AppState*>(d));
            }), st);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_new);

            if (!st->closed_tabs.empty()) {
                GtkWidget* item_reopen = gtk_menu_item_new_with_label("Reopen Closed Tab");
                g_signal_connect(item_reopen, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer d) {
                    reopen_closed_tab(static_cast<AppState*>(d));
                }), st);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_reopen);
            }
            gtk_widget_show_all(menu);
            gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)ev);
        }
    }
    return TRUE;
}

static gboolean tab_bar_motion(GtkWidget*, GdkEventMotion* ev, gpointer data) {
    auto* st = static_cast<AppState*>(data);

    if (st->tab_bar_dragging && st->tab_bar_drag_idx >= 0) {
        // Drag to reorder
        int width = gtk_widget_get_allocated_width(st->tab_bar_area);
        int n = (int)st->tabs.size();
        int tw = compute_tab_width(n, width);
        double delta = ev->x - st->tab_bar_drag_x;

        if (fabs(delta) > tw / 2) {
            int new_idx = st->tab_bar_drag_idx + (delta > 0 ? 1 : -1);
            if (new_idx >= 0 && new_idx < n) {
                std::swap(st->tabs[st->tab_bar_drag_idx], st->tabs[new_idx]);
                st->active_tab_idx = new_idx;
                st->tab_bar_drag_idx = new_idx;
                st->tab_bar_drag_x = ev->x;
                st->sync_ct();
            }
        }
        gtk_widget_queue_draw(st->tab_bar_area);
        return TRUE;
    }

    // Hover tracking
    auto hit = hit_test_tab_bar(st, ev->x, ev->y);
    int new_hover = (hit.type == HIT_TAB || hit.type == HIT_CLOSE) ? hit.tab_idx : -1;
    int new_close = (hit.type == HIT_CLOSE) ? hit.tab_idx : -1;
    if (new_hover != st->tab_bar_hover || new_close != st->tab_bar_close_hover) {
        st->tab_bar_hover = new_hover;
        st->tab_bar_close_hover = new_close;
        gtk_widget_queue_draw(st->tab_bar_area);
    }
    return TRUE;
}

static gboolean tab_bar_release(GtkWidget*, GdkEventButton*, gpointer data) {
    auto* st = static_cast<AppState*>(data);
    st->tab_bar_dragging = false;
    st->tab_bar_drag_idx = -1;
    return TRUE;
}

static gboolean tab_bar_leave(GtkWidget*, GdkEventCrossing*, gpointer data) {
    auto* st = static_cast<AppState*>(data);
    st->tab_bar_hover = -1;
    st->tab_bar_close_hover = -1;
    gtk_widget_queue_draw(st->tab_bar_area);
    return TRUE;
}

struct LinkClickData { AppState* st; std::string url; };
static gboolean on_link_click(GtkWidget*, GdkEventButton* ev, gpointer d) {
    if (ev->button==1 && ev->type==GDK_BUTTON_PRESS) {
        auto* ld = static_cast<LinkClickData*>(d);
        navigate(ld->st, ld->url);
    }
    return FALSE;
}
static void free_link_data(gpointer d, GClosure*) { delete static_cast<LinkClickData*>(d); }

// ---- Render DOM tree to GTK widgets ----

static BoxModel dom_node_to_boxmodel(DOMNode* node) {
    BoxModel bm;
    for (int i = 0; i < 4; ++i) bm.margin[i] = node->margin[i];
    for (int i = 0; i < 4; ++i) bm.padding[i] = node->padding[i];
    bm.width = node->width;
    bm.max_width = node->max_width;
    bm.height = node->height;
    for (int i = 0; i < 4; ++i) bm.border_width[i] = node->border_width[i];
    bm.border_radius = node->border_radius;
    bm.border_color = node->border_color;
    bm.border_style = node->border_style;
    bm.halign_center = node->halign_center;
    bm.display = static_cast<BoxModel::Display>(static_cast<int>(node->display));
    bm.floatdir = static_cast<BoxModel::Float>(static_cast<int>(node->floatdir));
    bm.bg_image = node->bg_image;
    bm.bg_color = node->bg_color;
    bm.box_shadow = node->box_shadow;
    bm.opacity = node->opacity;
    bm.overflow = node->overflow;
    { auto it = node->style_props.find("background-repeat");
      if (it != node->style_props.end()) bm.bg_repeat = it->second; }
    { auto it = node->style_props.find("background-size");
      if (it != node->style_props.end()) bm.bg_size = it->second; }
    { auto it = node->style_props.find("background-position");
      if (it != node->style_props.end()) { bm.bg_position = it->second;
        fprintf(stderr,"[DEBUG dom2bm] <%s id='%s'> bg_position='%s'\n", node->tag_name.c_str(), node->id.c_str(), bm.bg_position.c_str());
      } }

    // Overlay JS-set style_props on top of parsed values
    if (!node->style_props.empty()) {
        auto sp = [&](const char* k) -> std::string {
            auto it = node->style_props.find(k);
            return it != node->style_props.end() ? it->second : "";
        };
        std::string v;
        if (!(v = sp("background-color")).empty()) bm.bg_color = v;
        if (!(v = sp("color")).empty()) { /* handled in text render */ }
        if (!(v = sp("width")).empty()) { int px = atoi(v.c_str()); if (px > 0) bm.width = px; }
        if (!(v = sp("height")).empty()) { int px = atoi(v.c_str()); if (px > 0) bm.height = px; }
        if (!(v = sp("border-radius")).empty()) { int px = atoi(v.c_str()); if (px >= 0) bm.border_radius = px; }
        if (!(v = sp("padding")).empty()) {
            int px = atoi(v.c_str());
            if (px >= 0) for (int i = 0; i < 4; i++) bm.padding[i] = px;
        }
        if (!(v = sp("margin")).empty()) {
            int px = atoi(v.c_str());
            for (int i = 0; i < 4; i++) bm.margin[i] = px;
        }
        fprintf(stderr, "[DEBUG dom_node_to_boxmodel] <%s id='%s'> style_props=%zu bg_color='%s'\n",
                node->tag_name.c_str(), node->id.c_str(), node->style_props.size(), bm.bg_color.c_str());
    }

    return bm;
}

static void render_dom_to_gtk(AppState* st, TabState* tab, Document* doc, int gen);

static void render_node(AppState* st, TabState* tab, DOMNode* node, int gen,
                         std::vector<GtkWidget*>& cstack,
                         std::vector<GtkWidget*>& float_rows) {
    if (node->node_type == DOMNode::TEXT) {
        if (node->text_content.empty()) return;

        float_rows.back() = nullptr;

        int fw = node->fw_computed;  // -1 = inherit
        int fi = node->fi_computed;  // -1 = inherit
        int fs = node->fs_computed;
        double lh = node->lh_computed;
        std::string color = node->color_computed;
        int text_align = node->text_align_computed;
        int text_transform = node->text_transform;
        std::string font_family = node->font_family;
        std::string href = node->href;
        int text_decoration = node->text_decoration;  // NOT inherited per CSS spec
        int letter_spacing = node->letter_spacing;
        int font_variant = node->font_variant;
        int white_space = node->white_space;
        int font_stretch = node->font_stretch;

        // Inherit from ancestors (also check style_props for JS-set values)
        auto get_sp_color = [](DOMNode* n) -> std::string {
            auto it = n->style_props.find("color");
            return it != n->style_props.end() ? it->second : "";
        };
        // Check own style_props first
        { std::string sc = get_sp_color(node); if (!sc.empty()) color = sc; }

        DOMNode* p = node->parent;
        while (p) {
            if (fw == -1 && p->fw_computed != -1) fw = p->fw_computed;
            if (fi == -1 && p->fi_computed != -1) fi = p->fi_computed;
            if (fs <= 0 && p->fs_computed > 0) fs = p->fs_computed;
            if (color.empty()) {
                std::string sc = get_sp_color(p);
                if (!sc.empty()) color = sc;
                else if (!p->color_computed.empty()) color = p->color_computed;
            }
            if (text_align < 0 && p->text_align_computed >= 0) text_align = p->text_align_computed;
            if (text_transform < 0 && p->text_transform >= 0) text_transform = p->text_transform;
            if (font_family.empty() && !p->font_family.empty()) font_family = p->font_family;
            if (lh < 0 && p->lh_computed >= 0) lh = p->lh_computed;
            if (href.empty() && !p->href.empty()) href = p->href;
            // text_decoration: look at parent element (not inherited, but applied on element)
            if (text_decoration < 0 && p->text_decoration >= 0) text_decoration = p->text_decoration;
            // Inherited properties
            if (letter_spacing == INT_MIN && p->letter_spacing != INT_MIN) letter_spacing = p->letter_spacing;
            if (font_variant < 0 && p->font_variant >= 0) font_variant = p->font_variant;
            if (white_space < 0 && p->white_space >= 0) white_space = p->white_space;
            if (font_stretch < 0 && p->font_stretch >= 0) font_stretch = p->font_stretch;
            p = p->parent;
        }

        if (fw == -1) fw = PANGO_WEIGHT_NORMAL;
        if (fi == -1) fi = PANGO_STYLE_NORMAL;
        if (text_align < 0) text_align = 0;
        if (fs <= 0) fs = 16;

        // White-space handling: decide whether to collapse
        bool preserve_ws = (white_space >= 2); // pre, pre-wrap, pre-line
        std::string text = preserve_ws ? node->text_content : collapse_ws(node->text_content);
        if (text.empty()) return;

        // Apply text-transform
        if (text_transform == 1) { // uppercase
            gchar* up = g_utf8_strup(text.c_str(), -1);
            text = up; g_free(up);
        } else if (text_transform == 2) { // lowercase
            gchar* lo = g_utf8_strdown(text.c_str(), -1);
            text = lo; g_free(lo);
        } else if (text_transform == 3) { // capitalize
            std::string result;
            bool next_upper = true;
            const char* s = text.c_str();
            while (*s) {
                gunichar ch = g_utf8_get_char(s);
                if (g_unichar_isspace(ch)) { next_upper = true; }
                else if (next_upper) { ch = g_unichar_toupper(ch); next_upper = false; }
                char buf[6]; int len = g_unichar_to_utf8(ch, buf);
                result.append(buf, len);
                s = g_utf8_next_char(s);
            }
            text = result;
        }

        fprintf(stderr, "[DEBUG render_text] text='%.40s' fw=%d fi=%d fs=%d parent=<%s>\n",
                text.c_str(), fw, fi, fs, node->parent ? node->parent->tag_name.c_str() : "NONE");

        GtkWidget* cur = cstack.back();
        GtkWidget* lbl = gtk_label_new(text.c_str());
        // white-space: nowrap/pre disable wrapping
        if (white_space == 1 || white_space == 2) // nowrap or pre
            gtk_label_set_line_wrap(GTK_LABEL(lbl), FALSE);
        else
            gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_widget_set_halign(lbl, GTK_ALIGN_FILL);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_widget_set_margin_start(lbl, 8);
        gtk_widget_set_margin_end(lbl, 8);

        PangoAttrList* al = pango_attr_list_new();
        if (fw != PANGO_WEIGHT_NORMAL)
            pango_attr_list_insert(al, pango_attr_weight_new((PangoWeight)fw));
        if (fi != PANGO_STYLE_NORMAL)
            pango_attr_list_insert(al, pango_attr_style_new((PangoStyle)fi));
        pango_attr_list_insert(al, pango_attr_size_new_absolute(fs * PANGO_SCALE));
        if (!font_family.empty()) {
            PangoFontDescription* fd = pango_font_description_from_string(font_family.c_str());
            pango_attr_list_insert(al, pango_attr_font_desc_new(fd));
            pango_font_description_free(fd);
        }
        if (lh >= 0)
            pango_attr_list_insert(al, pango_attr_line_height_new(lh));
        // text-decoration via Pango (replaces hardcoded link underline)
        if (text_decoration > 0) {
            if (text_decoration & 1) // underline
                pango_attr_list_insert(al, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
            if (text_decoration & 4) // line-through
                pango_attr_list_insert(al, pango_attr_strikethrough_new(TRUE));
        }
        // letter-spacing
        if (letter_spacing != INT_MIN && letter_spacing != 0)
            pango_attr_list_insert(al, pango_attr_letter_spacing_new(letter_spacing));
        // font-variant: small-caps
        if (font_variant == 1)
            pango_attr_list_insert(al, pango_attr_variant_new(PANGO_VARIANT_SMALL_CAPS));
        // font-stretch
        if (font_stretch >= 0)
            pango_attr_list_insert(al, pango_attr_stretch_new((PangoStretch)font_stretch));
        if (!color.empty()) {
            GdkRGBA rgba = {0,0,0,1};
            if (gdk_rgba_parse(&rgba, color.c_str()))
                pango_attr_list_insert(al, pango_attr_foreground_new(
                    (guint16)(rgba.red*65535),
                    (guint16)(rgba.green*65535),
                    (guint16)(rgba.blue*65535)));
        }
        gtk_label_set_attributes(GTK_LABEL(lbl), al);
        pango_attr_list_unref(al);

        if (text_align == 1) {
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.5f);
            gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_CENTER);
        } else if (text_align == 2) {
            gtk_label_set_xalign(GTK_LABEL(lbl), 1.0f);
            gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_RIGHT);
        } else if (text_align == 3) {
            gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_FILL);
        }

        if (!href.empty()) {
            GtkWidget* eb = gtk_event_box_new();
            gtk_event_box_set_above_child(GTK_EVENT_BOX(eb), TRUE);
            gtk_widget_add_events(eb, GDK_BUTTON_PRESS_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
            gtk_container_add(GTK_CONTAINER(eb), lbl);
            g_signal_connect_data(eb, "button-press-event",
                G_CALLBACK(on_link_click),
                new LinkClickData{st, href}, free_link_data, (GConnectFlags)0);
            g_signal_connect(eb, "realize", G_CALLBACK(+[](GtkWidget* w, gpointer) {
                GdkCursor* cursor = gdk_cursor_new_for_display(gtk_widget_get_display(w), GDK_HAND2);
                gdk_window_set_cursor(gtk_widget_get_window(w), cursor);
                g_object_unref(cursor);
            }), nullptr);
            gtk_box_pack_start(GTK_BOX(cur), eb, FALSE, FALSE, 2);
            gtk_widget_show(eb);
            gtk_widget_show(lbl);
        } else {
            gtk_box_pack_start(GTK_BOX(cur), lbl, FALSE, FALSE, 2);
            gtk_widget_show(lbl);
        }
        return;
    }

    // ELEMENT node

    // <svg> — render with Cairo drawing area
    if (node->tag_name == "svg") {
        float_rows.back() = nullptr;
        GtkWidget* cur = cstack.back();
        int svg_w = 300, svg_h = 150; // SVG spec defaults
        auto w_it = node->attributes.find("width");
        auto h_it = node->attributes.find("height");
        if (w_it != node->attributes.end()) {
            try { svg_w = (int)std::stod(w_it->second); } catch (...) {}
        }
        if (h_it != node->attributes.end()) {
            try { svg_h = (int)std::stod(h_it->second); } catch (...) {}
        }
        if (w_it == node->attributes.end() || h_it == node->attributes.end()) {
            auto vb_it = node->attributes.find("viewbox");
            if (vb_it != node->attributes.end()) {
                SvgViewBox vb = svg_parse_viewbox(vb_it->second);
                if (vb.valid) {
                    if (w_it == node->attributes.end()) svg_w = (int)vb.width;
                    if (h_it == node->attributes.end()) svg_h = (int)vb.height;
                }
            }
        }
        GtkWidget* da = gtk_drawing_area_new();
        gtk_widget_set_size_request(da, svg_w, svg_h);
        g_signal_connect(da, "draw", G_CALLBACK(draw_svg), (gpointer)node);
        gtk_box_pack_start(GTK_BOX(cur), da, FALSE, FALSE, 2);
        gtk_widget_show(da);
        return;
    }

    // <canvas> — render with Cairo backing surface
    if (node->tag_name == "canvas") {
        float_rows.back() = nullptr;
        GtkWidget* cur = cstack.back();
        int cw = 300, ch = 150; // canvas defaults
        auto w_it = node->attributes.find("width");
        auto h_it = node->attributes.find("height");
        if (w_it != node->attributes.end()) {
            try { cw = (int)std::stod(w_it->second); } catch (...) {}
        }
        if (h_it != node->attributes.end()) {
            try { ch = (int)std::stod(h_it->second); } catch (...) {}
        }
        // Also check style_props for width/height
        {
            auto sit = node->style_props.find("width");
            if (sit != node->style_props.end()) {
                int px = atoi(sit->second.c_str());
                if (px > 0) cw = px;
            }
            sit = node->style_props.find("height");
            if (sit != node->style_props.end()) {
                int px = atoi(sit->second.c_str());
                if (px > 0) ch = px;
            }
        }
        // Create backing surface
        cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cw, ch);
        GtkWidget* da = gtk_drawing_area_new();
        gtk_widget_set_size_request(da, cw, ch);
        g_signal_connect(da, "draw", G_CALLBACK(draw_canvas), GUINT_TO_POINTER(node->node_id));
        gtk_box_pack_start(GTK_BOX(cur), da, FALSE, FALSE, 2);
        gtk_widget_show(da);
        // Store in canvas map
        CanvasState cs;
        cs.surface = surf;
        cs.drawing_area = da;
        cs.width = cw;
        cs.height = ch;
        g_canvas_map[node->node_id] = cs;
        // Store node_id on widget
        g_object_set_data(G_OBJECT(da), "dom_node_id", GUINT_TO_POINTER(node->node_id));
        tab->node_widget_map[node->node_id] = da;
        return;
    }

    if (node->tag_name == "img") {
        float_rows.back() = nullptr;
        GtkWidget* cur = cstack.back();
        GtkWidget* img = gtk_image_new();
        gtk_box_pack_start(GTK_BOX(cur), img, FALSE, FALSE, 2);
        gtk_widget_show(img);
        g_object_ref(img);
        std::string img_url = node->attributes.count("src") ? node->attributes.at("src") : "";
        if (!img_url.empty()) {
            std::thread([st, tab, img, img_url, gen]() {
                Buf ibuf;
                if (gen != tab->generation || !fetch(img_url, ibuf)) {
                    idle_add([img](){ g_object_unref(img); });
                    return;
                }
                GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
                GError* err = nullptr;
                gdk_pixbuf_loader_write(loader, (const guchar*)ibuf.data.data(), ibuf.data.size(), &err);
                gdk_pixbuf_loader_close(loader, nullptr);
                GdkPixbuf* pb = nullptr;
                if (!err) {
                    pb = gdk_pixbuf_loader_get_pixbuf(loader);
                    if (pb) {
                        int w = gdk_pixbuf_get_width(pb);
                        if (w > 900) {
                            int h = gdk_pixbuf_get_height(pb);
                            pb = gdk_pixbuf_scale_simple(pb, 900, (int)(h*900.0/w), GDK_INTERP_BILINEAR);
                        } else {
                            g_object_ref(pb);
                        }
                    }
                }
                if (err) g_error_free(err);
                g_object_unref(loader);
                idle_add([st, tab, img, pb, gen]() {
                    if (gen == tab->generation && pb)
                        gtk_image_set_from_pixbuf(GTK_IMAGE(img), pb);
                    if (pb) g_object_unref(pb);
                    g_object_unref(img);
                });
            }).detach();
        }
        return;
    }

    // <input> element — render as GTK widget based on type
    if (node->tag_name == "input") {
        float_rows.back() = nullptr;
        GtkWidget* cur = cstack.back();
        std::string type = "text";
        auto type_it = node->attributes.find("type");
        if (type_it != node->attributes.end()) type = tolower_s(type_it->second);
        std::string init_val;
        auto val_it = node->attributes.find("value");
        if (val_it != node->attributes.end()) init_val = val_it->second;
        bool disabled = node->attributes.count("disabled") > 0;
        uint32_t nid = node->node_id;

        if (type == "checkbox") {
            GtkWidget* cb = gtk_check_button_new();
            if (node->attributes.count("checked")) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb), TRUE);
            if (disabled) gtk_widget_set_sensitive(cb, FALSE);
            g_object_set_data(G_OBJECT(cb), "dom_node_id", GUINT_TO_POINTER(nid));
            g_signal_connect(cb, "toggled",
                G_CALLBACK(+[](GtkToggleButton* tb, gpointer d) {
                    auto* st = static_cast<AppState*>(d);
                    auto* tab = st->ct;
                    uint32_t nid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(tb), "dom_node_id"));
                    DOMNode* n = tab->document ? tab->document->node_map.count(nid) ? tab->document->node_map[nid] : nullptr : nullptr;
                    if (n) {
                        if (gtk_toggle_button_get_active(tb)) n->attributes["checked"] = "checked";
                        else n->attributes.erase("checked");
                    }
                    if (tab->js_engine) tab->js_engine->dispatchEvent(nid, "change", 0, 0);
                }), st);
            gtk_box_pack_start(GTK_BOX(cur), cb, FALSE, FALSE, 2);
            gtk_widget_show(cb);
            tab->node_widget_map[nid] = cb;
        } else if (type == "radio") {
            GtkWidget* rb = gtk_radio_button_new(nullptr);
            if (node->attributes.count("checked")) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rb), TRUE);
            if (disabled) gtk_widget_set_sensitive(rb, FALSE);
            g_object_set_data(G_OBJECT(rb), "dom_node_id", GUINT_TO_POINTER(nid));
            g_signal_connect(rb, "toggled",
                G_CALLBACK(+[](GtkToggleButton* tb, gpointer d) {
                    auto* st = static_cast<AppState*>(d);
                    auto* tab = st->ct;
                    uint32_t nid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(tb), "dom_node_id"));
                    DOMNode* n = tab->document ? tab->document->node_map.count(nid) ? tab->document->node_map[nid] : nullptr : nullptr;
                    if (n) {
                        if (gtk_toggle_button_get_active(tb)) n->attributes["checked"] = "checked";
                        else n->attributes.erase("checked");
                    }
                    if (tab->js_engine) tab->js_engine->dispatchEvent(nid, "change", 0, 0);
                }), st);
            gtk_box_pack_start(GTK_BOX(cur), rb, FALSE, FALSE, 2);
            gtk_widget_show(rb);
            tab->node_widget_map[nid] = rb;
        } else if (type == "submit" || type == "reset") {
            std::string label = init_val.empty() ? (type == "submit" ? "Submit" : "Reset") : init_val;
            GtkWidget* btn = gtk_button_new_with_label(label.c_str());
            if (disabled) gtk_widget_set_sensitive(btn, FALSE);
            g_object_set_data(G_OBJECT(btn), "dom_node_id", GUINT_TO_POINTER(nid));
            g_signal_connect(btn, "clicked",
                G_CALLBACK(+[](GtkWidget* w, gpointer d) {
                    auto* st = static_cast<AppState*>(d);
                    auto* tab = st->ct;
                    if (!tab->js_engine) return;
                    uint32_t nid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "dom_node_id"));
                    tab->js_engine->dispatchEvent(nid, "click", 0, 0);
                }), st);
            gtk_box_pack_start(GTK_BOX(cur), btn, FALSE, FALSE, 2);
            gtk_widget_show(btn);
            tab->node_widget_map[nid] = btn;
        } else {
            // text, password, email, number, search, etc.
            GtkWidget* entry = gtk_entry_new();
            if (!init_val.empty()) gtk_entry_set_text(GTK_ENTRY(entry), init_val.c_str());
            auto ph_it = node->attributes.find("placeholder");
            if (ph_it != node->attributes.end())
                gtk_entry_set_placeholder_text(GTK_ENTRY(entry), ph_it->second.c_str());
            if (type == "password") gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
            if (disabled) gtk_widget_set_sensitive(entry, FALSE);
            g_object_set_data(G_OBJECT(entry), "dom_node_id", GUINT_TO_POINTER(nid));
            g_signal_connect(entry, "changed",
                G_CALLBACK(+[](GtkEditable* e, gpointer d) {
                    auto* st = static_cast<AppState*>(d);
                    auto* tab = st->ct;
                    uint32_t nid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(e), "dom_node_id"));
                    DOMNode* n = tab->document ? tab->document->node_map.count(nid) ? tab->document->node_map[nid] : nullptr : nullptr;
                    if (n) n->attributes["value"] = gtk_entry_get_text(GTK_ENTRY(e));
                    if (tab->js_engine) tab->js_engine->dispatchEvent(nid, "input", 0, 0);
                }), st);
            // Dispatch change event on focus-out
            g_signal_connect(entry, "focus-out-event",
                G_CALLBACK(+[](GtkWidget* w, GdkEventFocus*, gpointer d) -> gboolean {
                    auto* st = static_cast<AppState*>(d);
                    auto* tab = st->ct;
                    uint32_t nid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "dom_node_id"));
                    if (tab->js_engine) tab->js_engine->dispatchEvent(nid, "change", 0, 0);
                    tab->focused_node_id = 0;
                    return FALSE;
                }), st);
            // Track focus for keyboard routing
            g_signal_connect(entry, "focus-in-event",
                G_CALLBACK(+[](GtkWidget* w, GdkEventFocus*, gpointer d) -> gboolean {
                    auto* st = static_cast<AppState*>(d);
                    auto* tab = st->ct;
                    tab->focused_node_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "dom_node_id"));
                    return FALSE;
                }), st);
            gtk_box_pack_start(GTK_BOX(cur), entry, FALSE, FALSE, 2);
            gtk_widget_show(entry);
            tab->node_widget_map[nid] = entry;
        }
        return;
    }

    // <textarea> element — render as GtkTextView
    if (node->tag_name == "textarea") {
        float_rows.back() = nullptr;
        GtkWidget* cur = cstack.back();
        uint32_t nid = node->node_id;
        bool disabled = node->attributes.count("disabled") > 0;
        std::string init_val = node->getTextContent();

        GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
            GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(scroll, 300, 80);
        GtkWidget* tv = gtk_text_view_new();
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
        if (!init_val.empty()) {
            GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
            gtk_text_buffer_set_text(buf, init_val.c_str(), -1);
        }
        if (disabled) gtk_widget_set_sensitive(tv, FALSE);
        gtk_container_add(GTK_CONTAINER(scroll), tv);
        g_object_set_data(G_OBJECT(tv), "dom_node_id", GUINT_TO_POINTER(nid));
        // Store AppState and node_id on buffer too for the changed callback
        g_object_set_data(G_OBJECT(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv))),
            "dom_node_id", GUINT_TO_POINTER(nid));
        g_object_set_data(G_OBJECT(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv))),
            "app_state", st);
        auto textarea_changed_cb = +[](GtkTextBuffer* buf, gpointer d) {
            (void)d;
            auto* st = static_cast<AppState*>(g_object_get_data(G_OBJECT(buf), "app_state"));
            uint32_t nid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(buf), "dom_node_id"));
            if (!st) return;
            auto* tab = st->ct;
            GtkTextIter start, end;
            gtk_text_buffer_get_bounds(buf, &start, &end);
            char* text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
            DOMNode* n = tab->document ? tab->document->node_map.count(nid) ? tab->document->node_map[nid] : nullptr : nullptr;
            if (n) n->attributes["value"] = text ? text : "";
            g_free(text);
            if (tab->js_engine) tab->js_engine->dispatchEvent(nid, "input", 0, 0);
        };
        g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), "changed",
            G_CALLBACK(textarea_changed_cb), nullptr);
        // Focus tracking
        g_signal_connect(tv, "focus-in-event",
            G_CALLBACK(+[](GtkWidget* w, GdkEventFocus*, gpointer d) -> gboolean {
                auto* st = static_cast<AppState*>(d);
                auto* tab = st->ct;
                tab->focused_node_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "dom_node_id"));
                return FALSE;
            }), st);
        g_signal_connect(tv, "focus-out-event",
            G_CALLBACK(+[](GtkWidget* w, GdkEventFocus*, gpointer d) -> gboolean {
                auto* st = static_cast<AppState*>(d);
                auto* tab = st->ct;
                uint32_t nid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "dom_node_id"));
                if (tab->js_engine) tab->js_engine->dispatchEvent(nid, "change", 0, 0);
                tab->focused_node_id = 0;
                return FALSE;
            }), st);
        gtk_box_pack_start(GTK_BOX(cur), scroll, FALSE, FALSE, 2);
        gtk_widget_show_all(scroll);
        tab->node_widget_map[nid] = scroll;
        return;
    }

    // <select> element — render as GtkComboBoxText
    if (node->tag_name == "select") {
        float_rows.back() = nullptr;
        GtkWidget* cur = cstack.back();
        uint32_t nid = node->node_id;
        bool disabled = node->attributes.count("disabled") > 0;
        GtkWidget* combo = gtk_combo_box_text_new();
        int selected = 0, idx = 0;
        for (auto& child : node->children) {
            if (child->tag_name == "option") {
                std::string text = child->getTextContent();
                auto v_it = child->attributes.find("value");
                std::string val = v_it != child->attributes.end() ? v_it->second : text;
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), text.c_str());
                if (child->attributes.count("selected")) selected = idx;
                ++idx;
            }
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), selected);
        if (disabled) gtk_widget_set_sensitive(combo, FALSE);
        // Set initial value
        if (idx > 0) {
            int sel_idx = 0;
            for (auto& child : node->children) {
                if (child->tag_name == "option") {
                    if (sel_idx == selected) {
                        auto v_it = child->attributes.find("value");
                        node->attributes["value"] = v_it != child->attributes.end() ? v_it->second : child->getTextContent();
                        break;
                    }
                    ++sel_idx;
                }
            }
        }
        g_object_set_data(G_OBJECT(combo), "dom_node_id", GUINT_TO_POINTER(nid));
        g_signal_connect(combo, "changed",
            G_CALLBACK(+[](GtkComboBox* cb, gpointer d) {
                auto* st = static_cast<AppState*>(d);
                auto* tab = st->ct;
                uint32_t nid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(cb), "dom_node_id"));
                DOMNode* n = tab->document ? tab->document->node_map.count(nid) ? tab->document->node_map[nid] : nullptr : nullptr;
                if (n) {
                    int active = gtk_combo_box_get_active(cb);
                    n->attributes["selectedindex"] = std::to_string(active);
                    int i = 0;
                    for (auto& c : n->children) {
                        if (c->tag_name == "option") {
                            if (i == active) {
                                auto v = c->attributes.find("value");
                                n->attributes["value"] = v != c->attributes.end() ? v->second : c->getTextContent();
                                break;
                            }
                            ++i;
                        }
                    }
                }
                if (tab->js_engine) tab->js_engine->dispatchEvent(nid, "change", 0, 0);
            }), st);
        gtk_box_pack_start(GTK_BOX(cur), combo, FALSE, FALSE, 2);
        gtk_widget_show(combo);
        tab->node_widget_map[nid] = combo;
        return;
    }

    // <button> element — render as a GTK button
    if (node->tag_name == "button") {
        float_rows.back() = nullptr;
        GtkWidget* cur = cstack.back();
        std::string label = node->getTextContent();
        GtkWidget* btn = gtk_button_new_with_label(label.c_str());
        gtk_widget_set_can_focus(btn, FALSE);

        // Apply test page CSS styling via CSS provider
        BoxModel bm = dom_node_to_boxmodel(node);
        std::string css;
        if (!bm.bg_color.empty())
            css += "background-color: " + bm.bg_color + "; background-image: none; ";
        css += "padding: 6px 14px; border-radius: 4px; color: white; border: none; ";
        if (!css.empty()) {
            GtkCssProvider* cp = gtk_css_provider_new();
            gtk_css_provider_load_from_data(cp, ("button { " + css + "}").c_str(), -1, nullptr);
            gtk_style_context_add_provider(gtk_widget_get_style_context(btn),
                GTK_STYLE_PROVIDER(cp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(cp);
        }

        // Wire click event to JS dispatcher
        uint32_t nid = node->node_id;
        g_object_set_data(G_OBJECT(btn), "dom_node_id", GUINT_TO_POINTER(nid));
        g_signal_connect(btn, "clicked",
            G_CALLBACK(+[](GtkWidget* w, gpointer d) {
                auto* st = static_cast<AppState*>(d);
                auto* tab = st->ct;
                if (!tab->js_engine) return;
                uint32_t nid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "dom_node_id"));
                tab->js_engine->dispatchEvent(nid, "click", 0, 0);
            }), st);

        gtk_box_pack_start(GTK_BOX(cur), btn, FALSE, FALSE, 2);
        gtk_widget_show(btn);
        tab->node_widget_map[nid] = btn;
        fprintf(stderr, "[DEBUG render_node] <button id='%s'> label='%s' node_id=%u\n",
                node->id.c_str(), label.c_str(), node->node_id);
        return;
    }

    // Block/container element
    bool floated = node->floatdir != DOMNode::Float::None;
    bool is_ib = node->display == DOMNode::Display::InlineBlock;
    bool is_flex = node->display == DOMNode::Display::Flex;
    bool is_positioned = node->position == 2 || node->position == 3; // absolute or fixed
    bool emits_block = floated || is_flex || is_ib || is_positioned
        || node->display == DOMNode::Display::Block
        || (node->isBlock() && node->display != DOMNode::Display::Inline);

    if (node->is_body) {
        BoxModel bm = dom_node_to_boxmodel(node);
        if (!bm.bg_color.empty()) {
            char* bgc = g_strdup(bm.bg_color.c_str());
            g_object_set_data_full(G_OBJECT(tab->content_box), "bg_color_str", bgc, g_free);
        }
        gtk_widget_set_margin_top(tab->content_box, std::max(0, bm.margin[0]));
        gtk_widget_set_margin_end(tab->content_box, std::max(0, bm.margin[1]));
        gtk_widget_set_margin_bottom(tab->content_box, std::max(0, bm.margin[2]));
        gtk_widget_set_margin_start(tab->content_box, std::max(0, bm.margin[3]));
        if (!bm.bg_color.empty() || !bm.bg_image.empty()) {
            gtk_widget_set_app_paintable(tab->content_box, TRUE);
            tab->body_draw_signal = g_signal_connect(tab->content_box, "draw", G_CALLBACK(draw_bg), nullptr);
        }
        if (!bm.bg_image.empty()) {
            if (!bm.bg_repeat.empty())
                g_object_set_data_full(G_OBJECT(tab->content_box), "bg_repeat", g_strdup(bm.bg_repeat.c_str()), g_free);
            if (!bm.bg_size.empty())
                g_object_set_data_full(G_OBJECT(tab->content_box), "bg_size", g_strdup(bm.bg_size.c_str()), g_free);
            if (!bm.bg_position.empty())
                g_object_set_data_full(G_OBJECT(tab->content_box), "bg_position", g_strdup(bm.bg_position.c_str()), g_free);
            std::string bg_url = bm.bg_image;
            std::thread([st, tab, bg_url, gen]() {
                Buf ibuf;
                if (gen != tab->generation || !fetch(bg_url, ibuf)) return;
                GdkPixbufLoader* ldr = gdk_pixbuf_loader_new();
                GError* err = nullptr;
                gdk_pixbuf_loader_write(ldr, (const guchar*)ibuf.data.data(), ibuf.data.size(), &err);
                gdk_pixbuf_loader_close(ldr, nullptr);
                GdkPixbuf* pb = nullptr;
                if (!err) { pb = gdk_pixbuf_loader_get_pixbuf(ldr); if (pb) g_object_ref(pb); }
                if (err) g_error_free(err);
                g_object_unref(ldr);
                idle_add([st, tab, pb, gen]() {
                    if (gen == tab->generation && pb) {
                        g_object_set_data_full(G_OBJECT(tab->content_box), "bg_pb", pb, (GDestroyNotify)g_object_unref);
                        gtk_widget_queue_draw(tab->content_box);
                    } else if (pb) g_object_unref(pb);
                });
            }).detach();
        }
        cstack.push_back(tab->content_box);
        float_rows.push_back(nullptr);
        for (auto& child : node->children)
            render_node(st, tab, child.get(), gen, cstack, float_rows);
        cstack.pop_back();
        float_rows.pop_back();
        return;
    }

    if (emits_block) {
        BoxModel bm = dom_node_to_boxmodel(node);
        GtkWidget* new_blk;
        if (floated || is_ib) {
            GtkWidget*& fr = float_rows.back();
            if (!fr) {
                fr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
                gtk_widget_set_hexpand(fr, TRUE);
                gtk_box_pack_start(GTK_BOX(cstack.back()), fr, FALSE, FALSE, 0);
                gtk_widget_show(fr);
            }
            bool to_end = (node->floatdir == DOMNode::Float::Right);
            new_blk = make_block(bm, fr, GTK_ORIENTATION_VERTICAL, to_end);
        } else {
            float_rows.back() = nullptr;
            GtkOrientation orient = GTK_ORIENTATION_VERTICAL;
            if (is_flex) {
                // flex-direction: 0=row, 1=column, 2=row-reverse, 3=column-reverse
                if (node->flex_direction == 0 || node->flex_direction == 2)
                    orient = GTK_ORIENTATION_HORIZONTAL;
                else
                    orient = GTK_ORIENTATION_VERTICAL;
            }
            bool to_end_flex = is_flex && (node->flex_direction == 2 || node->flex_direction == 3);
            new_blk = make_block(bm, cstack.back(), orient, to_end_flex);
            if (is_flex) {
                // gap
                if (node->gap > 0)
                    gtk_box_set_spacing(GTK_BOX(new_blk), node->gap);
                // justify-content: center
                if (node->justify_content == 2) {
                    gtk_widget_set_halign(new_blk, orient == GTK_ORIENTATION_HORIZONTAL ? GTK_ALIGN_CENTER : GTK_ALIGN_FILL);
                    gtk_widget_set_valign(new_blk, orient == GTK_ORIENTATION_VERTICAL ? GTK_ALIGN_CENTER : GTK_ALIGN_FILL);
                }
                // justify-content: space-between → homogeneous
                if (node->justify_content == 3)
                    gtk_box_set_homogeneous(GTK_BOX(new_blk), TRUE);
                // justify-content: end
                if (node->justify_content == 1) {
                    if (orient == GTK_ORIENTATION_HORIZONTAL)
                        gtk_widget_set_halign(new_blk, GTK_ALIGN_END);
                    else
                        gtk_widget_set_valign(new_blk, GTK_ALIGN_END);
                }
            }
        }
        fprintf(stderr,"[DEBUG render_block] <%s id='%s'> bg_image='%s' bg_position='%s' bg_repeat='%s'\n",
            node->tag_name.c_str(), node->id.c_str(), bm.bg_image.c_str(), bm.bg_position.c_str(), bm.bg_repeat.c_str());
        if (!bm.bg_image.empty()) {
            gtk_widget_set_app_paintable(new_blk, TRUE);
            g_signal_connect(new_blk, "draw", G_CALLBACK(draw_bg), nullptr);
            if (!bm.bg_repeat.empty())
                g_object_set_data_full(G_OBJECT(new_blk), "bg_repeat", g_strdup(bm.bg_repeat.c_str()), g_free);
            if (!bm.bg_size.empty())
                g_object_set_data_full(G_OBJECT(new_blk), "bg_size", g_strdup(bm.bg_size.c_str()), g_free);
            if (!bm.bg_position.empty())
                g_object_set_data_full(G_OBJECT(new_blk), "bg_position", g_strdup(bm.bg_position.c_str()), g_free);
            g_object_ref(new_blk);
            std::string bg_url = bm.bg_image;
            std::thread([st, tab, new_blk, bg_url, gen]() {
                Buf ibuf;
                if (gen != tab->generation || !fetch(bg_url, ibuf)) {
                    idle_add([new_blk](){ g_object_unref(new_blk); });
                    return;
                }
                GdkPixbufLoader* ldr = gdk_pixbuf_loader_new();
                GError* err = nullptr;
                gdk_pixbuf_loader_write(ldr, (const guchar*)ibuf.data.data(), ibuf.data.size(), &err);
                gdk_pixbuf_loader_close(ldr, nullptr);
                GdkPixbuf* pb = nullptr;
                if (!err) { pb = gdk_pixbuf_loader_get_pixbuf(ldr); if (pb) g_object_ref(pb); }
                if (err) g_error_free(err);
                g_object_unref(ldr);
                idle_add([st, tab, new_blk, pb, gen]() {
                    if (gen == tab->generation && pb) {
                        g_object_set_data_full(G_OBJECT(new_blk), "bg_pb", pb, (GDestroyNotify)g_object_unref);
                        gtk_widget_queue_draw(new_blk);
                    } else if (pb) g_object_unref(pb);
                    g_object_unref(new_blk);
                });
            }).detach();
        }
        // Apply CSS positioning
        if (node->position == 1) { // relative
            // Adjust margins to offset from normal position
            int cur_mt = gtk_widget_get_margin_top(new_blk);
            int cur_ms = gtk_widget_get_margin_start(new_blk);
            if (node->pos_top != INT_MIN) gtk_widget_set_margin_top(new_blk, cur_mt + node->pos_top);
            if (node->pos_left != INT_MIN) gtk_widget_set_margin_start(new_blk, cur_ms + node->pos_left);
            if (node->pos_bottom != INT_MIN && node->pos_top == INT_MIN)
                gtk_widget_set_margin_bottom(new_blk, gtk_widget_get_margin_bottom(new_blk) - node->pos_bottom);
            if (node->pos_right != INT_MIN && node->pos_left == INT_MIN)
                gtk_widget_set_margin_end(new_blk, gtk_widget_get_margin_end(new_blk) - node->pos_right);
        } else if (node->position == 2 || node->position == 3) { // absolute or fixed
            // For absolute/fixed: use GtkOverlay on the parent container
            // Remove from normal flow and position with GtkFixed
            GtkWidget* parent_container = (node->position == 3) ? tab->viewport : cstack.back();

            // Find the outer widget (make_block may return inner)
            GtkWidget* outer = new_blk;
            GtkWidget* p = gtk_widget_get_parent(new_blk);
            while (p && p != parent_container && p != cstack.back()) {
                outer = p;
                p = gtk_widget_get_parent(p);
            }

            // Reparent into a GtkFixed overlay
            // We'll use margin-based positioning on the widget instead
            // since GtkOverlay requires more complex setup
            int top = (node->pos_top != INT_MIN) ? node->pos_top : 0;
            int left = (node->pos_left != INT_MIN) ? node->pos_left : 0;
            gtk_widget_set_margin_top(new_blk, top);
            gtk_widget_set_margin_start(new_blk, left);
            if (node->pos_right != INT_MIN && node->pos_left == INT_MIN)
                gtk_widget_set_halign(new_blk, GTK_ALIGN_END);
            if (node->pos_bottom != INT_MIN && node->pos_top == INT_MIN)
                gtk_widget_set_valign(new_blk, GTK_ALIGN_END);
        }

        // Store node_id on widget for click dispatch
        g_object_set_data(G_OBJECT(new_blk), "dom_node_id",
            GUINT_TO_POINTER(node->node_id));
        tab->node_widget_map[node->node_id] = new_blk;

        // Add click event handling if node has listeners
        if (!node->listeners.empty()) {
            GtkWidget* eb = gtk_event_box_new();
            gtk_event_box_set_above_child(GTK_EVENT_BOX(eb), FALSE);
            gtk_widget_add_events(eb, GDK_BUTTON_PRESS_MASK);
            uint32_t nid = node->node_id;
            g_object_set_data(G_OBJECT(eb), "dom_node_id", GUINT_TO_POINTER(nid));
            g_signal_connect(eb, "button-press-event",
                G_CALLBACK(+[](GtkWidget* w, GdkEventButton* ev, gpointer d) -> gboolean {
                    if (ev->button != 1 || ev->type != GDK_BUTTON_PRESS) return FALSE;
                    auto* st = static_cast<AppState*>(d);
                    auto* tab = st->ct;
                    if (!tab->js_engine) return FALSE;
                    uint32_t nid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "dom_node_id"));
                    tab->js_engine->dispatchEvent(nid, "click", (int)ev->x, (int)ev->y);
                    return FALSE;
                }), st);
            // Wrap the block widget - reparent
            GtkWidget* parent_w = gtk_widget_get_parent(new_blk);
            if (parent_w) {
                g_object_ref(new_blk);
                gtk_container_remove(GTK_CONTAINER(parent_w), new_blk);
                gtk_container_add(GTK_CONTAINER(eb), new_blk);
                g_object_unref(new_blk);
                gtk_box_pack_start(GTK_BOX(parent_w), eb, FALSE, FALSE, 0);
                gtk_widget_show(eb);
            }
        }

        cstack.push_back(new_blk);
        float_rows.push_back(nullptr);
        for (auto& child : node->children)
            render_node(st, tab, child.get(), gen, cstack, float_rows);
        // Apply flex align-items to children
        if (is_flex && node->align_items != 0) {
            GtkOrientation orient = (node->flex_direction == 0 || node->flex_direction == 2)
                ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL;
            GList* ch = gtk_container_get_children(GTK_CONTAINER(new_blk));
            for (GList* l = ch; l; l = l->next) {
                GtkWidget* cw = GTK_WIDGET(l->data);
                if (orient == GTK_ORIENTATION_HORIZONTAL) {
                    // Cross axis is vertical
                    if (node->align_items == 1) gtk_widget_set_valign(cw, GTK_ALIGN_START);
                    else if (node->align_items == 2) gtk_widget_set_valign(cw, GTK_ALIGN_END);
                    else if (node->align_items == 3) gtk_widget_set_valign(cw, GTK_ALIGN_CENTER);
                } else {
                    // Cross axis is horizontal
                    if (node->align_items == 1) gtk_widget_set_halign(cw, GTK_ALIGN_START);
                    else if (node->align_items == 2) gtk_widget_set_halign(cw, GTK_ALIGN_END);
                    else if (node->align_items == 3) gtk_widget_set_halign(cw, GTK_ALIGN_CENTER);
                }
            }
            g_list_free(ch);
        }
        // For justify-content: space-between, expand children
        if (is_flex && node->justify_content == 3) {
            GList* ch = gtk_container_get_children(GTK_CONTAINER(new_blk));
            for (GList* l = ch; l; l = l->next) {
                GtkWidget* cw = GTK_WIDGET(l->data);
                gtk_widget_set_hexpand(cw, TRUE);
                gtk_widget_set_vexpand(cw, TRUE);
            }
            g_list_free(ch);
        }
        cstack.pop_back();
        float_rows.pop_back();
    } else {
        for (auto& child : node->children)
            render_node(st, tab, child.get(), gen, cstack, float_rows);
    }
}

static void render_dom_to_gtk(AppState* st, TabState* tab, Document* doc, int gen) {
    std::vector<GtkWidget*> cstack = {tab->content_box};
    std::vector<GtkWidget*> float_rows = {nullptr};
    // Debug: dump DOM tree structure
    std::function<void(DOMNode*, int)> dump_tree = [&](DOMNode* n, int depth) {
        if (!n) return;
        std::string indent(depth*2, ' ');
        if (n->node_type == DOMNode::TEXT) {
            std::string t = n->text_content.substr(0, 40);
            if (!collapse_ws(t).empty())
                fprintf(stderr, "%s TEXT: '%.40s' display=%d\n", indent.c_str(), t.c_str(), (int)n->display);
        } else {
            fprintf(stderr, "%s <%s id='%s' class='", indent.c_str(), n->tag_name.c_str(), n->id.c_str());
            for (size_t i=0; i<n->class_list.size() && i<3; i++) fprintf(stderr, "%s ", n->class_list[i].c_str());
            fprintf(stderr, "' display=%d> children=%zu\n", (int)n->display, n->children.size());
        }
        if (depth < 6) for (auto& c : n->children) dump_tree(c.get(), depth+1);
    };
    if (doc->body) {
        fprintf(stderr, "[RENDER] body has %zu children, display=%d\n",
            doc->body->children.size(), (int)doc->body->display);
        dump_tree(doc->body, 0);
    }
    if (doc->body) {
        render_node(st, tab, doc->body, gen, cstack, float_rows);
    } else {
        for (auto& child : doc->root->children)
            render_node(st, tab, child.get(), gen, cstack, float_rows);
    }
}

// Called by JSEngine::rerender_callback when DOM is dirty
void do_rerender(AppState* st, TabState* tab) {
    if (!tab || !tab->document) return;
    int gen = tab->generation;

    // Clean up previous body styles
    if (tab->body_draw_signal) {
        g_signal_handler_disconnect(tab->content_box, tab->body_draw_signal);
        tab->body_draw_signal = 0;
        gtk_widget_set_app_paintable(tab->content_box, FALSE);
        g_object_set_data(G_OBJECT(tab->content_box), "bg_pb", nullptr);
        g_object_set_data(G_OBJECT(tab->content_box), "bg_color_str", nullptr);
    }
    gtk_widget_set_margin_top(tab->content_box, 0);
    gtk_widget_set_margin_end(tab->content_box, 0);
    gtk_widget_set_margin_bottom(tab->content_box, 0);
    gtk_widget_set_margin_start(tab->content_box, 0);

    // Clear node-to-widget map
    tab->node_widget_map.clear();

    // Remove all children
    GList* ch = gtk_container_get_children(GTK_CONTAINER(tab->content_box));
    for (GList* l = ch; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);

    // Re-render
    render_dom_to_gtk(st, tab, tab->document.get(), gen);

    // Clear dirty flags
    std::function<void(DOMNode*)> clear_dirty = [&](DOMNode* n) {
        n->dirty = false;
        for (auto& c : n->children) clear_dirty(c.get());
    };
    clear_dirty(tab->document->root.get());
}

// ---- test mode (forward decls) ----
static bool g_test_mode = false;
static int g_probe_pass = 0;
static int g_probe_fail = 0;
static void run_test_probes(AppState* st);

// ---- page fetch ----

static void fetch_page(AppState* st, TabState* tab, std::string url, int gen) {
    bool is_vs = (url.size()>=12 && url.substr(0,12)=="view-source:");
    std::string fetch_url = is_vs ? url.substr(12) : url;

    Buf buf;
    if (!fetch(fetch_url, buf)) {
        idle_add([st, url]() {
            gtk_window_set_title(GTK_WINDOW(st->window), ("Failed: "+url).c_str());
        });
        return;
    }
    if (gen != tab->generation) return;

    if (is_vs) {
        std::string raw = std::move(buf.data);
        idle_add([st, tab, gen, url, raw=std::move(raw)]() {
            if (gen != tab->generation) return;
            gtk_window_set_title(GTK_WINDOW(st->window), url.c_str());
            GtkWidget* tv = gtk_text_view_new();
            gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
            gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_CHAR);
            GtkCssProvider* css = gtk_css_provider_new();
            gtk_css_provider_load_from_data(css,
                "textview, textview text { font-family: monospace; font-size: 10pt; }", -1, nullptr);
            gtk_style_context_add_provider(gtk_widget_get_style_context(tv),
                GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(css);
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)),
                raw.c_str(), (gint)raw.size());
            gtk_box_pack_start(GTK_BOX(tab->content_box), tv, TRUE, TRUE, 0);
            gtk_widget_show(tv);
        });
        return;
    }

    std::string raw_source = buf.data;  // save raw source for inspector
    auto doc = parse_html_to_dom(buf.data, fetch_url);

    fprintf(stderr, "[JS-TRACE] Parsed: %zu script_srcs, %zu inline scripts\n",
            doc->script_srcs.size(), doc->scripts.size());
    for (size_t si = 0; si < doc->script_srcs.size(); si++)
        fprintf(stderr, "[JS-TRACE]   src[%zu] = %s\n", si, doc->script_srcs[si].c_str());
    for (size_t si = 0; si < doc->scripts.size(); si++)
        fprintf(stderr, "[JS-TRACE]   inline[%zu] = %.80s...\n", si, doc->scripts[si].c_str());

    // Fetch external scripts synchronously (blocking, matches <script src> behavior)
    std::vector<std::string> external_scripts;
    for (const auto& src_url : doc->script_srcs) {
        if (gen != tab->generation) return;
        Buf sbuf;
        if (fetch(src_url, sbuf)) {
            external_scripts.push_back(std::move(sbuf.data));
        } else {
            fprintf(stderr, "[script] Failed to fetch: %s\n", src_url.c_str());
        }
    }

    idle_add([st, tab, gen, url, doc, raw_source=std::move(raw_source),
              external_scripts=std::move(external_scripts)]() {
        if (gen != tab->generation) return;
        gtk_window_set_title(GTK_WINDOW(st->window), url.c_str());
        tab->document = doc;
        tab->page_source = raw_source;

        // Extract title from DOM
        if (doc->body) {
            std::function<std::string(DOMNode*)> find_title = [&](DOMNode* n) -> std::string {
                if (n->tag_name == "title") return n->getTextContent();
                for (auto& c : n->children) {
                    std::string t = find_title(c.get());
                    if (!t.empty()) return t;
                }
                return "";
            };
            std::string t = find_title(doc->root.get());
            if (!t.empty()) tab->title = t;
            else tab->title = url;
        }

        render_dom_to_gtk(st, tab, doc.get(), gen);

        // Destroy previous JS engine
        if (tab->js_engine) {
            delete tab->js_engine;
            tab->js_engine = nullptr;
        }

        // Create JS engine and run scripts
        auto* engine = new JSEngine();
        tab->js_engine = engine;
        engine->page_url = tab->current_url;
        engine->init(st, tab, doc.get());

        // Wire up inspector console callback if inspector is open
        if (tab->inspector_visible) {
            engine->on_console_entry = [st, tab]() {
                if (tab->js_engine && !tab->js_engine->console_log.empty()) {
                    inspector_append_console_entry(st, tab->js_engine->console_log.back());
                }
            };
            inspector_update_elements(st);
            inspector_refresh_console(st);
        }

        // Execute external scripts first (in document order they were found)
        DOMNode* script_parent = doc->head ? doc->head : doc->body;
        for (size_t i = 0; i < external_scripts.size(); i++) {
            std::string fname = i < doc->script_srcs.size() ? doc->script_srcs[i] : "<external>";
            auto script_el = doc->createElement("script");
            script_el->attributes["src"] = fname;
            if (script_parent) doc->appendChild(script_parent, script_el);
            engine->has_current_script = true;
            engine->current_script_src = fname;
            engine->current_script_node = script_el.get();
            fprintf(stderr, "[JS-TRACE] Eval external: %s (%zu bytes)\n", fname.c_str(), external_scripts[i].size());
            bool ok = engine->eval(external_scripts[i], fname);
            fprintf(stderr, "[JS-TRACE]   => %s\n", ok ? "OK" : "FAILED");
            engine->has_current_script = false;
            engine->current_script_src.clear();
            engine->current_script_node = nullptr;
        }

        // Debug: patch Runner to dump results and trace stuck background tasks
        engine->eval(R"JS(
            // The test engine may use different names. Patch both Runner and Test.
            var _patchTarget = (typeof Runner !== 'undefined') ? Runner :
                               (typeof Test !== 'undefined') ? Test : null;
            if (_patchTarget && _patchTarget.prototype) {
                console.warn('[DEBUG] Patching: ' + (_patchTarget.name || 'unknown'));
                console.warn('[DEBUG] Proto keys: ' + Object.getOwnPropertyNames(_patchTarget.prototype).slice(0,20).join(', '));

                var _origFinished = _patchTarget.prototype.finished;
                if (_origFinished) {
                    _patchTarget.prototype.finished = function() {
                        var results = this.list ? this.list.toString() : 'NO LIST';
                        console.warn('[RESULTS] ' + results);
                        return _origFinished.call(this);
                    };
                }

                // No per-test wrapping - will fix root cause instead
                // Also patch checkForBackground to dump stuck tasks
                var _origCheck = _patchTarget.prototype.checkForBackground;
                var _checkCount = 0;
                _patchTarget.prototype.checkForBackground = function() {
                    _checkCount++;
                    if (_checkCount % 50 === 1) {
                        var bg = this.background || [];
                        if (bg.length > 0) {
                            var keys = [];
                            for (var i = 0; i < bg.length; i++) {
                                keys.push(bg[i].key || bg[i].name || 'item-' + i);
                            }
                            console.warn('[BG-STUCK] count=' + bg.length + ' keys=' + keys.join(', '));
                        } else {
                            console.warn('[BG-CHECK] no background tasks, but still polling? bgCount=' + this.backgroundCount);
                        }
                    }
                    return _origCheck.call(this);
                };
            } else {
                console.warn('[DEBUG] No Runner or Test found!');
            }
        )JS", "<debug>");

        // Execute inline scripts
        for (size_t i = 0; i < doc->scripts.size(); i++) {
            fprintf(stderr, "[JS-TRACE] Eval inline[%zu] (%zu bytes)\n", i, doc->scripts[i].size());
            auto script_el = doc->createElement("script");
            if (script_parent) doc->appendChild(script_parent, script_el);
            engine->has_current_script = true;
            engine->current_script_src.clear();
            engine->current_script_node = script_el.get();
            bool ok = engine->eval(doc->scripts[i], "<script>");
            fprintf(stderr, "[JS-TRACE]   => %s\n", ok ? "OK" : "FAILED");
            engine->has_current_script = false;
            engine->current_script_node = nullptr;
        }

        // Execute pending microtasks after all scripts
        engine->executePendingJobs();

        // Fire DOMContentLoaded and load events on window listeners
        {
            extern void js_dispatch_to_window_listeners(JSEngine* engine, const std::string& type, JSValue event);
            extern JSValue js_create_event(JSContext* ctx, const std::string& type, DOMNode* target, int clientX, int clientY);
            JSValue dcl_event = js_create_event(engine->ctx, "DOMContentLoaded", nullptr, 0, 0);
            js_dispatch_to_window_listeners(engine, "DOMContentLoaded", dcl_event);
            JS_FreeValue(engine->ctx, dcl_event);
            JSValue load_event = js_create_event(engine->ctx, "load", nullptr, 0, 0);
            js_dispatch_to_window_listeners(engine, "load", load_event);
            JS_FreeValue(engine->ctx, load_event);
            engine->executePendingJobs();
        }

        // Redraw tab bar for updated title
        if (st->tab_bar_area) gtk_widget_queue_draw(st->tab_bar_area);

        // Run C++ DOM probes if --test mode
        if (g_test_mode) {
            g_timeout_add(800, [](gpointer data) -> gboolean {
                auto* st = static_cast<AppState*>(data);
                auto* tab = st->ct;
                if (tab && tab->js_engine) tab->js_engine->executePendingJobs();
                run_test_probes(st);
                return G_SOURCE_REMOVE;
            }, st);
        }
    });
}

// ---- navigate ----

static void update_nav_buttons(AppState* st) {
    auto* tab = st->ct;
    if (!tab) return;
    gtk_widget_set_sensitive(st->btn_back, !tab->back_stack.empty());
    gtk_widget_set_sensitive(st->btn_fwd,  !tab->fwd_stack.empty());
}

// load_url: fetch without touching history
static void load_url(AppState* st, const std::string& url) {
    auto* tab = st->ct;
    if (!tab) return;
    gtk_entry_set_text(GTK_ENTRY(st->address_bar), url.c_str());

    int gen;
    { std::lock_guard<std::mutex> lk(tab->mu); gen = ++tab->generation; }

    // destroy JS engine from previous page
    if (tab->js_engine) {
        delete tab->js_engine;
        tab->js_engine = nullptr;
    }
    tab->document.reset();

    // Clean up canvas surfaces
    for (auto& [nid, cs] : g_canvas_map) {
        if (cs.surface) cairo_surface_destroy(cs.surface);
    }
    g_canvas_map.clear();

    // clean up previous body styles from content_box
    if (tab->body_draw_signal) {
        g_signal_handler_disconnect(tab->content_box, tab->body_draw_signal);
        tab->body_draw_signal = 0;
        gtk_widget_set_app_paintable(tab->content_box, FALSE);
        g_object_set_data(G_OBJECT(tab->content_box), "bg_pb", nullptr);
        g_object_set_data(G_OBJECT(tab->content_box), "bg_color_str", nullptr);
    }
    gtk_widget_set_margin_top(tab->content_box, 0);
    gtk_widget_set_margin_end(tab->content_box, 0);
    gtk_widget_set_margin_bottom(tab->content_box, 0);
    gtk_widget_set_margin_start(tab->content_box, 0);

    GList* ch = gtk_container_get_children(GTK_CONTAINER(tab->content_box));
    for (GList* l=ch; l; l=l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);

    gtk_window_set_title(GTK_WINDOW(st->window), ("Loading "+url+"...").c_str());
    tab->title = "Loading...";
    if (st->tab_bar_area) gtk_widget_queue_draw(st->tab_bar_area);
    std::thread(fetch_page, st, tab, url, gen).detach();
}

static void navigate(AppState* st, const std::string& raw) {
    auto* tab = st->ct;
    if (!tab) return;
    std::string url = normalize_url(raw);
    if (!tab->current_url.empty()) tab->back_stack.push_back(tab->current_url);
    tab->fwd_stack.clear();
    tab->current_url = url;
    update_nav_buttons(st);
    load_url(st, url);
}

static void on_go(GtkButton*, gpointer d) {
    auto* st = static_cast<AppState*>(d);
    navigate(st, gtk_entry_get_text(GTK_ENTRY(st->address_bar)));
}
static void on_activate(GtkEntry*, gpointer d) {
    auto* st = static_cast<AppState*>(d);
    navigate(st, gtk_entry_get_text(GTK_ENTRY(st->address_bar)));
}
static void on_back(GtkButton*, gpointer d) {
    auto* st = static_cast<AppState*>(d);
    auto* tab = st->ct;
    if (!tab || tab->back_stack.empty()) return;
    if (!tab->current_url.empty()) tab->fwd_stack.push_back(tab->current_url);
    tab->current_url = tab->back_stack.back(); tab->back_stack.pop_back();
    update_nav_buttons(st);
    load_url(st, tab->current_url);
}
static void on_fwd(GtkButton*, gpointer d) {
    auto* st = static_cast<AppState*>(d);
    auto* tab = st->ct;
    if (!tab || tab->fwd_stack.empty()) return;
    if (!tab->current_url.empty()) tab->back_stack.push_back(tab->current_url);
    tab->current_url = tab->fwd_stack.back(); tab->fwd_stack.pop_back();
    update_nav_buttons(st);
    load_url(st, tab->current_url);
}
static void on_refresh(GtkButton*, gpointer d) {
    auto* st = static_cast<AppState*>(d);
    auto* tab = st->ct;
    if (tab && !tab->current_url.empty()) load_url(st, tab->current_url);
}

// ---- C++ DOM probes (--test mode) ----
// These verify JS engine results by inspecting the DOM tree directly from C++.
// No JavaScript is used in any verification — only raw DOMNode field reads.

static void probe_check(const char* name, bool cond) {
    if (cond) {
        fprintf(stderr, "  \033[32mPASS\033[0m  %s\n", name);
        g_probe_pass++;
    } else {
        fprintf(stderr, "  \033[31mFAIL\033[0m  %s\n", name);
        g_probe_fail++;
    }
}

// Helper: get text content of a node by id (walks DOM tree, no JS)
static std::string probe_text(Document* doc, const std::string& id) {
    DOMNode* n = doc->getElementById(id);
    return n ? n->getTextContent() : "";
}

// Helper: get a DOMNode by id
static DOMNode* probe_node(Document* doc, const std::string& id) {
    return doc->getElementById(id);
}

// Phase 1: probes that run right after scripts finish (checks initial JS execution)
static void run_probes_phase1(AppState* st) {
    auto* tab = st->ct;
    if (!tab) return;
    Document* doc = tab->document.get();
    if (!doc) { fprintf(stderr, "FAIL: no document\n"); return; }

    fprintf(stderr, "\n\033[1m=== C++ DOM Probes: Phase 1 (post-script) ===\033[0m\n");

    // -- Probe: document.body exists
    probe_check("document.body exists", doc->body != nullptr);

    // -- Probe: getElementById works (node exists in id_map)
    probe_check("getElementById('query-target') exists", doc->getElementById("query-target") != nullptr);
    probe_check("getElementById('nonexistent') is null", doc->getElementById("nonexistent") == nullptr);

    // -- Probe: textContent was mutated by JS
    {
        std::string t = probe_text(doc, "text-target");
        probe_check("textContent set: 'Modified via textContent'",
            t == "Modified via textContent");
    }

    // -- Probe: query-result was filled by JS (contains 'PASS')
    {
        std::string t = probe_text(doc, "query-result");
        probe_check("query-result contains 'PASS'", t.find("PASS") != std::string::npos);
        probe_check("query-result contains 'getElementById'", t.find("getElementById") != std::string::npos);
    }

    // -- Probe: style-box textContent changed to "Red"
    probe_check("style-box text is 'Red'", probe_text(doc, "style-box") == "Red");
    probe_check("style-box2 text is 'Green'", probe_text(doc, "style-box2") == "Green");
    probe_check("style-box3 text is 'Blue'", probe_text(doc, "style-box3") == "Blue");

    // -- Probe: style-box has background-color set via JS
    {
        DOMNode* box = probe_node(doc, "style-box");
        probe_check("style-box style_props has background-color",
            box && box->style_props.count("background-color") > 0);
        if (box && box->style_props.count("background-color"))
            probe_check("style-box background-color is '#e74c3c'",
                box->style_props["background-color"] == "#e74c3c");
    }

    // -- Probe: classList mutations
    {
        DOMNode* ct = probe_node(doc, "class-target");
        probe_check("class-target exists", ct != nullptr);
        if (ct) {
            probe_check("class-target has 'new-class'", ct->hasClass("new-class"));
            probe_check("class-target lost 'original-class'", !ct->hasClass("original-class"));
            probe_check("class-target lost 'toggled-class'", !ct->hasClass("toggled-class"));
        }
    }

    // -- Probe: classList result text
    {
        std::string t = probe_text(doc, "class-result");
        probe_check("class-result all PASS", t.find("FAIL") == std::string::npos && t.find("PASS") != std::string::npos);
    }

    // -- Probe: createElement appended children to creation-container
    {
        DOMNode* cc = probe_node(doc, "creation-container");
        probe_check("creation-container exists", cc != nullptr);
        if (cc) {
            probe_check("creation-container has >= 3 children (div+p+text)",
                cc->children.size() >= 3);
            // First child should be a div with text "Dynamically created div"
            if (cc->children.size() >= 1) {
                probe_check("first child is <div>",
                    cc->children[0]->tag_name == "div");
                probe_check("first child text is 'Dynamically created div'",
                    cc->children[0]->getTextContent() == "Dynamically created div");
            }
            // Second child should be a p
            if (cc->children.size() >= 2) {
                probe_check("second child is <p>",
                    cc->children[1]->tag_name == "p");
            }
            // Third child should be a text node
            if (cc->children.size() >= 3) {
                probe_check("third child is TEXT node",
                    cc->children[2]->node_type == DOMNode::TEXT);
                probe_check("third child text is 'A raw text node'",
                    cc->children[2]->text_content == "A raw text node");
            }
        }
    }

    // -- Probe: attributes
    {
        DOMNode* at = probe_node(doc, "attr-target");
        probe_check("attr-target exists", at != nullptr);
        if (at) {
            // setAttribute("data-custom","hello-world") should have stuck
            probe_check("attr-target has data-custom='hello-world'",
                at->attributes.count("data-custom") && at->attributes["data-custom"] == "hello-world");
            // removeAttribute("data-name") should have removed it
            probe_check("attr-target lost data-name",
                at->attributes.count("data-name") == 0);
            // data-value should still be "42"
            probe_check("attr-target still has data-value='42'",
                at->attributes.count("data-value") && at->attributes["data-value"] == "42");
        }
    }

    // -- Probe: attr-result text contains all PASS
    {
        std::string t = probe_text(doc, "attr-result");
        probe_check("attr-result all PASS", t.find("FAIL") == std::string::npos && t.find("PASS") != std::string::npos);
    }

    // -- Probe: tree traversal
    {
        DOMNode* tp = probe_node(doc, "tree-parent");
        probe_check("tree-parent exists", tp != nullptr);
        if (tp) {
            // Count element children (skip text nodes)
            int elem_count = 0;
            for (auto& c : tp->children)
                if (c->node_type == DOMNode::ELEMENT) elem_count++;
            probe_check("tree-parent has 3 element children", elem_count == 3);

            DOMNode* c1 = probe_node(doc, "tree-child1");
            if (c1) {
                probe_check("tree-child1 parent is tree-parent",
                    c1->parent == tp);
            }
        }
    }

    // -- Probe: tree-result text
    {
        std::string t = probe_text(doc, "tree-result");
        probe_check("tree-result all PASS",
            t.find("FAIL") == std::string::npos && t.find("PASS") != std::string::npos);
    }

    // -- Probe: event listeners were attached
    {
        DOMNode* btn = probe_node(doc, "click-count-btn");
        probe_check("click-count-btn exists", btn != nullptr);
        if (btn) {
            fprintf(stderr, "[DEBUG probe] click-count-btn node_id=%u listeners=%zu tag=%s addr=%p\n",
                    btn->node_id, btn->listeners.size(), btn->tag_name.c_str(), (void*)btn);
            probe_check("click-count-btn has >= 1 listener",
                btn->listeners.size() >= 1);
            if (!btn->listeners.empty())
                probe_check("click-count-btn listener type is 'click'",
                    btn->listeners[0].type == "click");
        }
    }

    // -- Probe: console log captured entries
    if (tab->js_engine) {
        auto& log = tab->js_engine->console_log;
        probe_check("console_log has >= 10 entries", log.size() >= 10);
        // Check for specific messages
        bool found_log = false, found_warn = false, found_info = false;
        for (auto& e : log) {
            if (e.level == ConsoleLevel::LOG && e.message.find("Test 1") != std::string::npos) found_log = true;
            if (e.level == ConsoleLevel::WARN) found_warn = true;
            if (e.level == ConsoleLevel::INFO) found_info = true;
        }
        probe_check("console captured LOG entry 'Test 1...'", found_log);
        probe_check("console captured WARN entry", found_warn);
        probe_check("console captured INFO entry", found_info);
    }

    // -- Probe: querySelectorAll via Document C++ API
    {
        auto results = doc->querySelectorAll(".test-section");
        probe_check("querySelectorAll('.test-section') finds >= 10 nodes",
            results.size() >= 10);
    }

    // -- Probe: node_map integrity
    {
        bool ok = true;
        for (auto& [id, node] : doc->node_map) {
            if (node->node_id != id) { ok = false; break; }
        }
        probe_check("node_map IDs are consistent", ok);
    }

    // -- Probe: toggle-target initial inline style
    {
        DOMNode* tt = probe_node(doc, "toggle-target");
        probe_check("toggle-target exists", tt != nullptr);
        if (tt) {
            probe_check("toggle-target has padding in inline_style_raw",
                tt->inline_style_raw.find("padding") != std::string::npos);
        }
    }
}

// Phase 2: simulate a click on click-count-btn and verify DOM changes
static void run_probes_phase2(AppState* st) {
    auto* tab = st->ct; if (!tab) return;
    Document* doc = tab->document.get();
    if (!doc || !tab->js_engine) return;

    fprintf(stderr, "\n\033[1m=== C++ DOM Probes: Phase 2 (after simulated click) ===\033[0m\n");

    // Find click-count-btn and simulate a click via the event system
    DOMNode* btn = probe_node(doc, "click-count-btn");
    probe_check("click-count-btn exists for click sim", btn != nullptr);
    if (!btn) return;

    std::string before = btn->getTextContent();
    probe_check("click-count-btn text before click is 'Count: 0'", before == "Count: 0");

    // Dispatch click event from C++ (not JS)
    tab->js_engine->dispatchEvent(btn->node_id, "click", 0, 0);
    tab->js_engine->executePendingJobs();

    std::string after1 = btn->getTextContent();
    probe_check("click-count-btn text after 1 click is 'Count: 1'", after1 == "Count: 1");

    // Click again
    tab->js_engine->dispatchEvent(btn->node_id, "click", 0, 0);
    tab->js_engine->executePendingJobs();

    std::string after2 = btn->getTextContent();
    probe_check("click-count-btn text after 2 clicks is 'Count: 2'", after2 == "Count: 2");

    // After 2 clicks, event-result should be updated
    std::string ev_result = probe_text(doc, "event-result");
    probe_check("event-result updated after click",
        ev_result.find("Clicked") != std::string::npos || ev_result.find("time") != std::string::npos);
}

// Phase 3: simulate toggle-style click and verify style changes
static void run_probes_phase3(AppState* st) {
    auto* tab = st->ct; if (!tab) return;
    Document* doc = tab->document.get();
    if (!doc || !tab->js_engine) return;

    fprintf(stderr, "\n\033[1m=== C++ DOM Probes: Phase 3 (style toggle) ===\033[0m\n");

    DOMNode* toggle_btn = probe_node(doc, "toggle-style");
    DOMNode* target = probe_node(doc, "toggle-target");
    probe_check("toggle-style btn exists", toggle_btn != nullptr);
    probe_check("toggle-target exists", target != nullptr);
    if (!toggle_btn || !target) return;

    // Click toggle
    tab->js_engine->dispatchEvent(toggle_btn->node_id, "click", 0, 0);
    tab->js_engine->executePendingJobs();

    probe_check("toggle-target bg changed to '#e74c3c'",
        target->style_props.count("background-color") &&
        target->style_props["background-color"] == "#e74c3c");
    probe_check("toggle-target text after toggle ON",
        target->getTextContent().find("toggled ON") != std::string::npos);

    // Toggle back
    tab->js_engine->dispatchEvent(toggle_btn->node_id, "click", 0, 0);
    tab->js_engine->executePendingJobs();

    probe_check("toggle-target bg changed back to '#3498db'",
        target->style_props.count("background-color") &&
        target->style_props["background-color"] == "#3498db");
    probe_check("toggle-target text after toggle OFF",
        target->getTextContent().find("toggled OFF") != std::string::npos);
}

// Phase 4: todo list add/clear via simulated clicks
static void run_probes_phase4(AppState* st) {
    auto* tab = st->ct; if (!tab) return;
    Document* doc = tab->document.get();
    if (!doc || !tab->js_engine) return;

    fprintf(stderr, "\n\033[1m=== C++ DOM Probes: Phase 4 (todo list) ===\033[0m\n");

    DOMNode* add_btn = probe_node(doc, "add-todo");
    DOMNode* clear_btn = probe_node(doc, "clear-todos");
    DOMNode* list = probe_node(doc, "todo-list");
    probe_check("add-todo btn exists", add_btn != nullptr);
    probe_check("todo-list exists", list != nullptr);
    if (!add_btn || !list) return;

    // Add 3 items
    for (int i = 0; i < 3; i++) {
        tab->js_engine->dispatchEvent(add_btn->node_id, "click", 0, 0);
        tab->js_engine->executePendingJobs();
    }

    int li_count = 0;
    for (auto& c : list->children)
        if (c->node_type == DOMNode::ELEMENT && c->tag_name == "li") li_count++;
    probe_check("todo-list has 3 <li> children after 3 adds", li_count == 3);

    // Check todo-count text
    std::string count_text = probe_text(doc, "todo-count");
    probe_check("todo-count shows 'Items: 3'", count_text == "Items: 3");

    // Verify first li has content
    if (!list->children.empty()) {
        std::string li_text = list->children[0]->getTextContent();
        probe_check("first <li> has 'Todo item' text",
            li_text.find("Todo item") != std::string::npos);
        // Verify data-id attribute was set
        probe_check("first <li> has data-id attribute",
            list->children[0]->attributes.count("data-id") > 0);
    }

    // Clear all
    if (clear_btn) {
        tab->js_engine->dispatchEvent(clear_btn->node_id, "click", 0, 0);
        tab->js_engine->executePendingJobs();
    }

    li_count = 0;
    for (auto& c : list->children)
        if (c->node_type == DOMNode::ELEMENT && c->tag_name == "li") li_count++;
    probe_check("todo-list empty after clear", li_count == 0);

    count_text = probe_text(doc, "todo-count");
    probe_check("todo-count shows 'Items: 0' after clear", count_text == "Items: 0");
}

// Phase 5: error handling — trigger errors and verify they land in console_log
static void run_probes_phase5(AppState* st) {
    auto* tab = st->ct; if (!tab) return;
    Document* doc = tab->document.get();
    if (!doc || !tab->js_engine) return;

    fprintf(stderr, "\n\033[1m=== C++ DOM Probes: Phase 5 (error capture) ===\033[0m\n");

    size_t log_before = tab->js_engine->console_log.size();

    // Click trigger-error button (causes ReferenceError)
    DOMNode* err_btn = probe_node(doc, "trigger-error");
    if (err_btn) {
        tab->js_engine->dispatchEvent(err_btn->node_id, "click", 0, 0);
        tab->js_engine->executePendingJobs();
    }

    size_t log_after = tab->js_engine->console_log.size();
    probe_check("ReferenceError captured in console_log", log_after > log_before);
    if (log_after > log_before) {
        auto& last = tab->js_engine->console_log.back();
        probe_check("error entry level is ERROR", last.level == ConsoleLevel::ERROR);
        probe_check("error message mentions 'not defined' or similar",
            last.message.find("not defined") != std::string::npos ||
            last.message.find("ReferenceError") != std::string::npos);
    }

    // Click trigger-type-error button
    log_before = tab->js_engine->console_log.size();
    DOMNode* terr_btn = probe_node(doc, "trigger-type-error");
    if (terr_btn) {
        tab->js_engine->dispatchEvent(terr_btn->node_id, "click", 0, 0);
        tab->js_engine->executePendingJobs();
    }

    log_after = tab->js_engine->console_log.size();
    probe_check("TypeError captured in console_log", log_after > log_before);

    // Click log-all and verify 4 new entries
    log_before = tab->js_engine->console_log.size();
    DOMNode* log_btn = probe_node(doc, "log-all");
    if (log_btn) {
        tab->js_engine->dispatchEvent(log_btn->node_id, "click", 0, 0);
        tab->js_engine->executePendingJobs();
    }

    log_after = tab->js_engine->console_log.size();
    probe_check("log-all added 4 entries", log_after - log_before == 4);
    if (log_after - log_before >= 4) {
        auto& entries = tab->js_engine->console_log;
        probe_check("log-all: LOG level present",
            entries[log_before].level == ConsoleLevel::LOG);
        probe_check("log-all: INFO level present",
            entries[log_before+1].level == ConsoleLevel::INFO);
        probe_check("log-all: WARN level present",
            entries[log_before+2].level == ConsoleLevel::WARN);
        probe_check("log-all: ERROR level present",
            entries[log_before+3].level == ConsoleLevel::ERROR);
    }
}

// Phase 6: 50 new DOM probes across tests 15-40
static void run_probes_phase6(AppState* st) {
    auto* tab = st->ct; if (!tab) return;
    Document* doc = tab->document.get();
    if (!doc || !tab->js_engine) return;

    fprintf(stderr, "\n\033[1m=== C++ DOM Probes: Phase 6 (extended tests 15-40) ===\033[0m\n");

    // T15: Nested innerHTML
    {
        DOMNode* np = probe_node(doc, "nest-p");
        probe_check("T15: nest-p found via getElementById after innerHTML", np != nullptr);
        if (np) probe_check("T15: nest-p textContent='Hello World'",
            np->getTextContent() == "Hello World");
        DOMNode* outer = probe_node(doc, "nest-outer");
        if (outer) {
            int ec = 0; for (auto& c : outer->children) if (c->node_type == DOMNode::ELEMENT) ec++;
            probe_check("T15: nest-outer has 2 element children (p + ul)", ec == 2);
        }
    }

    // T16: classList bulk
    {
        DOMNode* mc = probe_node(doc, "multi-class");
        probe_check("T16: multi-class exists", mc != nullptr);
        if (mc) {
            probe_check("T16: has base-cls", mc->hasClass("base-cls"));
            probe_check("T16: has cls-a", mc->hasClass("cls-a"));
            probe_check("T16: has cls-c", mc->hasClass("cls-c"));
            probe_check("T16: lost cls-b", !mc->hasClass("cls-b"));
            probe_check("T16: lost cls-d (toggled off)", !mc->hasClass("cls-d"));
        }
    }

    // T17: Deep nesting
    {
        DOMNode* leaf = probe_node(doc, "deep-leaf");
        probe_check("T17: deep-leaf found", leaf != nullptr);
        if (leaf) {
            int depth = 0; DOMNode* p = leaf;
            while (p->parent) { p = p->parent; depth++; }
            probe_check("T17: depth >= 5", depth >= 5);
            probe_check("T17: leaf text='Leaf'", leaf->getTextContent() == "Leaf");
        }
    }

    // T18: Sibling navigation
    {
        DOMNode* a = probe_node(doc, "sib-a");
        DOMNode* c = probe_node(doc, "sib-c");
        DOMNode* list = probe_node(doc, "sib-list");
        probe_check("T18: sib-a exists", a != nullptr);
        probe_check("T18: sib-c exists", c != nullptr);
        if (list) {
            int li_cnt = 0;
            for (auto& ch : list->children) if (ch->node_type == DOMNode::ELEMENT) li_cnt++;
            probe_check("T18: sib-list has 3 <li> children", li_cnt == 3);
            probe_check("T18: firstChild='A'", !list->children.empty() && list->children.front()->getTextContent() == "A");
            probe_check("T18: lastChild='C'", !list->children.empty() && list->children.back()->getTextContent() == "C");
        }
    }

    // T19: Style chaining
    {
        DOMNode* sc = probe_node(doc, "style-chain");
        probe_check("T19: style-chain exists", sc != nullptr);
        if (sc) {
            probe_check("T19: bg-color set", sc->style_props.count("background-color") && sc->style_props["background-color"] == "#2ecc71");
            probe_check("T19: color set", sc->style_props.count("color") && sc->style_props["color"] == "#fff");
            probe_check("T19: border-radius set", sc->style_props.count("border-radius") && sc->style_props["border-radius"] == "8px");
            probe_check("T19: font-weight set", sc->style_props.count("font-weight") && sc->style_props["font-weight"] == "bold");
        }
    }

    // T20: Edge cases
    {
        DOMNode* ed = probe_node(doc, "empty-div");
        probe_check("T20: empty-div exists", ed != nullptr);
        if (ed) probe_check("T20: empty-div now='now filled'", ed->getTextContent() == "now filled");
        DOMNode* sp = probe_node(doc, "special-chars");
        probe_check("T20: special-chars exists", sp != nullptr);
        if (sp) probe_check("T20: special-chars has content", !sp->getTextContent().empty());
    }

    // T21: NodeList forEach
    {
        DOMNode* fp = probe_node(doc, "foreach-parent");
        probe_check("T21: foreach-parent exists", fp != nullptr);
        if (fp) {
            int span_cnt = 0;
            for (auto& c : fp->children) if (c->node_type == DOMNode::ELEMENT && c->tag_name == "span") span_cnt++;
            probe_check("T21: 4 span children", span_cnt == 4);
        }
        std::string fr = probe_text(doc, "foreach-result");
        probe_check("T21: forEach result contains PASS", fr.find("PASS") != std::string::npos);
    }

    // T22: createElement variety
    {
        DOMNode* cv = probe_node(doc, "create-variety");
        probe_check("T22: create-variety exists", cv != nullptr);
        if (cv) {
            int ec = 0; for (auto& c : cv->children) if (c->node_type == DOMNode::ELEMENT) ec++;
            probe_check("T22: 5 created elements", ec == 5);
            if (ec >= 5) {
                probe_check("T22: first is div", cv->children[0]->tag_name == "div");
                probe_check("T22: last is li", cv->children[4]->tag_name == "li");
            }
        }
    }

    // T23: Data attributes
    {
        DOMNode* dd = probe_node(doc, "data-div");
        probe_check("T23: data-div exists", dd != nullptr);
        if (dd) {
            probe_check("T23: data-x=10", dd->attributes.count("data-x") && dd->attributes["data-x"] == "10");
            probe_check("T23: data-new=42 (JS-set)", dd->attributes.count("data-new") && dd->attributes["data-new"] == "42");
            probe_check("T23: data-flag removed", dd->attributes.count("data-flag") == 0);
        }
    }

    // T24: Selector combinators
    {
        std::string sr = probe_text(doc, "sel-result");
        probe_check("T24: sel-result has PASS", sr.find("PASS") != std::string::npos);
        auto inners = doc->querySelectorAll(".sel-inner");
        probe_check("T24: querySelectorAll .sel-inner finds 2", inners.size() == 2);
    }

    // T26: textContent on nested
    {
        DOMNode* tc = probe_node(doc, "tc-nested");
        probe_check("T26: tc-nested exists", tc != nullptr);
        if (tc) {
            probe_check("T26: text='Replaced all'", tc->getTextContent() == "Replaced all");
            int ec = 0; for (auto& c : tc->children) if (c->node_type == DOMNode::ELEMENT) ec++;
            probe_check("T26: children cleared (0 elements)", ec == 0);
        }
    }

    // T27: className
    {
        DOMNode* cn = probe_node(doc, "cn-target");
        probe_check("T27: cn-target exists", cn != nullptr);
        if (cn) {
            probe_check("T27: has 'gamma'", cn->hasClass("gamma"));
            probe_check("T27: has 'delta'", cn->hasClass("delta"));
            probe_check("T27: has 'epsilon'", cn->hasClass("epsilon"));
            probe_check("T27: lost 'alpha'", !cn->hasClass("alpha"));
        }
    }

    // T28: parentNode chain
    {
        DOMNode* deep = probe_node(doc, "pc-deep");
        probe_check("T28: pc-deep exists", deep != nullptr);
        if (deep) {
            probe_check("T28: parent is pc-mid", deep->parent && deep->parent->id == "pc-mid");
            probe_check("T28: grandparent is pc-root",
                deep->parent && deep->parent->parent && deep->parent->parent->id == "pc-root");
        }
    }

    // T29: removeChild
    {
        DOMNode* rcp = probe_node(doc, "rc-parent");
        probe_check("T29: rc-parent exists", rcp != nullptr);
        if (rcp) {
            int ec = 0; for (auto& c : rcp->children) if (c->node_type == DOMNode::ELEMENT) ec++;
            probe_check("T29: 2 children after removeChild", ec == 2);
            probe_check("T29: first is C1", !rcp->children.empty() && rcp->children.front()->getTextContent() == "C1");
            probe_check("T29: last is C3", !rcp->children.empty() && rcp->children.back()->getTextContent() == "C3");
        }
    }

    // T30: innerHTML with attributes
    {
        DOMNode* ihn = probe_node(doc, "ih-new");
        probe_check("T30: ih-new found after innerHTML", ihn != nullptr);
        if (ihn) {
            probe_check("T30: data-val=99", ihn->attributes.count("data-val") && ihn->attributes["data-val"] == "99");
            probe_check("T30: has class ih-cls", ihn->hasClass("ih-cls"));
            probe_check("T30: text='Attributed'", ihn->getTextContent() == "Attributed");
        }
    }

    // T31: Style computed values
    {
        DOMNode* scb = probe_node(doc, "sc-box");
        probe_check("T31: sc-box exists", scb != nullptr);
        if (scb) {
            probe_check("T31: style.width set", scb->style_props.count("width") > 0);
            probe_check("T31: style.bg set", scb->style_props.count("background-color") > 0);
        }
    }

    // T33: nodeType
    {
        DOMNode* nte = probe_node(doc, "nt-elem");
        probe_check("T33: nt-elem exists", nte != nullptr);
        if (nte) {
            probe_check("T33: element nodeType=1", nte->node_type == DOMNode::ELEMENT);
            bool has_text = false;
            for (auto& c : nte->children) if (c->node_type == DOMNode::TEXT) { has_text = true; break; }
            probe_check("T33: has TEXT child", has_text);
        }
    }

    // T34: tagName
    {
        DOMNode* d = probe_node(doc, "tn-div");
        DOMNode* p = probe_node(doc, "tn-p");
        probe_check("T34: tn-div tag=div", d && d->tag_name == "div");
        probe_check("T34: tn-p tag=p", p && p->tag_name == "p");
    }

    // T35: Toggle chain
    {
        DOMNode* tc = probe_node(doc, "tog-chain");
        probe_check("T35: tog-chain exists", tc != nullptr);
        if (tc) probe_check("T35: has 'on' (toggled back on)", tc->hasClass("on"));
    }

    // T36: Rapid mutations
    {
        DOMNode* rm = probe_node(doc, "rapid-mut");
        probe_check("T36: rapid-mut exists", rm != nullptr);
        if (rm) {
            int ec = 0; for (auto& c : rm->children) if (c->node_type == DOMNode::ELEMENT) ec++;
            probe_check("T36: 20 children created", ec == 20);
            if (ec >= 20) {
                probe_check("T36: first text=item0", rm->children[0]->getTextContent() == "item0");
                probe_check("T36: last text=item19", rm->children[ec-1]->getTextContent() == "item19");
                probe_check("T36: child[10] data-idx=10",
                    rm->children[10]->attributes.count("data-idx") && rm->children[10]->attributes["data-idx"] == "10");
            }
        }
    }

    // T37: Large NodeList
    {
        DOMNode* ll = probe_node(doc, "large-list");
        probe_check("T37: large-list exists", ll != nullptr);
        if (ll) {
            int gen = 0;
            for (auto& c : ll->children)
                if (c->node_type == DOMNode::ELEMENT && c->hasClass("list-item-gen")) gen++;
            probe_check("T37: 50 .list-item-gen children", gen == 50);
        }
    }

    // T38: Overwrite innerHTML
    {
        DOMNode* ow = probe_node(doc, "overwrite-ih");
        probe_check("T38: overwrite-ih exists", ow != nullptr);
        if (ow) probe_check("T38: text='Final'", ow->getTextContent() == "Final");
    }

    // T39: Void elements in innerHTML
    {
        DOMNode* vi = probe_node(doc, "void-ih");
        probe_check("T39: void-ih exists", vi != nullptr);
        if (vi) {
            int ec = 0; for (auto& c : vi->children) if (c->node_type == DOMNode::ELEMENT) ec++;
            probe_check("T39: has >= 3 element children (br, br, hr)", ec >= 3);
            std::string t = vi->getTextContent();
            probe_check("T39: text contains Line1", t.find("Line1") != std::string::npos);
            probe_check("T39: text contains End", t.find("End") != std::string::npos);
        }
    }

    // T40: Re-parent node
    {
        DOMNode* src = probe_node(doc, "rp-src");
        DOMNode* dst = probe_node(doc, "rp-dst");
        DOMNode* child = probe_node(doc, "rp-child");
        probe_check("T40: rp-child exists", child != nullptr);
        if (src && dst && child) {
            int src_ec = 0; for (auto& c : src->children) if (c->node_type == DOMNode::ELEMENT) src_ec++;
            int dst_ec = 0; for (auto& c : dst->children) if (c->node_type == DOMNode::ELEMENT) dst_ec++;
            probe_check("T40: src has 0 children", src_ec == 0);
            probe_check("T40: dst has 1 child", dst_ec == 1);
            probe_check("T40: child parent is rp-dst", child->parent && child->parent->id == "rp-dst");
        }
    }
}

// Phase 7: new CSS/form feature probes (tests 41-48)
static void run_probes_phase7(AppState* st) {
    auto* tab = st->ct; if (!tab) return;
    Document* doc = tab->document.get();
    if (!doc) return;

    fprintf(stderr, "\n\033[1m=== C++ DOM Probes: Phase 7 (CSS/form features 41-48) ===\033[0m\n");

    // T41: text-transform (DOM node exists, text_transform field set)
    {
        DOMNode* el = probe_node(doc, "tt-upper");
        probe_check("T41: tt-upper exists", el != nullptr);
        if (el) probe_check("T41: tt-upper text_transform=1 (uppercase)",
            el->text_transform == 1);
        DOMNode* el2 = probe_node(doc, "tt-lower");
        probe_check("T41: tt-lower exists", el2 != nullptr);
        if (el2) probe_check("T41: tt-lower text_transform=2 (lowercase)",
            el2->text_transform == 2);
        DOMNode* el3 = probe_node(doc, "tt-cap");
        probe_check("T41: tt-cap exists", el3 != nullptr);
        if (el3) probe_check("T41: tt-cap text_transform=3 (capitalize)",
            el3->text_transform == 3);
    }

    // T42: font-family
    {
        DOMNode* el = probe_node(doc, "ff-serif");
        probe_check("T42: ff-serif exists", el != nullptr);
        if (el) probe_check("T42: ff-serif font_family contains 'serif'",
            el->font_family.find("serif") != std::string::npos);
        DOMNode* el2 = probe_node(doc, "ff-mono");
        probe_check("T42: ff-mono exists", el2 != nullptr);
        if (el2) probe_check("T42: ff-mono font_family contains 'monospace'",
            el2->font_family.find("monospace") != std::string::npos);
    }

    // T43: box-shadow + opacity
    {
        DOMNode* card = probe_node(doc, "bs-card");
        probe_check("T43: bs-card exists", card != nullptr);
        if (card) probe_check("T43: bs-card has box_shadow",
            !card->box_shadow.empty());
        DOMNode* op = probe_node(doc, "op-half");
        probe_check("T43: op-half exists", op != nullptr);
        if (op) probe_check("T43: op-half opacity=0.5",
            op->opacity >= 0.49 && op->opacity <= 0.51);
    }

    // T44: overflow hidden
    {
        DOMNode* el = probe_node(doc, "ov-hidden");
        probe_check("T44: ov-hidden exists", el != nullptr);
        if (el) probe_check("T44: ov-hidden overflow=1 (hidden)",
            el->overflow == 1);
    }

    // T45: Attribute selectors (via querySelectorAll from C++)
    {
        auto results = doc->querySelectorAll("[data-test-attr]");
        probe_check("T45: querySelectorAll [data-test-attr] finds >= 1", results.size() >= 1);
        auto results2 = doc->querySelectorAll("[data-test-val=\"hello\"]");
        probe_check("T45: querySelectorAll [data-test-val=hello] finds >= 1", results2.size() >= 1);
        auto results3 = doc->querySelectorAll("#as-has:first-child");
        // This might not be first-child, just testing it doesn't crash
        probe_check("T45: :first-child selector runs without crash", true);
    }

    // T46: Flexbox
    {
        DOMNode* row = probe_node(doc, "flex-row");
        probe_check("T46: flex-row exists", row != nullptr);
        if (row) {
            probe_check("T46: flex-row display=Flex",
                row->display == DOMNode::Display::Flex);
            probe_check("T46: flex-row flex_direction=0 (row)",
                row->flex_direction == 0);
            probe_check("T46: flex-row gap=10",
                row->gap == 10);
            int ec = 0; for (auto& c : row->children) if (c->node_type == DOMNode::ELEMENT) ec++;
            probe_check("T46: flex-row has 3 children", ec == 3);
        }
        DOMNode* col = probe_node(doc, "flex-col");
        probe_check("T46: flex-col exists", col != nullptr);
        if (col) probe_check("T46: flex-col flex_direction=1 (column)",
            col->flex_direction == 1);
    }

    // T47: Form widgets
    {
        DOMNode* text = probe_node(doc, "fw-text");
        probe_check("T47: fw-text exists", text != nullptr);
        if (text) {
            probe_check("T47: fw-text tag=input", text->tag_name == "input");
            probe_check("T47: fw-text value='initial'",
                text->attributes.count("value") && text->attributes["value"] == "initial");
        }
        DOMNode* check = probe_node(doc, "fw-check");
        probe_check("T47: fw-check exists", check != nullptr);
        if (check) probe_check("T47: fw-check has 'checked' attr",
            check->attributes.count("checked") > 0);
        DOMNode* sel = probe_node(doc, "fw-select");
        probe_check("T47: fw-select exists", sel != nullptr);
        if (sel) {
            int opt_count = 0;
            for (auto& c : sel->children) if (c->tag_name == "option") opt_count++;
            probe_check("T47: fw-select has 3 options", opt_count == 3);
        }
    }

    // T48: CSS positioning
    {
        DOMNode* rel = probe_node(doc, "pos-rel");
        probe_check("T48: pos-rel exists", rel != nullptr);
        if (rel) {
            probe_check("T48: pos-rel position=1 (relative)", rel->position == 1);
            probe_check("T48: pos-rel pos_top=10", rel->pos_top == 10);
            probe_check("T48: pos-rel pos_left=20", rel->pos_left == 20);
        }
    }

    // T41-result check
    {
        std::string t = probe_text(doc, "tt-result");
        probe_check("T41: tt-result text set", !t.empty() && t != "Waiting...");
    }

    // T45-result check
    {
        std::string t = probe_text(doc, "as-result");
        probe_check("T45: as-result contains PASS",
            t.find("PASS") != std::string::npos);
    }

    // T46-result check
    {
        std::string t = probe_text(doc, "flex-result");
        probe_check("T46: flex-result contains PASS",
            t.find("PASS") != std::string::npos);
    }

    // T47-result check
    {
        std::string t = probe_text(doc, "fw-result");
        probe_check("T47: fw-result contains PASS",
            t.find("PASS") != std::string::npos);
    }
}

// Phase 8: Font/text feature probes (tests 49-58)
static void run_probes_phase8(AppState* st) {
    auto* tab = st->ct; if (!tab) return;
    Document* doc = tab->document.get();
    if (!doc) return;

    fprintf(stderr, "\n\033[1m=== C++ DOM Probes: Phase 8 (Font/text features 49-58) ===\033[0m\n");

    // T49: font-style from CSS rules
    {
        DOMNode* el = probe_node(doc, "fi-css-italic");
        probe_check("T49: fi-css-italic exists", el != nullptr);
        if (el) probe_check("T49: fi-css-italic fi_computed=ITALIC",
            el->fi_computed == PANGO_STYLE_ITALIC);
        DOMNode* el2 = probe_node(doc, "fi-css-oblique");
        probe_check("T49: fi-css-oblique exists", el2 != nullptr);
        if (el2) probe_check("T49: fi-css-oblique fi_computed=OBLIQUE",
            el2->fi_computed == PANGO_STYLE_OBLIQUE);
    }

    // T50: text-decoration
    {
        DOMNode* ul = probe_node(doc, "td-underline");
        probe_check("T50: td-underline exists", ul != nullptr);
        if (ul) probe_check("T50: td-underline text_decoration has underline bit",
            (ul->text_decoration & 1) != 0);
        DOMNode* lt = probe_node(doc, "td-linethrough");
        probe_check("T50: td-linethrough exists", lt != nullptr);
        if (lt) probe_check("T50: td-linethrough text_decoration has line-through bit",
            (lt->text_decoration & 4) != 0);
        DOMNode* inl = probe_node(doc, "td-inline");
        probe_check("T50: td-inline exists", inl != nullptr);
        if (inl) probe_check("T50: td-inline text_decoration has underline bit",
            (inl->text_decoration & 1) != 0);
        DOMNode* none = probe_node(doc, "td-none");
        probe_check("T50: td-none exists", none != nullptr);
        if (none) probe_check("T50: td-none text_decoration=0 (none)",
            none->text_decoration == 0);
        DOMNode* lnone = probe_node(doc, "td-link-none");
        probe_check("T50: td-link-none exists", lnone != nullptr);
        if (lnone) probe_check("T50: td-link-none text_decoration=0 (CSS overrides link default)",
            lnone->text_decoration == 0);
    }

    // T51: letter-spacing
    {
        DOMNode* el = probe_node(doc, "ls-wide");
        probe_check("T51: ls-wide exists", el != nullptr);
        if (el) probe_check("T51: ls-wide letter_spacing = 5*PANGO_SCALE",
            el->letter_spacing == 5 * PANGO_SCALE);
        DOMNode* el2 = probe_node(doc, "ls-inline");
        probe_check("T51: ls-inline exists", el2 != nullptr);
        if (el2) probe_check("T51: ls-inline letter_spacing = 3*PANGO_SCALE",
            el2->letter_spacing == 3 * PANGO_SCALE);
    }

    // T52: font-variant / white-space
    {
        DOMNode* sc = probe_node(doc, "fv-smallcaps");
        probe_check("T52: fv-smallcaps exists", sc != nullptr);
        if (sc) probe_check("T52: fv-smallcaps font_variant=1 (small-caps)",
            sc->font_variant == 1);
        DOMNode* nw = probe_node(doc, "ws-nowrap");
        probe_check("T52: ws-nowrap exists", nw != nullptr);
        if (nw) probe_check("T52: ws-nowrap white_space=1 (nowrap)",
            nw->white_space == 1);
        DOMNode* pre = probe_node(doc, "ws-pre");
        probe_check("T52: ws-pre exists", pre != nullptr);
        if (pre) probe_check("T52: ws-pre white_space=2 (pre)",
            pre->white_space == 2);
    }

    // T53: Semantic tag UA defaults
    {
        DOMNode* s = probe_node(doc, "sem-s");
        probe_check("T53: sem-s exists", s != nullptr);
        if (s) probe_check("T53: sem-s text_decoration=4 (line-through)",
            s->text_decoration == 4);
        DOMNode* del_el = probe_node(doc, "sem-del");
        probe_check("T53: sem-del exists", del_el != nullptr);
        if (del_el) probe_check("T53: sem-del text_decoration=4 (line-through)",
            del_el->text_decoration == 4);
        DOMNode* u = probe_node(doc, "sem-u");
        probe_check("T53: sem-u exists", u != nullptr);
        if (u) probe_check("T53: sem-u text_decoration=1 (underline)",
            u->text_decoration == 1);
        DOMNode* ins = probe_node(doc, "sem-ins");
        probe_check("T53: sem-ins exists", ins != nullptr);
        if (ins) probe_check("T53: sem-ins text_decoration=1 (underline)",
            ins->text_decoration == 1);
        DOMNode* code = probe_node(doc, "sem-code");
        probe_check("T53: sem-code exists", code != nullptr);
        if (code) probe_check("T53: sem-code font_family contains 'monospace'",
            code->font_family.find("monospace") != std::string::npos);
        DOMNode* kbd = probe_node(doc, "sem-kbd");
        probe_check("T53: sem-kbd exists", kbd != nullptr);
        if (kbd) probe_check("T53: sem-kbd font_family contains 'monospace'",
            kbd->font_family.find("monospace") != std::string::npos);
        DOMNode* small_el = probe_node(doc, "sem-small");
        probe_check("T53: sem-small exists", small_el != nullptr);
        if (small_el) probe_check("T53: sem-small fs_computed < 16 (0.83em)",
            small_el->fs_computed < 16 && small_el->fs_computed > 0);
        DOMNode* big_el = probe_node(doc, "sem-big");
        probe_check("T53: sem-big exists", big_el != nullptr);
        if (big_el) probe_check("T53: sem-big fs_computed > 16 (1.17em)",
            big_el->fs_computed > 16);
        DOMNode* mark = probe_node(doc, "sem-mark");
        probe_check("T53: sem-mark exists", mark != nullptr);
        if (mark) probe_check("T53: sem-mark bg_color is yellow",
            mark->bg_color == "yellow");
        DOMNode* abbr = probe_node(doc, "sem-abbr");
        probe_check("T53: sem-abbr exists", abbr != nullptr);
        if (abbr) {
            probe_check("T53: sem-abbr text_decoration=1 (underline)",
                abbr->text_decoration == 1);
            probe_check("T53: sem-abbr text_decoration_style=2 (dotted)",
                abbr->text_decoration_style == 2);
        }
    }

    // T54: font shorthand
    {
        DOMNode* s1 = probe_node(doc, "fs-short1");
        probe_check("T54: fs-short1 exists", s1 != nullptr);
        if (s1) {
            probe_check("T54: fs-short1 fi_computed=ITALIC",
                s1->fi_computed == PANGO_STYLE_ITALIC);
            probe_check("T54: fs-short1 fw_computed=BOLD",
                s1->fw_computed == PANGO_WEIGHT_BOLD);
            probe_check("T54: fs-short1 fs_computed=20",
                s1->fs_computed == 20);
            probe_check("T54: fs-short1 font_family contains 'serif'",
                s1->font_family.find("serif") != std::string::npos);
        }
        DOMNode* s2 = probe_node(doc, "fs-short2");
        probe_check("T54: fs-short2 exists", s2 != nullptr);
        if (s2) {
            probe_check("T54: fs-short2 fs_computed=14",
                s2->fs_computed == 14);
            probe_check("T54: fs-short2 font_family contains 'monospace'",
                s2->font_family.find("monospace") != std::string::npos);
        }
    }

    // T55: JS Font Style API (check JS result text)
    {
        std::string t = probe_text(doc, "jsfs-result");
        probe_check("T55: jsfs-result contains PASS",
            t.find("PASS") != std::string::npos);
        probe_check("T55: jsfs-result has no FAIL",
            t.find("FAIL") == std::string::npos);
    }

    // T56: text-decoration sub-properties
    {
        DOMNode* color = probe_node(doc, "td-color");
        probe_check("T56: td-color exists", color != nullptr);
        if (color) probe_check("T56: td-color text_decoration_color is 'red'",
            color->text_decoration_color == "red");
        DOMNode* style = probe_node(doc, "td-style");
        probe_check("T56: td-style exists", style != nullptr);
        if (style) probe_check("T56: td-style text_decoration_style=2 (dotted)",
            style->text_decoration_style == 2);
        DOMNode* line = probe_node(doc, "td-line");
        probe_check("T56: td-line exists", line != nullptr);
        if (line) probe_check("T56: td-line text_decoration=4 (line-through)",
            (line->text_decoration & 4) != 0);
        // JS result
        std::string t = probe_text(doc, "td-sub-result");
        probe_check("T56: td-sub-result contains PASS",
            t.find("PASS") != std::string::npos);
    }

    // T57: text-indent / text-overflow / word-spacing
    {
        DOMNode* ti = probe_node(doc, "ti-30");
        probe_check("T57: ti-30 exists", ti != nullptr);
        if (ti) probe_check("T57: ti-30 text_indent=30",
            ti->text_indent == 30);
        DOMNode* to_el = probe_node(doc, "to-ellipsis");
        probe_check("T57: to-ellipsis exists", to_el != nullptr);
        if (to_el) probe_check("T57: to-ellipsis text_overflow=1",
            to_el->text_overflow == 1);
        DOMNode* ws = probe_node(doc, "wsp-wide");
        probe_check("T57: wsp-wide exists", ws != nullptr);
        if (ws) probe_check("T57: wsp-wide word_spacing=10",
            ws->word_spacing == 10);
        // JS result
        std::string t = probe_text(doc, "ti-to-result");
        probe_check("T57: ti-to-result contains PASS",
            t.find("PASS") != std::string::npos);
    }

    // T58: font-stretch / text-shadow
    {
        DOMNode* fst = probe_node(doc, "fst-condensed");
        probe_check("T58: fst-condensed exists", fst != nullptr);
        if (fst) probe_check("T58: fst-condensed font_stretch=CONDENSED",
            fst->font_stretch == PANGO_STRETCH_CONDENSED);
        DOMNode* tsh = probe_node(doc, "tsh-shadow");
        probe_check("T58: tsh-shadow exists", tsh != nullptr);
        if (tsh) probe_check("T58: tsh-shadow text_shadow not empty",
            !tsh->text_shadow.empty());
        // JS result
        std::string t = probe_text(doc, "fst-tsh-result");
        probe_check("T58: fst-tsh-result contains PASS",
            t.find("PASS") != std::string::npos);
    }
}

static void run_test_probes(AppState* st) {
    fprintf(stderr, "\n\033[1;36m╔══════════════════════════════════════════╗\033[0m\n");
    fprintf(stderr, "\033[1;36m║     C++ DOM PROBE TEST SUITE             ║\033[0m\n");
    fprintf(stderr, "\033[1;36m╚══════════════════════════════════════════╝\033[0m\n");

    run_probes_phase1(st);
    run_probes_phase2(st);
    run_probes_phase3(st);
    run_probes_phase4(st);
    run_probes_phase5(st);
    run_probes_phase6(st);
    run_probes_phase7(st);
    run_probes_phase8(st);

    fprintf(stderr, "\n\033[1m═══════════════════════════════════════════\033[0m\n");
    fprintf(stderr, "  Total: %d passed, %d failed, %d total\n",
        g_probe_pass, g_probe_fail, g_probe_pass + g_probe_fail);
    if (g_probe_fail == 0)
        fprintf(stderr, "  \033[32;1mALL TESTS PASSED\033[0m\n");
    else
        fprintf(stderr, "  \033[31;1m%d TESTS FAILED\033[0m\n", g_probe_fail);
    fprintf(stderr, "\033[1m═══════════════════════════════════════════\033[0m\n\n");

    // Exit after test
    gtk_main_quit();
}

// ---- main ----

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    gtk_init(&argc, &argv);

    AppState* st = new AppState();
    st->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(st->window), "Browser");
    gtk_window_set_default_size(GTK_WINDOW(st->window), 1100, 800);
    gtk_window_set_decorated(GTK_WINDOW(st->window), FALSE); // custom titlebar via tab bar
    g_signal_connect(st->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(st->window), root);

    // ---- Tab bar (Cairo-drawn, replaces OS titlebar) ----
    st->tab_bar_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(st->tab_bar_area, -1, TAB_BAR_HEIGHT);
    gtk_widget_add_events(st->tab_bar_area,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(st->tab_bar_area, "draw", G_CALLBACK(draw_tab_bar), st);
    g_signal_connect(st->tab_bar_area, "button-press-event", G_CALLBACK(tab_bar_press), st);
    g_signal_connect(st->tab_bar_area, "button-release-event", G_CALLBACK(tab_bar_release), st);
    g_signal_connect(st->tab_bar_area, "motion-notify-event", G_CALLBACK(tab_bar_motion), st);
    g_signal_connect(st->tab_bar_area, "leave-notify-event", G_CALLBACK(tab_bar_leave), st);

    // Style the tab bar background
    {
        GtkCssProvider* cp = gtk_css_provider_new();
        gtk_css_provider_load_from_data(cp, "drawingarea { background-color: #383838; }", -1, nullptr);
        gtk_style_context_add_provider(gtk_widget_get_style_context(st->tab_bar_area),
            GTK_STYLE_PROVIDER(cp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(cp);
    }
    gtk_box_pack_start(GTK_BOX(root), st->tab_bar_area, FALSE, FALSE, 0);

    // ---- Navigation bar ----
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start(bar, 4);
    gtk_widget_set_margin_end(bar, 4);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 4);
    gtk_box_pack_start(GTK_BOX(root), bar, FALSE, FALSE, 0);

    st->btn_back = gtk_button_new_with_label("\xe2\x86\x90"); // ←
    gtk_widget_set_sensitive(st->btn_back, FALSE);
    gtk_box_pack_start(GTK_BOX(bar), st->btn_back, FALSE, FALSE, 0);
    g_signal_connect(st->btn_back, "clicked", G_CALLBACK(on_back), st);

    st->btn_fwd = gtk_button_new_with_label("\xe2\x86\x92"); // →
    gtk_widget_set_sensitive(st->btn_fwd, FALSE);
    gtk_box_pack_start(GTK_BOX(bar), st->btn_fwd, FALSE, FALSE, 0);
    g_signal_connect(st->btn_fwd, "clicked", G_CALLBACK(on_fwd), st);

    GtkWidget* btn_refresh = gtk_button_new_with_label("\xe2\x86\xba"); // ↺
    gtk_box_pack_start(GTK_BOX(bar), btn_refresh, FALSE, FALSE, 4);
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh), st);

    st->address_bar = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(bar), st->address_bar, TRUE, TRUE, 0);
    g_signal_connect(st->address_bar, "activate", G_CALLBACK(on_activate), st);

    GtkWidget* go_btn = gtk_button_new_with_label("Go");
    gtk_box_pack_start(GTK_BOX(bar), go_btn, FALSE, FALSE, 4);
    g_signal_connect(go_btn, "clicked", G_CALLBACK(on_go), st);

    // ---- Content stack (holds per-tab paned widgets) ----
    st->content_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(st->content_stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_box_pack_start(GTK_BOX(root), st->content_stack, TRUE, TRUE, 0);

    // ---- Keyboard event handler ----
    g_signal_connect(st->window, "key-press-event",
        G_CALLBACK(+[](GtkWidget*, GdkEventKey* ev, gpointer d) -> gboolean {
            auto* st = static_cast<AppState*>(d);
            auto* tab = st->ct;
            bool ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
            bool shift = (ev->state & GDK_SHIFT_MASK) != 0;

            // Tab shortcuts
            if (ctrl && shift && (ev->keyval == GDK_KEY_N || ev->keyval == GDK_KEY_n)) {
                new_tab(st); return TRUE;
            }
            if (ctrl && (ev->keyval == GDK_KEY_W || ev->keyval == GDK_KEY_w)) {
                if (!st->tabs.empty()) close_tab(st, st->active_tab_idx);
                return TRUE;
            }
            if (ctrl && shift && (ev->keyval == GDK_KEY_T || ev->keyval == GDK_KEY_t)) {
                reopen_closed_tab(st); return TRUE;
            }
            if (ctrl && ev->keyval == GDK_KEY_Tab) {
                int next = (st->active_tab_idx + 1) % (int)st->tabs.size();
                switch_to_tab(st, next); return TRUE;
            }
            if (ctrl && ev->keyval == GDK_KEY_ISO_Left_Tab) { // Ctrl+Shift+Tab
                int prev = (st->active_tab_idx - 1 + (int)st->tabs.size()) % (int)st->tabs.size();
                switch_to_tab(st, prev); return TRUE;
            }
            // Ctrl+1-9: switch to tab by index
            if (ctrl && ev->keyval >= GDK_KEY_1 && ev->keyval <= GDK_KEY_9) {
                int idx = ev->keyval - GDK_KEY_1;
                if (ev->keyval == GDK_KEY_9) idx = (int)st->tabs.size() - 1;
                if (idx < (int)st->tabs.size()) switch_to_tab(st, idx);
                return TRUE;
            }
            // Ctrl+N: new window
            if (ctrl && !shift && (ev->keyval == GDK_KEY_N || ev->keyval == GDK_KEY_n)) {
                // Spawn new process
                char self[1024];
                ssize_t len = readlink("/proc/self/exe", self, sizeof(self)-1);
                if (len > 0) { self[len] = 0; fork() == 0 ? (execl(self, self, (char*)nullptr), exit(1)) : (void)0; }
                return TRUE;
            }

            if (ev->keyval == GDK_KEY_F12) {
                inspector_toggle(st);
                return TRUE;
            }

            // Dispatch keydown to JS
            if (tab && tab->js_engine && tab->document) {
                const char* keyname = gdk_keyval_name(ev->keyval);
                std::string key = keyname ? keyname : "";
                if (key == "Return") key = "Enter";
                else if (key == "Escape") key = "Escape";
                else if (key == "BackSpace") key = "Backspace";
                else if (key == "Tab") key = "Tab";
                else if (key == "space") key = " ";
                else if (key.size() > 1 && key.substr(0,5) == "Shift") key = "Shift";
                else if (key.size() > 1 && key.substr(0,7) == "Control") key = "Control";
                else if (key.size() > 1 && key.substr(0,3) == "Alt") key = "Alt";
                if (key.size() == 1 && !(ev->state & GDK_SHIFT_MASK))
                    key[0] = tolower((unsigned char)key[0]);

                std::string code = keyname ? std::string("Key") + (char)toupper((unsigned char)(keyname[0])) : "";
                if (ev->keyval >= GDK_KEY_0 && ev->keyval <= GDK_KEY_9)
                    code = std::string("Digit") + (char)('0' + (ev->keyval - GDK_KEY_0));
                else if (ev->keyval == GDK_KEY_space) code = "Space";
                else if (ev->keyval == GDK_KEY_Return) code = "Enter";
                else if (ev->keyval == GDK_KEY_Escape) code = "Escape";
                else if (ev->keyval == GDK_KEY_BackSpace) code = "Backspace";
                else if (ev->keyval == GDK_KEY_Tab) code = "Tab";
                else if (ev->keyval >= GDK_KEY_F1 && ev->keyval <= GDK_KEY_F12)
                    code = "F" + std::to_string(ev->keyval - GDK_KEY_F1 + 1);

                uint32_t target = tab->focused_node_id;
                if (!target && tab->document->body) target = tab->document->body->node_id;
                js_dispatch_key_event(tab->js_engine, target,
                    "keydown", key, code, ev->hardware_keycode,
                    (ev->state & GDK_SHIFT_MASK) != 0,
                    (ev->state & GDK_CONTROL_MASK) != 0,
                    (ev->state & GDK_MOD1_MASK) != 0,
                    (ev->state & GDK_META_MASK) != 0);
            }
            return FALSE;
        }), st);

    // key-release-event for keyup
    g_signal_connect(st->window, "key-release-event",
        G_CALLBACK(+[](GtkWidget*, GdkEventKey* ev, gpointer d) -> gboolean {
            auto* st = static_cast<AppState*>(d);
            auto* tab = st->ct;
            if (tab && tab->js_engine && tab->document) {
                const char* keyname = gdk_keyval_name(ev->keyval);
                std::string key = keyname ? keyname : "";
                if (key == "Return") key = "Enter";
                else if (key == "BackSpace") key = "Backspace";
                else if (key == "space") key = " ";
                if (key.size() == 1) key[0] = tolower((unsigned char)key[0]);
                std::string code = keyname ? std::string("Key") + (char)toupper((unsigned char)(keyname[0])) : "";
                uint32_t target = tab->focused_node_id;
                if (!target && tab->document->body) target = tab->document->body->node_id;
                js_dispatch_key_event(tab->js_engine, target,
                    "keyup", key, code, ev->hardware_keycode,
                    (ev->state & GDK_SHIFT_MASK) != 0,
                    (ev->state & GDK_CONTROL_MASK) != 0,
                    (ev->state & GDK_MOD1_MASK) != 0,
                    (ev->state & GDK_META_MASK) != 0);
            }
            return FALSE;
        }), st);

    // Allow double-click on tab bar to maximize/restore
    g_signal_connect(st->tab_bar_area, "button-press-event",
        G_CALLBACK(+[](GtkWidget*, GdkEventButton* ev, gpointer d) -> gboolean {
            if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1) {
                auto* st = static_cast<AppState*>(d);
                auto hit = hit_test_tab_bar(st, ev->x, ev->y);
                if (hit.type == HIT_EMPTY) {
                    if (gtk_window_is_maximized(GTK_WINDOW(st->window)))
                        gtk_window_unmaximize(GTK_WINDOW(st->window));
                    else
                        gtk_window_maximize(GTK_WINDOW(st->window));
                    return TRUE;
                }
            }
            return FALSE;
        }), st);

    gtk_widget_show_all(st->window);

    // Accept URL and flags from command line
    std::string start_url = "file:///mnt/1tb-ssd/random/browser/api_test.html";
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--test") { g_test_mode = true; continue; }
        if (!arg.empty() && arg[0] != '-') { start_url = arg; }
    }

    // Create first tab
    new_tab(st, start_url);

    gtk_main();
    curl_global_cleanup();
    delete st;
    return 0;
}
