// furniture.h — procedural room furniture geometry
// Vertex format: [px py pz  nx ny nz  u v]  (matches level.h)
//
// Rooms:
//   A (living):  x -6..6,  z  0..10   sofa against south wall, TV on north wall
//   B (dining):  x -6..6,  z 10..22   table + 6 chairs
//   C (kitchen): x  6..14, z  0..10   L-shaped cabinets on east + north walls

#pragma once
#include <vector>
#include <cmath>
#include "level.h"   // lvQ

struct FurGeo {
    std::vector<float>    woodv, stonev, uphv, scrnv;
    std::vector<unsigned> woodi, stonei, uphi, scrni;
};

// All 6 faces of an AABB, CCW winding from outside, UV = world extents
static void furBox(std::vector<float>& V, std::vector<unsigned>& I,
                   float x0, float y0, float z0,
                   float x1, float y1, float z1)
{
    float lx=x1-x0, ly=y1-y0, lz=z1-z0;
    // +X
    lvQ(V,I,{x1,y0,z0},{x1,y1,z0},{x1,y1,z1},{x1,y0,z1},{1,0,0},  {0,0},{0,ly},{lz,ly},{lz,0});
    // -X
    lvQ(V,I,{x0,y0,z1},{x0,y1,z1},{x0,y1,z0},{x0,y0,z0},{-1,0,0}, {0,0},{0,ly},{lz,ly},{lz,0});
    // +Y
    lvQ(V,I,{x0,y1,z1},{x1,y1,z1},{x1,y1,z0},{x0,y1,z0},{0,1,0},  {0,0},{lx,0},{lx,lz},{0,lz});
    // -Y
    lvQ(V,I,{x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1},{0,-1,0}, {0,0},{lx,0},{lx,lz},{0,lz});
    // +Z
    lvQ(V,I,{x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1},{0,0,1},  {0,0},{lx,0},{lx,ly},{0,ly});
    // -Z
    lvQ(V,I,{x1,y0,z0},{x0,y0,z0},{x0,y1,z0},{x1,y1,z0},{0,0,-1}, {0,0},{lx,0},{lx,ly},{0,ly});
}

static FurGeo buildFurniture()
{
    FurGeo g;

    // Aliases: wood / stone counter / upholstery / dark-screen
    auto fw = [&](float x0,float y0,float z0,float x1,float y1,float z1)
        { furBox(g.woodv, g.woodi, x0,y0,z0,x1,y1,z1); };
    auto fs = [&](float x0,float y0,float z0,float x1,float y1,float z1)
        { furBox(g.stonev,g.stonei,x0,y0,z0,x1,y1,z1); };
    auto fu = [&](float x0,float y0,float z0,float x1,float y1,float z1)
        { furBox(g.uphv,  g.uphi,  x0,y0,z0,x1,y1,z1); };
    auto fd = [&](float x0,float y0,float z0,float x1,float y1,float z1)
        { furBox(g.scrnv, g.scrni, x0,y0,z0,x1,y1,z1); };

    // ── KITCHEN (Room C: x 6..14, z 0..10) ──────────────────────────────────
    // Inner east wall face: x=13.90   Inner north wall face: z=9.90
    // East wall: lower cabinets + stone counter + upper cabinets
    fw(13.20f, 0.00f, 0.50f,  13.90f, 0.86f, 9.50f);   // lower cabs
    fs(13.10f, 0.86f, 0.50f,  13.90f, 0.92f, 9.50f);   // counter
    fw(13.55f, 1.45f, 1.00f,  13.90f, 2.20f, 9.00f);   // upper cabs
    // North wall: lower cabinets + stone counter + upper cabinets
    fw( 6.50f, 0.00f, 9.20f,  13.20f, 0.86f, 9.90f);   // lower cabs
    fs( 6.50f, 0.86f, 9.10f,  13.20f, 0.92f, 9.90f);   // counter
    fw( 6.50f, 1.45f, 9.55f,  13.20f, 2.20f, 9.90f);   // upper cabs
    // Corner filler (east-north join)
    fw(13.20f, 0.00f, 9.20f,  13.90f, 0.86f, 9.90f);
    fs(13.10f, 0.86f, 9.10f,  13.90f, 0.92f, 9.90f);

    // ── DINING ROOM (Room B: x -6..6, z 10..22) ─────────────────────────────
    // Table centred at (0, -, 16), 2.5 ft wide × 5 ft long
    fw(-1.25f, 0.74f, 13.50f,  1.25f, 0.78f, 18.50f);  // table top
    // 4 legs (0.08 × 0.08 cross-section)
    const float LS = 0.04f;
    fw(-1.17f-LS,0.00f,13.65f-LS, -1.17f+LS,0.74f,13.65f+LS); // NW
    fw(-1.17f-LS,0.00f,18.35f-LS, -1.17f+LS,0.74f,18.35f+LS); // SW
    fw( 1.17f-LS,0.00f,13.65f-LS,  1.17f+LS,0.74f,13.65f+LS); // NE
    fw( 1.17f-LS,0.00f,18.35f-LS,  1.17f+LS,0.74f,18.35f+LS); // SE

    // Chair lambda: bx = back-face x, dx = facing direction (+1=east, -1=west), cz = z centre
    auto furChair = [&](float bx, float dx, float cz) {
        float fx  = bx + dx*0.42f;              // front face x
        float x0  = dx > 0 ? bx : fx;
        float x1  = dx > 0 ? fx : bx;
        float bl0 = dx > 0 ? bx        : fx-0.04f;  // back-leg x0
        float bl1 = dx > 0 ? bx+0.04f  : fx;        // back-leg x1
        float fl0 = dx > 0 ? fx-0.04f  : bx;        // front-leg x0
        float fl1 = dx > 0 ? fx        : bx+0.04f;  // front-leg x1
        fw(x0, 0.42f, cz-0.21f, x1, 0.46f, cz+0.21f);           // seat board
        fw(bl0,0.00f, cz-0.21f, bl1,0.88f, cz-0.17f);            // back leg L
        fw(bl0,0.00f, cz+0.17f, bl1,0.88f, cz+0.21f);            // back leg R
        fw(fl0,0.00f, cz-0.21f, fl1,0.42f, cz-0.17f);            // front leg L
        fw(fl0,0.00f, cz+0.17f, fl1,0.42f, cz+0.21f);            // front leg R
        fw(bl0,0.70f, cz-0.21f, bl1,0.86f, cz+0.21f);            // back rail
        fu(x0+0.02f,0.46f,cz-0.19f, x1-0.02f,0.56f,cz+0.19f);   // seat cushion
    };
    // 3 west-side chairs (back at x=-1.80, facing east dx=+1)
    furChair(-1.80f,+1.f,14.20f);
    furChair(-1.80f,+1.f,16.00f);
    furChair(-1.80f,+1.f,17.80f);
    // 3 east-side chairs (back at x=+1.80, facing west dx=-1)
    furChair(+1.80f,-1.f,14.20f);
    furChair(+1.80f,-1.f,16.00f);
    furChair(+1.80f,-1.f,17.80f);

    // ── LIVING ROOM (Room A: x -6..6, z 0..10) ──────────────────────────────
    // Sofa against south wall (z=0.10), facing north; placed east of entrance
    // Back cushion (upholstery, vertical)
    fu(1.80f, 0.10f, 0.10f,  5.50f, 1.00f, 0.62f);
    // Seat cushions (upholstery, horizontal)
    fu(1.80f, 0.10f, 0.62f,  5.50f, 0.55f, 2.00f);
    // Left arm rest
    fu(1.80f, 0.55f, 0.62f,  2.30f, 0.76f, 2.00f);
    // Right arm rest
    fu(5.00f, 0.55f, 0.62f,  5.50f, 0.76f, 2.00f);
    // Base frame (wood)
    fw(1.80f, 0.00f, 0.10f,  5.50f, 0.10f, 2.00f);
    // 4 stubby legs (wood, 5" × 5" × 6")
    fw(1.80f,0.00f,0.10f, 1.97f,0.18f,0.27f);
    fw(1.80f,0.00f,1.83f, 1.97f,0.18f,2.00f);
    fw(5.33f,0.00f,0.10f, 5.50f,0.18f,0.27f);
    fw(5.33f,0.00f,1.83f, 5.50f,0.18f,2.00f);

    // TV on north wall (z=9.90 inner face), centred x≈4.0, facing south
    // Stand base + column (dark metal)
    fd(3.45f,0.40f,9.35f,  4.55f,0.44f,9.90f);    // base foot
    fd(3.75f,0.44f,9.60f,  4.25f,0.82f,9.90f);    // column
    // TV panel + screen
    fd(2.80f,0.80f,9.62f,  5.20f,1.72f,9.90f);    // bezel (dark surround)
    fd(2.84f,0.84f,9.58f,  5.16f,1.68f,9.58f);    // screen (slightly in front)

    return g;
}
