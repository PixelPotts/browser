#!/usr/bin/env python3
"""Stage wireframe preview - shows what the browser SHOULD render."""
import tkinter as tk

def draw_stage_10():
    root = tk.Tk()
    root.title("EXPECTED: Stage 10 - Canvas Drawing + Color Swatches")
    root.geometry("800x600")
    root.configure(bg="#222")

    c = tk.Canvas(root, width=800, height=600, bg="#222", highlightthickness=0)
    c.pack(fill="both", expand=True)

    VL, VT, VR, VB = 20, 20, 780, 580
    sidebar_w = int(260 / 1100 * (VR - VL))
    SB_RIGHT = VL + sidebar_w

    # Header
    H_BOTTOM = VT + 48
    c.create_rectangle(VL, VT, VR, H_BOTTOM, fill="#1e293b", outline="#475569")
    c.create_text(VL+24, VT+24, text="NoteSketch", anchor="w", fill="#38bdf8", font=("sans-serif", 14, "bold"))

    # Status bar
    S_TOP = VB - 28
    c.create_rectangle(VL, S_TOP, VR, VB, fill="#0ea5e9", outline="")
    c.create_text(VL+16, S_TOP+14, text="Stage 10: Canvas drawing + color swatches", anchor="w", fill="white", font=("sans-serif", 9))

    BODY_TOP = H_BOTTOM
    BODY_BOTTOM = S_TOP

    # === Sidebar (same as stage 9) ===
    SH_H = 44
    SF_H = 44
    c.create_rectangle(VL, BODY_TOP, SB_RIGHT, BODY_TOP + SH_H, fill="#1e293b", outline="#334155")
    c.create_rectangle(VL+12, BODY_TOP+10, SB_RIGHT-12, BODY_TOP+SH_H-10, fill="#0f172a", outline="#475569")
    c.create_text(VL+24, BODY_TOP+SH_H//2, text="Search notes...", anchor="w", fill="#64748b", font=("sans-serif", 9))

    LIST_TOP = BODY_TOP + SH_H
    LIST_BOTTOM = BODY_BOTTOM - SF_H
    c.create_rectangle(VL, LIST_TOP, SB_RIGHT, LIST_BOTTOM, fill="#1e293b", outline="")

    notes = [
        ("Welcome to NoteSketch", True),
        ("Shopping List", False),
        ("Meeting Notes", False),
        ("Ideas", False),
        ("Todo", False),
    ]
    ni_y = LIST_TOP + 8
    ni_h = 34
    for title, active in notes:
        bg = "#0ea5e9" if active else "#1e293b"
        tc = "white" if active else "#e2e8f0"
        c.create_rectangle(VL+8, ni_y, SB_RIGHT-8, ni_y+ni_h, fill=bg, outline="")
        c.create_text(VL+18, ni_y+ni_h//2, text=title, anchor="w", fill=tc, font=("sans-serif", 8))
        ni_y += ni_h + 4

    c.create_rectangle(VL, BODY_BOTTOM - SF_H, SB_RIGHT, BODY_BOTTOM, fill="#1e293b", outline="#334155")
    btn_y = BODY_BOTTOM - SF_H + 10
    btn_h = 24
    c.create_rectangle(VL+12, btn_y, VL+95, btn_y+btn_h, fill="#0ea5e9", outline="#0284c7")
    c.create_text(VL+53, btn_y+btn_h//2, text="+ New Note", fill="white", font=("sans-serif", 8))
    c.create_rectangle(VL+103, btn_y, VL+160, btn_y+btn_h, fill="#334155", outline="#475569")
    c.create_text(VL+131, btn_y+btn_h//2, text="Export", fill="#e2e8f0", font=("sans-serif", 8))

    c.create_line(SB_RIGHT, BODY_TOP, SB_RIGHT, BODY_BOTTOM, fill="#334155", width=1)

    # === Main area ===
    TB_H = 40
    MS_H = 28

    c.create_rectangle(SB_RIGHT, BODY_TOP, VR, BODY_TOP + TB_H, fill="#1e293b", outline="#334155")
    inp_left = SB_RIGHT + 16
    inp_right = VR - 170
    c.create_rectangle(inp_left, BODY_TOP+8, inp_right, BODY_TOP+TB_H-8, fill="#0f172a", outline="#475569")
    c.create_text(inp_left+12, BODY_TOP+TB_H//2, text="Note title...", anchor="w", fill="#64748b", font=("sans-serif", 9))
    bx = inp_right + 12
    c.create_rectangle(bx, BODY_TOP+8, bx+50, BODY_TOP+TB_H-8, fill="#334155", outline="#475569")
    c.create_text(bx+25, BODY_TOP+TB_H//2, text="+ Tag", fill="#e2e8f0", font=("sans-serif", 8))
    bx2 = bx + 58
    c.create_rectangle(bx2, BODY_TOP+8, bx2+55, BODY_TOP+TB_H-8, fill="#ef4444", outline="#dc2626")
    c.create_text(bx2+27, BODY_TOP+TB_H//2, text="Delete", fill="white", font=("sans-serif", 8))

    ED_TOP = BODY_TOP + TB_H
    ED_BOTTOM = BODY_BOTTOM - MS_H
    canvas_w = int(320 / 840 * (VR - SB_RIGHT))
    CP_LEFT = VR - canvas_w

    # Text editor
    c.create_rectangle(SB_RIGHT, ED_TOP, CP_LEFT, ED_BOTTOM, fill="#0f172a", outline="")
    ta_m = 16
    c.create_rectangle(SB_RIGHT+ta_m, ED_TOP+ta_m, CP_LEFT-ta_m, ED_BOTTOM-ta_m, fill="#1e293b", outline="#475569")
    c.create_text(SB_RIGHT+ta_m+12, ED_TOP+ta_m+14, text="Start typing your note...", anchor="w", fill="#64748b", font=("sans-serif", 9))

    # Canvas panel
    c.create_rectangle(CP_LEFT, ED_TOP, VR, ED_BOTTOM, fill="#1e293b", outline="")
    c.create_line(CP_LEFT, ED_TOP, CP_LEFT, ED_BOTTOM, fill="#334155", width=1)
    c.create_text(CP_LEFT+16, ED_TOP+16, text="Sketch Pad", anchor="w", fill="#94a3b8", font=("sans-serif", 9))

    # Canvas with sample drawing
    cv_top = ED_TOP + 36
    cv_h = min(180, ED_BOTTOM - cv_top - 60)
    c.create_rectangle(CP_LEFT+16, cv_top, VR-16, cv_top+cv_h, fill="#0f172a", outline="#475569")
    # Sample squiggle to show it's drawable
    pts = [CP_LEFT+40, cv_top+30, CP_LEFT+60, cv_top+60, CP_LEFT+90, cv_top+40,
           CP_LEFT+110, cv_top+80, CP_LEFT+130, cv_top+50]
    c.create_line(*pts, fill="#e2e8f0", width=3, smooth=True)

    # Color swatches below canvas
    sw_y = cv_top + cv_h + 12
    sw_size = 20
    colors = ["#e2e8f0", "#ef4444", "#22c55e", "#3b82f6", "#eab308"]
    sx = CP_LEFT + 16
    for i, col in enumerate(colors):
        outline = "#38bdf8" if i == 0 else "#475569"
        c.create_rectangle(sx, sw_y, sx+sw_size, sw_y+sw_size, fill=col, outline=outline, width=2)
        sx += sw_size + 6

    # Main status
    c.create_rectangle(SB_RIGHT, BODY_BOTTOM - MS_H, VR, BODY_BOTTOM, fill="#1e293b", outline="#334155")
    c.create_text(SB_RIGHT+16, BODY_BOTTOM - MS_H//2, text="0 words, 0 chars", anchor="w", fill="#94a3b8", font=("sans-serif", 8))

    c.create_text(400, 595, text="NEW: Canvas drawing (mouse events) + clickable color swatches (inline-block, fixed size)", fill="#aaa", font=("sans-serif", 8))

    root.mainloop()

if __name__ == "__main__":
    draw_stage_10()
