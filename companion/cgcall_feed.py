import json, os, socket, sys, threading, time

HOST, PORT = "127.0.0.1", 2442
AUTO_SEND = True          # False = only fill the box; you press Send yourself
MIN_GAP_SECONDS = 15      # minimum pause between auto-sent messages
LOCK_PORT = 47653         # any unused local port; used for single-instance + takeover

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


def _listen_for_takeover(s):
    """Background thread: if a newer instance says QUIT, exit so it can take over."""
    while True:
        try:
            c, _ = s.accept()
            with c:
                c.settimeout(2)
                if c.recv(16).strip() == b"QUIT":
                    log("Newer instance asked me to quit; exiting.")
                    os._exit(0)
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
    open(OUTBOX, "a").close()
    pos = os.path.getsize(OUTBOX)
    last_sent = 0.0
    log(f"Watching {OUTBOX} (auto-send {'ON' if AUTO_SEND else 'OFF'})")
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
