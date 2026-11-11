import os, re, sys
import tkinter as tk
from tkinter import messagebox

BASE = os.path.dirname(sys.executable if getattr(sys, "frozen", False)
                       else os.path.abspath(__file__))
OUTBOX = os.path.join(BASE, "outbox.txt")       # the file the feed watches
PAUSE_FILE = os.path.join(BASE, "pause.txt")

PREFIX = "TRK"   # first word of the message; change it to whatever your receiver expects

# (variable name, label, minimum, maximum)
FIELDS = [
    ("id",        "id (0 - 4294967295)",                          0, 4294967295),
    ("latTenths", "latTenths: latitude in tenths of a degree",    -900, 900),
    ("lonTenths", "lonTenths: longitude in tenths of a degree",  -1800, 1800),
    ("course",    "course: degrees (0 - 359)",                    0, 359),
    ("speed",     "speed (0 - 999)",                              0, 999),
]


def format_message(v):
    """Turns the saved variables into the text that goes to the transmit box."""
    return f"{PREFIX} {v['id']} {v['latTenths']} {v['lonTenths']} {v['course']} {v['speed']}"


class App:
    def __init__(self, root):
        self.root = root
        self.values = {}        # id, latTenths, lonTenths, course, speed after a successful Send
        self.entries = {}
        root.title("CGCall Entry")
        for row, (name, label, lo, hi) in enumerate(FIELDS):
            tk.Label(root, text=label, anchor="w").grid(row=row, column=0, sticky="w", padx=8, pady=4)
            e = tk.Entry(root, width=18)
            e.grid(row=row, column=1, padx=8, pady=4)
            self.entries[name] = e
        self.status = tk.Label(root, text="", fg="#444", anchor="w")
        self.status.grid(row=len(FIELDS) + 1, column=0, columnspan=2, sticky="w", padx=8)
        tk.Button(root, text="Send", width=12, command=self.send).grid(
            row=len(FIELDS), column=0, columnspan=2, pady=8)
        root.bind("<Return>", lambda _e: self.send())
        self.entries["id"].focus_set()

    def read_values(self):
        values, errors = {}, []
        for name, _label, lo, hi in FIELDS:
            text = self.entries[name].get().strip()
            if not re.fullmatch(r"-?\d+", text):
                errors.append(f"{name}: enter a whole number")
                continue
            n = int(text)
            if not lo <= n <= hi:
                errors.append(f"{name}: must be between {lo} and {hi}")
                continue
            values[name] = n
        return values, errors

    def send(self):
        values, errors = self.read_values()
        if errors:
            messagebox.showerror("Check your entries", "\n".join(errors))
            return
        msg = format_message(values)
        note = "\n\nSending is currently PAUSED (pause.txt exists)." if os.path.exists(PAUSE_FILE) else ""
        if not messagebox.askyesno("Confirm", f"This will be queued for transmission:\n\n{msg}{note}"):
            return
        self.values = values            # saved variables
        with open(OUTBOX, "a", encoding="utf-8") as f:
            f.write(msg + "\n")
        self.status.config(text=f"Queued: {msg}")


if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()
