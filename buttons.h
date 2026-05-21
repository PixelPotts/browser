// buttons.h — interactive wall-mounted button panels
// 6 buttons read from spheres.cache, placed on the south wall (z=0).
// Vertex format: [px py pz  nx ny nz  u v]  (matches level.h / furniture.h)

#pragma once
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>
#include "level.h"  // lvQ, WALL_T

// ── Weird 5×7 pixel glyphs (bit4 = leftmost col, rows stored top→bottom) ────
//   Ψ        Ω        ⌘        ⚙        ☯        ⍟
static const uint8_t BTN_GLYPHS[6][7] = {
    {21,21,27,14, 4,14,14},   // Ψ  psi
    {14,17,17,10,10,31,21},   // Ω  omega
    {10,31,21,31,21,31,10},   // ⌘  lattice/command
    {10,31,14,31,14,31,10},   // ⚙  gear
    {14,27,30,14,15,27,14},   // ☯  yin-yang
    {14,21,31,14,31,21,14},   // ⍟  asterisk-circle
};

// ── Button color states (off → cycle on each F press) ────────────────────────
static const glm::vec3 BTN_COLORS[5] = {
    {0.25f, 0.25f, 0.28f},   // 0  off – slate grey
    {0.85f, 0.10f, 0.10f},   // 1  red
    {0.90f, 0.60f, 0.05f},   // 2  amber
    {0.10f, 0.82f, 0.20f},   // 3  green
    {0.15f, 0.45f, 0.95f},   // 4  blue
};
static const int BTN_NUM_STATES = 5;

// ── Per-button raw geometry (uploaded to GPU by fps.cpp) ─────────────────────
struct BtnGeo {
    std::vector<float>    bodyv, symv;
    std::vector<unsigned> bodyi, symi;
    glm::vec3 mn, mx;   // world-space AABB for ray intersection
};

// Build one button centred at (cx, cy) on the south wall inner face.
static BtnGeo buildOneButton(float cx, float cy, int symIdx)
{
    BtnGeo b;

    const float HW    = 0.15f;           // half width/height of button face
    const float DEPTH = 0.10f;           // how far it protrudes from wall
    const float WF    = WALL_T * 0.5f;   // half wall thickness = 0.10
    // Outdoor (south) face: back flush with outer face z=-WF, front at z=-WF-DEPTH
    const float zBack  = -WF;            // flush with outer wall face  (z = -0.10)
    const float zFront = -WF - DEPTH;    // protrudes outward            (z = -0.20)
    const float z2     = zFront - 0.003f;// symbol: slightly proud of front face

    b.mn = { cx-HW, cy-HW, zFront };
    b.mx = { cx+HW, cy+HW, zBack  };

    float x0 = cx-HW, x1 = cx+HW;
    float y0 = cy-HW, y1 = cy+HW;

    // ── Body: 5 visible faces, front faces -Z (toward outdoor) ───────────────
    // Front (-Z)
    lvQ(b.bodyv,b.bodyi, {x1,y0,zFront},{x0,y0,zFront},{x0,y1,zFront},{x1,y1,zFront}, {0,0,-1},
        {0,0},{1,0},{1,1},{0,1});
    // Top (+Y)
    lvQ(b.bodyv,b.bodyi, {x0,y1,zFront},{x1,y1,zFront},{x1,y1,zBack},{x0,y1,zBack}, {0,1,0},
        {0,0},{1,0},{1,1},{0,1});
    // Bottom (-Y)
    lvQ(b.bodyv,b.bodyi, {x1,y0,zFront},{x0,y0,zFront},{x0,y0,zBack},{x1,y0,zBack}, {0,-1,0},
        {0,0},{1,0},{1,1},{0,1});
    // Left (-X)  — left when looking from outside (-Z dir)
    lvQ(b.bodyv,b.bodyi, {x0,y0,zBack},{x0,y0,zFront},{x0,y1,zFront},{x0,y1,zBack}, {-1,0,0},
        {0,0},{1,0},{1,1},{0,1});
    // Right (+X)
    lvQ(b.bodyv,b.bodyi, {x1,y0,zFront},{x1,y0,zBack},{x1,y1,zBack},{x1,y1,zFront}, {1,0,0},
        {0,0},{1,0},{1,1},{0,1});

    // ── Symbol: 5×7 glyph pixel-quads on the front (-Z) face ────────────────
    const float PS  = (HW*2.f) / 7.f;
    const float sox = (HW*2.f - 5.f*PS) * 0.5f;

    for (int row = 0; row < 7; row++) {
        uint8_t bits = BTN_GLYPHS[symIdx][row];
        for (int col = 0; col < 5; col++) {
            if (!(bits & (1 << (4-col)))) continue;
            float px = x0 + sox + col * PS;
            float py = y0 + float(6-row) * PS;
            // CCW winding seen from -Z (outdoor viewer)
            lvQ(b.symv,b.symi,
                {px+PS,py,   z2}, {px,   py,   z2},
                {px,   py+PS,z2}, {px+PS,py+PS,z2},
                {0,0,-1},
                {0,0},{1,0},{1,1},{0,1});
        }
    }

    return b;
}

// Load all 6 buttons from spheres.cache.
// Returns empty vector if cache missing / wrong count.
static std::vector<BtnGeo> loadButtons(const char* cachePath)
{
    std::vector<BtnGeo> out;
    FILE* f = fopen(cachePath, "r");
    if (!f) return out;

    float x, y, z;
    int   idx = 0;
    while (fscanf(f, "%f %f %f", &x, &y, &z) == 3 && idx < 6) {
        out.push_back(buildOneButton(x, y, idx));
        idx++;
    }
    fclose(f);
    return out;
}

// Ray–AABB test (returns false if miss or behind origin).
static bool rayButton(glm::vec3 o, glm::vec3 d, const BtnGeo& b, float& tOut)
{
    float tmin = 0.f, tmax = 1e9f;
    for (int i = 0; i < 3; i++) {
        if (fabsf(d[i]) < 1e-7f) {
            if (o[i] < b.mn[i] || o[i] > b.mx[i]) return false;
        } else {
            float inv = 1.f / d[i];
            float t1  = (b.mn[i] - o[i]) * inv;
            float t2  = (b.mx[i] - o[i]) * inv;
            if (t1 > t2) { float tmp=t1; t1=t2; t2=tmp; }
            tmin = fmaxf(tmin, t1);
            tmax = fminf(tmax, t2);
            if (tmin > tmax) return false;
        }
    }
    tOut = tmin;
    return tOut > 0.001f;
}
