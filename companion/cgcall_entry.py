
import os
import re
import sys
import tkinter as tk
from tkinter import messagebox

BASE = os.path.dirname(
    sys.executable if getattr(sys, "frozen", False)
    else os.path.abspath(__file__)
)

OUTBOX = os.path.join(BASE, "outbox.txt")
PAUSE_FILE = os.path.join(BASE, "pause.txt")

PREFIX = "OPS"


def format_message(v):
    """Format all values into the OPS transmission message."""

    return (
        f"{PREFIX} "
        f"{v['callsign']} "
        f"{v['latDeg']:02d} "
        f"{v['latMin']:.1f} "
        f"{v['latDir']} "
        f"{v['lonDeg']:03d} "
        f"{v['lonMin']:.1f} "
        f"{v['lonDir']} "
        f"{v['course']} "
        f"{v['speed']}"
    )


class App:

    def __init__(self, root):
        self.root = root
        self.values = {}
        self.entries = {}

        root.title("CGCall OPS Entry")

        # Callsign
        self.add_field(0, "Callsign (0 - 99999)", "callsign")

        # Latitude
        tk.Label(
            root, text="Latitude", anchor="w"
        ).grid(row=1, column=0, sticky="w", padx=8, pady=4)

        self.add_entry(1, "latDeg", 1)

        tk.Label(root, text="Degrees").grid(
            row=1, column=2, sticky="w"
        )

        self.add_entry(2, "latMin", 1)

        tk.Label(root, text="Minutes").grid(
            row=2, column=2, sticky="w"
        )

        self.latDir = tk.StringVar(value="N")

        tk.OptionMenu(
            root, self.latDir, "N", "S"
        ).grid(row=1, column=3, padx=5)

        # Longitude
        tk.Label(
            root, text="Longitude", anchor="w"
        ).grid(row=3, column=0, sticky="w", padx=8, pady=4)

        self.add_entry(3, "lonDeg", 1)

        tk.Label(root, text="Degrees").grid(
            row=3, column=2, sticky="w"
        )

        self.add_entry(4, "lonMin", 1)

        tk.Label(root, text="Minutes").grid(
            row=4, column=2, sticky="w"
        )

        self.lonDir = tk.StringVar(value="W")

        tk.OptionMenu(
            root, self.lonDir, "E", "W"
        ).grid(row=3, column=3, padx=5)

        # Course and speed
        self.add_field(5, "Course (0 - 359)", "course")
        self.add_field(6, "Speed (0 - 63)", "speed")

        # Status
        self.status = tk.Label(
            root, text="", fg="#444", anchor="w"
        )

        self.status.grid(
            row=8, column=0, columnspan=4,
            sticky="w", padx=8
        )

        # Send button
        tk.Button(
            root,
            text="Send",
            width=12,
            command=self.send
        ).grid(row=7, column=0, columnspan=4, pady=8)

        root.bind("<Return>", lambda _e: self.send())

        self.entries["callsign"].focus_set()

    def add_entry(self, row, name, column):
        e = tk.Entry(self.root, width=12)
        e.grid(row=row, column=column, padx=5, pady=4)
        self.entries[name] = e

    def add_field(self, row, label, name):
        tk.Label(
            self.root, text=label, anchor="w"
        ).grid(row=row, column=0, sticky="w", padx=8, pady=4)

        e = tk.Entry(self.root, width=18)
        e.grid(row=row, column=1, padx=5, pady=4)

        self.entries[name] = e

    def read_values(self):

        values = {}
        errors = []

        # Integer fields
        integer_ranges = {
            "callsign": (0, 99999),
            "latDeg": (0, 90),
            "lonDeg": (0, 180),
            "course": (0, 359),
            "speed": (0, 63),
        }

        for name, (lo, hi) in integer_ranges.items():

            text = self.entries[name].get().strip()

            if not re.fullmatch(r"\d+", text):
                errors.append(f"{name}: enter a whole number")
                continue

            n = int(text)

            if not lo <= n <= hi:
                errors.append(
                    f"{name}: must be between {lo} and {hi}"
                )
                continue

            values[name] = n

        # Latitude and longitude minutes
        for name in ("latMin", "lonMin"):

            text = self.entries[name].get().strip()

            if not re.fullmatch(r"\d+(?:\.\d)?", text):
                errors.append(
                    f"{name}: enter a number with at most one decimal place"
                )
                continue

            n = float(text)

            if not 0 <= n < 60:
                errors.append(
                    f"{name}: must be between 0.0 and 59.9"
                )
                continue

            values[name] = n

        # Coordinate boundary checks
        if "latDeg" in values and "latMin" in values:
            if values["latDeg"] == 90 and values["latMin"] != 0:
                errors.append(
                    "Latitude: minutes must be 0 at 90 degrees"
                )

        if "lonDeg" in values and "lonMin" in values:
            if values["lonDeg"] == 180 and values["lonMin"] != 0:
                errors.append(
                    "Longitude: minutes must be 0 at 180 degrees"
                )

        # Direction selectors
        values["latDir"] = self.latDir.get()
        values["lonDir"] = self.lonDir.get()

        return values, errors

    def send(self):

        values, errors = self.read_values()

        if errors:
            messagebox.showerror(
                "Check your entries",
                "\n".join(errors)
            )
            return

        msg = format_message(values)

        note = (
            "\n\nSending is currently PAUSED (pause.txt exists)."
            if os.path.exists(PAUSE_FILE)
            else ""
        )

        if not messagebox.askyesno(
            "Confirm",
            f"This will be queued for transmission:\n\n{msg}{note}"
        ):
            return

        self.values = values

        with open(OUTBOX, "a", encoding="utf-8") as f:
            f.write(msg + "\n")

        self.status.config(text=f"Queued: {msg}")


if __name__ == "__main__":

    root = tk.Tk()
    App(root)
    root.mainloop()
