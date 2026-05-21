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
#include <memory>
#include "dom.h"
#include "js_engine.h"
#include "js_event.h"

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
                    || !prop_val(decls,"color").empty()      || !prop_val(decls,"text-align").empty();
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
                             "input","link","meta","param","source","track","wbr",nullptr};
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

// Match a single simple selector token (tag, .class, #id, tag.class, .a.b, with optional :pseudo)
static bool simple_match(const std::string& raw, const StackEntry& e) {
    if (raw.empty() || raw=="*") return true;
    // strip pseudo-class
    std::string tok = raw;
    size_t colon = tok.find(':');
    if (colon != std::string::npos) tok = tok.substr(0, colon);
    if (tok.empty()) return true;
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
            if (!closing && !is_void(tname)) skip_depth++;
            else if (closing) { if (--skip_depth == 0) acc.clear(); }
            continue;
        }

        if (!closing && tname=="script") { state=SCRIPT_SKIP; continue; }
        if (!closing && tname=="style")  { state=STYLE_SKIP;  continue; }

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

    enum { NORM, SCRIPT_CAP, STYLE_SKIP, COMMENT } state = NORM;
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
            if (!closing && !is_void(tname)) skip_depth++;
            else if (closing) { if (--skip_depth == 0) acc.clear(); }
            continue;
        }

        if (!closing && tname == "script") {
            script_src = extract_attr(tag, "src");
            if (!script_src.empty()) script_src = resolve(base, script_src);
            state = SCRIPT_CAP; continue;
        }
        if (!closing && tname == "style") { state = STYLE_SKIP; continue; }

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

        if (!closing && !is_void(tname)) {
            int parent_fs = cur_parent ? cur_parent->fs_computed : 16;
            auto elem = doc->createElement(tname);
            elem->attributes = extract_all_attrs(tag);
            // Use properly parsed attributes for class/id
            auto cls_it = elem->attributes.find("class");
            elem->class_list = cls_it != elem->attributes.end() ? split_classes(cls_it->second) : std::vector<std::string>{};
            auto id_it = elem->attributes.find("id");
            elem->id = id_it != elem->attributes.end() ? tolower_s(id_it->second) : "";
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
            }

            // anchor href
            if (tname == "a") {
                auto href_it = elem->attributes.find("href");
                if (href_it != elem->attributes.end() && !href_it->second.empty())
                    elem->href = resolve(base, href_it->second);
            }

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

// ---- AppState ----

struct AppState {
    GtkWidget* window;
    GtkWidget* address_bar;
    GtkWidget* content_box;
    GtkWidget* viewport;
    GtkWidget* btn_back;
    GtkWidget* btn_fwd;
    std::mutex mu;
    int generation = 0;
    std::string current_url;
    std::vector<std::string> back_stack;
    std::vector<std::string> fwd_stack;
    gulong body_draw_signal = 0;

    // DOM tree for current page
    std::shared_ptr<Document> document;

    // JS engine for current page
    JSEngine* js_engine = nullptr;

    // Raw page source for inspector Elements tab
    std::string page_source;

    // Inspector panel
    GtkWidget* paned = nullptr;          // main horizontal pane
    GtkWidget* inspector_box = nullptr;  // the right panel container
    GtkWidget* inspector_notebook = nullptr; // tabs: Console, Elements
    GtkWidget* console_text_view = nullptr;  // Console tab text view
    GtkWidget* elements_text_view = nullptr; // Elements tab text view
    bool inspector_visible = false;
    int inspector_width = 500;
};

// ---- block container builder (main thread only) ----

static GtkWidget* make_block(const BoxModel& box, GtkWidget* parent,
                              GtkOrientation orient = GTK_ORIENTATION_VERTICAL,
                              bool to_end = false) {
    GtkWidget* outer = gtk_box_new(orient, 0);
    {
        std::string props;
        if (!box.bg_color.empty())
            props += "background-color: " + box.bg_color + "; ";
        if (box.border_radius > 0)
            props += "border-radius: " + std::to_string(box.border_radius) + "px; ";
        bool has_border = box.border_width[0]||box.border_width[1]||box.border_width[2]||box.border_width[3];
        if (has_border) {
            std::string bstyle = box.border_style.empty() ? "solid" : box.border_style;
            std::string bcolor = box.border_color.empty() ? "currentColor" : box.border_color;
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
        if (!props.empty()) {
            GtkCssProvider* cp = gtk_css_provider_new();
            gtk_css_provider_load_from_data(cp, ("box { " + props + "}").c_str(), -1, nullptr);
            gtk_style_context_add_provider(gtk_widget_get_style_context(outer),
                GTK_STYLE_PROVIDER(cp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(cp);
        }
    }
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

// ---- page fetch ----

static void navigate(AppState* st, const std::string& raw); // forward decl

static gboolean draw_bg(GtkWidget* w, cairo_t* cr, gpointer) {
    GdkPixbuf* pb    = (GdkPixbuf*)g_object_get_data(G_OBJECT(w), "bg_pb");
    const char* bgc  = (const char*)g_object_get_data(G_OBJECT(w), "bg_color_str");
    {
        FILE* f = fopen("/tmp/browser_debug.log","a");
        if (f) { fprintf(f,"draw_bg called: pb=%s bgc=%s\n", pb?"ok":"null", bgc?bgc:"none"); fclose(f); }
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
    // tile the image (background-repeat: repeat is the default)
    cairo_save(cr);
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    cairo_t* tc = cairo_create(surf);
    gdk_cairo_set_source_pixbuf(tc, pb, 0, 0);
    cairo_paint(tc);
    cairo_destroy(tc);
    cairo_pattern_t* pat = cairo_pattern_create_for_surface(surf);
    cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);
    cairo_surface_destroy(surf);
    cairo_restore(cr);
    return FALSE;
}

// ---- Inspector panel helpers ----

static void inspector_update_elements(AppState* st) {
    if (!st->elements_text_view) return;
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->elements_text_view));
    gtk_text_buffer_set_text(buf, st->page_source.c_str(), (gint)st->page_source.size());
}

static void inspector_append_console_entry(AppState* st, const ConsoleEntry& entry) {
    if (!st->console_text_view) return;
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->console_text_view));
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
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(st->console_text_view), mark);
}

static void inspector_refresh_console(AppState* st) {
    if (!st->console_text_view) return;
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->console_text_view));
    gtk_text_buffer_set_text(buf, "", 0);
    if (st->js_engine) {
        for (const auto& entry : st->js_engine->console_log) {
            inspector_append_console_entry(st, entry);
        }
    }
}

static void inspector_create_panel(AppState* st) {
    // Create the inspector container
    st->inspector_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(st->inspector_box, 200, -1); // minimum width

    // Apply dark background style
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "notebook, notebook tab, notebook header { background: #1e1e1e; color: #ccc; }"
        "notebook tab:checked { background: #2d2d2d; color: #fff; }"
        "textview, textview text { background-color: #1e1e1e; color: #d4d4d4; "
        "  font-family: monospace; font-size: 9pt; }", -1, nullptr);
    gtk_style_context_add_provider(gtk_widget_get_style_context(st->inspector_box),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Create notebook (tabs)
    st->inspector_notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(st->inspector_notebook), GTK_POS_TOP);
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(st->inspector_notebook),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // ---- Console tab ----
    GtkWidget* console_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(console_scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    st->console_text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(st->console_text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(st->console_text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(st->console_text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(st->console_text_view), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(st->console_text_view), 4);

    // Apply style to console text view
    gtk_style_context_add_provider(gtk_widget_get_style_context(st->console_text_view),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Create text tags for different log levels
    GtkTextBuffer* cbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->console_text_view));
    gtk_text_buffer_create_tag(cbuf, "error", "foreground", "#f44747", nullptr);
    gtk_text_buffer_create_tag(cbuf, "warn", "foreground", "#cca700", nullptr);
    gtk_text_buffer_create_tag(cbuf, "info", "foreground", "#3794ff", nullptr);
    gtk_text_buffer_create_tag(cbuf, "log", "foreground", "#d4d4d4", nullptr);

    gtk_container_add(GTK_CONTAINER(console_scroll), st->console_text_view);
    gtk_notebook_append_page(GTK_NOTEBOOK(st->inspector_notebook),
        console_scroll, gtk_label_new("Console"));

    // ---- Elements tab ----
    GtkWidget* elements_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(elements_scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    st->elements_text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(st->elements_text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(st->elements_text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(st->elements_text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(st->elements_text_view), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(st->elements_text_view), 4);

    // Apply style to elements text view
    gtk_style_context_add_provider(gtk_widget_get_style_context(st->elements_text_view),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    gtk_container_add(GTK_CONTAINER(elements_scroll), st->elements_text_view);
    gtk_notebook_append_page(GTK_NOTEBOOK(st->inspector_notebook),
        elements_scroll, gtk_label_new("Elements"));

    g_object_unref(css);

    gtk_box_pack_start(GTK_BOX(st->inspector_box), st->inspector_notebook, TRUE, TRUE, 0);
}

static void inspector_show(AppState* st) {
    if (st->inspector_visible) return;
    if (!st->inspector_box) inspector_create_panel(st);

    // Add inspector to right side of paned
    gtk_paned_pack2(GTK_PANED(st->paned), st->inspector_box, FALSE, FALSE);

    // Set the divider position (window width - inspector width)
    int win_w;
    gtk_window_get_size(GTK_WINDOW(st->window), &win_w, nullptr);
    gtk_paned_set_position(GTK_PANED(st->paned), win_w - st->inspector_width);

    gtk_widget_show_all(st->inspector_box);
    st->inspector_visible = true;

    // Populate content
    inspector_update_elements(st);
    inspector_refresh_console(st);

    // Wire up console entry callback
    if (st->js_engine) {
        st->js_engine->on_console_entry = [st]() {
            if (st->js_engine && !st->js_engine->console_log.empty()) {
                inspector_append_console_entry(st, st->js_engine->console_log.back());
            }
        };
    }
}

static void inspector_hide(AppState* st) {
    if (!st->inspector_visible) return;

    // Save current width
    int win_w;
    gtk_window_get_size(GTK_WINDOW(st->window), &win_w, nullptr);
    int pos = gtk_paned_get_position(GTK_PANED(st->paned));
    st->inspector_width = win_w - pos;
    if (st->inspector_width < 200) st->inspector_width = 200;

    // Remove from paned (but don't destroy)
    g_object_ref(st->inspector_box);
    gtk_container_remove(GTK_CONTAINER(st->paned), st->inspector_box);

    // Disconnect console callback
    if (st->js_engine) st->js_engine->on_console_entry = nullptr;

    st->inspector_visible = false;
}

static void inspector_toggle(AppState* st) {
    if (st->inspector_visible) inspector_hide(st);
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
    return bm;
}

static void render_dom_to_gtk(AppState* st, Document* doc, int gen);

static void render_node(AppState* st, DOMNode* node, int gen,
                         std::vector<GtkWidget*>& cstack,
                         std::vector<GtkWidget*>& float_rows) {
    if (node->node_type == DOMNode::TEXT) {
        if (node->text_content.empty()) return;
        std::string text = collapse_ws(node->text_content);
        if (text.empty()) return;

        float_rows.back() = nullptr;

        int fw = node->fw_computed;  // -1 = inherit
        int fi = node->fi_computed;  // -1 = inherit
        int fs = node->fs_computed;
        double lh = node->lh_computed;
        std::string color = node->color_computed;
        int text_align = node->text_align_computed;
        std::string href = node->href;

        // Inherit from ancestors
        DOMNode* p = node->parent;
        while (p) {
            if (fw == -1 && p->fw_computed != -1) fw = p->fw_computed;
            if (fi == -1 && p->fi_computed != -1) fi = p->fi_computed;
            if (fs <= 0 && p->fs_computed > 0) fs = p->fs_computed;
            if (color.empty() && !p->color_computed.empty()) color = p->color_computed;
            if (text_align < 0 && p->text_align_computed >= 0) text_align = p->text_align_computed;
            if (lh < 0 && p->lh_computed >= 0) lh = p->lh_computed;
            if (href.empty() && !p->href.empty()) href = p->href;
            p = p->parent;
        }

        if (fw == -1) fw = PANGO_WEIGHT_NORMAL;
        if (fi == -1) fi = PANGO_STYLE_NORMAL;
        if (text_align < 0) text_align = 0;
        if (fs <= 0) fs = 16;

        fprintf(stderr, "[DEBUG render_text] text='%.40s' fw=%d fi=%d fs=%d parent=<%s>\n",
                text.c_str(), fw, fi, fs, node->parent ? node->parent->tag_name.c_str() : "NONE");

        GtkWidget* cur = cstack.back();
        GtkWidget* lbl = gtk_label_new(text.c_str());
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
        if (lh >= 0)
            pango_attr_list_insert(al, pango_attr_line_height_new(lh));
        if (!href.empty())
            pango_attr_list_insert(al, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
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
    if (node->tag_name == "img") {
        float_rows.back() = nullptr;
        GtkWidget* cur = cstack.back();
        GtkWidget* img = gtk_image_new();
        gtk_box_pack_start(GTK_BOX(cur), img, FALSE, FALSE, 2);
        gtk_widget_show(img);
        g_object_ref(img);
        std::string img_url = node->attributes.count("src") ? node->attributes.at("src") : "";
        if (!img_url.empty()) {
            std::thread([st, img, img_url, gen]() {
                Buf ibuf;
                if (gen != st->generation || !fetch(img_url, ibuf)) {
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
                idle_add([st, img, pb, gen]() {
                    if (gen == st->generation && pb)
                        gtk_image_set_from_pixbuf(GTK_IMAGE(img), pb);
                    if (pb) g_object_unref(pb);
                    g_object_unref(img);
                });
            }).detach();
        }
        return;
    }

    // Block/container element
    bool floated = node->floatdir != DOMNode::Float::None;
    bool is_ib = node->display == DOMNode::Display::InlineBlock;
    bool is_flex = node->display == DOMNode::Display::Flex;
    bool emits_block = floated || is_flex || is_ib
        || node->display == DOMNode::Display::Block
        || (node->isBlock() && node->display != DOMNode::Display::Inline);

    if (node->is_body) {
        BoxModel bm = dom_node_to_boxmodel(node);
        if (!bm.bg_color.empty()) {
            char* bgc = g_strdup(bm.bg_color.c_str());
            g_object_set_data_full(G_OBJECT(st->content_box), "bg_color_str", bgc, g_free);
        }
        gtk_widget_set_margin_top(st->content_box, std::max(0, bm.margin[0]));
        gtk_widget_set_margin_end(st->content_box, std::max(0, bm.margin[1]));
        gtk_widget_set_margin_bottom(st->content_box, std::max(0, bm.margin[2]));
        gtk_widget_set_margin_start(st->content_box, std::max(0, bm.margin[3]));
        if (!bm.bg_color.empty() || !bm.bg_image.empty()) {
            gtk_widget_set_app_paintable(st->content_box, TRUE);
            st->body_draw_signal = g_signal_connect(st->content_box, "draw", G_CALLBACK(draw_bg), nullptr);
        }
        if (!bm.bg_image.empty()) {
            std::string bg_url = bm.bg_image;
            std::thread([st, bg_url, gen]() {
                Buf ibuf;
                if (gen != st->generation || !fetch(bg_url, ibuf)) return;
                GdkPixbufLoader* ldr = gdk_pixbuf_loader_new();
                GError* err = nullptr;
                gdk_pixbuf_loader_write(ldr, (const guchar*)ibuf.data.data(), ibuf.data.size(), &err);
                gdk_pixbuf_loader_close(ldr, nullptr);
                GdkPixbuf* pb = nullptr;
                if (!err) { pb = gdk_pixbuf_loader_get_pixbuf(ldr); if (pb) g_object_ref(pb); }
                if (err) g_error_free(err);
                g_object_unref(ldr);
                idle_add([st, pb, gen]() {
                    if (gen == st->generation && pb) {
                        g_object_set_data_full(G_OBJECT(st->content_box), "bg_pb", pb, (GDestroyNotify)g_object_unref);
                        gtk_widget_queue_draw(st->content_box);
                    } else if (pb) g_object_unref(pb);
                });
            }).detach();
        }
        cstack.push_back(st->content_box);
        float_rows.push_back(nullptr);
        for (auto& child : node->children)
            render_node(st, child.get(), gen, cstack, float_rows);
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
            GtkOrientation orient = is_flex ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL;
            new_blk = make_block(bm, cstack.back(), orient);
        }
        if (!bm.bg_image.empty()) {
            gtk_widget_set_app_paintable(new_blk, TRUE);
            g_signal_connect(new_blk, "draw", G_CALLBACK(draw_bg), nullptr);
            g_object_ref(new_blk);
            std::string bg_url = bm.bg_image;
            std::thread([st, new_blk, bg_url, gen]() {
                Buf ibuf;
                if (gen != st->generation || !fetch(bg_url, ibuf)) {
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
                idle_add([st, new_blk, pb, gen]() {
                    if (gen == st->generation && pb) {
                        g_object_set_data_full(G_OBJECT(new_blk), "bg_pb", pb, (GDestroyNotify)g_object_unref);
                        gtk_widget_queue_draw(new_blk);
                    } else if (pb) g_object_unref(pb);
                    g_object_unref(new_blk);
                });
            }).detach();
        }
        // Store node_id on widget for click dispatch
        g_object_set_data(G_OBJECT(new_blk), "dom_node_id",
            GUINT_TO_POINTER(node->node_id));

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
                    if (!st->js_engine) return FALSE;
                    uint32_t nid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "dom_node_id"));
                    st->js_engine->dispatchEvent(nid, "click", (int)ev->x, (int)ev->y);
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
            render_node(st, child.get(), gen, cstack, float_rows);
        cstack.pop_back();
        float_rows.pop_back();
    } else {
        for (auto& child : node->children)
            render_node(st, child.get(), gen, cstack, float_rows);
    }
}

static void render_dom_to_gtk(AppState* st, Document* doc, int gen) {
    std::vector<GtkWidget*> cstack = {st->content_box};
    std::vector<GtkWidget*> float_rows = {nullptr};
    if (doc->body) {
        render_node(st, doc->body, gen, cstack, float_rows);
    } else {
        for (auto& child : doc->root->children)
            render_node(st, child.get(), gen, cstack, float_rows);
    }
}

// Called by JSEngine::rerender_callback when DOM is dirty
void do_rerender(AppState* st) {
    if (!st->document) return;
    int gen = st->generation;

    // Clean up previous body styles
    if (st->body_draw_signal) {
        g_signal_handler_disconnect(st->content_box, st->body_draw_signal);
        st->body_draw_signal = 0;
        gtk_widget_set_app_paintable(st->content_box, FALSE);
        g_object_set_data(G_OBJECT(st->content_box), "bg_pb", nullptr);
        g_object_set_data(G_OBJECT(st->content_box), "bg_color_str", nullptr);
    }
    gtk_widget_set_margin_top(st->content_box, 0);
    gtk_widget_set_margin_end(st->content_box, 0);
    gtk_widget_set_margin_bottom(st->content_box, 0);
    gtk_widget_set_margin_start(st->content_box, 0);

    // Remove all children
    GList* ch = gtk_container_get_children(GTK_CONTAINER(st->content_box));
    for (GList* l = ch; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);

    // Re-render
    render_dom_to_gtk(st, st->document.get(), gen);

    // Clear dirty flags
    std::function<void(DOMNode*)> clear_dirty = [&](DOMNode* n) {
        n->dirty = false;
        for (auto& c : n->children) clear_dirty(c.get());
    };
    clear_dirty(st->document->root.get());
}

// ---- test mode (forward decls) ----
static bool g_test_mode = false;
static int g_probe_pass = 0;
static int g_probe_fail = 0;
static void run_test_probes(AppState* st);

// ---- page fetch ----

static void fetch_page(AppState* st, std::string url, int gen) {
    bool is_vs = (url.size()>=12 && url.substr(0,12)=="view-source:");
    std::string fetch_url = is_vs ? url.substr(12) : url;

    Buf buf;
    if (!fetch(fetch_url, buf)) {
        idle_add([st, url]() {
            gtk_window_set_title(GTK_WINDOW(st->window), ("Failed: "+url).c_str());
        });
        return;
    }
    if (gen != st->generation) return;

    if (is_vs) {
        std::string raw = std::move(buf.data);
        idle_add([st, gen, url, raw=std::move(raw)]() {
            if (gen != st->generation) return;
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
            gtk_box_pack_start(GTK_BOX(st->content_box), tv, TRUE, TRUE, 0);
            gtk_widget_show(tv);
        });
        return;
    }

    std::string raw_source = buf.data;  // save raw source for inspector
    auto doc = parse_html_to_dom(buf.data, fetch_url);

    // Fetch external scripts synchronously (blocking, matches <script src> behavior)
    std::vector<std::string> external_scripts;
    for (const auto& src_url : doc->script_srcs) {
        if (gen != st->generation) return;
        Buf sbuf;
        if (fetch(src_url, sbuf)) {
            external_scripts.push_back(std::move(sbuf.data));
        } else {
            fprintf(stderr, "[script] Failed to fetch: %s\n", src_url.c_str());
        }
    }

    idle_add([st, gen, url, doc, raw_source=std::move(raw_source),
              external_scripts=std::move(external_scripts)]() {
        if (gen != st->generation) return;
        gtk_window_set_title(GTK_WINDOW(st->window), url.c_str());
        st->document = doc;
        st->page_source = raw_source;
        render_dom_to_gtk(st, doc.get(), gen);

        // Destroy previous JS engine
        if (st->js_engine) {
            delete st->js_engine;
            st->js_engine = nullptr;
        }

        // Create JS engine and run scripts
        auto* engine = new JSEngine();
        st->js_engine = engine;
        engine->init(st, doc.get());

        // Wire up inspector console callback if inspector is open
        if (st->inspector_visible) {
            engine->on_console_entry = [st]() {
                if (st->js_engine && !st->js_engine->console_log.empty()) {
                    inspector_append_console_entry(st, st->js_engine->console_log.back());
                }
            };
            inspector_update_elements(st);
            inspector_refresh_console(st);
        }

        // Execute external scripts first (in document order they were found)
        for (size_t i = 0; i < external_scripts.size(); i++) {
            std::string fname = i < doc->script_srcs.size() ? doc->script_srcs[i] : "<external>";
            engine->eval(external_scripts[i], fname);
        }

        // Execute inline scripts
        for (size_t i = 0; i < doc->scripts.size(); i++) {
            engine->eval(doc->scripts[i], "<script>");
        }

        // Execute pending microtasks after all scripts
        engine->executePendingJobs();

        // Run C++ DOM probes if --test mode
        if (g_test_mode) {
            // Schedule probes after a short delay so timers (setTimeout 500ms) fire first
            g_timeout_add(800, [](gpointer data) -> gboolean {
                auto* st = static_cast<AppState*>(data);
                if (st->js_engine) st->js_engine->executePendingJobs();
                run_test_probes(st);
                return G_SOURCE_REMOVE;
            }, st);
        }
    });
}

// ---- navigate ----

static void update_nav_buttons(AppState* st) {
    gtk_widget_set_sensitive(st->btn_back, !st->back_stack.empty());
    gtk_widget_set_sensitive(st->btn_fwd,  !st->fwd_stack.empty());
}

// load_url: fetch without touching history
static void load_url(AppState* st, const std::string& url) {
    gtk_entry_set_text(GTK_ENTRY(st->address_bar), url.c_str());

    int gen;
    { std::lock_guard<std::mutex> lk(st->mu); gen = ++st->generation; }

    // destroy JS engine from previous page
    if (st->js_engine) {
        delete st->js_engine;
        st->js_engine = nullptr;
    }
    st->document.reset();

    // clean up previous body styles from content_box
    if (st->body_draw_signal) {
        g_signal_handler_disconnect(st->content_box, st->body_draw_signal);
        st->body_draw_signal = 0;
        gtk_widget_set_app_paintable(st->content_box, FALSE);
        g_object_set_data(G_OBJECT(st->content_box), "bg_pb", nullptr);
        g_object_set_data(G_OBJECT(st->content_box), "bg_color_str", nullptr);
    }
    gtk_widget_set_margin_top(st->content_box, 0);
    gtk_widget_set_margin_end(st->content_box, 0);
    gtk_widget_set_margin_bottom(st->content_box, 0);
    gtk_widget_set_margin_start(st->content_box, 0);

    GList* ch = gtk_container_get_children(GTK_CONTAINER(st->content_box));
    for (GList* l=ch; l; l=l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);

    gtk_window_set_title(GTK_WINDOW(st->window), ("Loading "+url+"...").c_str());
    std::thread(fetch_page, st, url, gen).detach();
}

static void navigate(AppState* st, const std::string& raw) {
    std::string url = normalize_url(raw);
    if (!st->current_url.empty()) st->back_stack.push_back(st->current_url);
    st->fwd_stack.clear();
    st->current_url = url;
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
    if (st->back_stack.empty()) return;
    if (!st->current_url.empty()) st->fwd_stack.push_back(st->current_url);
    st->current_url = st->back_stack.back(); st->back_stack.pop_back();
    update_nav_buttons(st);
    load_url(st, st->current_url);
}
static void on_fwd(GtkButton*, gpointer d) {
    auto* st = static_cast<AppState*>(d);
    if (st->fwd_stack.empty()) return;
    if (!st->current_url.empty()) st->back_stack.push_back(st->current_url);
    st->current_url = st->fwd_stack.back(); st->fwd_stack.pop_back();
    update_nav_buttons(st);
    load_url(st, st->current_url);
}
static void on_refresh(GtkButton*, gpointer d) {
    auto* st = static_cast<AppState*>(d);
    if (!st->current_url.empty()) load_url(st, st->current_url);
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
    Document* doc = st->document.get();
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
        DOMNode* btn = probe_node(doc, "click-btn");
        probe_check("click-btn exists", btn != nullptr);
        if (btn) {
            fprintf(stderr, "[DEBUG probe] click-btn node_id=%u listeners=%zu tag=%s addr=%p\n",
                    btn->node_id, btn->listeners.size(), btn->tag_name.c_str(), (void*)btn);
            // Also check node_map for this node_id
            auto it = doc->node_map.find(btn->node_id);
            if (it != doc->node_map.end()) {
                fprintf(stderr, "[DEBUG probe] node_map[%u] listeners=%zu same_ptr=%d addr=%p\n",
                        btn->node_id, it->second->listeners.size(), it->second == btn, (void*)it->second);
            } else {
                fprintf(stderr, "[DEBUG probe] node_map[%u] NOT FOUND! id_map has %zu entries\n",
                        btn->node_id, doc->id_map.size());
            }
            probe_check("click-btn has >= 1 listener",
                btn->listeners.size() >= 1);
            if (!btn->listeners.empty())
                probe_check("click-btn listener type is 'click'",
                    btn->listeners[0].type == "click");
        }
    }

    // -- Probe: console log captured entries
    if (st->js_engine) {
        auto& log = st->js_engine->console_log;
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
    Document* doc = st->document.get();
    if (!doc || !st->js_engine) return;

    fprintf(stderr, "\n\033[1m=== C++ DOM Probes: Phase 2 (after simulated click) ===\033[0m\n");

    // Find click-count-btn and simulate a click via the event system
    DOMNode* btn = probe_node(doc, "click-count-btn");
    probe_check("click-count-btn exists for click sim", btn != nullptr);
    if (!btn) return;

    std::string before = btn->getTextContent();
    probe_check("click-count-btn text before click is 'Count: 0'", before == "Count: 0");

    // Dispatch click event from C++ (not JS)
    st->js_engine->dispatchEvent(btn->node_id, "click", 0, 0);
    st->js_engine->executePendingJobs();

    std::string after1 = btn->getTextContent();
    probe_check("click-count-btn text after 1 click is 'Count: 1'", after1 == "Count: 1");

    // Click again
    st->js_engine->dispatchEvent(btn->node_id, "click", 0, 0);
    st->js_engine->executePendingJobs();

    std::string after2 = btn->getTextContent();
    probe_check("click-count-btn text after 2 clicks is 'Count: 2'", after2 == "Count: 2");

    // Click the main click-btn and check event-result
    DOMNode* click_btn = probe_node(doc, "click-btn");
    if (click_btn) {
        st->js_engine->dispatchEvent(click_btn->node_id, "click", 42, 99);
        st->js_engine->executePendingJobs();
        std::string ev_result = probe_text(doc, "event-result");
        probe_check("event-result updated after click", ev_result.find("clicked") != std::string::npos);
    }
}

// Phase 3: simulate toggle-style click and verify style changes
static void run_probes_phase3(AppState* st) {
    Document* doc = st->document.get();
    if (!doc || !st->js_engine) return;

    fprintf(stderr, "\n\033[1m=== C++ DOM Probes: Phase 3 (style toggle) ===\033[0m\n");

    DOMNode* toggle_btn = probe_node(doc, "toggle-style");
    DOMNode* target = probe_node(doc, "toggle-target");
    probe_check("toggle-style btn exists", toggle_btn != nullptr);
    probe_check("toggle-target exists", target != nullptr);
    if (!toggle_btn || !target) return;

    // Click toggle
    st->js_engine->dispatchEvent(toggle_btn->node_id, "click", 0, 0);
    st->js_engine->executePendingJobs();

    probe_check("toggle-target bg changed to '#e74c3c'",
        target->style_props.count("background-color") &&
        target->style_props["background-color"] == "#e74c3c");
    probe_check("toggle-target text after toggle ON",
        target->getTextContent().find("toggled ON") != std::string::npos);

    // Toggle back
    st->js_engine->dispatchEvent(toggle_btn->node_id, "click", 0, 0);
    st->js_engine->executePendingJobs();

    probe_check("toggle-target bg changed back to '#3498db'",
        target->style_props.count("background-color") &&
        target->style_props["background-color"] == "#3498db");
    probe_check("toggle-target text after toggle OFF",
        target->getTextContent().find("toggled OFF") != std::string::npos);
}

// Phase 4: todo list add/clear via simulated clicks
static void run_probes_phase4(AppState* st) {
    Document* doc = st->document.get();
    if (!doc || !st->js_engine) return;

    fprintf(stderr, "\n\033[1m=== C++ DOM Probes: Phase 4 (todo list) ===\033[0m\n");

    DOMNode* add_btn = probe_node(doc, "add-todo");
    DOMNode* clear_btn = probe_node(doc, "clear-todos");
    DOMNode* list = probe_node(doc, "todo-list");
    probe_check("add-todo btn exists", add_btn != nullptr);
    probe_check("todo-list exists", list != nullptr);
    if (!add_btn || !list) return;

    // Add 3 items
    for (int i = 0; i < 3; i++) {
        st->js_engine->dispatchEvent(add_btn->node_id, "click", 0, 0);
        st->js_engine->executePendingJobs();
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
        st->js_engine->dispatchEvent(clear_btn->node_id, "click", 0, 0);
        st->js_engine->executePendingJobs();
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
    Document* doc = st->document.get();
    if (!doc || !st->js_engine) return;

    fprintf(stderr, "\n\033[1m=== C++ DOM Probes: Phase 5 (error capture) ===\033[0m\n");

    size_t log_before = st->js_engine->console_log.size();

    // Click trigger-error button (causes ReferenceError)
    DOMNode* err_btn = probe_node(doc, "trigger-error");
    if (err_btn) {
        st->js_engine->dispatchEvent(err_btn->node_id, "click", 0, 0);
        st->js_engine->executePendingJobs();
    }

    size_t log_after = st->js_engine->console_log.size();
    probe_check("ReferenceError captured in console_log", log_after > log_before);
    if (log_after > log_before) {
        auto& last = st->js_engine->console_log.back();
        probe_check("error entry level is ERROR", last.level == ConsoleLevel::ERROR);
        probe_check("error message mentions 'not defined' or similar",
            last.message.find("not defined") != std::string::npos ||
            last.message.find("ReferenceError") != std::string::npos);
    }

    // Click trigger-type-error button
    log_before = st->js_engine->console_log.size();
    DOMNode* terr_btn = probe_node(doc, "trigger-type-error");
    if (terr_btn) {
        st->js_engine->dispatchEvent(terr_btn->node_id, "click", 0, 0);
        st->js_engine->executePendingJobs();
    }

    log_after = st->js_engine->console_log.size();
    probe_check("TypeError captured in console_log", log_after > log_before);

    // Click log-all and verify 4 new entries
    log_before = st->js_engine->console_log.size();
    DOMNode* log_btn = probe_node(doc, "log-all");
    if (log_btn) {
        st->js_engine->dispatchEvent(log_btn->node_id, "click", 0, 0);
        st->js_engine->executePendingJobs();
    }

    log_after = st->js_engine->console_log.size();
    probe_check("log-all added 4 entries", log_after - log_before == 4);
    if (log_after - log_before >= 4) {
        auto& entries = st->js_engine->console_log;
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

static void run_test_probes(AppState* st) {
    fprintf(stderr, "\n\033[1;36m╔══════════════════════════════════════════╗\033[0m\n");
    fprintf(stderr, "\033[1;36m║     C++ DOM PROBE TEST SUITE             ║\033[0m\n");
    fprintf(stderr, "\033[1;36m╚══════════════════════════════════════════╝\033[0m\n");

    run_probes_phase1(st);
    run_probes_phase2(st);
    run_probes_phase3(st);
    run_probes_phase4(st);
    run_probes_phase5(st);

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
    gtk_window_set_default_size(GTK_WINDOW(st->window), 960, 800);
    g_signal_connect(st->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(st->window), root);

    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start(bar, 4);
    gtk_widget_set_margin_end(bar, 4);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 4);
    gtk_box_pack_start(GTK_BOX(root), bar, FALSE, FALSE, 0);

    st->btn_back = gtk_button_new_with_label("←");
    gtk_widget_set_sensitive(st->btn_back, FALSE);
    gtk_box_pack_start(GTK_BOX(bar), st->btn_back, FALSE, FALSE, 0);
    g_signal_connect(st->btn_back, "clicked", G_CALLBACK(on_back), st);

    st->btn_fwd = gtk_button_new_with_label("→");
    gtk_widget_set_sensitive(st->btn_fwd, FALSE);
    gtk_box_pack_start(GTK_BOX(bar), st->btn_fwd, FALSE, FALSE, 0);
    g_signal_connect(st->btn_fwd, "clicked", G_CALLBACK(on_fwd), st);

    GtkWidget* btn_refresh = gtk_button_new_with_label("↺");
    gtk_box_pack_start(GTK_BOX(bar), btn_refresh, FALSE, FALSE, 4);
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh), st);

    st->address_bar = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(st->address_bar), "mattmontag.com");
    gtk_box_pack_start(GTK_BOX(bar), st->address_bar, TRUE, TRUE, 0);
    g_signal_connect(st->address_bar, "activate", G_CALLBACK(on_activate), st);

    GtkWidget* go_btn = gtk_button_new_with_label("Go");
    gtk_box_pack_start(GTK_BOX(bar), go_btn, FALSE, FALSE, 4);
    g_signal_connect(go_btn, "clicked", G_CALLBACK(on_go), st);

    // Horizontal paned: left = page content, right = inspector (when open)
    st->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(root), st->paned, TRUE, TRUE, 0);

    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_paned_pack1(GTK_PANED(st->paned), scroll, TRUE, FALSE);

    GtkWidget* viewport = gtk_viewport_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(scroll), viewport);
    gtk_widget_add_events(viewport, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(viewport, "button-press-event", G_CALLBACK(on_content_click), st);
    st->viewport = viewport;

    st->content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(st->content_box, TRUE);
    gtk_widget_set_vexpand(st->content_box, TRUE);
    gtk_container_add(GTK_CONTAINER(viewport), st->content_box);

    // F12 to toggle inspector
    g_signal_connect(st->window, "key-press-event",
        G_CALLBACK(+[](GtkWidget*, GdkEventKey* ev, gpointer d) -> gboolean {
            if (ev->keyval == GDK_KEY_F12) {
                inspector_toggle(static_cast<AppState*>(d));
                return TRUE;
            }
            return FALSE;
        }), st);

    gtk_widget_show_all(st->window);

    // Accept URL and flags from command line
    std::string start_url = "mattmontag.com";
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--test") { g_test_mode = true; continue; }
        if (!arg.empty() && arg[0] != '-') { start_url = arg; }
    }
    gtk_entry_set_text(GTK_ENTRY(st->address_bar), start_url.c_str());
    navigate(st, start_url);

    gtk_main();
    curl_global_cleanup();
    delete st;
    return 0;
}
