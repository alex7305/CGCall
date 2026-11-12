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
AUTO_SEND = True                      # False = fill CGCall's transmit box only
MIN_GAP_SECONDS = 15                  # Minimum pause between auto-sent messages
LOCK_PORT = 47654                     # Single-instance port

CGCALL_EXE = "JS8Call.exe"            # CGCall process name
GRACE_SECONDS = 60                    # Wait this long for CGCall to appear
CHECK_EVERY = 3                       # Seconds between process checks

# Run with --standalone to keep the window open without CGCall.
WATCH_CGCALL = "--standalone" not in sys.argv

PREFIX = "OPS"


# Store logs and pause.txt beside the executable when frozen,
# or beside this Python script when running from source.
BASE = os.path.dirname(
    sys.executable if getattr(sys, "frozen", False)
    else os.path.abspath(__file__)
)

PAUSE_FILE = os.path.join(BASE, "pause.txt")
LOG_FILE = os.path.join(BASE, "cgcall_entry.log")


# ------------------------------------------------------------ shared state

send_queue = queue.Queue()
ui_queue = queue.Queue()

_pending = 0
_pending_lock = threading.Lock()


def log(msg):
    """Print to the console, if available, and append to the log file."""
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}"

    try:
        print(line, flush=True)
    except Exception:
        pass  # A windowed executable may not have a console.

    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def status(text):
    """Send a status update to the Tkinter thread."""
    ui_queue.put(("status", text))


# --------------------------------------------------------------- CGCall API

def api(msg_type, text):
    """Send one JSON command to CGCall's TCP API."""
    msg = json.dumps({
        "type": msg_type,
        "value": text,
        "params": {}
    }) + "\n"

    with socket.create_connection((HOST, PORT), timeout=3) as s:
        s.sendall(msg.encode("utf-8"))


def sender_loop():
    """Send queued messages one at a time, respecting pause.txt and the gap."""
    global _pending

    last_sent = 0.0

    while True:
        msg = send_queue.get()

        while True:
            if os.path.exists(PAUSE_FILE):
                status("Paused (pause.txt exists); holding: " + msg)
                time.sleep(1)
                continue

            wait = (
                MIN_GAP_SECONDS - (time.time() - last_sent)
                if AUTO_SEND else 0
            )

            if wait > 0:
                status(f"Waiting {int(wait) + 1}s before sending: {msg}")
                time.sleep(min(wait, 1))
                continue

            try:
                api(
                    "TX.SEND_MESSAGE" if AUTO_SEND else "TX.SET_TEXT",
                    msg
                )

                last_sent = time.time()

                action = "Sent" if AUTO_SEND else "Filled transmit box"
                log(f"{action}: {msg}")
                status(f"{action}: {msg}")
                break

            except OSError as e:
                log(f"CGCall not reachable ({e}); retrying")
                status("CGCall not reachable; retrying...")
                time.sleep(2)

        with _pending_lock:
            _pending -= 1

        send_queue.task_done()


def enqueue(msg):
    """Add a message to the send queue; return how many are already ahead."""
    global _pending

    with _pending_lock:
        ahead = _pending
        _pending += 1

    send_queue.put(msg)
    return ahead


# ------------------------------------------------------- CGCall lifetime

def cgcall_running():
    """Return whether CGCall is running. If uncertain, assume it is."""
    try:
        result = subprocess.run(
            ["tasklist", "/FI", f"IMAGENAME eq {CGCALL_EXE}", "/NH"],
            capture_output=True,
            text=True,
            timeout=5,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )

        return CGCALL_EXE.lower() in result.stdout.lower()

    except Exception:
        return True


def watch_cgcall():
    """Close the entry window after CGCall has exited."""
    started = time.time()
    seen = False
    misses = 0

    while True:
        time.sleep(CHECK_EVERY)

        if cgcall_running():
            seen = True
            misses = 0

        elif seen or time.time() - started > GRACE_SECONDS:
            misses += 1

            # Require two consecutive checks showing that CGCall is absent.
            if misses >= 2:
                ui_queue.put((
                    "quit",
                    f"{CGCALL_EXE} is not running; closing."
                ))
                return


# ------------------------------------------------------- single instance

def _listen_for_raise(sock):
    """Let a second copy ask the existing window to come to the front."""
    while True:
        try:
            conn, _ = sock.accept()

            with conn:
                conn.settimeout(2)

                if conn.recv(16).strip() == b"RAISE":
                    ui_queue.put(("raise",))

        except OSError:
            continue


def single_instance():
    """Ensure only one copy of this entry window is running."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    try:
        sock.bind(("127.0.0.1", LOCK_PORT))
        sock.listen(1)

    except OSError:
        sock.close()

        try:
            with socket.create_connection(
                ("127.0.0.1", LOCK_PORT), timeout=2
            ) as conn:
                conn.sendall(b"RAISE\n")

            log("Input window is already open; asked it to come to the front.")
            sys.exit(0)

        except OSError:
            log(f"Port {LOCK_PORT} is busy and nothing answered; exiting.")
            sys.exit(1)

    threading.Thread(
        target=_listen_for_raise,
        args=(sock,),
        daemon=True
    ).start()

    return sock


# ------------------------------------------------------------- formatting

def format_message(values):
    """Format an OPS position/status message."""
    return (
        f"{PREFIX} "
        f"{values['to']} "
        f"{values['state']} "
        f"{values['latDeg']:02d} "
        f"{values['latMin']:02d} "
        f"{values['latDir']} "
        f"{values['lonDeg']:03d} "
        f"{values['lonMin']:02d} "
        f"{values['lonDir']} "
        f"{values['course']} "
        f"{values['speed']}"
    )


# ------------------------------------------------------------------- window

class App:

    def __init__(self, root):
        self.root = root
        self.values = {}
        self.entries = {}

        root.title("CGCall OPS Entry")
        root.resizable(False, False)

        # Destination / recipient
        self.add_field(0, "To (0 - 127, 0 = broadcast)", "to")

        # Underway / Anchored
        tk.Label(root, text="Status", anchor="w").grid(
            row=1, column=0, sticky="w", padx=8, pady=4
        )

        self.state = tk.StringVar(value="Underway")

        tk.OptionMenu(
            root, self.state, "Underway", "Anchored"
        ).grid(row=1, column=1, sticky="w", padx=5)

        # Latitude
        tk.Label(root, text="Latitude (deg / min)", anchor="w").grid(
            row=2, column=0, sticky="w", padx=8, pady=4
        )

        self.add_entry(2, "latDeg", 1, width=6)
        self.add_entry(2, "latMin", 2, width=6)

        self.latDir = tk.StringVar(value="N")

        tk.OptionMenu(root, self.latDir, "N", "S").grid(
            row=2, column=3, padx=5
        )

        # Longitude
        tk.Label(root, text="Longitude (deg / min)", anchor="w").grid(
            row=3, column=0, sticky="w", padx=8, pady=4
        )

        self.add_entry(3, "lonDeg", 1, width=6)
        self.add_entry(3, "lonMin", 2, width=6)

        self.lonDir = tk.StringVar(value="W")

        tk.OptionMenu(root, self.lonDir, "E", "W").grid(
            row=3, column=3, padx=5
        )

        # Course and speed
        self.add_field(4, "Course (0 - 359)", "course")
        self.add_field(5, "Speed (0 - 63)", "speed")

        # Send button
        tk.Button(
            root,
            text="Send",
            width=12,
            command=self.send
        ).grid(row=6, column=0, columnspan=4, pady=8)

        # Status
        self.status = tk.Label(
            root,
            text="",
            fg="#444",
            anchor="w",
            justify="left",
            wraplength=360
        )

        self.status.grid(
            row=7, column=0, columnspan=4,
            sticky="w", padx=8, pady=(0, 8)
        )

        root.bind("<Return>", lambda _event: self.send())
        root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.entries["to"].focus_set()
        self.root.after(200, self.poll)

    def add_entry(self, row, name, column, width=12):
        """Create and register an entry box."""
        entry = tk.Entry(self.root, width=width)
        entry.grid(row=row, column=column, padx=5, pady=4)
        self.entries[name] = entry

    def add_field(self, row, label, name):
        """Create a labeled text-entry field."""
        tk.Label(
            self.root,
            text=label,
            anchor="w"
        ).grid(row=row, column=0, sticky="w", padx=8, pady=4)

        entry = tk.Entry(self.root, width=18)
        entry.grid(row=row, column=1, padx=5, pady=4)
        self.entries[name] = entry

    # ---- Background messages -> window

    def poll(self):
        """Process updates from background threads on the Tkinter thread."""
        try:
            while True:
                kind, *rest = ui_queue.get_nowait()

                if kind == "status":
                    self.status.config(text=rest[0])

                elif kind == "raise":
                    self.root.deiconify()
                    self.root.lift()
                    self.root.attributes("-topmost", True)
                    self.root.after(
                        300,
                        lambda: self.root.attributes("-topmost", False)
                    )
                    self.root.focus_force()

                elif kind == "quit":
                    log(rest[0])
                    self.root.destroy()
                    return

        except queue.Empty:
            pass

        self.root.after(200, self.poll)

    def on_close(self):
        """Warn before closing if messages are still queued or in flight."""
        with _pending_lock:
            pending = _pending

        if pending and not messagebox.askyesno(
            "Unsent messages",
            f"{pending} message(s) have not been sent yet "
            "and will be lost.\n\nClose anyway?"
        ):
            return

        self.root.destroy()

    def read_values(self):
        """Read and validate all form fields."""
        values = {}
        errors = []

        integer_ranges = {
            "to": (0, 127),
            "latDeg": (0, 90),
            "latMin": (0, 59),
            "lonDeg": (0, 180),
            "lonMin": (0, 59),
            "course": (0, 359),
            "speed": (0, 63),
        }

        for name, (low, high) in integer_ranges.items():
            text_value = self.entries[name].get().strip()

            if not re.fullmatch(r"\d+", text_value):
                errors.append(f"{name}: enter a whole number")
                continue

            number = int(text_value)

            if not low <= number <= high:
                errors.append(
                    f"{name}: must be between {low} and {high}"
                )
                continue

            values[name] = number

        # Coordinate boundary checks
        if (
            values.get("latDeg") == 90
            and values.get("latMin", 0) != 0
        ):
            errors.append("Latitude: minutes must be 0 at 90 degrees")

        if (
            values.get("lonDeg") == 180
            and values.get("lonMin", 0) != 0
        ):
            errors.append("Longitude: minutes must be 0 at 180 degrees")

        # Selection fields
        values["state"] = (
            "U" if self.state.get() == "Underway" else "A"
        )
        values["latDir"] = self.latDir.get()
        values["lonDir"] = self.lonDir.get()

        return values, errors

    def send(self):
        """Validate, confirm, and queue the message."""
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


# ------------------------------------------------------------------- main

if __name__ == "__main__":

    lock = single_instance()  # Keep the socket alive for this process.

    threading.Thread(
        target=sender_loop,
        daemon=True
    ).start()

    if WATCH_CGCALL:
        threading.Thread(
            target=watch_cgcall,
            daemon=True
        ).start()

    root = tk.Tk()
    App(root)

    log(
        "Input window opened "
        f"({'closes with ' + CGCALL_EXE if WATCH_CGCALL else 'standalone'}, "
        f"auto-send {'ON' if AUTO_SEND else 'OFF'})"
    )

    root.mainloop()
