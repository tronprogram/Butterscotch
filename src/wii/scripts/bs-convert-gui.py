#!/usr/bin/env python3
"""Tiny GUI wrapper around bs-convert.py (tkinter, stdlib only)."""
from __future__ import annotations

import subprocess
import sys
import threading
from pathlib import Path

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
except ImportError:
    print("tkinter is required for the converter GUI.", file=sys.stderr)
    raise SystemExit(1)

SCRIPT_DIR = Path(__file__).resolve().parent
CONVERT = SCRIPT_DIR / "bs-convert.py"


class App(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Butterscotch Wii texture convert")
        self.geometry("640x360")
        self.profile = tk.StringVar(value="dr")
        self.src = tk.StringVar()
        self.dst = tk.StringVar()

        pad = {"padx": 8, "pady": 4}
        ttk.Label(self, text="Game profile").grid(row=0, column=0, sticky="w", **pad)
        ttk.Combobox(
            self, textvariable=self.profile, values=("ut", "dr"), state="readonly", width=8
        ).grid(row=0, column=1, sticky="w", **pad)
        ttk.Label(self, text="ut = Undertale (WTL1)   dr = Deltarune (WTL2)").grid(
            row=0, column=2, sticky="w", **pad
        )

        ttk.Label(self, text="Input data.win").grid(row=1, column=0, sticky="w", **pad)
        ttk.Entry(self, textvariable=self.src, width=50).grid(row=1, column=1, columnspan=2, sticky="ew", **pad)
        ttk.Button(self, text="Browse…", command=self.browse_src).grid(row=1, column=3, **pad)

        ttk.Label(self, text="Output data.win").grid(row=2, column=0, sticky="w", **pad)
        ttk.Entry(self, textvariable=self.dst, width=50).grid(row=2, column=1, columnspan=2, sticky="ew", **pad)
        ttk.Button(self, text="Browse…", command=self.browse_dst).grid(row=2, column=3, **pad)

        self.go = ttk.Button(self, text="Convert", command=self.run)
        self.go.grid(row=3, column=1, sticky="w", **pad)

        self.log = tk.Text(self, height=12, wrap="word")
        self.log.grid(row=4, column=0, columnspan=4, sticky="nsew", padx=8, pady=8)
        self.grid_columnconfigure(1, weight=1)
        self.grid_rowconfigure(4, weight=1)

    def browse_src(self) -> None:
        p = filedialog.askopenfilename(title="Source data.win", filetypes=[("data.win", "*.win"), ("All", "*")])
        if p:
            self.src.set(p)

    def browse_dst(self) -> None:
        p = filedialog.asksaveasfilename(title="Converted data.win", defaultextension=".win")
        if p:
            self.dst.set(p)

    def append(self, text: str) -> None:
        self.log.insert("end", text)
        self.log.see("end")

    def run(self) -> None:
        src, dst = self.src.get().strip(), self.dst.get().strip()
        if not src or not dst:
            messagebox.showerror("Convert", "Pick input and output paths.")
            return
        self.go.configure(state="disabled")
        self.append(f"$ {CONVERT.name} {self.profile.get()} -i {src} -o {dst}\n")

        def worker() -> None:
            try:
                proc = subprocess.run(
                    [sys.executable, str(CONVERT), self.profile.get(), "-i", src, "-o", dst],
                    capture_output=True, text=True, check=False,
                )
                out = (proc.stdout or "") + (proc.stderr or "")
                self.after(0, lambda: self.done(proc.returncode, out))
            except Exception as exc:  # noqa: BLE001
                self.after(0, lambda: self.done(1, str(exc)))

        threading.Thread(target=worker, daemon=True).start()

    def done(self, code: int, out: str) -> None:
        self.append(out + ("\n" if out and not out.endswith("\n") else ""))
        self.append("done.\n" if code == 0 else f"failed ({code}).\n")
        self.go.configure(state="normal")
        if code == 0:
            messagebox.showinfo("Convert", "Conversion finished.")
        else:
            messagebox.showerror("Convert", "Conversion failed — see log.")


if __name__ == "__main__":
    App().mainloop()
