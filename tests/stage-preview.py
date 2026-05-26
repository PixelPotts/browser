#!/usr/bin/env python3
"""Stage wireframe preview - shows what the browser SHOULD render."""
import tkinter as tk

def draw_stage_1():
    root = tk.Tk()
    root.title("EXPECTED: Stage 1 - Basic Structure + Dark Theme")
    root.geometry("800x600")
    root.configure(bg="#222")

    c = tk.Canvas(root, width=800, height=600, bg="#222", highlightthickness=0)
    c.pack(fill="both", expand=True)

    # Browser viewport outline
    c.create_rectangle(20, 20, 780, 580, outline="#555", width=2)

    # Header bar - dark gray (#1e293b)
    c.create_rectangle(20, 20, 780, 68, fill="#1e293b", outline="#475569")
    c.create_text(44, 44, text="NoteSketch", anchor="w", fill="#38bdf8", font=("sans-serif", 16, "bold"))

    # Body area - very dark (#0f172a)
    c.create_rectangle(20, 68, 780, 548, fill="#0f172a", outline="")
    c.create_text(44, 100, text="Stage 1: Basic structure with dark theme.", anchor="w", fill="#e2e8f0", font=("sans-serif", 12))
    c.create_text(44, 122, text="This tests block layout, background-color, color, font-size, and padding.", anchor="w", fill="#e2e8f0", font=("sans-serif", 12))

    # Status bar - blue (#0ea5e9)
    c.create_rectangle(20, 548, 780, 580, fill="#0ea5e9", outline="")
    c.create_text(36, 564, text="Ready", anchor="w", fill="white", font=("sans-serif", 10))

    # Labels
    c.create_text(400, 595, text="What you should see: header (dark gray) | body (very dark) | status bar (blue)", fill="#aaa", font=("sans-serif", 9))

    root.mainloop()

if __name__ == "__main__":
    draw_stage_1()
