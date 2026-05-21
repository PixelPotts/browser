// level.h — level geometry: thick walls, door boxes, collision
// Vertex format: [px py pz  nx ny nz  u v] = 8 floats
// Stone buffer (sv/si): floors, wall top caps
// Wall  buffer (wv/wi): drywall faces + jamb caps
// Ceil  buffer (cv/ci): ceilings
// Door  buffer (dv/di): wood door leaves

#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <cmath>
#include <algorithm>

static const float WALL_H    = 3.2f;
static const float WALL_T    = 0.20f;  // wall thickness
static const float DOOR_H    = 2.4f;
static const float DOOR_THICK = 0.055f; // door leaf thickness

// Trim profiles
static const float CROWN_OUT  = 0.10f;   // crown projection from wall face
static const float CROWN_DROP = 0.12f;   // crown drop from ceiling
static const float CROWN_SPR  = 0.025f;  // crown spring (vertical nub at wall)
static const float BASE_H     = 0.12f;   // baseboard height
static const float BASE_OUT   = 0.05f;   // baseboard projection
static const float CHAIR_H    = 0.08f;   // chair rail height
static const float CHAIR_OUT  = 0.035f;  // chair rail projection
static const float CHAIR_Y    = 0.90f;   // chair rail centre (from floor)

struct CollWall {
    bool  cx;       // true: x=const (wall runs along Z), false: z=const
    float c;        // constant coordinate (wall centreline)
    float lo, hi;   // range along the perpendicular axis
    float yBase = 0.f; // floor y of this wall's level
};

struct LevelGeo {
    std::vector<float>    sv, wv, cv, dv, tv, bv, painv, pathv, marv;
    std::vector<unsigned> si, wi, ci, di, ti, bi, paini, pathi, mari;
    std::vector<CollWall> walls;
};

// ── Low-level helpers ─────────────────────────────────────────────────────────

static inline void lvV(std::vector<float>& V,
                       glm::vec3 p, glm::vec3 n, glm::vec2 uv) {
    V.insert(V.end(), { p.x,p.y,p.z, n.x,n.y,n.z, uv.x,uv.y });
}

static inline void lvQ(std::vector<float>& V, std::vector<unsigned>& I,
                       glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                       glm::vec3 n,
                       glm::vec2 ua, glm::vec2 ub, glm::vec2 uc, glm::vec2 ud)
{
    auto base = (unsigned)(V.size() / 8);
    lvV(V,a,n,ua); lvV(V,b,n,ub); lvV(V,c,n,uc); lvV(V,d,n,ud);
    I.insert(I.end(), { base,base+1,base+2, base+2,base+3,base });
}

// ── Floors & ceilings ─────────────────────────────────────────────────────────

static inline void lvFloor(LevelGeo& g, float x0,float z0, float x1,float z1, float y=0.f)
{
    lvQ(g.sv, g.si,
        {x0,y,z0},{x1,y,z0},{x1,y,z1},{x0,y,z1}, {0,1,0},
        {x0,z0},{x1,z0},{x1,z1},{x0,z1});
}

static inline void lvCeil(LevelGeo& g, float x0,float z0, float x1,float z1, float y)
{
    lvQ(g.cv, g.ci,
        {x0,y,z1},{x1,y,z1},{x1,y,z0},{x0,y,z0}, {0,-1,0},
        {x0,z1},{x1,z1},{x1,z0},{x0,z0});
}

// ── Thick wall segment ────────────────────────────────────────────────────────
// Produces: front face, back face, top cap (stone), two end caps (drywall).
// For headers (yBot > 0): also adds a visible underside face.

static inline void lvWallThick(LevelGeo& g,
                                float x0,float z0, float x1,float z1,
                                float yBot, float yTop,
                                bool coll=true, float yBase=0.f)
{
    float dx=x1-x0, dz=z1-z0;
    float len = sqrtf(dx*dx+dz*dz);
    if (len<1e-4f) return;

    glm::vec3 dir(dx/len,0,dz/len);
    glm::vec3 nrm = glm::cross(dir,{0,1,0});  // perpendicular, points "right"
    glm::vec3 off = nrm*(WALL_T*0.5f);
    float ht = yTop-yBot;

    glm::vec3 p0(x0,yBase,z0), p1(x1,yBase,z1);

    // Front face (normal = nrm)
    lvQ(g.wv,g.wi,
        p0+off+glm::vec3(0,yBot,0), p1+off+glm::vec3(0,yBot,0),
        p1+off+glm::vec3(0,yTop,0), p0+off+glm::vec3(0,yTop,0), nrm,
        {0,0},{len,0},{len,ht},{0,ht});

    // Back face (normal = -nrm)
    lvQ(g.wv,g.wi,
        p1-off+glm::vec3(0,yBot,0), p0-off+glm::vec3(0,yBot,0),
        p0-off+glm::vec3(0,yTop,0), p1-off+glm::vec3(0,yTop,0), -nrm,
        {0,0},{len,0},{len,ht},{0,ht});

    // Top cap → stone buffer, UV = world XZ
    lvQ(g.sv,g.si,
        p0-off+glm::vec3(0,yTop,0), p0+off+glm::vec3(0,yTop,0),
        p1+off+glm::vec3(0,yTop,0), p1-off+glm::vec3(0,yTop,0), {0,1,0},
        {(p0-off).x,(p0-off).z},{(p0+off).x,(p0+off).z},
        {(p1+off).x,(p1+off).z},{(p1-off).x,(p1-off).z});

    // Underside (visible for header above doorways)
    if (yBot > 0.01f) {
        lvQ(g.wv,g.wi,
            p0+off+glm::vec3(0,yBot,0), p0-off+glm::vec3(0,yBot,0),
            p1-off+glm::vec3(0,yBot,0), p1+off+glm::vec3(0,yBot,0), {0,-1,0},
            {0,0},{WALL_T,0},{WALL_T,len},{0,len});
    }

    // End cap at start (normal = -dir, jamb face)
    lvQ(g.wv,g.wi,
        p0+off+glm::vec3(0,yBot,0), p0-off+glm::vec3(0,yBot,0),
        p0-off+glm::vec3(0,yTop,0), p0+off+glm::vec3(0,yTop,0), -dir,
        {0,0},{WALL_T,0},{WALL_T,ht},{0,ht});

    // End cap at end (normal = +dir)
    lvQ(g.wv,g.wi,
        p1-off+glm::vec3(0,yBot,0), p1+off+glm::vec3(0,yBot,0),
        p1+off+glm::vec3(0,yTop,0), p1-off+glm::vec3(0,yTop,0), dir,
        {0,0},{WALL_T,0},{WALL_T,ht},{0,ht});

    if (!coll) return;
    bool xConst = fabsf(dx)<1e-3f, zConst = fabsf(dz)<1e-3f;
    if (xConst) g.walls.push_back({true,  x0, std::min(z0,z1), std::max(z0,z1), yBase});
    else if (zConst) g.walls.push_back({false, z0, std::min(x0,x1), std::max(x0,x1), yBase});
}

// ── Round classical pillar ────────────────────────────────────────────────────
// Shaft (r=R) with wider trim rings (r=RT, height=TH) at base and capital.
// Center at (cx,cz), floor at yBase, full height = WALL_H.
static inline void lvRoundPillar(LevelGeo& g, float cx, float cz, float yBase,
                                  int nSides = 16)
{
    const float PI2  = 6.28318530f;
    const float R    = 0.25f;   // shaft radius
    const float RT   = 0.42f;   // trim ring radius
    const float TH   = 0.14f;   // trim ring height
    const float y0   = yBase,          y1   = yBase + WALL_H;
    const float y_bt = yBase + TH,     y_ct = yBase + WALL_H - TH;

    // All pillar geometry goes into the marble buffer (marv/mari)
    // Cylinder side faces — outward normals, CCW winding
    auto sides = [&](float r, float ya, float yb) {
        float segU = PI2 * r / nSides, ht = yb - ya;
        for (int i = 0; i < nSides; i++) {
            float a0 = PI2*i/nSides, a1 = PI2*(i+1)/nSides, am = (a0+a1)*0.5f;
            float x0=cx+r*cosf(a0), z0=cz+r*sinf(a0);
            float x1=cx+r*cosf(a1), z1=cz+r*sinf(a1);
            float u0=i*segU, u1=(i+1)*segU;
            lvQ(g.marv,g.mari, {x1,ya,z1},{x0,ya,z0},{x0,yb,z0},{x1,yb,z1},
                {cosf(am),0,sinf(am)}, {u1,0},{u0,0},{u0,ht},{u1,ht});
        }
    };

    // Horizontal annulus (ring), facing up (+Y) or down (-Y)
    auto ring = [&](float rI, float rO, float y, bool up) {
        glm::vec3 n(0, up?1.f:-1.f, 0);
        for (int i = 0; i < nSides; i++) {
            float a0=PI2*i/nSides, a1=PI2*(i+1)/nSides;
            float xi0=cx+rI*cosf(a0), zi0=cz+rI*sinf(a0);
            float xi1=cx+rI*cosf(a1), zi1=cz+rI*sinf(a1);
            float xo0=cx+rO*cosf(a0), zo0=cz+rO*sinf(a0);
            float xo1=cx+rO*cosf(a1), zo1=cz+rO*sinf(a1);
            if (up)
                lvQ(g.marv,g.mari, {xi0,y,zi0},{xi1,y,zi1},{xo1,y,zo1},{xo0,y,zo0}, n,
                    {xi0,zi0},{xi1,zi1},{xo1,zo1},{xo0,zo0});
            else
                lvQ(g.marv,g.mari, {xo0,y,zo0},{xo1,y,zo1},{xi1,y,zi1},{xi0,y,zi0}, n,
                    {xo0,zo0},{xo1,zo1},{xi1,zi1},{xi0,zi0});
        }
    };

    // Solid disc cap (fan of triangles)
    auto disc = [&](float r, float y, bool up) {
        glm::vec3 n(0, up?1.f:-1.f, 0);
        for (int i = 0; i < nSides; i++) {
            float a0=PI2*i/nSides, a1=PI2*(i+1)/nSides;
            float x0=cx+r*cosf(a0), z0=cz+r*sinf(a0);
            float x1=cx+r*cosf(a1), z1=cz+r*sinf(a1);
            unsigned base=(unsigned)(g.marv.size()/8);
            lvV(g.marv,{cx,y,cz},n,{cx,cz});
            if (up) { lvV(g.marv,{x1,y,z1},n,{x1,z1}); lvV(g.marv,{x0,y,z0},n,{x0,z0}); }
            else    { lvV(g.marv,{x0,y,z0},n,{x0,z0}); lvV(g.marv,{x1,y,z1},n,{x1,z1}); }
            g.mari.insert(g.mari.end(),{base,base+1,base+2});
        }
    };

    sides(RT, y0,   y_bt);          // base trim sides
    ring (R,  RT,   y_bt, true);    // base trim top (step inward, face up)
    sides(R,  y_bt, y_ct);          // shaft
    ring (R,  RT,   y_ct, false);   // capital trim bottom (step outward, face down)
    sides(RT, y_ct, y1);            // capital trim sides
    disc (RT, y1,   true);          // top cap

    // Square collision (shaft radius)
    g.walls.push_back({true,  cx-R, cz-R, cz+R, yBase});
    g.walls.push_back({true,  cx+R, cz-R, cz+R, yBase});
    g.walls.push_back({false, cz-R, cx-R, cx+R, yBase});
    g.walls.push_back({false, cz+R, cx-R, cx+R, yBase});
}

// ── Walls with door openings ───────────────────────────────────────────────────

// Z-constant wall (runs along X) with a door gap at x=[doorLo,doorHi]
static inline void lvZWallDoor(LevelGeo& g, float z,
                                float wallLo, float wallHi,
                                float doorLo, float doorHi)
{
    if (doorLo > wallLo) lvWallThick(g, wallLo,z, doorLo,z, 0,    WALL_H);
    lvWallThick(g, doorLo,z, doorHi,z, DOOR_H, WALL_H, false); // header
    if (wallHi > doorHi) lvWallThick(g, doorHi,z, wallHi,z, 0,    WALL_H);
}

// X-constant wall (runs along Z) with a door gap at z=[doorLo,doorHi]
static inline void lvXWallDoor(LevelGeo& g, float x,
                                float wallLo, float wallHi,
                                float doorLo, float doorHi)
{
    if (doorLo > wallLo) lvWallThick(g, x,wallLo, x,doorLo, 0,    WALL_H);
    lvWallThick(g, x,doorLo, x,doorHi, DOOR_H, WALL_H, false); // header
    if (wallHi > doorHi) lvWallThick(g, x,doorHi, x,wallHi, 0,    WALL_H);
}

// ── Trim on one wall face ──────────────────────────────────────────────────────
// p0,p1 = wall-face endpoints at y=0; out = unit outward normal; len = segment length
// Emits: baseboard (front + top cap), chair rail (front + top cap), crown (spring nub + angled face)
static inline void lvTrimFace(LevelGeo& g,
                               glm::vec3 p0, glm::vec3 p1,
                               glm::vec3 out, float len)
{
    glm::vec3 up(0,1,0);

    // ── Baseboard ─────────────────────────────────────────────────────────────
    {
        glm::vec3 f0 = p0 + out*BASE_OUT, f1 = p1 + out*BASE_OUT;
        lvQ(g.tv,g.ti, f0, f1, f1+glm::vec3(0,BASE_H,0), f0+glm::vec3(0,BASE_H,0), out,
            {0.f,0.f},{len,0.f},{len,1.f},{0.f,1.f});
        lvQ(g.tv,g.ti,
            p0+glm::vec3(0,BASE_H,0), f0+glm::vec3(0,BASE_H,0),
            f1+glm::vec3(0,BASE_H,0), p1+glm::vec3(0,BASE_H,0), up,
            {0.f,0.f},{1.f,0.f},{1.f,1.f},{0.f,1.f});
    }

    // ── Chair rail ────────────────────────────────────────────────────────────
    {
        float cy = CHAIR_Y - CHAIR_H*0.5f;
        glm::vec3 f0 = p0 + out*CHAIR_OUT, f1 = p1 + out*CHAIR_OUT;
        lvQ(g.tv,g.ti,
            f0+glm::vec3(0,cy,0), f1+glm::vec3(0,cy,0),
            f1+glm::vec3(0,cy+CHAIR_H,0), f0+glm::vec3(0,cy+CHAIR_H,0), out,
            {0.f,0.f},{len,0.f},{len,1.f},{0.f,1.f});
        // Ovolo (convex quarter-round) at chair rail top — 4 segments
        const int CR_N = 4;
        const float PI_F = 3.14159265f;
        for (int ci = 0; ci < CR_N; ci++) {
            float t0 = PI_F*0.5f * ci / CR_N;
            float t1 = PI_F*0.5f * (ci+1) / CR_N;
            float o0 = CHAIR_OUT*cosf(t0), h0 = CHAIR_OUT*sinf(t0);
            float o1 = CHAIR_OUT*cosf(t1), h1 = CHAIR_OUT*sinf(t1);
            glm::vec3 cn = glm::normalize(out*cosf((t0+t1)*0.5f)
                                        + glm::vec3(0,sinf((t0+t1)*0.5f),0));
            glm::vec3 va = p0+out*o0+glm::vec3(0,cy+CHAIR_H+h0,0);
            glm::vec3 vb = p1+out*o0+glm::vec3(0,cy+CHAIR_H+h0,0);
            glm::vec3 vc = p1+out*o1+glm::vec3(0,cy+CHAIR_H+h1,0);
            glm::vec3 vd = p0+out*o1+glm::vec3(0,cy+CHAIR_H+h1,0);
            float v0 = float(ci)/CR_N, v1 = float(ci+1)/CR_N;
            lvQ(g.tv,g.ti, va, vb, vc, vd, cn,
                {0.f,v0},{len,v0},{len,v1},{0.f,v1});
        }
    }

    // ── Crown moulding ────────────────────────────────────────────────────────
    {
        glm::vec3 crNrm = glm::normalize(out + up);
        // Spring: small vertical nub at wall where crown meets wall
        float sy = WALL_H - CROWN_DROP - CROWN_SPR;
        lvQ(g.tv,g.ti,
            p0+glm::vec3(0,sy,0), p1+glm::vec3(0,sy,0),
            p1+glm::vec3(0,sy+CROWN_SPR,0), p0+glm::vec3(0,sy+CROWN_SPR,0), out,
            {0.f,0.f},{len,0.f},{len,1.f},{0.f,1.f});
        // Main angled face
        lvQ(g.tv,g.ti,
            p0+glm::vec3(0,WALL_H-CROWN_DROP,0), p1+glm::vec3(0,WALL_H-CROWN_DROP,0),
            p1+out*CROWN_OUT+glm::vec3(0,WALL_H,0), p0+out*CROWN_OUT+glm::vec3(0,WALL_H,0), crNrm,
            {0.f,0.f},{len,0.f},{len,1.f},{0.f,1.f});
    }

    // ── Wainscoting panel (flat, between baseboard top and chair rail bottom) ─
    {
        const float WAIN_OFF = 0.004f;   // tiny projection to avoid z-fight
        const float y0 = BASE_H;
        const float y1 = CHAIR_Y - CHAIR_H * 0.5f;
        float ht = y1 - y0;
        glm::vec3 f0 = p0 + out*WAIN_OFF;
        glm::vec3 f1 = p1 + out*WAIN_OFF;
        auto base = (unsigned)(g.painv.size()/8);
        lvV(g.painv, f0+glm::vec3(0,y0,0), out, {0.f, 0.f});
        lvV(g.painv, f1+glm::vec3(0,y0,0), out, {len, 0.f});
        lvV(g.painv, f1+glm::vec3(0,y1,0), out, {len, ht});
        lvV(g.painv, f0+glm::vec3(0,y1,0), out, {0.f, ht});
        g.paini.insert(g.paini.end(), {base,base+1,base+2, base+2,base+3,base});
    }
}

// ── Trim on both sides of a wall segment ──────────────────────────────────────
static inline void lvTrimSeg(LevelGeo& g, float x0,float z0, float x1,float z1)
{
    float dx=x1-x0, dz=z1-z0;
    float len = sqrtf(dx*dx+dz*dz);
    if (len<1e-4f) return;
    glm::vec3 dir(dx/len,0,dz/len);
    glm::vec3 nrm = glm::cross(dir,{0,1,0});
    glm::vec3 off = nrm*(WALL_T*0.5f);
    glm::vec3 p0b(x0,0,z0), p1b(x1,0,z1);
    lvTrimFace(g, p0b+off,  p1b+off,   nrm, len);  // front side
    lvTrimFace(g, p1b-off,  p0b-off,  -nrm, len);  // back side (reversed dir)
}

// ── Crown moulding only (for door headers) ────────────────────────────────────
static inline void lvCrownFace(LevelGeo& g,
                                glm::vec3 p0, glm::vec3 p1,
                                glm::vec3 out, float len)
{
    glm::vec3 crNrm = glm::normalize(out + glm::vec3(0,1,0));
    float sy = WALL_H - CROWN_DROP - CROWN_SPR;
    lvQ(g.tv,g.ti,
        p0+glm::vec3(0,sy,0), p1+glm::vec3(0,sy,0),
        p1+glm::vec3(0,sy+CROWN_SPR,0), p0+glm::vec3(0,sy+CROWN_SPR,0), out,
        {0.f,0.f},{len,0.f},{len,1.f},{0.f,1.f});
    lvQ(g.tv,g.ti,
        p0+glm::vec3(0,WALL_H-CROWN_DROP,0), p1+glm::vec3(0,WALL_H-CROWN_DROP,0),
        p1+out*CROWN_OUT+glm::vec3(0,WALL_H,0), p0+out*CROWN_OUT+glm::vec3(0,WALL_H,0), crNrm,
        {0.f,0.f},{len,0.f},{len,1.f},{0.f,1.f});
}

static inline void lvCrownSeg(LevelGeo& g, float x0,float z0, float x1,float z1)
{
    float dx=x1-x0, dz=z1-z0;
    float len = sqrtf(dx*dx+dz*dz);
    if (len<1e-4f) return;
    glm::vec3 dir(dx/len,0,dz/len);
    glm::vec3 nrm = glm::cross(dir,{0,1,0});
    glm::vec3 off = nrm*(WALL_T*0.5f);
    glm::vec3 p0b(x0,0,z0), p1b(x1,0,z1);
    lvCrownFace(g, p0b+off,  p1b+off,   nrm, len);
    lvCrownFace(g, p1b-off,  p0b-off,  -nrm, len);
}

// ── Door leaf box ─────────────────────────────────────────────────────────────
// wideFaceZ=false: door stands in the Z direction, wide face has normal along X
//                 (for doors in Z-constant walls, opened into the room along Z)
// wideFaceZ=true:  door stands in the X direction, wide face has normal along Z
//                 (for doors in X-constant walls, opened into the room along X)

static inline void lvDoorBox(LevelGeo& g, glm::vec3 mn, glm::vec3 mx, bool wideFaceZ)
{
    float H = mx.y - mn.y;

    if (!wideFaceZ) {
        // Wide (wood grain) faces: +X and -X  (large Z×Y)
        lvQ(g.dv,g.di,
            {mx.x,mn.y,mx.z},{mx.x,mn.y,mn.z},{mx.x,mx.y,mn.z},{mx.x,mx.y,mx.z}, {1,0,0},
            {1,0},{0,0},{0,1},{1,1});
        lvQ(g.dv,g.di,
            {mn.x,mn.y,mn.z},{mn.x,mn.y,mx.z},{mn.x,mx.y,mx.z},{mn.x,mx.y,mn.z}, {-1,0,0},
            {0,0},{1,0},{1,1},{0,1});
        // Narrow Z edges
        lvQ(g.dv,g.di,
            {mn.x,mn.y,mx.z},{mx.x,mn.y,mx.z},{mx.x,mx.y,mx.z},{mn.x,mx.y,mx.z}, {0,0,1},
            {0,0},{1,0},{1,H},{0,H});
        lvQ(g.dv,g.di,
            {mx.x,mn.y,mn.z},{mn.x,mn.y,mn.z},{mn.x,mx.y,mn.z},{mx.x,mx.y,mn.z}, {0,0,-1},
            {0,0},{1,0},{1,H},{0,H});
    } else {
        // Wide faces: +Z and -Z  (large X×Y)
        lvQ(g.dv,g.di,
            {mn.x,mn.y,mx.z},{mx.x,mn.y,mx.z},{mx.x,mx.y,mx.z},{mn.x,mx.y,mx.z}, {0,0,1},
            {0,0},{1,0},{1,1},{0,1});
        lvQ(g.dv,g.di,
            {mx.x,mn.y,mn.z},{mn.x,mn.y,mn.z},{mn.x,mx.y,mn.z},{mx.x,mx.y,mn.z}, {0,0,-1},
            {0,0},{1,0},{1,1},{0,1});
        // Narrow X edges
        lvQ(g.dv,g.di,
            {mx.x,mn.y,mx.z},{mx.x,mn.y,mn.z},{mx.x,mx.y,mn.z},{mx.x,mx.y,mx.z}, {1,0,0},
            {0,0},{1,0},{1,H},{0,H});
        lvQ(g.dv,g.di,
            {mn.x,mn.y,mn.z},{mn.x,mn.y,mx.z},{mn.x,mx.y,mx.z},{mn.x,mx.y,mn.z}, {-1,0,0},
            {0,0},{1,0},{1,H},{0,H});
    }
    // Top / bottom
    lvQ(g.dv,g.di,
        {mn.x,mx.y,mx.z},{mx.x,mx.y,mx.z},{mx.x,mx.y,mn.z},{mn.x,mx.y,mn.z}, {0,1,0},
        {0,1},{1,1},{1,0},{0,0});
    lvQ(g.dv,g.di,
        {mn.x,mn.y,mn.z},{mx.x,mn.y,mn.z},{mx.x,mn.y,mx.z},{mn.x,mn.y,mx.z}, {0,-1,0},
        {0,0},{1,0},{1,1},{0,1});
}

// ── Ground path segment (writes into pathv/pathi) ────────────────────────────
// Flat quad on the outdoor floor, slightly above y=0 to avoid z-fight.
// width = 4 * marble_diameter = 3.6
static const float PATH_W = 3.6f;

static inline void lvPathSeg(LevelGeo& g, glm::vec2 a, glm::vec2 b)
{
    glm::vec2 d = b - a;
    float len = glm::length(d);
    if (len < 1e-4f) return;
    glm::vec2 dir = d / len;
    glm::vec2 perp(-dir.y, dir.x);
    float h = PATH_W * 0.5f;
    const float Y = 0.006f;
    glm::vec3 n(0,1,0);
    glm::vec3 v0(a.x+perp.x*h, Y, a.y+perp.y*h);
    glm::vec3 v1(a.x-perp.x*h, Y, a.y-perp.y*h);
    glm::vec3 v2(b.x-perp.x*h, Y, b.y-perp.y*h);
    glm::vec3 v3(b.x+perp.x*h, Y, b.y+perp.y*h);
    auto base=(unsigned)(g.pathv.size()/8);
    lvV(g.pathv,v0,n,{0,0}); lvV(g.pathv,v1,n,{1,0});
    lvV(g.pathv,v2,n,{1,len}); lvV(g.pathv,v3,n,{0,len});
    g.pathi.insert(g.pathi.end(),{base,base+1,base+2,base+2,base+3,base});
}

// ── Brick wall segment (writes into bv/bi) ────────────────────────────────────
static inline void lvBrickWall(LevelGeo& g,
                                float x0,float z0, float x1,float z1,
                                float yBot, float yTop,
                                bool coll=true)
{
    float dx=x1-x0, dz=z1-z0;
    float len = sqrtf(dx*dx+dz*dz);
    if (len<1e-4f) return;
    glm::vec3 dir(dx/len,0,dz/len);
    glm::vec3 nrm = glm::cross(dir,{0,1,0});
    glm::vec3 off = nrm*(WALL_T*0.5f);
    float ht = yTop-yBot;
    glm::vec3 p0(x0,0,z0), p1(x1,0,z1);

    auto Q = [&](glm::vec3 a,glm::vec3 b,glm::vec3 c,glm::vec3 d,
                 glm::vec3 n,glm::vec2 ua,glm::vec2 ub,glm::vec2 uc,glm::vec2 ud){
        auto base=(unsigned)(g.bv.size()/8);
        lvV(g.bv,a,n,ua); lvV(g.bv,b,n,ub); lvV(g.bv,c,n,uc); lvV(g.bv,d,n,ud);
        g.bi.insert(g.bi.end(),{base,base+1,base+2,base+2,base+3,base});
    };

    Q(p0+off+glm::vec3(0,yBot,0), p1+off+glm::vec3(0,yBot,0),
      p1+off+glm::vec3(0,yTop,0), p0+off+glm::vec3(0,yTop,0), nrm,
      {0,0},{len,0},{len,ht},{0,ht});
    Q(p1-off+glm::vec3(0,yBot,0), p0-off+glm::vec3(0,yBot,0),
      p0-off+glm::vec3(0,yTop,0), p1-off+glm::vec3(0,yTop,0), -nrm,
      {0,0},{len,0},{len,ht},{0,ht});
    // Top cap
    Q(p0-off+glm::vec3(0,yTop,0), p0+off+glm::vec3(0,yTop,0),
      p1+off+glm::vec3(0,yTop,0), p1-off+glm::vec3(0,yTop,0), {0,1,0},
      {(p0-off).x,(p0-off).z},{(p0+off).x,(p0+off).z},
      {(p1+off).x,(p1+off).z},{(p1-off).x,(p1-off).z});

    if (!coll) return;
    bool xConst=fabsf(dx)<1e-3f, zConst=fabsf(dz)<1e-3f;
    if (xConst) g.walls.push_back({true,  x0, std::min(z0,z1), std::max(z0,z1)});
    else if (zConst) g.walls.push_back({false, z0, std::min(x0,x1), std::max(x0,x1)});
}

// ── Level definition ──────────────────────────────────────────────────────────
//
//  top-down XZ  (Z = north/+, X = east/+)
//
//  z=-25 ╔═══════════════════╗
//        ║  OUTDOOR (open)   ║  x -20..20
//  z= 0  ╠══╦═══════╦══════╦╣
//        ║  ║ RM A  ║ RM C ║║  A: x -6..6,  z 0..10   entry hall
//  z=10  ║  ╠═══════╩══════╝║  C: x  6..14, z 0..10   side room
//        ║  ║   RM B        ║  B: x -6..6,  z 10..22  large room
//  z=22  ║  ╚═══════════════╝
//
//  Doors:  outdoor→A  z=0,  x -1.5..1.5
//          A→B        z=10, x -1.5..1.5
//          A↔C        x=6,  z  3.5..6.5
// ─────────────────────────────────────────────────────────────────────────────

static inline void buildBasement(LevelGeo& g)
{
    const float BY  = -100.f;
    const float BX0 = -12.f, BX1 = 12.f, BZ0 = -4.f, BZ1 = 18.f;
    const float DW  = 1.5f;   // half-door width (3-unit doors)
    const float RD  = 16.f;   // depth of each side room

    // Door centres on south/north walls (equally spaced along 24 units)
    const float DX[4] = { -9.f, -3.f, 3.f, 9.f };
    // Door centres on east/west walls (equally spaced along 22 units)
    const float DZ[4] = { -1.25f, 4.25f, 9.75f, 15.25f };
    // Room z-boundaries for east/west rooms
    const float RZ[5] = { BZ0, 1.5f, 7.f, 12.5f, BZ1 };

    // Wall-with-4-doors helpers (lambdas capture BY, DW, DX, DZ)
    auto zWall4 = [&](float z, float x0, float x1) {
        float cur = x0;
        for (int i = 0; i < 4; i++) {
            float dlo = DX[i]-DW, dhi = DX[i]+DW;
            if (dlo > cur) lvWallThick(g, cur,z, dlo,z, 0.f,WALL_H, true, BY);
            lvWallThick(g, dlo,z, dhi,z, DOOR_H,WALL_H, false, BY);
            cur = dhi;
        }
        if (cur < x1) lvWallThick(g, cur,z, x1,z, 0.f,WALL_H, true, BY);
    };
    auto xWall4 = [&](float x, float z0, float z1) {
        float cur = z0;
        for (int i = 0; i < 4; i++) {
            float dlo = DZ[i]-DW, dhi = DZ[i]+DW;
            if (dlo > cur) lvWallThick(g, x,cur, x,dlo, 0.f,WALL_H, true, BY);
            lvWallThick(g, x,dlo, x,dhi, DOOR_H,WALL_H, false, BY);
            cur = dhi;
        }
        if (cur < z1) lvWallThick(g, x,cur, x,z1, 0.f,WALL_H, true, BY);
    };

    // ── Main basement room ────────────────────────────────────────────────────
    lvCeil(g, BX0,BZ0, BX1,BZ1, BY+WALL_H);
    zWall4(BZ0, BX0, BX1);   // south wall
    zWall4(BZ1, BX0, BX1);   // north wall
    xWall4(BX0, BZ0, BZ1);   // west wall
    xWall4(BX1, BZ0, BZ1);   // east wall

    // ── North rooms (z = BZ1..BZ1+RD, divided at x = -6, 0, 6) ──────────────
    float NZ1 = BZ1 + RD;
    lvFloor(g, BX0,BZ1, BX1,NZ1, BY);
    lvCeil (g, BX0,BZ1, BX1,NZ1, BY+WALL_H);
    lvWallThick(g, BX0,NZ1, BX1,NZ1, 0.f,WALL_H, true,BY);
    lvWallThick(g, BX0,BZ1, BX0,NZ1, 0.f,WALL_H, true,BY);
    lvWallThick(g, BX1,BZ1, BX1,NZ1, 0.f,WALL_H, true,BY);
    for (int i=1;i<4;i++) lvWallThick(g, BX0+i*6.f,BZ1, BX0+i*6.f,NZ1, 0.f,WALL_H, true,BY);

    // ── South rooms (z = BZ0-RD..BZ0) ────────────────────────────────────────
    float SZ0 = BZ0 - RD;
    lvFloor(g, BX0,SZ0, BX1,BZ0, BY);
    lvCeil (g, BX0,SZ0, BX1,BZ0, BY+WALL_H);
    lvWallThick(g, BX0,SZ0, BX1,SZ0, 0.f,WALL_H, true,BY);
    lvWallThick(g, BX0,SZ0, BX0,BZ0, 0.f,WALL_H, true,BY);
    lvWallThick(g, BX1,SZ0, BX1,BZ0, 0.f,WALL_H, true,BY);
    for (int i=1;i<4;i++) lvWallThick(g, BX0+i*6.f,SZ0, BX0+i*6.f,BZ0, 0.f,WALL_H, true,BY);

    // ── Marble room pillars (south room 0: x=-12..-6, z=-20..-4) ─────────────
    // Round columns 0.4 units out from each long-side wall face.
    {
        const float WX = BX0     + WALL_T*0.5f + 0.4f;  // west row  (-11.5)
        const float EX = BX0+6.f - WALL_T*0.5f - 0.4f;  // east row  (-6.5)
        const float ZS[] = { -18.f, -14.f, -10.f, -6.f };
        for (float pz : ZS) {
            lvRoundPillar(g, WX, pz, BY);
            lvRoundPillar(g, EX, pz, BY);
        }
    }

    // ── West rooms (x = BX0-RD..BX0, divided at z = RZ[1..3]) ───────────────
    float WX0 = BX0 - RD;
    lvFloor(g, WX0,BZ0, BX0,BZ1, BY);
    lvCeil (g, WX0,BZ0, BX0,BZ1, BY+WALL_H);
    lvWallThick(g, WX0,BZ0, WX0,BZ1, 0.f,WALL_H, true,BY);
    lvWallThick(g, WX0,BZ0, BX0,BZ0, 0.f,WALL_H, true,BY);
    lvWallThick(g, WX0,BZ1, BX0,BZ1, 0.f,WALL_H, true,BY);
    for (int i=1;i<4;i++) lvWallThick(g, WX0,RZ[i], BX0,RZ[i], 0.f,WALL_H, true,BY);

    // ── East rooms (x = BX1..BX1+RD) ─────────────────────────────────────────
    float EX1 = BX1 + RD;
    lvFloor(g, BX1,BZ0, EX1,BZ1, BY);
    lvCeil (g, BX1,BZ0, EX1,BZ1, BY+WALL_H);
    lvWallThick(g, EX1,BZ0, EX1,BZ1, 0.f,WALL_H, true,BY);
    lvWallThick(g, BX1,BZ0, EX1,BZ0, 0.f,WALL_H, true,BY);
    lvWallThick(g, BX1,BZ1, EX1,BZ1, 0.f,WALL_H, true,BY);
    for (int i=1;i<4;i++) lvWallThick(g, BX1,RZ[i], EX1,RZ[i], 0.f,WALL_H, true,BY);
}

static inline LevelGeo buildLevel()
{
    LevelGeo g;

    // ── Floors ────────────────────────────────────────────────────────────────
    lvFloor(g, -20,-25,  20,  0);
    // Room A floor — split to leave hole for well at (0, 5.6), r=1.0
    lvFloor(g,  -6,  0,   6,  4.6f);   // south strip
    lvFloor(g,  -6,  6.6f, 6, 10);     // north strip
    lvFloor(g,  -6,  4.6f,-1.0f,6.6f); // west strip
    lvFloor(g, 1.0f, 4.6f, 6, 6.6f);  // east strip
    lvFloor(g,  -6, 10,   6, 22);
    lvFloor(g,   6,  0,  14, 10);

    // ── Basement floor at y=-100 (shaft from Room A drops here) ──────────────
    lvFloor(g, -12, -4,  12, 18, -100.f);

    // ── Ceilings ──────────────────────────────────────────────────────────────
    lvCeil(g, -6,  0,  6, 10, WALL_H);
    lvCeil(g, -6, 10,  6, 22, WALL_H);
    lvCeil(g,  6,  0, 14, 10, WALL_H);

    // ── Outer south face (z=0): RM A (with door) + RM C south ─────────────────
    lvZWallDoor(g, 0.f, -6.f, 6.f, -1.5f, 1.5f);
    lvWallThick(g, 6,0, 14,0, 0, WALL_H);

    // ── Room A west (x=-6, z 0..10) ───────────────────────────────────────────
    lvWallThick(g, -6,0, -6,10, 0, WALL_H);

    // ── A/C divider (x=6, z 0..10) with door at z 3.5..6.5 ───────────────────
    lvXWallDoor(g, 6.f, 0.f, 10.f, 3.5f, 6.5f);

    // ── A/B divider (z=10, x -6..6) with door at x -1.5..1.5 ─────────────────
    lvZWallDoor(g, 10.f, -6.f, 6.f, -1.5f, 1.5f);

    // ── Room C east + north ────────────────────────────────────────────────────
    lvWallThick(g, 14,0, 14,10, 0, WALL_H);
    lvWallThick(g,  6,10, 14,10, 0, WALL_H);

    // ── Room B west, east, north ───────────────────────────────────────────────
    lvWallThick(g, -6,10, -6,22, 0, WALL_H);
    lvWallThick(g,  6,10,  6,22, 0, WALL_H);
    lvWallThick(g, -6,22,  6,22, 0, WALL_H);

    // ── Double doors (open 90°, pushed perpendicular against inner wall face) ─
    // Each leaf: 1.45 wide, DOOR_H tall, DOOR_THICK deep.
    // Inner wall face offset from centreline = WALL_T/2.
    const float LW = 1.45f;               // leaf width
    const float HT = WALL_T * 0.5f;       // half-thickness offset to inner face
    const float DH = DOOR_THICK * 0.5f;   // half door thickness

    // South wall door → swings into Room A (+Z)
    // Leaves sit flush with jamb face: right/left face at x=±1.5, door inside wall volume
    lvDoorBox(g, {-1.5f-DOOR_THICK, 0, HT}, {-1.5f,            DOOR_H, HT+LW}, false);
    lvDoorBox(g, { 1.5f,            0, HT}, { 1.5f+DOOR_THICK, DOOR_H, HT+LW}, false);

    // A/B divider door → swings into Room B (+Z)
    lvDoorBox(g, {-1.5f-DOOR_THICK, 0, 10+HT}, {-1.5f,            DOOR_H, 10+HT+LW}, false);
    lvDoorBox(g, { 1.5f,            0, 10+HT}, { 1.5f+DOOR_THICK, DOOR_H, 10+HT+LW}, false);

    // A/C door → swings into Room C (+X)
    // Leaves flush with jamb: +Z face at z=3.5, -Z face at z=6.5
    lvDoorBox(g, {6+HT, 0, 3.5f-DOOR_THICK}, {6+HT+LW, DOOR_H, 3.5f           }, true);
    lvDoorBox(g, {6+HT, 0, 6.5f           }, {6+HT+LW, DOOR_H, 6.5f+DOOR_THICK}, true);

    // ── Crown over door headers (full-width, only crown — no base/chair) ────────
    lvCrownSeg(g, -1.5f, 0.f,   1.5f,  0.f);   // south entrance header
    lvCrownSeg(g, -1.5f, 10.f,  1.5f, 10.f);   // A/B header
    lvCrownSeg(g,  6.f,  3.5f,  6.f,  6.5f);   // A/C header

    // ── Trim (baseboard + chair rail + crown) on all interior wall segments ────
    // South face, either side of entrance door
    lvTrimSeg(g, -6.f,  0.f,  -1.5f,  0.f);
    lvTrimSeg(g,  1.5f, 0.f,   6.f,   0.f);
    lvTrimSeg(g,  6.f,  0.f,  14.f,   0.f);  // Room C south
    // Room A west
    lvTrimSeg(g, -6.f,  0.f,  -6.f,  10.f);
    // A/C divider, either side of door
    lvTrimSeg(g,  6.f,  0.f,   6.f,   3.5f);
    lvTrimSeg(g,  6.f,  6.5f,  6.f,  10.f);
    // A/B divider, either side of door
    lvTrimSeg(g, -6.f, 10.f,  -1.5f, 10.f);
    lvTrimSeg(g,  1.5f,10.f,   6.f,  10.f);
    // Room C east + north
    lvTrimSeg(g, 14.f,  0.f,  14.f,  10.f);
    lvTrimSeg(g,  6.f, 10.f,  14.f,  10.f);
    // Room B west, east, north
    lvTrimSeg(g, -6.f, 10.f,  -6.f,  22.f);
    lvTrimSeg(g,  6.f, 10.f,   6.f,  22.f);
    lvTrimSeg(g, -6.f, 22.f,   6.f,  22.f);

    // ── Outdoor winding ground path (white, unlit) ───────────────────────────
    // Width = 4 marble diameters (PATH_W = 3.6). Designed in XZ, y lifted 6mm.
    // Two gaps, two cross-connections that create visual junctions.
    auto ps=[&](float x0,float z0,float x1,float z1){ lvPathSeg(g,{x0,z0},{x1,z1}); };

    // Leg 1: starts SW, snakes east across the south band
    ps(-16,-24, -9,-21);
    ps( -9,-21, -3,-23);
    ps( -3,-23,  4,-20);
    ps(  4,-20, 10,-18);
    //  ~~~ GAP 1 (≈6 units) ~~~

    // Leg 2: east side, winds north
    ps( 15,-14, 12,-10);
    ps( 12,-10,  6, -8);
    ps(  6, -8,  1, -9);
    //  ~~~ GAP 2 (≈6 units) ~~~

    // Leg 3: west side, finishes near building
    ps( -5,-10, -11, -6);
    ps(-11, -6,  -9, -2);
    ps( -9, -2,  -4, -1);

    // Cross A: diagonal from leg-1 node 3 → through mid-field → meets leg-2 end
    ps( -3,-23,   0,-16);
    ps(  0,-16,   1, -9);   // junction with leg-2 tail

    // Cross B: branch from leg-1 node 2 → sweeps south-west → meets leg-3 head
    ps( -9,-21, -12,-15);
    ps(-12,-15,  -5,-10);   // junction with leg-3 head

    // ── Outdoor brick perimeter (x -20..20, z -25..0) ────────────────────────
    const float BH = 2.5f;   // brick wall height
    // South wall
    lvBrickWall(g, -20,-25, 20,-25, 0, BH);
    // West wall
    lvBrickWall(g, -20,-25, -20,0, 0, BH);
    // East wall — split for prison cell door gap at z ≈ -20 (gap = 1.2 wide)
    lvBrickWall(g,  20,-25,   20,-20.6f, 0, BH);  // south section
    lvBrickWall(g,  20,-19.4f, 20, 0,   0, BH);  // north section
    // North fill-in (z=0, flanking the building entrance: x -20..-6 and x 14..20)
    lvBrickWall(g, -20,0, -6,0, 0, BH);
    lvBrickWall(g,  14,0, 20,0, 0, BH);

    buildBasement(g);

    return g;
}
