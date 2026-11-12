import json
import os
import queue
import re
import socket
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import messagebox

# ---------------------------------------------------------------- settings
HOST, PORT = "127.0.0.1", 2442       # CGCall's TCP API
AUTO_SEND = True                      # False = only fill CGCall's transmit box; you press Send there
MIN_GAP_SECONDS = 15                  # minimum pause between auto-sent messages
LOCK_PORT = 47654                     # single-instance port (different from the old feed's 47653)

CGCALL_EXE = "JS8Call.exe"            # process name of CGCall; this window closes when it closes
GRACE_SECONDS = 60                    # how long to wait for CGCall to appear before giving up
CHECK_EVERY = 3                       # seconds between checks
# Run with --standalone (e.g. for debugging) to keep the window open without CGCall.
WATCH_CGCALL = "--standalone" not in sys.argv

PREFIX = "OPS"

BASE = os.path.dirname(
    sys.executable if getattr(sys, "frozen", False)
    else os.path.abspath(__file__)
)
PAUSE_FILE = os.path.join(BASE, "pause.txt")   # if this file exists, nothing is sent
LOG_FILE = os.path.join(BASE, "cgcall_entry.log")

# ------------------------------------------------------------ shared state
send_queue = queue.Queue()     # messages waiting to be sent to CGCall
ui_queue = queue.Queue()       # things worker threads want the window to do
_pending = 0                   # messages queued or in flight
_pending_lock = threading.Lock()


def log(msg):
    """Print to the console (if any) and append to cgcall_entry.log."""
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}"
    try:
        print(line, flush=True)
    except Exception:
        pass                      # windowed exe has no console
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def status(text):
    ui_queue.put(("status", text))


# --------------------------------------------------------------- CGCall API
def api(msg_type, text):
    msg = json.dumps({"type": msg_type, "value": text, "params": {}}) + "\n"
    with socket.create_connection((HOST, PORT), timeout=3) as s:
        s.sendall(msg.encode("utf-8"))


def sender_loop():
    """Worker thread: send queued messages one at a time, respecting the gap and pause file."""
    global _pending
    last_sent = 0.0
    while True:
        msg = send_queue.get()
        while True:
            if os.path.exists(PAUSE_FILE):
                status("Paused (pause.txt exists); holding: " + msg)
                time.sleep(1)
                continue
            wait = (MIN_GAP_SECONDS - (time.time() - last_sent)) if AUTO_SEND else 0
            if wait > 0:
                status(f"Waiting {int(wait) + 1}s before sending: {msg}")
                time.sleep(min(wait, 1))
                continue
            try:
                api("TX.SEND_MESSAGE" if AUTO_SEND else "TX.SET_TEXT", msg)
                last_sent = time.time()
                log(f"{'Sent' if AUTO_SEND else 'Filled transmit box'}: {msg}")
                status(f"{'Sent' if AUTO_SEND else 'In transmit box'}: {msg}")
                break
            except OSError as e:
                log(f"CGCall not reachable ({e}); retrying")
                status("CGCall not reachable; retrying...")
                time.sleep(2)
        with _pending_lock:
            _pending -= 1


def enqueue(msg):
    """Add a message to the send queue; returns how many are ahead of it."""
    global _pending
    with _pending_lock:
        ahead = _pending
        _pending += 1
    send_queue.put(msg)
    return ahead


# ------------------------------------------------------- CGCall lifetime
def cgcall_running():
    """True if CGCall's process is alive. If we can't tell, assume it is."""
    try:
        out = subprocess.run(
            ["tasklist", "/FI", f"IMAGENAME eq {CGCALL_EXE}", "/NH"],
            capture_output=True, text=True, timeout=5,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        ).stdout
        return CGCALL_EXE.lower() in out.lower()
    except Exception:
        return True


def watch_cgcall():
    """Worker thread: ask the window to close once CGCall has closed."""
    started = time.time()
    seen = False
    misses = 0
    while True:
        time.sleep(CHECK_EVERY)
        if cgcall_running():
            seen = True
            misses = 0
        elif seen or time.time() - started > GRACE_SECONDS:
            misses += 1                  # require two misses in a row
            if misses >= 2:
                ui_queue.put(("quit", f"{CGCALL_EXE} is not running; closing."))
                return


# ------------------------------------------------------- single instance
def _listen_for_raise(s):
    """Worker thread: a second copy asks us to come to the front instead of opening twice."""
    while True:
        try:
            c, _ = s.accept()
            with c:
                c.settimeout(2)
                if c.recv(16).strip() == b"RAISE":
                    ui_queue.put(("raise",))
        except OSError:
            continue


def single_instance():
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", LOCK_PORT))
        s.listen(1)
    except OSError:
        s.close()
        try:
            with socket.create_connection(("127.0.0.1", LOCK_PORT), timeout=2) as c:
                c.sendall(b"RAISE\n")
            log("Input window is already open; asked it to come to the front.")
            sys.exit(0)
        except OSError:
            log(f"Port {LOCK_PORT} is busy and nothing answered; exiting.")
            sys.exit(1)
    threading.Thread(target=_listen_for_raise, args=(s,), daemon=True).start()
    return s


# ------------------------------------------------------------- formatting
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


# ------------------------------------------------------------------- window
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
            root, text="", fg="#444", anchor="w",
            justify="left", wraplength=360
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
        root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.entries["callsign"].focus_set()
        self.root.after(200, self.poll)

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

    # ---- background messages -> window
    def poll(self):
        try:
            while True:
                kind, *rest = ui_queue.get_nowait()
                if kind == "status":
                    self.status.config(text=rest[0])
                elif kind == "raise":
                    self.root.deiconify()
                    self.root.lift()
                    self.root.attributes("-topmost", True)
                    self.root.after(300, lambda: self.root.attributes("-topmost", False))
                    self.root.focus_force()
                elif kind == "quit":
                    log(rest[0])
                    self.root.destroy()
                    return
        except queue.Empty:
            pass
        self.root.after(200, self.poll)

    def on_close(self):
        with _pending_lock:
            n = _pending
        if n and not messagebox.askyesno(
            "Unsent messages",
            f"{n} message(s) have not been sent yet and will be lost.\n\nClose anyway?"
        ):
            return
        self.root.destroy()

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
            f"This will be sent to CGCall:\n\n{msg}{note}"
        ):
            return

        self.values = values

        ahead = enqueue(msg)
        extra = f" ({ahead} ahead of it)" if ahead else ""
        self.status.config(text=f"Queued: {msg}{extra}")


if __name__ == "__main__":

    lock = single_instance()           # keep this variable alive
    threading.Thread(target=sender_loop, daemon=True).start()
    if WATCH_CGCALL:
        threading.Thread(target=watch_cgcall, daemon=True).start()

    root = tk.Tk()
    App(root)
    log("Input window opened "
        f"({'closes with ' + CGCALL_EXE if WATCH_CGCALL else 'standalone'}, "
        f"auto-send {'ON' if AUTO_SEND else 'OFF'})")
    root.mainloop()
