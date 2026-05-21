// baphomet.h — Baphomet figure geometry built from primitive shapes
// Placed at the back of the marble room (south room 0)
// Uses props.h addBox / addCylinder / addSphere / addCone
#pragma once
#include "props.h"

// Builds a seated Baphomet figure with two child figures flanking it.
// cx,cz = center position, yFloor = floor y-level of the room (e.g. -100)
static inline void buildBaphomet(std::vector<float>& V, std::vector<unsigned>& I,
                                  float cx, float cz, float yFloor)
{
    const float F = yFloor;

    // ── Pedestal ──────────────────────────────────────────────────────────────
    addBox(V,I, cx,cz, F,       0.65f, 0.12f);   // wide base slab
    addBox(V,I, cx,cz, F+0.12f, 0.50f, 0.08f);   // step

    // ── Seated legs ──────────────────────────────────────────────────────────
    // thighs (angled forward)
    addBox(V,I, cx-0.23f, cz+0.20f, F+0.20f, 0.13f, 0.50f);
    addBox(V,I, cx+0.23f, cz+0.20f, F+0.20f, 0.13f, 0.50f);
    // lower legs/feet
    addBox(V,I, cx-0.23f, cz+0.10f, F+0.20f, 0.10f, 0.20f);
    addBox(V,I, cx+0.23f, cz+0.10f, F+0.20f, 0.10f, 0.20f);

    // ── Torso ────────────────────────────────────────────────────────────────
    addBox(V,I, cx, cz, F+0.70f, 0.30f, 0.90f);

    // ── Wings (large flat planes on each side, angled up) ────────────────────
    // Each wing is three stacked flat boxes fanning outward
    addBox(V,I, cx-0.85f, cz-0.10f, F+1.00f, 0.55f, 0.08f, 0.35f);
    addBox(V,I, cx-1.15f, cz-0.15f, F+1.20f, 0.45f, 0.07f, 0.55f);
    addBox(V,I, cx-1.35f, cz-0.20f, F+1.40f, 0.35f, 0.06f, 0.70f);

    addBox(V,I, cx+0.85f, cz-0.10f, F+1.00f, 0.55f, 0.08f, -0.35f);
    addBox(V,I, cx+1.15f, cz-0.15f, F+1.20f, 0.45f, 0.07f, -0.55f);
    addBox(V,I, cx+1.35f, cz-0.20f, F+1.40f, 0.35f, 0.06f, -0.70f);

    // ── Arms ─────────────────────────────────────────────────────────────────
    // Left arm raised (upward and slightly left)
    addBox(V,I, cx-0.52f, cz, F+1.25f, 0.09f, 0.60f, 0.25f);
    // Right arm lowered (down and slightly right)
    addBox(V,I, cx+0.52f, cz, F+0.90f, 0.09f, 0.45f, -0.25f);

    // ── Neck ─────────────────────────────────────────────────────────────────
    addCylinder(V,I, cx, cz, F+1.58f, 0.09f, 0.22f);

    // ── Head (goat/skull sphere) ──────────────────────────────────────────────
    addSphere(V,I, cx, cz, F+1.78f, 0.28f);

    // ── Snout ────────────────────────────────────────────────────────────────
    addBox(V,I, cx, cz+0.18f, F+1.90f, 0.09f, 0.14f);

    // ── Horns (cones, one per side, curving outward and up) ───────────────────
    addCone(V,I, cx-0.22f, cz-0.08f, F+2.10f, 0.07f, 0.52f);
    addCone(V,I, cx+0.22f, cz-0.08f, F+2.10f, 0.07f, 0.52f);

    // ── Torch between horns ───────────────────────────────────────────────────
    addCylinder(V,I, cx, cz, F+2.05f, 0.025f, 0.42f);
    addSphere   (V,I, cx, cz, F+2.46f, 0.07f);  // flame tip

    // ── Inverted pentagram symbol on chest (hint) ─────────────────────────────
    // (simplified as a small recessed box on the torso)
    addBox(V,I, cx, cz-0.31f, F+0.98f, 0.18f, 0.18f);

    // ── Child figure — left ───────────────────────────────────────────────────
    {
        float lx = cx - 0.98f, lz = cz + 0.10f;
        addBox     (V,I, lx, lz, F+0.20f, 0.09f, 0.45f);  // body
        addCylinder(V,I, lx, lz, F+0.62f, 0.05f, 0.12f);  // neck
        addSphere  (V,I, lx, lz, F+0.72f, 0.11f);          // head
        // raised arms touching Baphomet's left hand
        addBox(V,I, lx+0.25f, lz, F+0.50f, 0.06f, 0.08f);
    }

    // ── Child figure — right ──────────────────────────────────────────────────
    {
        float rx = cx + 0.98f, rz = cz + 0.10f;
        addBox     (V,I, rx, rz, F+0.20f, 0.09f, 0.45f);
        addCylinder(V,I, rx, rz, F+0.62f, 0.05f, 0.12f);
        addSphere  (V,I, rx, rz, F+0.72f, 0.11f);
        addBox(V,I, rx-0.25f, rz, F+0.50f, 0.06f, 0.08f);
    }
}
