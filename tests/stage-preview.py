#!/usr/bin/env python3
"""Stage wireframe preview - shows what the browser SHOULD render."""
import tkinter as tk

def draw_stage_5():
    root = tk.Tk()
    root.title("EXPECTED: Stage 5 - Main Area Flex Column")
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

    # Status bar (global)
    S_TOP = VB - 28
    c.create_rectangle(VL, S_TOP, VR, VB, fill="#0ea5e9", outline="")
    c.create_text(VL+16, S_TOP+14, text="Stage 5: Main flex-column", anchor="w", fill="white", font=("sans-serif", 9))

    BODY_TOP = H_BOTTOM
    BODY_BOTTOM = S_TOP

    # === Sidebar (same as stage 4) ===
    SH_H = 38
    SF_H = 38
    c.create_rectangle(VL, BODY_TOP, SB_RIGHT, BODY_TOP + SH_H, fill="#1e293b", outline="#334155")
    c.create_text(VL+12, BODY_TOP + 19, text="Search notes...", anchor="w", fill="#94a3b8", font=("sans-serif", 9))

    LIST_TOP = BODY_TOP + SH_H
    LIST_BOTTOM = BODY_BOTTOM - SF_H
    c.create_rectangle(VL, LIST_TOP, SB_RIGHT, LIST_BOTTOM, fill="#1e293b", outline="")

    # Note items (compact)
    ni_y = LIST_TOP + 8
    ni_h = 40
    c.create_rectangle(VL+8, ni_y, SB_RIGHT-8, ni_y+ni_h, fill="#0ea5e9", outline="")
    c.create_text(VL+18, ni_y+12, text="Welcome to NoteSketch", anchor="w", fill="white", font=("sans-serif", 8))
    c.create_text(VL+18, ni_y+28, text="This is a demo app...", anchor="w", fill="#e0f2fe", font=("sans-serif", 7))
    ni_y += ni_h + 4
    c.create_rectangle(VL+8, ni_y, SB_RIGHT-8, ni_y+ni_h, fill="#1e293b", outline="")
    c.create_text(VL+18, ni_y+12, text="Shopping List", anchor="w", fill="#e2e8f0", font=("sans-serif", 8))
    c.create_text(VL+18, ni_y+28, text="Milk, Eggs, Bread...", anchor="w", fill="#94a3b8", font=("sans-serif", 7))
    ni_y += ni_h + 4
    c.create_rectangle(VL+8, ni_y, SB_RIGHT-8, ni_y+ni_h, fill="#1e293b", outline="")
    c.create_text(VL+18, ni_y+12, text="Meeting Notes", anchor="w", fill="#e2e8f0", font=("sans-serif", 8))
    c.create_text(VL+18, ni_y+28, text="Q3 Planning...", anchor="w", fill="#94a3b8", font=("sans-serif", 7))

    c.create_rectangle(VL, BODY_BOTTOM - SF_H, SB_RIGHT, BODY_BOTTOM, fill="#1e293b", outline="#334155")
    c.create_text(VL+12, BODY_BOTTOM - SF_H//2, text="[+ New Note] [Export]", anchor="w", fill="#94a3b8", font=("sans-serif", 9))
    c.create_line(SB_RIGHT, BODY_TOP, SB_RIGHT, BODY_BOTTOM, fill="#334155", width=1)

    # === Main area (flex column) ===
    TB_H = 36  # toolbar height
    MS_H = 28  # main-status height

    # Toolbar
    c.create_rectangle(SB_RIGHT, BODY_TOP, VR, BODY_TOP + TB_H, fill="#1e293b", outline="#334155")
    c.create_text(SB_RIGHT+16, BODY_TOP + TB_H//2, text="Note title... | [+ Tag] [Delete]", anchor="w", fill="#94a3b8", font=("sans-serif", 9))

    # Editor area (flex:1 fills middle)
    c.create_rectangle(SB_RIGHT, BODY_TOP + TB_H, VR, BODY_BOTTOM - MS_H, fill="#131c2e", outline="")
    c.create_text(SB_RIGHT+16, BODY_TOP + TB_H + 20, text="Editor content area", anchor="w", fill="#94a3b8", font=("sans-serif", 10))
    c.create_text(SB_RIGHT+16, BODY_TOP + TB_H + 38, text="(flex:1, fills remaining space)", anchor="w", fill="#64748b", font=("sans-serif", 8))

    # Main status
    c.create_rectangle(SB_RIGHT, BODY_BOTTOM - MS_H, VR, BODY_BOTTOM, fill="#1e293b", outline="#334155")
    c.create_text(SB_RIGHT+16, BODY_BOTTOM - MS_H//2, text="0 words, 0 chars", anchor="w", fill="#94a3b8", font=("sans-serif", 8))

    c.create_text(400, 595, text="Main: toolbar(top) + editor(flex:1, middle) + word-count(bottom)", fill="#aaa", font=("sans-serif", 8))

    root.mainloop()

if __name__ == "__main__":
    draw_stage_5()
