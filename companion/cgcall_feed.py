import json, os, socket, sys, time

HOST, PORT = "127.0.0.1", 2442
AUTO_SEND = True          # False = only fill the box; you press Send yourself
MIN_GAP_SECONDS = 15      # minimum pause between auto-sent messages

BASE = os.path.dirname(sys.executable if getattr(sys, "frozen", False)
                       else os.path.abspath(__file__))
OUTBOX = os.path.join(BASE, "outbox.txt")
PAUSE_FILE = os.path.join(BASE, "pause.txt")   # if this file exists, nothing is sent


def api(msg_type, text):
    msg = json.dumps({"type": msg_type, "value": text, "params": {}}) + "\n"
    with socket.create_connection((HOST, PORT), timeout=3) as s:
        s.sendall(msg.encode("utf-8"))

def single_instance():
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", 47653))   # any unused local port
    except OSError:
        sys.exit(0)                    # a companion is already running
    return s
  
def main():
    lock = single_instance()           # keep this variable alive
    open(OUTBOX, "a").close()
    pos = os.path.getsize(OUTBOX)
    last_sent = 0.0
    print(f"Watching {OUTBOX} (auto-send {'ON' if AUTO_SEND else 'OFF'})")
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
                except OSError as e:
                    print(f"CGCall not reachable ({e}); retrying")
                    time.sleep(2)
                    continue
            pos += len(raw)
            continue
        time.sleep(0.5)


if __name__ == "__main__":
    main()
