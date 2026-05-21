// fps.cpp — blue marble FPS, full indoor/outdoor level
// Mouse: look  |  WASD: move  |  Space: jump  |  Esc: quit

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <iostream>
#include <random>
#include <fstream>
#include "level.h"
#include "texload.h"
#include "furniture.h"
#include "props.h"
#include "buttons.h"
#include "baphomet.h"
#include "sphere_scene.h"

static const int   WIN_W = 720, WIN_H = 480;
static const float PI    = 3.14159265f;
static const float MARBLE_R   = 0.45f;
static const float EYE_ABOVE  = 0.65f;
static const float GRAVITY    = 24.f;
static const float JUMP_V     = 10.f;
static const float MOVE_SPEED = 7.f;
static const float MOUSE_SENS = 0.12f;

// ── Player state ──────────────────────────────────────────────────────────────
// Starts outdoors, facing the building entrance (yaw=180 → looking +Z)
static glm::vec3 playerPos(0.f, MARBLE_R, -12.f);
static glm::vec3 vel(0.f);
static float     yaw = 180.f, pitch = 0.f;
static bool      onGround = false;
static double    lastX = WIN_W/2, lastY = WIN_H/2;
static bool      firstMouse = true;
static float     gBright = 1.0f;
static bool      gDither = true;

// ── Pin marker state ──────────────────────────────────────────────────────────
static const char*        SPHERE_CACHE = "spheres.cache";
static std::vector<glm::vec3> pins;           // placed marker spheres
static int                hoveredPin   = -1;  // index of hovered (green) pin
static glm::vec3          cursorHit(0.f,-1.f,0.f); // raycast hit this frame
static bool               leftJust     = false;    // consumed each frame
static bool               rightJust    = false;    // right-click, consumed each frame
static bool               rDotVisible  = false;    // R key toggles cursor dot

// ── Scene state ───────────────────────────────────────────────────────────────
static int        gScene    = 0;    // 0 = normal, 1 = sphere arena
static glm::vec3  gEntryPos(0.f);   // basement pos saved on sphere entry

// ── Shaders ───────────────────────────────────────────────────────────────────

// Shared VS for all level geometry (stone / drywall / ceiling)
static const char* VS_LV = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in vec2 aTex;
uniform mat4 uVP;
out vec3 vPos; out vec3 vNorm; out vec2 vTex;
void main() {
    vPos = aPos; vNorm = aNorm; vTex = aTex;
    gl_Position = uVP * vec4(aPos, 1.0);
}
)";

// Shared PBR fragment shader: derivative TBN + parallax offset + texture sampling
// uDiff = sRGB albedo (sampled linear), uNorm = OpenGL normal map, uDisp = height
static const char* FS_LV = R"(
#version 330 core
in vec3 vPos, vNorm; in vec2 vTex;
uniform sampler2D uDiff, uNorm, uDisp;
uniform vec3  uCamPos;
uniform float uUVScale;
uniform float uParallax;
uniform float uBright;
out vec4 fragColor;
const vec3 lp0=vec3(0.0,3.1,5.0),lp1=vec3(0.0,3.1,16.0),lp2=vec3(10.0,3.1,5.0),lp3=vec3(0.0,3.1,12.0);
const vec3 lc =vec3(1.0,0.92,0.72);

// Single point light for marble room — center of ceiling
const vec3 ML_POS = vec3(-9.0, -96.85, -12.0);
const vec3 ML_COL = vec3(1.0, 0.96, 0.88);

void main(){
    vec3 N0 = normalize(vNorm)*(gl_FrontFacing?1.0:-1.0);
    vec3 V  = normalize(uCamPos - vPos);
    vec2 uv = vTex * uUVScale;
    // Derivative-based TBN (no tangent vertex attributes needed)
    vec3 dp1=dFdx(vPos), dp2=dFdy(vPos);
    vec2 duv1=dFdx(uv),  duv2=dFdy(uv);
    vec3 T = normalize(dp1*duv2.y - dp2*duv1.y);
    T = normalize(T - dot(T,N0)*N0);
    vec3 B = cross(N0, T);
    // Parallax offset mapping
    float vdT=dot(V,T), vdB=dot(V,B), vdN=dot(V,N0);
    float h = texture(uDisp, uv).r;
    float vc = abs(vdN)>0.08 ? vdN : (vdN>=0.0?0.08:-0.08);
    uv += (h-0.5)*uParallax * vec2(vdT,vdB) / vc;
    // Sample textures
    vec3 col = texture(uDiff, uv).rgb;
    vec3 tn  = texture(uNorm, uv).xyz*2.0-1.0;
    vec3 N   = normalize(T*tn.x + B*tn.y + N0*tn.z);

    vec3 result;

    // Marble room: south room 0 (x=-12..-6, z=-20..-4, basement)
    bool inMarble = (vPos.x > -12.05 && vPos.x < -5.95 &&
                     vPos.z > -20.05 && vPos.z < -4.02 &&
                     vPos.y < -1.0);
    if (inMarble) {
        // No ambient — pure black unless hit by the room's light
        vec3 lv    = ML_POS - vPos;
        float ldist = length(lv);
        vec3  ldir  = lv / ldist;
        float atten = 1.0 / (1.0 + 0.04*ldist + 0.018*ldist*ldist);

        float diff  = max(dot(N, ldir), 0.0);
        vec3  H     = normalize(ldir + V);
        float spec  = pow(max(dot(N, H), 0.0), 320.0);

        // Nearly-black diffuse so surface colour is suppressed; specular carries the shine
        result = col * 0.012 * diff * ML_COL * atten * 6.0
               + ML_COL * spec * atten * 5.0;
    } else {
        // Normal level lighting
        vec3 L = normalize(vec3(0.5,1.0,-0.3));
        result = col*(0.04+0.08*max(dot(N,L),0.0));
        {vec3 t=lp0-vPos;float r=length(t);result+=col*lc*max(dot(N,t/r),0.0)/(1.0+0.07*r+0.025*r*r);}
        {vec3 t=lp1-vPos;float r=length(t);result+=col*lc*max(dot(N,t/r),0.0)/(1.0+0.07*r+0.025*r*r);}
        {vec3 t=lp2-vPos;float r=length(t);result+=col*lc*max(dot(N,t/r),0.0)/(1.0+0.07*r+0.025*r*r);}
        {vec3 t=lp3-vPos;float r=length(t);result+=col*lc*max(dot(N,t/r),0.0)/(1.0+0.07*r+0.025*r*r);}
    }

    fragColor=vec4(pow(clamp(result*uBright,0.0,1.0),vec3(1.0/2.2)),1.0);
}
)";

// Door shader — same fragment as FS_LV but applies a per-leaf model matrix
static const char* VS_DOOR = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in vec2 aTex;
uniform mat4 uVP;
uniform mat4 uModel;
out vec3 vPos; out vec3 vNorm; out vec2 vTex;
void main() {
    vec4 wp = uModel * vec4(aPos, 1.0);
    vPos  = wp.xyz;
    vNorm = mat3(uModel) * aNorm;
    vTex  = aTex;
    gl_Position = uVP * wp;
}
)";

// Marble: shiny cobalt blue, Phong + Fresnel
static const char* VS_MB = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
uniform mat4 uMVP; uniform mat4 uM;
out vec3 vPos; out vec3 vNorm;
void main(){
    vPos=(uM*vec4(aPos,1)).xyz;
    vNorm=mat3(transpose(inverse(uM)))*aNorm;
    gl_Position=uMVP*vec4(aPos,1);
}
)";
static const char* FS_MB = R"(
#version 330 core
in vec3 vPos, vNorm;
uniform vec3  uCamPos;
uniform float uBright;
out vec4 fragColor;
void main(){
    vec3 N=normalize(vNorm), L=normalize(vec3(8,30,12)-vPos);
    vec3 V=normalize(uCamPos-vPos), R=reflect(-L,N);
    float diff=max(dot(N,L),0.0)*0.40;
    float spec=pow(max(dot(V,R),0.0),90.0)*0.70;
    float fres=pow(1.0-max(dot(N,V),0.0),3.0);
    vec3 base=mix(vec3(0.08,0.25,0.80),vec3(0.28,0.52,1.00),fres);
    vec3 result=base*(0.06+diff)+vec3(0.85,0.92,1.0)*spec;
    const vec3 lp0=vec3(0.0,3.1,5.0),lp1=vec3(0.0,3.1,16.0),lp2=vec3(10.0,3.1,5.0),lp3=vec3(0.0,3.1,12.0);
    const vec3 lc=vec3(1.0,0.92,0.72);
    {vec3 t=lp0-vPos;float r=length(t);result+=base*lc*max(dot(N,t/r),0.0)/(1.0+0.07*r+0.025*r*r)*0.8;}
    {vec3 t=lp1-vPos;float r=length(t);result+=base*lc*max(dot(N,t/r),0.0)/(1.0+0.07*r+0.025*r*r)*0.8;}
    {vec3 t=lp2-vPos;float r=length(t);result+=base*lc*max(dot(N,t/r),0.0)/(1.0+0.07*r+0.025*r*r)*0.8;}
    {vec3 t=lp3-vPos;float r=length(t);result+=base*lc*max(dot(N,t/r),0.0)/(1.0+0.07*r+0.025*r*r)*0.8;}
    fragColor=vec4(clamp(result*uBright,0.0,1.0),1.0);
}
)";

// Cotton cloud billboard (unchanged from cloud_view)
static const char* VS_CL = R"(
#version 330 core
layout(location=0) in vec2 aCorner;
uniform vec3 uCenter; uniform float uSize;
uniform mat4 uView;   uniform mat4 uProj;
out vec2 vUV;
void main(){
    vec4 eye=uView*vec4(uCenter,1.0);
    eye.xy+=aCorner*uSize;
    gl_Position=uProj*eye;
    vUV=aCorner*0.5+0.5;
}
)";
static const char* FS_CL = R"(
#version 330 core
in vec2 vUV;
uniform vec3 uColor; uniform float uAlpha;
out vec4 fragColor;
void main(){
    vec2 d=vUV-vec2(0.5); float r=length(d);
    if(r>0.5) discard;
    float ang=atan(d.y,d.x);
    float wob=sin(ang*5.0)*0.08+sin(ang*9.0+1.1)*0.05
             +sin(ang*14.0+2.7)*0.03+sin(ang*21.0+0.5)*0.015;
    float ref=r/(0.5+wob);
    if(ref>1.0) discard;
    float body=exp(-ref*ref*3.2);
    float fray=abs(sin(ang*19.0+r*60.0))*0.5+0.5;
    fragColor=vec4(uColor, body*mix(1.0,fray,smoothstep(0.55,0.95,ref))*uAlpha);
}
)";

// Flat-colour shader for upholstery, TV/metal surfaces (no texture, uniform colour)
static const char* FS_FLAT = R"(
#version 330 core
in vec3 vPos, vNorm; in vec2 vTex;
uniform vec3  uColor;
uniform float uBright;
out vec4 fragColor;
const vec3 lp0=vec3(0.0,3.1,5.0),lp1=vec3(0.0,3.1,16.0),lp2=vec3(10.0,3.1,5.0),lp3=vec3(0.0,3.1,12.0);
const vec3 lc=vec3(1.0,0.92,0.72);
void main(){
    vec3 N=normalize(vNorm)*(gl_FrontFacing?1.0:-1.0);
    vec3 L=normalize(vec3(0.5,1.0,-0.3));
    vec3 col=uColor;
    vec3 result=col*(0.04+0.08*max(dot(N,L),0.0));
    {vec3 t=lp0-vPos;float r=length(t);result+=col*lc*max(dot(N,t/r),0.0)/(1.0+0.07*r+0.025*r*r);}
    {vec3 t=lp1-vPos;float r=length(t);result+=col*lc*max(dot(N,t/r),0.0)/(1.0+0.07*r+0.025*r*r);}
    {vec3 t=lp2-vPos;float r=length(t);result+=col*lc*max(dot(N,t/r),0.0)/(1.0+0.07*r+0.025*r*r);}
    {vec3 t=lp3-vPos;float r=length(t);result+=col*lc*max(dot(N,t/r),0.0)/(1.0+0.07*r+0.025*r*r);}
    fragColor=vec4(pow(clamp(result*uBright,0.0,1.0),vec3(1.0/2.2)),1.0);
}
)";

// Well shaft: dark stone-brick pattern with horizontal courses and vertical seams
static const char* FS_WELL = R"(
#version 330 core
in vec3 vPos, vNorm; in vec2 vTex;
uniform float uBright;
out vec4 fragColor;
void main(){
    // Horizontal stone courses every 0.5 world-units
    float y    = -vPos.y;
    float ry   = fract(y / 0.5);
    float hm   = smoothstep(0.0,0.05,ry)*(1.0-smoothstep(0.95,1.0,ry));
    // Alternating vertical seams (arc-length UV, ~0.85 units per block)
    float course = floor(y / 0.5);
    float blockW = 0.85;
    float offset = mod(course,2.0)*0.425;
    float ru   = fract((vTex.x + offset)/blockW);
    float vm   = smoothstep(0.0,0.06,ru)*(1.0-smoothstep(0.93,1.0,ru));
    float pat  = hm * vm;
    // Per-block tone variation
    float blk  = sin(course*7.3+floor((vTex.x+offset)/blockW)*5.1)*0.5+0.5;
    vec3 stone  = vec3(0.018,0.018,0.022)*(0.82+0.18*blk);
    vec3 mortar = vec3(0.038,0.038,0.042);
    vec3 col    = mix(mortar, stone, pat*0.65+0.35);
    fragColor   = vec4(clamp(col*uBright,0.0,1.0), 1.0);
}
)";

// ── Basement patterned door panels ────────────────────────────────────────────
static const char* VS_PAT = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
uniform mat4 uMVP;
out vec2 vUV;
void main(){ vUV=aUV; gl_Position=uMVP*vec4(aPos,1.0); }
)";
static const char* FS_PAT = R"(
#version 330 core
in vec2 vUV;
uniform int  uPat;
uniform vec3 uC1, uC2;
out vec4 fragColor;
void main(){
    vec2 uv=vUV; float t=0.0;
    if      (uPat== 0){ vec2 p=fract(uv*vec2(4.0,6.0))-0.5;                       t=step(length(p*vec2(0.8,1.0)),0.30); }  // polka dots
    else if (uPat== 1){ float r=mod(floor(uv.y*8.0),2.0);                          // herringbone
                        float s=r<1.0?fract((uv.x-uv.y)*6.0):fract((uv.x+uv.y)*6.0); t=step(0.55,s); }
    else if (uPat== 2){ t=step(0.82,fract(uv.y*18.0)); }                            // thin h-stripes
    else if (uPat== 3){ t=step(0.5,fract(uv.y*5.0)); }                              // wide h-stripes
    else if (uPat== 4){ vec2 p=fract(uv*vec2(5.0,4.0))-0.5;                        // diamonds
                        t=step(abs(p.x)+abs(p.y),0.38); }
    else if (uPat== 5){ t=mod(floor(uv.x*8.0)+floor(uv.y*8.0),2.0); }              // checkerboard
    else if (uPat== 6){ t=step(0.82,fract(uv.x*18.0)); }                            // thin v-stripes
    else if (uPat== 7){ t=step(0.65,fract((uv.x-uv.y)*8.0)); }                     // diagonal /
    else if (uPat== 8){ t=step(0.65,fract((uv.x+uv.y)*8.0)); }                     // diagonal backslash
    else if (uPat== 9){ t=max(step(0.88,fract(uv.x*14.0)),step(0.88,fract(uv.y*14.0))); } // crosshatch
    else if (uPat==10){ float z=abs(fract(uv.x*5.0)-0.5)*2.0;                      // chevron
                        t=step(0.5,fract(uv.y*8.0+z*0.4)); }
    else if (uPat==11){ vec2 p=(uv-0.5)*vec2(1.25,1.0);                             // rings
                        t=step(0.5,fract(length(p)*10.0)); }
    else if (uPat==12){ vec2 p=fract(uv*7.0);                                       // triangles
                        float e=mod(floor(uv.x*7.0)+floor(uv.y*7.0),2.0);
                        t=e<1.0?step(p.y,p.x):step(p.x,p.y); }
    else if (uPat==13){ float w=sin(uv.x*18.85)*0.25+0.5;                           // wave
                        t=step(w,fract(uv.y*7.0)); }
    else if (uPat==14){ vec2 p=fract(uv*3.0)-0.5;                                   // large dots
                        t=step(length(p*vec2(0.8,1.0)),0.40); }
    else              { t=step(0.5,fract(uv.x*4.0)); }                              // wide v-stripes
    fragColor=vec4(mix(uC1,uC2,t),1.0);
}
)";

// Ceiling light fixture: position-only verts, emissive frosted-glass look
static const char* VS_FIX = R"(
#version 330 core
layout(location=0) in vec3 aPos;
uniform mat4 uMVP;
void main(){ gl_Position = uMVP * vec4(aPos,1.0); }
)";
static const char* FS_FIX = R"(
#version 330 core
uniform float uBright;
out vec4 fragColor;
void main(){ fragColor = vec4(vec3(1.0, 0.94, 0.76)*clamp(uBright,0.0,1.5), 0.78); }
)";

// Unlit flat-white shader for floor markings
static const char* FS_MARK = R"(
#version 330 core
in vec3 vPos, vNorm; in vec2 vTex;
out vec4 fragColor;
void main(){ fragColor = vec4(1.0); }
)";

// Unlit coloured sphere — for pin markers and cursor dot
static const char* VS_PIN = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
uniform mat4 uMVP;
void main(){ gl_Position = uMVP * vec4(aPos,1.0); }
)";
static const char* FS_PIN = R"(
#version 330 core
uniform vec3 uColor;
out vec4 fragColor;
void main(){ fragColor = vec4(uColor,1.0); }
)";

// Post-process: 1-bit Bayer dither + exponential fog + vignette
static const char* VS_PP = R"(
#version 330 core
layout(location=0) in vec2 aPos;
out vec2 vUV;
void main(){ vUV=aPos*0.5+0.5; gl_Position=vec4(aPos,0,1); }
)";
static const char* FS_PP = R"(
#version 330 core
in vec2 vUV;
uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform mat4      uInvVP;
uniform vec3      uCamPos;
uniform int       uDither;
out vec4 fragColor;

float bayer8(ivec2 p){
    const int B[64]=int[](
         0,32, 8,40, 2,34,10,42,
        48,16,56,24,50,18,58,26,
        12,44, 4,36,14,46, 6,38,
        60,28,52,20,62,30,54,22,
         3,35,11,43, 1,33, 9,41,
        51,19,59,27,49,17,57,25,
        15,47, 7,39,13,45, 5,37,
        63,31,55,23,61,29,53,21);
    return (float(B[(p.x&7)+(p.y&7)*8])+0.5)/64.0;
}

void main(){
    vec3 col = texture(uScene, vUV).rgb;

    // ── Volumetric fog (depth → world distance → exponential²) ───────────────
    // Skip fog for sky/background pixels (depth≈1) — clouds don't write depth
    // so their pixels would otherwise be fogged to black.
    float depth = texture(uDepth, vUV).r;
    if (depth < 0.9999) {
        vec4 ndcPos = vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
        vec4 worldP = uInvVP * ndcPos;
        worldP /= worldP.w;
        float dist = length(worldP.xyz - uCamPos);
        float fogD = 0.033;
        float fogF = clamp(exp(-fogD * fogD * dist * dist), 0.0, 1.0);
        col = mix(vec3(0.01, 0.01, 0.03), col, fogF);
    }

    // ── Vignette ─────────────────────────────────────────────────────────────
    vec2  uvc  = vUV - 0.5;
    float vd   = dot(uvc, uvc);          // 0 at centre, ~0.5 at corner
    float vign = 1.0 - smoothstep(0.28, 0.50, vd) * 0.65;
    col *= vign;

    // ── 1-bit Bayer dither ───────────────────────────────────────────────────
    if (uDither != 0) {
        float lum = dot(col, vec3(0.299, 0.587, 0.114));
        fragColor = vec4(vec3(step(bayer8(ivec2(gl_FragCoord.xy)), lum)), 1.0);
    } else {
        fragColor = vec4(col, 1.0);
    }
}
)";

// Flat white 2D overlay shader (vertices in NDC)
static const char* VS_TEXT = R"(
#version 330 core
layout(location=0) in vec2 aPos;
void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }
)";
static const char* FS_TEXT = R"(
#version 330 core
out vec4 fragColor;
void main(){ fragColor = vec4(1.0); }
)";

// ── GL helpers ────────────────────────────────────────────────────────────────

static GLuint buildProg(const char* vs, const char* fs)
{
    auto comp=[](GLenum t,const char* src){
        GLuint s=glCreateShader(t); glShaderSource(s,1,&src,nullptr); glCompileShader(s);
        int ok; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
        if(!ok){char b[512];glGetShaderInfoLog(s,512,nullptr,b);std::cerr<<b<<"\n";}
        return s;
    };
    GLuint v=comp(GL_VERTEX_SHADER,vs), f=comp(GL_FRAGMENT_SHADER,fs);
    GLuint p=glCreateProgram();
    glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

struct LvMesh { GLuint vao=0,vbo=0,ebo=0; int count=0; };

static LvMesh uploadLvMesh(const std::vector<float>& V, const std::vector<unsigned>& I)
{
    if (V.empty()||I.empty()) return {};
    LvMesh m;
    glGenVertexArrays(1,&m.vao); glGenBuffers(1,&m.vbo); glGenBuffers(1,&m.ebo);
    glBindVertexArray(m.vao);
    glBindBuffer(GL_ARRAY_BUFFER,m.vbo);
    glBufferData(GL_ARRAY_BUFFER,V.size()*4,V.data(),GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,m.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,I.size()*4,I.data(),GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,32,(void*)0);  glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,32,(void*)12); glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,32,(void*)24); glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    m.count=(int)I.size();
    return m;
}

static void drawLvMesh(GLuint prog, const LvMesh& m, const glm::mat4& vp,
                       const glm::vec3& cam,
                       GLuint tdiff, GLuint tnorm, GLuint tdisp,
                       float uvScale, float parallax)
{
    if (!m.count) return;
    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog,"uVP"),1,GL_FALSE,glm::value_ptr(vp));
    glUniform3fv(glGetUniformLocation(prog,"uCamPos"),1,glm::value_ptr(cam));
    glUniform1f(glGetUniformLocation(prog,"uUVScale"),  uvScale);
    glUniform1f(glGetUniformLocation(prog,"uParallax"), parallax);
    glUniform1f(glGetUniformLocation(prog,"uBright"),   gBright);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tdiff); glUniform1i(glGetUniformLocation(prog,"uDiff"),0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,tnorm); glUniform1i(glGetUniformLocation(prog,"uNorm"),1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D,tdisp); glUniform1i(glGetUniformLocation(prog,"uDisp"),2);
    glBindVertexArray(m.vao);
    glDrawElements(GL_TRIANGLES,m.count,GL_UNSIGNED_INT,nullptr);
    glBindVertexArray(0);
}

static void drawFlatMesh(GLuint prog, const LvMesh& m, const glm::mat4& vp,
                         const glm::vec3& col)
{
    if (!m.count) return;
    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog,"uVP"),1,GL_FALSE,glm::value_ptr(vp));
    glUniform3fv(glGetUniformLocation(prog,"uColor"),1,glm::value_ptr(col));
    glUniform1f(glGetUniformLocation(prog,"uBright"), gBright);
    glBindVertexArray(m.vao);
    glDrawElements(GL_TRIANGLES,m.count,GL_UNSIGNED_INT,nullptr);
    glBindVertexArray(0);
}

// ── Wall button (interactive panel) ──────────────────────────────────────────
struct WallButton {
    BtnGeo  geo;
    LvMesh  bodyMesh, symMesh;
    int     colorState = 0;
    bool    active     = false;
};

// ── Door leaf (animated) ──────────────────────────────────────────────────────
struct DoorLeaf {
    glm::vec3 hinge;                   // world hinge, y=0
    float closedYaw, openYaw;          // target angles (degrees)
    float curYaw,    targetYaw;        // animated current + lerp target
    bool  open;
    float lw = 1.45f;                  // leaf width (default = standard door)
};

// Ray–OBB via local-space AABB test.  Leaf local box: [0,LW]×[0,DOOR_H]×[-DH,DH].
static bool rayHitDoor(glm::vec3 eye, glm::vec3 dir, const DoorLeaf& d, float& tHit)
{
    const float DH = DOOR_THICK * 0.5f, LW = d.lw;
    glm::mat4 model = glm::translate(glm::mat4(1), d.hinge)
                    * glm::rotate(glm::mat4(1), glm::radians(d.curYaw), glm::vec3(0,1,0));
    glm::mat4 inv = glm::inverse(model);
    glm::vec3 ro = glm::vec3(inv * glm::vec4(eye, 1.f));
    glm::vec3 rd = glm::vec3(inv * glm::vec4(dir, 0.f));
    glm::vec3 mn(0, 0, -DH), mx(LW, DOOR_H, DH);
    float tmin = 0.f, tmax = 1e9f;
    for (int i = 0; i < 3; i++) {
        if (fabsf(rd[i]) < 1e-6f) { if (ro[i] < mn[i] || ro[i] > mx[i]) return false; }
        else {
            float t1 = (mn[i]-ro[i])/rd[i], t2 = (mx[i]-ro[i])/rd[i];
            if (t1 > t2) std::swap(t1,t2);
            tmin = std::max(tmin,t1); tmax = std::min(tmax,t2);
            if (tmin > tmax) return false;
        }
    }
    tHit = tmin;
    return tmin >= 0.f;
}

static void drawDoor(GLuint prog, const LvMesh& mesh, const glm::mat4& vp,
                     const glm::vec3& eye, GLuint tdiff, GLuint tnorm, GLuint tdisp,
                     const DoorLeaf& d)
{
    glm::mat4 model = glm::translate(glm::mat4(1), d.hinge)
                    * glm::rotate(glm::mat4(1), glm::radians(d.curYaw), glm::vec3(0,1,0))
                    * glm::scale(glm::mat4(1), glm::vec3(d.lw/1.45f, 1.0f, 1.0f));
    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog,"uVP"),   1,GL_FALSE,glm::value_ptr(vp));
    glUniformMatrix4fv(glGetUniformLocation(prog,"uModel"),1,GL_FALSE,glm::value_ptr(model));
    glUniform3fv(glGetUniformLocation(prog,"uCamPos"),1,glm::value_ptr(eye));
    glUniform1f(glGetUniformLocation(prog,"uUVScale"),  1.0f);
    glUniform1f(glGetUniformLocation(prog,"uParallax"), 0.02f);
    glUniform1f(glGetUniformLocation(prog,"uBright"),   gBright);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tdiff); glUniform1i(glGetUniformLocation(prog,"uDiff"),0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,tnorm); glUniform1i(glGetUniformLocation(prog,"uNorm"),1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D,tdisp); glUniform1i(glGetUniformLocation(prog,"uDisp"),2);
    glBindVertexArray(mesh.vao);
    glDrawElements(GL_TRIANGLES,mesh.count,GL_UNSIGNED_INT,nullptr);
    glBindVertexArray(0);
}

struct Mesh { GLuint vao,vbo,ebo; int count; };

static Mesh makeSphere(float r, int st, int sl)
{
    std::vector<float> v; std::vector<unsigned> i;
    for(int s=0;s<=st;s++){
        float phi=PI*s/st;
        for(int t=0;t<=sl;t++){
            float th=2*PI*t/sl;
            float x=r*sinf(phi)*cosf(th), y=r*cosf(phi), z=r*sinf(phi)*sinf(th);
            v.insert(v.end(),{x,y,z, x/r,y/r,z/r});
        }
    }
    for(int s=0;s<st;s++)
        for(int t=0;t<sl;t++){
            unsigned p=s*(sl+1)+t, q=p+sl+1;
            i.insert(i.end(),{p,q,p+1,p+1,q,q+1});
        }
    Mesh m;
    glGenVertexArrays(1,&m.vao); glGenBuffers(1,&m.vbo); glGenBuffers(1,&m.ebo);
    glBindVertexArray(m.vao);
    glBindBuffer(GL_ARRAY_BUFFER,m.vbo);
    glBufferData(GL_ARRAY_BUFFER,v.size()*4,v.data(),GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,m.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,i.size()*4,i.data(),GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,24,(void*)0); glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,24,(void*)12); glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    m.count=(int)i.size();
    return m;
}

static GLuint makeQuadVAO()
{
    float v[]={-1,-1,1,-1,1,1,-1,1}; unsigned i[]={0,1,2,2,3,0};
    GLuint vao,vbo,ebo;
    glGenVertexArrays(1,&vao); glGenBuffers(1,&vbo); glGenBuffers(1,&ebo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER,vbo); glBufferData(GL_ARRAY_BUFFER,sizeof(v),v,GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo); glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(i),i,GL_STATIC_DRAW);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr); glEnableVertexAttribArray(0);
    glBindVertexArray(0); return vao;
}

// ── Ceiling fixture cylinder ──────────────────────────────────────────────────
// Top ring at y=0 (flush with ceiling), bottom ring at y=-h (diffuser face)
struct FixMesh { GLuint vao=0, vbo=0, ebo=0; int count=0; };

static FixMesh makeFixCylinder(float r, float h, int N)
{
    const float TWO_PI = 6.2831853f;
    std::vector<float> v;
    std::vector<unsigned> idx;
    // Bottom ring (diffuser edge): y = -h
    for (int i = 0; i < N; i++) {
        float a = TWO_PI*i/N;
        v.insert(v.end(), {r*cosf(a), -h, r*sinf(a)});
    }
    // Top ring (against ceiling): y = 0
    for (int i = 0; i < N; i++) {
        float a = TWO_PI*i/N;
        v.insert(v.end(), {r*cosf(a), 0.f, r*sinf(a)});
    }
    // Bottom centre for disc fan
    v.insert(v.end(), {0.f, -h, 0.f});
    // Side quads
    for (int i = 0; i < N; i++) {
        unsigned b0=i, b1=(i+1)%N, t0=N+i, t1=N+(i+1)%N;
        idx.insert(idx.end(), {b0,b1,t1, b0,t1,t0});
    }
    // Bottom diffuser disc
    unsigned ctr = 2*N;
    for (int i = 0; i < N; i++)
        idx.insert(idx.end(), {ctr, (unsigned)i, (unsigned)((i+1)%N)});
    FixMesh m;
    glGenVertexArrays(1,&m.vao); glGenBuffers(1,&m.vbo); glGenBuffers(1,&m.ebo);
    glBindVertexArray(m.vao);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size()*4, v.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size()*4, idx.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    m.count = (int)idx.size();
    return m;
}

// ── Well / shaft mesh ─────────────────────────────────────────────────────────
// Inner cylinder walls (normals inward) + floor disc at y=-depth.
// UV: x = arc-length around circumference, y = 0(top)→1(bottom)
// rim_h: height of raised stone curb above floor (makes hole visible from player eye level)
static LvMesh makeWellMesh(float cx, float cz, float r, float depth, int N, float rim_h = 0.15f)
{
    const float TWO_PI = 6.2831853f;
    std::vector<float>    V;
    std::vector<unsigned> I;

    // ── Shaft walls: from rim_h above floor down to -depth ───────────────────
    float yTop = rim_h, yBot = -depth;
    for (int i = 0; i <= N; i++) {
        float a  = TWO_PI * i / N;
        float ca = cosf(a), sa = sinf(a);
        float px = cx + r*ca, pz = cz + r*sa;
        float u  = (float)i / N * (TWO_PI * r);   // arc-length 0 → 2πr
        lvV(V, {px, yTop, pz}, {-ca, 0.f, -sa}, {u, 0.f});
        lvV(V, {px, yBot, pz}, {-ca, 0.f, -sa}, {u, (yTop - yBot) / (depth + rim_h)});
    }
    for (int i = 0; i < N; i++) {
        unsigned t0=i*2, t1=i*2+1, t2=(i+1)*2, t3=(i+1)*2+1;
        I.insert(I.end(), {t0,t3,t2,  t0,t1,t3});
    }

    // ── Rim outer wall (outward-facing ring above floor) ─────────────────────
    float ro = r + 0.12f;  // outer radius of rim curb
    for (int i = 0; i <= N; i++) {
        float a  = TWO_PI * i / N;
        float ca = cosf(a), sa = sinf(a);
        float u  = (float)i / N * (TWO_PI * ro);
        lvV(V, {cx+ro*ca, 0.f,   cz+ro*sa}, {ca, 0.f, sa}, {u, 0.f});
        lvV(V, {cx+ro*ca, rim_h, cz+ro*sa}, {ca, 0.f, sa}, {u, rim_h});
    }
    unsigned owBase = 2*(N+1);
    for (int i = 0; i < N; i++) {
        unsigned t0=owBase+i*2, t1=owBase+i*2+1, t2=owBase+(i+1)*2, t3=owBase+(i+1)*2+1;
        I.insert(I.end(), {t0,t2,t3,  t0,t3,t1});
    }

    // ── Rim top annulus (flat ring from r to ro at y=rim_h, normal up) ───────
    unsigned rb = (unsigned)(V.size()/8);
    for (int i = 0; i <= N; i++) {
        float a = TWO_PI * i / N;
        float ca = cosf(a), sa = sinf(a);
        float ui = (float)i / N;
        lvV(V, {cx+r *ca, rim_h, cz+r *sa}, {0.f,1.f,0.f}, {ui, 0.f});
        lvV(V, {cx+ro*ca, rim_h, cz+ro*sa}, {0.f,1.f,0.f}, {ui, 1.f});
    }
    for (int i = 0; i < N; i++) {
        unsigned t0=rb+i*2, t1=rb+i*2+1, t2=rb+(i+1)*2, t3=rb+(i+1)*2+1;
        I.insert(I.end(), {t0,t2,t3,  t0,t3,t1});
    }

    return uploadLvMesh(V, I);
}

// ── Clouds ────────────────────────────────────────────────────────────────────
struct Puff { glm::vec3 pos,col; float size,alpha,eyeDist; };

static void addCloud(std::vector<Puff>& out, glm::vec3 center, uint32_t seed, float sc)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> dx(0,1.1f*sc), dy(0,0.5f*sc), dz(0,1.1f*sc);
    std::normal_distribution<float> wx(0,2.2f*sc), wy(0,0.75f*sc), wz(0,2.2f*sc);
    std::uniform_real_distribution<float> csz(0.45f*sc,1.05f*sc), wsz(0.20f*sc,0.55f*sc);
    std::uniform_real_distribution<float> ca(0.48f,0.76f), wa(0.18f,0.34f);
    std::uniform_real_distribution<float> jit(-0.025f,0.025f);
    for(int i=0;i<150;i++){
        Puff p; p.pos=center+glm::vec3(dx(rng),dy(rng),dz(rng));
        p.size=csz(rng); p.alpha=ca(rng);
        float d=glm::length(p.pos-center)/(2.2f*sc);
        p.col=glm::clamp(glm::vec3(1.f+jit(rng),0.97f-d*.04f+jit(rng),0.96f-d*.06f+jit(rng)),0.f,1.f);
        out.push_back(p);
    }
    for(int i=0;i<55;i++){
        Puff p; p.pos=center+glm::vec3(wx(rng),wy(rng),wz(rng));
        p.size=wsz(rng); p.alpha=wa(rng);
        p.col=glm::clamp(glm::vec3(0.95f+jit(rng),0.95f+jit(rng),0.97f+jit(rng)),0.f,1.f);
        out.push_back(p);
    }
}

// ── Garbage pile generator ────────────────────────────────────────────────────
// Pyramid shape: 4 marble-widths across base, tapers up to ~1.8 units.
// Objects at random yaw angles scattered per layer using a seeded RNG.
struct PileBuffers {
    std::vector<float>    bxV,cyV,spV,cnV,hcV;
    std::vector<unsigned> bxI,cyI,spI,cnI,hcI;
};

static void buildGarbagePile(PileBuffers& pb, float cx, float cz, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> rnd(0,1);
    auto rf=[&](float lo,float hi){ return lo+(hi-lo)*rnd(rng); };
    auto ri=[&](int n){ return (int)(rnd(rng)*n); };

    // 4 layers: base radius tapers, height rises
    struct LayerCfg { float maxR, yBase, szLo,szHi, hLo,hHi; int count; };
    LayerCfg layers[]={
        {1.7f, 0.00f, 0.35f,0.50f, 0.50f,0.72f, 5},
        {0.9f, 0.55f, 0.26f,0.38f, 0.35f,0.52f, 3},
        {0.4f, 1.05f, 0.18f,0.28f, 0.25f,0.38f, 2},
        {0.0f, 1.45f, 0.14f,0.20f, 0.18f,0.28f, 1},
    };
    for(auto& L : layers){
        for(int i=0;i<L.count;i++){
            float ang  = rf(0,6.283f);
            float dist = L.maxR>0 ? sqrtf(rnd(rng))*L.maxR : 0;
            float ox   = cx+dist*cosf(ang);
            float oz   = cz+dist*sinf(ang);
            float yaw  = rf(0,6.283f);
            float sz   = rf(L.szLo,L.szHi);
            float ht   = rf(L.hLo, L.hHi);
            float yB   = std::max(0.f, L.yBase+rf(-0.07f,0.07f));
            // 5 types: box, cylinder, sphere, cone, horizontal cylinder
            switch(ri(5)){
                case 0: addBox      (pb.bxV,pb.bxI, ox,oz, yB, sz, ht, yaw);  break;
                case 1: addCylinder (pb.cyV,pb.cyI, ox,oz, yB, sz, ht);        break;
                case 2: addSphere   (pb.spV,pb.spI, ox,oz, yB, sz);            break;
                case 3: addCone     (pb.cnV,pb.cnI, ox,oz, yB, sz, ht);        break;
                case 4: addHCylinder(pb.hcV,pb.hcI, ox,oz, yB, sz, ht, yaw);  break;
            }
        }
    }
}

// ── Physics ───────────────────────────────────────────────────────────────────
static void physics(float dt, const LevelGeo& lv)
{
    vel.y -= GRAVITY * dt;
    playerPos += vel * dt;

    onGround = false;

    // Which level are we on? Shaft zone bridges y=0 and y=-100.
    float wdx = playerPos.x - 0.f, wdz = playerPos.z - 5.6f;
    bool inShaft = (wdx*wdx + wdz*wdz) < 1.0f && playerPos.y < 0.f;
    bool inBasement = playerPos.y < -1.f && !inShaft;

    // Surface floor at y=0 — blocked by well opening
    if (!inShaft && !inBasement) {
        float dx = playerPos.x - 0.f, dz = playerPos.z - 5.6f;
        bool overWellOpening = (dx*dx + dz*dz) < 1.0f;
        if (!overWellOpening && playerPos.y <= MARBLE_R && vel.y <= 0.f) {
            playerPos.y = MARBLE_R; vel.y = 0.f; onGround = true;
        }
    }

    // Basement floor at y=-100 (main room + all side rooms: x -28..28, z -20..34)
    if (inBasement || inShaft) {
        if (playerPos.x > -28.f && playerPos.x < 28.f &&
            playerPos.z > -20.f && playerPos.z < 34.f &&
            playerPos.y <= -100.f + MARBLE_R && vel.y <= 0.f) {
            playerPos.y = -100.f + MARBLE_R; vel.y = 0.f; onGround = true;
        }
        // Basement ceiling clamp
        float bsMaxY = -100.f + WALL_H - MARBLE_R;
        if (playerPos.y > bsMaxY && playerPos.y < -1.f) { playerPos.y = bsMaxY; vel.y = 0.f; }
    }

    // Ceiling clamp (only applies on surface level)
    if (playerPos.y > 0.f) {
        float maxY = WALL_H - MARBLE_R;
        if (playerPos.y > maxY) { playerPos.y = maxY; vel.y = 0.f; }
    }

    // Respawn if fallen off the world entirely
    if (playerPos.y < -110.f) {
        playerPos = glm::vec3(0.f, MARBLE_R, -12.f);
        vel = glm::vec3(0.f);
    }

    // Wall collision (axis-aligned only)
    // collDist = marble radius + half wall thickness (walls stored by centreline)
    const float collDist = MARBLE_R + WALL_T * 0.5f;
    for (const auto& w : lv.walls) {
        // skip walls not on the player's level
        if (playerPos.y < w.yBase - 0.5f || playerPos.y > w.yBase + WALL_H + 0.5f) continue;
        if (w.cx) {
            // wall at x=w.c, spans z [lo,hi]
            if (playerPos.z > w.lo - collDist && playerPos.z < w.hi + collDist) {
                float d = playerPos.x - w.c;
                if (fabsf(d) < collDist) {
                    playerPos.x = w.c + (d >= 0.f ? collDist : -collDist);
                    vel.x = 0.f;
                }
            }
        } else {
            // wall at z=w.c, spans x [lo,hi]
            if (playerPos.x > w.lo - collDist && playerPos.x < w.hi + collDist) {
                float d = playerPos.z - w.c;
                if (fabsf(d) < collDist) {
                    playerPos.z = w.c + (d >= 0.f ? collDist : -collDist);
                    vel.z = 0.f;
                }
            }
        }
    }
}

// ── Ray–sphere intersection ────────────────────────────────────────────────────
static bool raySphere(glm::vec3 o, glm::vec3 d, glm::vec3 c, float r, float& t) {
    glm::vec3 oc = o - c;
    float b = glm::dot(oc, d);
    float disc = b*b - (glm::dot(oc,oc) - r*r);
    if (disc < 0.f) return false;
    t = -b - sqrtf(disc);
    return t > 0.001f;
}

// ── Input ─────────────────────────────────────────────────────────────────────
static bool fKeyDown = false;  // tracks F for just-pressed detection

static void cbKey(GLFWwindow* w, int k, int, int a, int) {
    if (k==GLFW_KEY_ESCAPE && a==GLFW_PRESS) glfwSetWindowShouldClose(w,1);
    if (a==GLFW_PRESS || a==GLFW_REPEAT) {
        if (k==GLFW_KEY_EQUAL)  gBright = glm::clamp(gBright+0.1f, 0.1f, 4.0f);
        if (k==GLFW_KEY_MINUS)  gBright = glm::clamp(gBright-0.1f, 0.1f, 4.0f);
    }
    if (k==GLFW_KEY_F) fKeyDown = (a != GLFW_RELEASE);
    if (k==GLFW_KEY_R && a==GLFW_PRESS) rDotVisible = !rDotVisible;
}
static void cbMouse(GLFWwindow*, double x, double y) {
    if (firstMouse) { lastX=x; lastY=y; firstMouse=false; }
    yaw   += float(x-lastX)*MOUSE_SENS;
    pitch -= float(y-lastY)*MOUSE_SENS;
    pitch  = glm::clamp(pitch,-85.f,85.f);
    lastX=x; lastY=y;
}
static void cbMouseBtn(GLFWwindow*, int btn, int action, int) {
    if (btn == GLFW_MOUSE_BUTTON_LEFT  && action == GLFW_PRESS) leftJust  = true;
    if (btn == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) rightJust = true;
}

// ── Bitmap font (5×7) ─────────────────────────────────────────────────────────
// Supported chars: 0-9  . - X Y Z  (space = fallback)
// Each glyph: 7 rows, each row = 5 bits, bit4=left col.
static const uint8_t FONT_GLYPHS[16][7] = {
    {14,17,17,17,17,17,14}, // '0'
    { 4,12, 4, 4, 4, 4,14}, // '1'
    {14,17, 1, 6, 8,16,31}, // '2'
    {30, 1, 1,14, 1, 1,30}, // '3'
    { 2, 6,10,18,31, 2, 2}, // '4'
    {31,16,16,30, 1, 1,30}, // '5'
    {14,16,16,30,17,17,14}, // '6'
    {31, 1, 2, 4, 8, 8, 8}, // '7'
    {14,17,17,14,17,17,14}, // '8'
    {14,17,17,15, 1, 1,14}, // '9'
    { 0, 0, 0, 0, 0, 4, 0}, // '.'
    { 0, 0, 0,31, 0, 0, 0}, // '-'
    {17,17,10, 4,10,17,17}, // 'X'
    {17,17,10, 4, 4, 4, 4}, // 'Y'
    {31, 1, 2, 4, 8,16,31}, // 'Z'
    { 0, 0, 0, 0, 0, 0, 0}, // ' ' / fallback
};
static int charGlyph(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c=='.') return 10; if (c=='-') return 11;
    if (c=='X') return 12; if (c=='Y') return 13; if (c=='Z') return 14;
    return 15;
}
// Build NDC triangles for text string.  Origin = bottom-right of viewport.
static void buildTextVerts(const char* str, std::vector<float>& v, int vpW, int vpH)
{
    const int SC=2, CW=5, CH=7, SP=1, MARGIN=8;
    int n = (int)strlen(str);
    int tw = n*(CW+SP)*SC - SP*SC;
    int x0 = vpW - MARGIN - tw;
    int yb = MARGIN;
    v.clear();
    for (int ci=0; ci<n; ci++) {
        const uint8_t* g = FONT_GLYPHS[charGlyph(str[ci])];
        for (int r=0; r<CH; r++) {
            for (int col=0; col<CW; col++) {
                if (!((g[r]>>(CW-1-col))&1)) continue;
                int sx0 = x0 + (ci*(CW+SP)+col)*SC;
                int sy0 = yb + (CH-1-r)*SC;
                float fx0=sx0*2.f/vpW-1.f, fx1=(sx0+SC)*2.f/vpW-1.f;
                float fy0=sy0*2.f/vpH-1.f, fy1=(sy0+SC)*2.f/vpH-1.f;
                v.insert(v.end(),{fx0,fy0,fx1,fy0,fx1,fy1,fx0,fy0,fx1,fy1,fx0,fy1});
            }
        }
    }
}

// Ray vs axis-aligned level scene → world-space hit point (or sentinel {0,-1,0})
static glm::vec3 raycastScene(glm::vec3 eye, glm::vec3 dir,
                               const LevelGeo& lv,
                               const DoorLeaf* doors, int nDoors)
{
    float best = 80.f;
    glm::vec3 hit(0,-1,0);

    auto tryT = [&](float t, glm::vec3 p){
        if (t>0.001f && t<best) { best=t; hit=p; }
    };

    // Floor y=0
    if (fabsf(dir.y)>1e-5f) {
        float t = -eye.y/dir.y;
        tryT(t, eye+dir*t);
    }
    // Ceiling y=WALL_H
    if (fabsf(dir.y)>1e-5f) {
        float t = (WALL_H-eye.y)/dir.y;
        tryT(t, eye+dir*t);
    }
    // Axis-aligned walls
    for (const auto& w : lv.walls) {
        if (w.cx) {
            if (fabsf(dir.x)<1e-5f) continue;
            float t=(w.c-eye.x)/dir.x;
            glm::vec3 p=eye+dir*t;
            if (p.z>=w.lo&&p.z<=w.hi&&p.y>=0.f&&p.y<=WALL_H) tryT(t,p);
        } else {
            if (fabsf(dir.z)<1e-5f) continue;
            float t=(w.c-eye.z)/dir.z;
            glm::vec3 p=eye+dir*t;
            if (p.x>=w.lo&&p.x<=w.hi&&p.y>=0.f&&p.y<=WALL_H) tryT(t,p);
        }
    }
    // Door leaves
    for (int i=0;i<nDoors;i++) {
        float t; if (rayHitDoor(eye,dir,doors[i],t)) tryT(t,eye+dir*t);
    }
    return hit;
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main()
{
    // ── Music ─────────────────────────────────────────────────────────────────
    SDL_Init(SDL_INIT_AUDIO);
    Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048);
    Mix_Music* bgm = Mix_LoadMUS("creepy_classical.mp3");
    // Music plays only when player is inside — starts paused

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES,4);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(mon);
    GLFWwindow* win = glfwCreateWindow(mode->width, mode->height,
        "Blue Marble  |  WASD: move   Space: jump   Esc: quit", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwSetWindowPos(win, 0, 0);
    glfwMakeContextCurrent(win);
    glfwSetInputMode(win,GLFW_CURSOR,GLFW_CURSOR_DISABLED);
    glfwSetKeyCallback(win,cbKey);
    glfwSetCursorPosCallback(win,cbMouse);
    glfwSetMouseButtonCallback(win,cbMouseBtn);

    glewExperimental=GL_TRUE;
    if (glewInit()!=GLEW_OK) return 1;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);
    glDisable(GL_CULL_FACE);

    // Shaders
    GLuint progLv     = buildProg(VS_LV,   FS_LV);
    GLuint progFlat   = buildProg(VS_LV,   FS_FLAT);
    GLuint progDoor   = buildProg(VS_DOOR, FS_LV);
    GLuint progText   = buildProg(VS_TEXT, FS_TEXT);
    GLuint progMarble = buildProg(VS_MB,   FS_MB);
    GLuint progCloud  = buildProg(VS_CL,   FS_CL);
    GLuint progFix    = buildProg(VS_FIX,  FS_FIX);
    GLuint progPP     = buildProg(VS_PP,   FS_PP);
    GLuint progMark   = buildProg(VS_LV,   FS_MARK);
    GLuint progPin    = buildProg(VS_PIN,  FS_PIN);
    GLuint progWell   = buildProg(VS_LV,   FS_WELL);
    GLuint progPat    = buildProg(VS_PAT,  FS_PAT);

    // FBO: colour + depth textures (depth needed for world-space dither reconstruction)
    GLuint fbo=0, fboTex=0, fboDepTex=0;
    int fboW=0, fboH=0;
    auto rebuildFBO=[&](int w,int h){
        if(fboTex)    glDeleteTextures(1,&fboTex);
        if(fboDepTex) glDeleteTextures(1,&fboDepTex);
        if(fbo)       glDeleteFramebuffers(1,&fbo);
        glGenFramebuffers(1,&fbo);
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        // colour attachment
        glGenTextures(1,&fboTex);
        glBindTexture(GL_TEXTURE_2D,fboTex);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,w,h,0,GL_RGB,GL_UNSIGNED_BYTE,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,fboTex,0);
        // depth attachment (texture so it can be sampled for world-pos reconstruction)
        glGenTextures(1,&fboDepTex);
        glBindTexture(GL_TEXTURE_2D,fboDepTex);
        glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT32F,w,h,0,GL_DEPTH_COMPONENT,GL_FLOAT,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,fboDepTex,0);
        glBindFramebuffer(GL_FRAMEBUFFER,0);
        fboW=w; fboH=h;
    };
    rebuildFBO(WIN_W,WIN_H);

    // Sphere arena GPU init (needs GL context)
    sphInit();

    // Textures
    MatTex matFloor = loadMat(TEXTURES_DIR, "slate_floor_02");
    MatTex matWall  = loadMat(TEXTURES_DIR, "beige_wall_001");
    MatTex matCeil  = loadMat(TEXTURES_DIR, "concrete_floor_worn_001");
    MatTex matWood  = loadMat(TEXTURES_DIR, "kitchen_wood");
    MatTex matTrim  = loadMat(TEXTURES_DIR, "white_plaster_02");
    MatTex matBrick       = loadMat(TEXTURES_DIR, "brick_wall");
    MatTex matBlackMarble = loadMat(TEXTURES_DIR, "black_marble");

    // Level
    LevelGeo lv = buildLevel();
    LvMesh stoneMesh = uploadLvMesh(lv.sv, lv.si);

    // Black marble floor: south room 0 (door at x=-9, z=-4), slightly above stone
    LvMesh blackMarbleFloor;
    {
        LevelGeo tmp;
        lvFloor(tmp, -12.f, -20.f, -6.f, -4.f, -99.995f);
        blackMarbleFloor = uploadLvMesh(tmp.sv, tmp.si);
    }
    LvMesh wallMesh  = uploadLvMesh(lv.wv, lv.wi);
    LvMesh ceilMesh  = uploadLvMesh(lv.cv, lv.ci);
    LvMesh doorMesh  = uploadLvMesh(lv.dv, lv.di);
    LvMesh trimMesh  = uploadLvMesh(lv.tv, lv.ti);
    LvMesh brickMesh        = uploadLvMesh(lv.bv,    lv.bi);
    LvMesh wainMesh         = uploadLvMesh(lv.painv, lv.paini);
    LvMesh pathMesh         = uploadLvMesh(lv.pathv, lv.pathi);
    LvMesh marblePillarMesh = uploadLvMesh(lv.marv,  lv.mari);

    // ── Garbage piles — one per path segment, pyramid shape, random angles ───
    const float pilePos[][2] = {
        {-13.f,-22.f}, { -6.f,-22.5f}, {  1.f,-21.f},  {  7.f,-18.5f},
        { 14.f,-12.f}, {  9.5f,-8.5f}, {  3.5f,-7.5f}, { -8.f, -8.5f},
        {-10.f,  -4.f},{-6.5f,  -1.5f},{-1.5f, -19.f}, {0.5f, -12.f},
        {-10.f,-18.5f},{ -8.f,-12.5f},
    };
    PileBuffers pb;
    for(int i=0;i<14;i++)
        buildGarbagePile(pb, pilePos[i][0], pilePos[i][1], (uint32_t)(i*7919+42));
    LvMesh propBoxMesh = uploadLvMesh(pb.bxV, pb.bxI);
    LvMesh propCylMesh = uploadLvMesh(pb.cyV, pb.cyI);
    LvMesh propSphMesh = uploadLvMesh(pb.spV, pb.spI);
    LvMesh propCnMesh  = uploadLvMesh(pb.cnV, pb.cnI);
    LvMesh propHcMesh  = uploadLvMesh(pb.hcV, pb.hcI);

    FixMesh fixMesh  = makeFixCylinder(0.22f, 0.07f, 32);

    // ── Pattern door panels (one unit quad, 16 draw calls) ───────────────────
    GLuint patVAO=0, patVBO=0, patEBO=0;
    {
        float v[]={-0.5f,0.f,0.f,0.f,0.f,  0.5f,0.f,0.f,1.f,0.f,
                    0.5f,1.f,0.f,1.f,1.f,  -0.5f,1.f,0.f,0.f,1.f};
        unsigned idx[]={0,1,2,0,2,3};
        glGenVertexArrays(1,&patVAO); glGenBuffers(1,&patVBO); glGenBuffers(1,&patEBO);
        glBindVertexArray(patVAO);
        glBindBuffer(GL_ARRAY_BUFFER,patVBO);
        glBufferData(GL_ARRAY_BUFFER,sizeof(v),v,GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,patEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(idx),idx,GL_STATIC_DRAW);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,20,(void*)0);  glEnableVertexAttribArray(0);
        glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,20,(void*)12); glEnableVertexAttribArray(1);
        glBindVertexArray(0);
    }
    struct BsDoor { glm::vec3 pos; float rotY; int pat; glm::vec3 c1,c2; };
    // 4 walls × 4 doors; c1=dark background, c2=bright foreground
    static const BsDoor bsDoors[16] = {
        // ── South wall (z=-4), x = -9,-3,3,9 ─────────────────────────────
        {{-9.f,-100.f,-4.f},0.f, 0,{.03f,.03f,.06f},{.85f,.82f,.95f}}, // polka dots: indigo
        {{-3.f,-100.f,-4.f},0.f, 1,{.06f,.04f,.02f},{.80f,.68f,.45f}}, // herringbone: warm tan
        {{ 3.f,-100.f,-4.f},0.f, 2,{.06f,.01f,.01f},{.92f,.88f,.80f}}, // thin h-stripes: red/cream
        {{ 9.f,-100.f,-4.f},0.f, 3,{.02f,.07f,.07f},{.80f,.95f,.92f}}, // wide h-stripes: teal
        // ── North wall (z=18), x = -9,-3,3,9 ─────────────────────────────
        {{-9.f,-100.f,18.f},0.f, 4,{.05f,.02f,.08f},{.82f,.72f,.18f}}, // diamonds: purple/gold
        {{-3.f,-100.f,18.f},0.f, 5,{.02f,.02f,.02f},{.92f,.92f,.92f}}, // checkerboard: B&W
        {{ 3.f,-100.f,18.f},0.f, 6,{.02f,.06f,.02f},{.85f,.92f,.80f}}, // thin v-stripes: green
        {{ 9.f,-100.f,18.f},0.f, 7,{.05f,.05f,.01f},{.88f,.85f,.65f}}, // diagonal /: khaki
        // ── West wall (x=-12), z = -1.25, 4.25, 9.75, 15.25 ──────────────
        {{-12.f,-100.f,-1.25f},90.f, 8,{.08f,.04f,.01f},{.92f,.72f,.40f}}, // diagonal \: orange
        {{-12.f,-100.f, 4.25f},90.f, 9,{.06f,.01f,.02f},{.88f,.82f,.72f}}, // crosshatch: wine
        {{-12.f,-100.f, 9.75f},90.f,10,{.04f,.04f,.01f},{.90f,.85f,.22f}}, // chevron: yellow
        {{-12.f,-100.f,15.25f},90.f,11,{.01f,.07f,.08f},{.60f,.92f,.88f}}, // rings: cyan
        // ── East wall (x=12), z = -1.25, 4.25, 9.75, 15.25 ───────────────
        {{12.f,-100.f,-1.25f},90.f,12,{.04f,.04f,.08f},{.80f,.82f,.92f}}, // triangles: slate
        {{12.f,-100.f, 4.25f},90.f,13,{.07f,.02f,.01f},{.90f,.62f,.48f}}, // wave: coral
        {{12.f,-100.f, 9.75f},90.f,14,{.04f,.02f,.08f},{.78f,.72f,.95f}}, // large dots: lavender
        {{12.f,-100.f,15.25f},90.f,15,{.02f,.06f,.02f},{.75f,.92f,.70f}}, // wide v-stripes: mint
    };

    // Furniture
    FurGeo fur = buildFurniture();
    LvMesh furWoodMesh  = uploadLvMesh(fur.woodv,  fur.woodi);
    LvMesh furStoneMesh = uploadLvMesh(fur.stonev, fur.stonei);
    LvMesh furUphMesh   = uploadLvMesh(fur.uphv,   fur.uphi);
    LvMesh furScrnMesh  = uploadLvMesh(fur.scrnv,  fur.scrni);

    // Baphomet figure — back of basement marble room (south room 0, x=-9, z=-18)
    LvMesh baphometMesh;
    {
        std::vector<float>    bv;
        std::vector<unsigned> bi;
        buildBaphomet(bv, bi, -9.f, -18.f, -100.f);
        baphometMesh = uploadLvMesh(bv, bi);
    }

    // Light fixture positions: lp0/lp1/lp2/lp3 upstairs + basement marble room
    const glm::vec3 ROOM_LIGHTS[] = {
        {0.f,  WALL_H,   5.f},   // Room A
        {0.f,  WALL_H,  16.f},   // Room B north half
        {10.f, WALL_H,   5.f},   // Room C
        {0.f,  WALL_H,  12.f},   // Room B south half (lp3)
        {-9.f, -96.8f, -12.f},   // basement marble room
    };

    // Marble
    Mesh sphereMesh = makeSphere(MARBLE_R, 32, 32);
    // Pin marker sphere (small)
    Mesh pinSphere  = makeSphere(0.06f, 12, 12);

    // Well at last-placed pin position
    LvMesh wellMesh = makeWellMesh(0.0f, 5.6f, 1.0f, 100.0f, 48);

    // ── Wall buttons (positions identified with the pin tool, now hardcoded) ────
    // South wall (z=0), right cluster x≈+1.85, left cluster x≈-1.95
    static const float BTN_POS[6][2] = {
        { 1.8559f,  1.26723f},  // right low
        { 1.81612f, 1.82723f},  // right mid
        { 1.84594f, 2.33097f},  // right high
        {-1.91107f, 2.17641f},  // left high
        {-1.9518f,  1.59806f},  // left mid
        {-1.98248f, 1.139f  },  // left low
    };
    std::vector<WallButton> wButtons;
    for (int i = 0; i < 6; i++) {
        auto bg = buildOneButton(BTN_POS[i][0], BTN_POS[i][1], i);
        WallButton wb;
        wb.geo      = bg;
        wb.bodyMesh = uploadLvMesh(bg.bodyv, bg.bodyi);
        wb.symMesh  = uploadLvMesh(bg.symv,  bg.symi);
        wButtons.push_back(std::move(wb));
    }

    // ── Clear pin cache each startup (fresh placement session) ───────────────
    { std::ofstream f(SPHERE_CACHE, std::ios::trunc); }

    // ── Animated door leaves ──────────────────────────────────────────────────
    // Local mesh: x[0..LW], y[0..DOOR_H], z[-DH..DH].  Wide faces on ±Z.
    LvMesh localDoorMesh;
    {
        LevelGeo tmp;
        lvDoorBox(tmp, {0,0,-DOOR_THICK*0.5f}, {1.45f,DOOR_H,DOOR_THICK*0.5f}, true);
        localDoorMesh = uploadLvMesh(tmp.dv, tmp.di);
    }
    // {hinge, closedYaw, openYaw, curYaw, targetYaw, open [, lw]}
    DoorLeaf doors[7] = {
        // South entrance (z=0, gap x=-1.5..1.5) → opens into Room A (+Z)
        { {-1.5f,0,0},    0.f,  -90.f,   0.f,   0.f, false },  // left  leaf
        { { 1.5f,0,0},  180.f,  270.f, 180.f, 180.f, false },  // right leaf
        // A/B divider (z=10, gap x=-1.5..1.5) → opens into Room B (+Z)
        { {-1.5f,0,10},   0.f,  -90.f,   0.f,   0.f, false },
        { { 1.5f,0,10}, 180.f,  270.f, 180.f, 180.f, false },
        // A/C divider (x=6, gap z=3.5..6.5) → opens into Room C (+X)
        { {6,0,3.5f},  -90.f,    0.f, -90.f, -90.f, false },  // south leaf
        { {6,0,6.5f},   90.f,    0.f,  90.f,  90.f, false },  // north leaf
        // Prison cell door — east perimeter wall (x=20, gap z=-20.6..-19.4)
        // Single leaf, 1 marble wide (0.9), swings outward (+X) when open
        { {20,0,-20.6f}, -90.f, 0.f, -90.f, -90.f, false, 0.9f },
    };

    bool prevSpace = false;

    // Text overlay VAO/VBO (dynamic, updated each frame)
    GLuint textVAO, textVBO;
    glGenVertexArrays(1,&textVAO); glGenBuffers(1,&textVBO);
    glBindVertexArray(textVAO);
    glBindBuffer(GL_ARRAY_BUFFER,textVBO);
    glBufferData(GL_ARRAY_BUFFER,0,nullptr,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,8,nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // Clouds — three layers: low/local, mid-sky, far horizon
    GLuint quadVAO = makeQuadVAO();
    std::vector<Puff> puffs;
    struct CloudDef { float x,y,z; uint32_t seed; float sc; };
    static const CloudDef cdata[] = {
        // ── low layer: directly above outdoor + building ──
        {  -3, 13,  -7,   777, 1.10f}, {  11, 12, -15,    42, 0.90f},
        {  -8, 14, -20,  1337, 1.20f}, {   4, 13,  -4,    99, 0.85f},
        {  -6, 15, -12,   555, 1.00f}, {  14, 11, -22,  2048, 0.95f},
        {   7, 14,   3,  3141, 1.05f}, { -15, 12, -18,  2718, 0.80f},
        {  18, 13,  -8,  9999, 1.15f}, {  -2, 11, -28,  4567, 0.90f},
        {  10, 15, -32,  8765, 1.10f}, { -20, 14,  -5,  1111, 1.00f},
        // ── mid layer: wider spread, higher, bigger ───────
        {  -5, 18, -40,  2222, 1.40f}, {  20, 17, -35,  3333, 1.30f},
        { -30, 19, -25,  4444, 1.60f}, {  35, 18, -10,  5555, 1.20f},
        {  -1, 20, -50,  6666, 1.50f}, {  15, 19,  10,  7777, 1.10f},
        { -40, 17, -15,  8888, 1.30f}, {  28, 21, -45,  9090, 1.70f},
        { -18, 16,  15,  1234, 1.00f}, {  45, 20, -30,  5678, 1.40f},
        {   2, 18,  20,  9753, 1.25f}, { -25, 20, -52,  3579, 1.45f},
        {  38, 17,  -2,  2468, 1.15f}, { -12, 19, -38,  1357, 1.35f},
        // ── far horizon layer: massive, very distant ──────
        { -55, 24, -60,  1010, 2.00f}, {  50, 22, -55,  2020, 1.80f},
        {  10, 25, -65,  3030, 2.20f}, { -35, 23,  25,  4040, 1.90f},
        {  60, 21,  15,  5050, 1.70f}, { -60, 26, -35,  6060, 2.10f},
        {  30, 24,  30,  7070, 1.80f}, { -10, 27, -70,  8080, 2.30f},
        {  55, 25, -15,  9191, 2.00f}, { -45, 23,  40,  8282, 1.95f},
    };
    for (auto& c : cdata) addCloud(puffs, {c.x,c.y,c.z}, c.seed, c.sc);

    double prevTime = glfwGetTime();

    while (!glfwWindowShouldClose(win)) {
        double now = glfwGetTime();
        float  dt  = glm::clamp(float(now-prevTime), 0.f, 0.05f);
        prevTime   = now;

        // ── View direction (needed pre-physics for door interaction) ──────────
        float yr = glm::radians(yaw);
        float pr = glm::radians(pitch);
        glm::vec3 eye = playerPos + glm::vec3(0,EYE_ABOVE,0);
        glm::vec3 dir(sinf(yr)*cosf(pr), sinf(pr), -cosf(yr)*cosf(pr));

        // ── WASD ──────────────────────────────────────────────────────────────
        glm::vec3 fwd(sinf(yr),0,-cosf(yr));
        glm::vec3 rgt = glm::normalize(glm::cross(fwd,glm::vec3(0,1,0)));
        glm::vec3 move(0);
        if (glfwGetKey(win,GLFW_KEY_W)==GLFW_PRESS) move+=fwd;
        if (glfwGetKey(win,GLFW_KEY_S)==GLFW_PRESS) move-=fwd;
        if (glfwGetKey(win,GLFW_KEY_A)==GLFW_PRESS) move-=rgt;
        if (glfwGetKey(win,GLFW_KEY_D)==GLFW_PRESS) move+=rgt;
        if (glm::length(move)>0.001f) { move=glm::normalize(move)*MOVE_SPEED; vel.x=move.x; vel.z=move.z; }
        else { vel.x*=0.80f; vel.z*=0.80f; }

        // ── Door animation ────────────────────────────────────────────────────
        for (auto& d : doors) {
            float delta = d.targetYaw - d.curYaw;
            float step  = 150.f * dt;
            if (fabsf(delta) <= step) d.curYaw = d.targetYaw;
            else d.curYaw += (delta > 0.f ? step : -step);
        }

        // ── Door interaction (space, just-pressed, near + raycast) ────────────
        bool spaceDown = (glfwGetKey(win,GLFW_KEY_SPACE)==GLFW_PRESS);
        bool spaceJust = spaceDown && !prevSpace;
        prevSpace = spaceDown;
        bool doorToggled = false;
        if (spaceJust) {
            float bestT = 5.f;
            int   bestI = -1;
            for (int i = 0; i < 7; i++) {
                glm::vec3 h2d(doors[i].hinge.x, 0, doors[i].hinge.z);
                if (glm::length(h2d - glm::vec3(playerPos.x,0,playerPos.z)) > 3.5f) continue;
                float t;
                if (rayHitDoor(eye, dir, doors[i], t) && t < bestT) { bestT=t; bestI=i; }
            }
            if (bestI >= 0) {
                doors[bestI].open       = !doors[bestI].open;
                doors[bestI].targetYaw  =  doors[bestI].open
                                        ?  doors[bestI].openYaw
                                        :  doors[bestI].closedYaw;
                doorToggled = true;
            }
        }

        // ── Jump ──────────────────────────────────────────────────────────────
        if (spaceJust && !doorToggled && onGround) { vel.y=JUMP_V; onGround=false; }

        // ── F: press nearest looked-at button within 3.5 m ───────────────────
        static bool prevFKey = false;
        bool fJust = fKeyDown && !prevFKey;
        prevFKey = fKeyDown;
        if (fJust && !wButtons.empty()) {
            float bestT = 3.5f;
            int   bestI = -1;
            for (int i = 0; i < (int)wButtons.size(); i++) {
                float t;
                if (rayButton(eye, dir, wButtons[i].geo, t) && t < bestT)
                    { bestT = t; bestI = i; }
            }
            if (bestI >= 0)
                wButtons[bestI].colorState = (wButtons[bestI].colorState + 1) % BTN_NUM_STATES;
        }

        if (gScene == 0) physics(dt, lv);

        // ── Sphere arena entry: rings door (x=-12, z=13.75..16.75, basement) ───
        if (gScene == 0) {
            static int sphPrevSide = 0;
            int sphCurSide = (playerPos.x < -12.f) ? 1 : 2;
            bool inRingsDoorZ = (playerPos.z >= 13.75f && playerPos.z <= 16.75f);
            if (playerPos.y < -1.f && inRingsDoorZ &&
                sphPrevSide == 2 && sphCurSide == 1) {
                gEntryPos = playerPos;
                gScene    = 1;
                sphReset();
            }
            sphPrevSide = sphCurSide;
        }

        // ── Sphere scene update ────────────────────────────────────────────────
        if (gScene == 1) {
            bool kW = glfwGetKey(win,GLFW_KEY_W)==GLFW_PRESS;
            bool kA = glfwGetKey(win,GLFW_KEY_A)==GLFW_PRESS;
            bool kS = glfwGetKey(win,GLFW_KEY_S)==GLFW_PRESS;
            bool kD = glfwGetKey(win,GLFW_KEY_D)==GLFW_PRESS;

            // Weapons
            if (leftJust)  { leftJust  = false; sphFireBall(sphPos,  dir); }
            if (rightJust) { rightJust = false; sphFireLaser(sphPos, dir); }

            bool hitWall = sphUpdate(dt, yaw, pitch, kW, kA, kS, kD);
            if (hitWall) {
                // Exit sphere — land player back east of the rings door
                gScene      = 0;
                playerPos   = gEntryPos + glm::vec3(0.6f, 0.f, 0.f);
                vel         = glm::vec3(0.f);
            }
        }

        // ── Music: play creepy classical only when inside building/basement ────
        if (bgm) {
            bool insideNow = (playerPos.x >= -6.f && playerPos.x <= 14.f &&
                              playerPos.z >=  0.f && playerPos.z <= 22.f) ||
                             (playerPos.y < -1.f);
            static bool wasInside = false;
            if (insideNow && !wasInside) {
                Mix_PlayMusic(bgm, -1);
            } else if (!insideNow && wasInside) {
                Mix_HaltMusic();
            }
            wasInside = insideNow;
        }

        // ── Camera (update eye after physics moves player) ────────────────────
        eye = playerPos + glm::vec3(0,EYE_ABOVE,0);

        // ── Raycast hit, pin hover, left-click ───────────────────────────────
        cursorHit = raycastScene(eye, dir, lv, doors, 7);
        hoveredPin = -1;
        {
            float bestT = 1e9f;
            for (int i = 0; i < (int)pins.size(); i++) {
                float t;
                if (raySphere(eye, dir, pins[i], 0.12f, t) && t < bestT)
                    { bestT = t; hoveredPin = i; }
            }
        }
        rightJust = false;  // unused in normal scene
        // ── Left-click: activate wall buttons (checked before pin placement) ──
        if (leftJust && !wButtons.empty()) {
            float bestT = 3.5f;
            int   bestI = -1;
            for (int i = 0; i < (int)wButtons.size(); i++) {
                float t;
                if (rayButton(eye, dir, wButtons[i].geo, t) && t < bestT)
                    { bestT = t; bestI = i; }
            }
            if (bestI >= 0) {
                wButtons[bestI].active = !wButtons[bestI].active;
                leftJust = false;
            }
        }
        if (leftJust) {
            leftJust = false;
            if (hoveredPin >= 0) {
                pins.erase(pins.begin() + hoveredPin);
                hoveredPin = -1;
            } else if (cursorHit.y >= -0.5f) {
                pins.push_back(cursorHit);
            }
            std::ofstream f(SPHERE_CACHE);
            for (auto& p : pins) f << p.x << " " << p.y << " " << p.z << "\n";
        }

        int fw,fh; glfwGetFramebufferSize(win,&fw,&fh);

        glm::mat4 proj = glm::perspective(glm::radians(75.f),float(WIN_W)/WIN_H,0.02f,300.f);

        // ── Sphere scene: render direct to screen (bypass FBO fog) ───────────
        if (gScene == 1) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            float aspect = float(WIN_W)/float(WIN_H);
            int vpW=fw, vpH=int(fw/aspect);
            if(vpH>fh){vpH=fh;vpW=int(fh*aspect);}
            glViewport((fw-vpW)/2,(fh-vpH)/2,vpW,vpH);
            sphDraw(proj, yaw, pitch);
            glfwSwapBuffers(win);
            glfwPollEvents();
            continue;  // skip normal scene rendering
        }

        glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        glViewport(0,0,fboW,fboH);
        glm::mat4 view = glm::lookAt(eye, eye+dir, glm::vec3(0,1,0));
        glm::mat4 vp   = proj*view;

        glClearColor(0.01f,0.01f,0.03f,1.f);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

        // ── Level geometry ────────────────────────────────────────────────────
        drawLvMesh(progLv, stoneMesh,       vp, eye, matFloor.diff,       matFloor.norm,       matFloor.disp,       0.5f,  0.04f);
        drawLvMesh(progLv, blackMarbleFloor, vp, eye, matBlackMarble.diff, matBlackMarble.norm, matBlackMarble.disp, 0.5f,  0.02f);
        drawLvMesh(progLv, marblePillarMesh, vp, eye, matBlackMarble.diff, matBlackMarble.norm, matBlackMarble.disp, 0.5f,  0.02f);
        // ── Well shaft ────────────────────────────────────────────────────────
        if (wellMesh.count) {
            glUseProgram(progWell);
            glUniformMatrix4fv(glGetUniformLocation(progWell,"uVP"),1,GL_FALSE,glm::value_ptr(vp));
            glUniform1f(glGetUniformLocation(progWell,"uBright"), gBright);
            glBindVertexArray(wellMesh.vao);
            glDrawElements(GL_TRIANGLES,wellMesh.count,GL_UNSIGNED_INT,nullptr);
            glBindVertexArray(0);
        }
        // ── Basement patterned doors ──────────────────────────────────────────
        {
            glUseProgram(progPat);
            GLint uMVP=glGetUniformLocation(progPat,"uMVP");
            GLint uPat=glGetUniformLocation(progPat,"uPat");
            GLint uC1 =glGetUniformLocation(progPat,"uC1");
            GLint uC2 =glGetUniformLocation(progPat,"uC2");
            glBindVertexArray(patVAO);
            for (const auto& d : bsDoors) {
                glm::mat4 M = glm::translate(glm::mat4(1.f), d.pos)
                            * glm::rotate(glm::mat4(1.f), glm::radians(d.rotY), glm::vec3(0,1,0))
                            * glm::scale(glm::mat4(1.f), glm::vec3(3.f, DOOR_H, 1.f));
                glUniformMatrix4fv(uMVP,1,GL_FALSE,glm::value_ptr(vp*M));
                glUniform1i(uPat, d.pat);
                glUniform3fv(uC1,1,glm::value_ptr(d.c1));
                glUniform3fv(uC2,1,glm::value_ptr(d.c2));
                glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,nullptr);
            }
            glBindVertexArray(0);
        }
        // White winding path on outdoor floor
        if (pathMesh.count) {
            glUseProgram(progMark);
            glUniformMatrix4fv(glGetUniformLocation(progMark,"uVP"),1,GL_FALSE,glm::value_ptr(vp));
            glBindVertexArray(pathMesh.vao);
            glDrawElements(GL_TRIANGLES,pathMesh.count,GL_UNSIGNED_INT,nullptr);
            glBindVertexArray(0);
        }
        drawLvMesh(progLv, wallMesh,     vp, eye, matWall.diff,  matWall.norm,  matWall.disp,  0.5f,  0.02f);
        drawLvMesh(progLv, ceilMesh,     vp, eye, matCeil.diff,  matCeil.norm,  matCeil.disp,  0.25f, 0.015f);
        for (int i = 0; i < 7; i++)
            drawDoor(progDoor, localDoorMesh, vp, eye, matWood.diff, matWood.norm, matWood.disp, doors[i]);
        drawLvMesh(progLv, trimMesh,     vp, eye, matTrim.diff,  matTrim.norm,  matTrim.disp,  0.25f, 0.01f);
        drawLvMesh(progLv, brickMesh,    vp, eye, matBrick.diff, matBrick.norm, matBrick.disp, 0.5f,  0.03f);
        drawLvMesh(progLv, wainMesh,     vp, eye, matWood.diff,  matWood.norm,  matWood.disp,  0.6f,  0.008f);
        // ── Furniture ─────────────────────────────────────────────────────────
        drawLvMesh(progLv, furWoodMesh,  vp, eye, matWood.diff,  matWood.norm,  matWood.disp,  0.8f,  0.012f);
        drawLvMesh(progLv, furStoneMesh, vp, eye, matFloor.diff, matFloor.norm, matFloor.disp, 0.4f,  0.02f);
        drawFlatMesh(progFlat, furUphMesh,  vp, {0.13f, 0.15f, 0.22f});  // charcoal blue
        drawFlatMesh(progFlat, furScrnMesh, vp, {0.04f, 0.04f, 0.05f});  // near-black
        // Baphomet — dark reddish-black stone
        drawFlatMesh(progFlat, baphometMesh, vp, {0.10f, 0.03f, 0.03f});
        // ── Outdoor prop piles ────────────────────────────────────────────────
        drawFlatMesh(progFlat, propBoxMesh, vp, {0.58f, 0.56f, 0.52f});  // concrete
        drawFlatMesh(progFlat, propCylMesh, vp, {0.48f, 0.34f, 0.22f});  // rust brown
        drawFlatMesh(progFlat, propSphMesh, vp, {0.20f, 0.35f, 0.70f});  // cobalt
        drawFlatMesh(progFlat, propCnMesh,  vp, {0.52f, 0.48f, 0.28f});  // sandy
        drawFlatMesh(progFlat, propHcMesh,  vp, {0.38f, 0.42f, 0.36f});  // olive

        // ── Wall buttons ─────────────────────────────────────────────────────
        for (auto& wb : wButtons) {
            drawFlatMesh(progFlat, wb.bodyMesh, vp, BTN_COLORS[wb.colorState]);
            if (!wb.active)
                drawFlatMesh(progFlat, wb.symMesh, vp, {1.f, 1.f, 1.f});
        }

        // ── Marble (look down to see it) ──────────────────────────────────────
        {
            glm::mat4 M   = glm::translate(glm::mat4(1),playerPos);
            glm::mat4 MVP = vp*M;
            glUseProgram(progMarble);
            glUniformMatrix4fv(glGetUniformLocation(progMarble,"uMVP"),1,GL_FALSE,glm::value_ptr(MVP));
            glUniformMatrix4fv(glGetUniformLocation(progMarble,"uM"),  1,GL_FALSE,glm::value_ptr(M));
            glUniform3fv(glGetUniformLocation(progMarble,"uCamPos"),1,glm::value_ptr(eye));
            glUniform1f(glGetUniformLocation(progMarble,"uBright"), gBright);
            glBindVertexArray(sphereMesh.vao);
            glDrawElements(GL_TRIANGLES,sphereMesh.count,GL_UNSIGNED_INT,nullptr);
        }

        // ── Light fixtures (semi-transparent, after all opaque) ───────────────
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glUseProgram(progFix);
        glUniform1f(glGetUniformLocation(progFix,"uBright"), gBright);
        glBindVertexArray(fixMesh.vao);
        for (const auto& lp : ROOM_LIGHTS) {
            glm::mat4 MVP = vp * glm::translate(glm::mat4(1), lp);
            glUniformMatrix4fv(glGetUniformLocation(progFix,"uMVP"),1,GL_FALSE,glm::value_ptr(MVP));
            glDrawElements(GL_TRIANGLES, fixMesh.count, GL_UNSIGNED_INT, nullptr);
        }
        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);

        // ── Clouds ────────────────────────────────────────────────────────────
        for (auto& p:puffs) { glm::vec4 ec=view*glm::vec4(p.pos,1); p.eyeDist=-ec.z; }
        std::sort(puffs.begin(),puffs.end(),[](const Puff& a,const Puff& b){ return a.eyeDist<b.eyeDist; });

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glUseProgram(progCloud);
        glUniformMatrix4fv(glGetUniformLocation(progCloud,"uView"),1,GL_FALSE,glm::value_ptr(view));
        glUniformMatrix4fv(glGetUniformLocation(progCloud,"uProj"),1,GL_FALSE,glm::value_ptr(proj));
        GLint lC=glGetUniformLocation(progCloud,"uCenter");
        GLint lS=glGetUniformLocation(progCloud,"uSize");
        GLint lCl=glGetUniformLocation(progCloud,"uColor");
        GLint lA=glGetUniformLocation(progCloud,"uAlpha");
        glBindVertexArray(quadVAO);
        for (const auto& p:puffs){
            glUniform3fv(lC,1,glm::value_ptr(p.pos));
            glUniform1f(lS,p.size);
            glUniform3fv(lCl,1,glm::value_ptr(p.col));
            glUniform1f(lA,p.alpha);
            glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,nullptr);
        }
        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);

        // ── Doorway dither toggle: south wall door 0 (x=-9, z=-4, indigo polka) ─
        // Fires only when the player actually crosses the wall plane through the
        // door opening.  prevSide tracks which side of z=-4 they were on last
        // frame; a change while within the door's x-span means they crossed.
        {
            static int prevSide = 0;          // 0=unset, 1=south(z<-4), 2=north(z>-4)
            int curSide = (playerPos.z < -4.f) ? 1 : 2;
            bool inDoorX = (playerPos.x >= -10.5f && playerPos.x <= -7.5f);

            if (playerPos.y < -1.f && inDoorX && prevSide != 0 && prevSide != curSide)
                gDither = !gDither;

            prevSide = curSide;
        }

        // ── FBO resolution: full monitor when in room, low-res otherwise ──────
        {
            int desiredW = gDither ? WIN_W : fw;
            int desiredH = gDither ? WIN_H : fh;
            if (fboW != desiredW || fboH != desiredH) rebuildFBO(desiredW, desiredH);
        }

        // ── 1-bit world-space dither post-process ────────────────────────────
        glBindFramebuffer(GL_FRAMEBUFFER,0);
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
        float aspect = float(WIN_W)/float(WIN_H);
        int vpW = fw, vpH = int(fw/aspect);
        if(vpH > fh) { vpH = fh; vpW = int(fh*aspect); }
        glViewport((fw-vpW)/2, (fh-vpH)/2, vpW, vpH);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(progPP);
        // scene colour
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D,fboTex);
        glUniform1i(glGetUniformLocation(progPP,"uScene"),0);
        // depth texture for world-pos reconstruction
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D,fboDepTex);
        glUniform1i(glGetUniformLocation(progPP,"uDepth"),1);
        glm::mat4 invVP = glm::inverse(vp);
        glUniformMatrix4fv(glGetUniformLocation(progPP,"uInvVP"),1,GL_FALSE,glm::value_ptr(invVP));
        glUniform3fv(glGetUniformLocation(progPP,"uCamPos"),1,glm::value_ptr(eye));
        glUniform1i(glGetUniformLocation(progPP,"uDither"), gDither ? 1 : 0);
        glBindVertexArray(quadVAO);
        glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,nullptr);
        glBindVertexArray(0);
        glEnable(GL_DEPTH_TEST);

        // ── Coord overlay (raycast → hit XYZ, bottom-right) ──────────────────
        {
            char buf[48];
            if (cursorHit.y < -0.5f) snprintf(buf,sizeof(buf),"X--- Y--- Z---");
            else              snprintf(buf,sizeof(buf),"X%.1f Y%.1f Z%.1f",cursorHit.x,cursorHit.y,cursorHit.z);
            std::vector<float> tv;
            buildTextVerts(buf, tv, vpW, vpH);
            if (!tv.empty()) {
                glDisable(GL_DEPTH_TEST);
                glBindBuffer(GL_ARRAY_BUFFER,textVBO);
                glBufferData(GL_ARRAY_BUFFER,tv.size()*4,tv.data(),GL_DYNAMIC_DRAW);
                glUseProgram(progText);
                glBindVertexArray(textVAO);
                glDrawArrays(GL_TRIANGLES,0,(GLsizei)(tv.size()/2));
                glBindVertexArray(0);
                glEnable(GL_DEPTH_TEST);
            }
        }

        // ── Active button symbols (post-dither, green, no dithering) ────────
        {
            glViewport((fw-vpW)/2, (fh-vpH)/2, vpW, vpH);
            glDisable(GL_DEPTH_TEST);
            for (auto& wb : wButtons) {
                if (wb.active)
                    drawFlatMesh(progFlat, wb.symMesh, vp, {0.f, 1.f, 0.f});
            }
            glEnable(GL_DEPTH_TEST);
        }

        // ── Pin spheres + cursor dot (post-dither, full colour, always on top) ─
        {
            glViewport((fw-vpW)/2, (fh-vpH)/2, vpW, vpH);
            glDisable(GL_DEPTH_TEST);
            glUseProgram(progPin);
            for (int i = 0; i < (int)pins.size(); i++) {
                glm::vec3 col = (i == hoveredPin) ? glm::vec3(0.f,1.f,0.f) : glm::vec3(1.f,0.f,0.f);
                glm::mat4 MVP = vp * glm::translate(glm::mat4(1.f), pins[i]);
                glUniformMatrix4fv(glGetUniformLocation(progPin,"uMVP"),1,GL_FALSE,glm::value_ptr(MVP));
                glUniform3fv(glGetUniformLocation(progPin,"uColor"),1,glm::value_ptr(col));
                glBindVertexArray(pinSphere.vao);
                glDrawElements(GL_TRIANGLES,pinSphere.count,GL_UNSIGNED_INT,nullptr);
            }
            // Cursor dot: red, only shown when R is toggled on and not hovering a pin
            if (rDotVisible && cursorHit.y >= -0.5f && hoveredPin < 0) {
                glm::vec3 col(1.f,0.f,0.f);
                glm::mat4 MVP = vp * glm::translate(glm::mat4(1.f), cursorHit);
                glUniformMatrix4fv(glGetUniformLocation(progPin,"uMVP"),1,GL_FALSE,glm::value_ptr(MVP));
                glUniform3fv(glGetUniformLocation(progPin,"uColor"),1,glm::value_ptr(col));
                glBindVertexArray(pinSphere.vao);
                glDrawElements(GL_TRIANGLES,pinSphere.count,GL_UNSIGNED_INT,nullptr);
            }
            glBindVertexArray(0);
            glEnable(GL_DEPTH_TEST);
        }

        glfwSwapBuffers(win);
        glfwPollEvents();
    }
    glfwTerminate();
    if (bgm) { Mix_HaltMusic(); Mix_FreeMusic(bgm); }
    Mix_CloseAudio();
    SDL_Quit();

    return 0;
}
