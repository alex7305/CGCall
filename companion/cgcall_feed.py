import json, os, shutil, socket, subprocess, sys, threading, time

HOST, PORT = "127.0.0.1", 2442
AUTO_SEND = True          # False = only fill the box; you press Send yourself
MIN_GAP_SECONDS = 15      # minimum pause between auto-sent messages
LOCK_PORT = 47653         # any unused local port; used for single-instance + takeover

CGCALL_EXE = "JS8Call.exe"   # process name of CGCall; the feed quits when it closes
GRACE_SECONDS = 60           # how long to wait for CGCall to appear before giving up
CHECK_EVERY = 3              # seconds between checks
# Run with --standalone (e.g. for debugging) to keep the feed alive without CGCall.
WATCH_CGCALL = "--standalone" not in sys.argv
# The feed also opens the input window (cgcall_entry) and closes it on exit.
# Run with --no-entry to skip that.
LAUNCH_ENTRY = "--no-entry" not in sys.argv
ENTRY_NAME = "cgcall_entry"     # cgcall_entry.exe (built) or cgcall_entry.py (script)

BASE = os.path.dirname(sys.executable if getattr(sys, "frozen", False)
                       else os.path.abspath(__file__))
OUTBOX = os.path.join(BASE, "outbox.txt")
PAUSE_FILE = os.path.join(BASE, "pause.txt")   # if this file exists, nothing is sent
LOG_FILE = os.path.join(BASE, "cgcall_feed.log")


def log(msg):
    """Print to the console (if any) and append to cgcall_feed.log."""
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


def api(msg_type, text):
    msg = json.dumps({"type": msg_type, "value": text, "params": {}}) + "\n"
    with socket.create_connection((HOST, PORT), timeout=3) as s:
        s.sendall(msg.encode("utf-8"))


_entry_proc = None


def launch_entry():
    """Open the input window: cgcall_entry.exe if present, else cgcall_entry.py."""
    global _entry_proc
    exe = os.path.join(BASE, ENTRY_NAME + ".exe")
    py = os.path.join(BASE, ENTRY_NAME + ".py")
    if os.path.exists(exe):
        cmd = [exe]
    elif os.path.exists(py):
        runner = shutil.which("pythonw") or shutil.which("pyw") or shutil.which("python")
        if not runner:
            log("No Python found to launch cgcall_entry.py")
            return
        cmd = [runner, py]
    else:
        log(f"Could not find {ENTRY_NAME}.exe or {ENTRY_NAME}.py in {BASE}")
        return
    try:
        _entry_proc = subprocess.Popen(cmd, cwd=BASE)
        log(f"Launched input window: {os.path.basename(cmd[-1])}")
    except OSError as e:
        log(f"Could not launch input window ({e})")


def shutdown():
    """Close the input window, then exit the feed."""
    if _entry_proc and _entry_proc.poll() is None:
        try:
            _entry_proc.terminate()
        except OSError:
            pass
    os._exit(0)


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


def _watch_cgcall():
    """Background thread: exit the feed once CGCall has closed."""
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
                log(f"{CGCALL_EXE} is not running; closing the feed.")
                shutdown()


def _listen_for_takeover(s):
    """Background thread: if a newer instance says QUIT, exit so it can take over."""
    while True:
        try:
            c, _ = s.accept()
            with c:
                c.settimeout(2)
                if c.recv(16).strip() == b"QUIT":
                    log("Newer instance asked me to quit; exiting.")
                    shutdown()
        except OSError:
            continue


def single_instance():
    """Hold the lock port. If another copy has it, ask it to quit and take over."""
    for _ in range(3):
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", LOCK_PORT))
            s.listen(1)
            threading.Thread(target=_listen_for_takeover, args=(s,), daemon=True).start()
            return s
        except OSError:
            s.close()
            log("Another cgcall_feed is running; asking it to quit...")
            try:
                with socket.create_connection(("127.0.0.1", LOCK_PORT), timeout=2) as c:
                    c.sendall(b"QUIT\n")
            except OSError:
                pass
            time.sleep(1.5)
    log(f"Port {LOCK_PORT} is busy and nothing answered; exiting.")
    sys.exit(1)


def main():
    lock = single_instance()           # keep this variable alive
    if WATCH_CGCALL:
        threading.Thread(target=_watch_cgcall, daemon=True).start()
    if LAUNCH_ENTRY:
        launch_entry()
    open(OUTBOX, "a").close()
    pos = os.path.getsize(OUTBOX)
    last_sent = 0.0
    log(f"Watching {OUTBOX} (auto-send {'ON' if AUTO_SEND else 'OFF'}, "
        f"{'closes with ' + CGCALL_EXE if WATCH_CGCALL else 'standalone'})")
    while True:
        if os.path.exists(PAUSE_FILE):
            time.sleep(1)
            continue
        size = os.path.getsize(OUTBOX)
        if size < pos:              # file was cleared
            pos = 0
        if size > pos:
            with open(OUTBOX, "rb") as f:
                f.seek(pos)
                raw = f.readline()
            if not raw.endswith(b"\n"):     # wait for a complete line
                time.sleep(0.5)
                continue
            text = raw.decode("utf-8", "replace").strip()
            if text:
                wait = MIN_GAP_SECONDS - (time.time() - last_sent)
                if AUTO_SEND and wait > 0:
                    time.sleep(min(wait, 1))
                    continue
                try:
                    api("TX.SEND_MESSAGE" if AUTO_SEND else "TX.SET_TEXT", text)
                    last_sent = time.time()
                    log(f"Sent: {text}")
                except OSError as e:
                    log(f"CGCall not reachable ({e}); retrying")
                    time.sleep(2)
                    continue
            pos += len(raw)
            continue
        time.sleep(0.5)


if __name__ == "__main__":
    main()
