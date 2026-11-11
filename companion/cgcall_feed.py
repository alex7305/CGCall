import json, os, socket, sys, time

HOST, PORT = "127.0.0.1", 2442
BASE = os.path.dirname(sys.executable if getattr(sys, "frozen", False)
                       else os.path.abspath(__file__))
OUTBOX = os.path.join(BASE, "outbox.txt")


def set_tx_text(text):
    msg = json.dumps({"type": "TX.SET_TEXT", "value": text, "params": {}}) + "\n"
    with socket.create_connection((HOST, PORT), timeout=3) as s:
        s.sendall(msg.encode("utf-8"))


def main():
    open(OUTBOX, "a").close()
    pos = os.path.getsize(OUTBOX)
    print(f"Watching {OUTBOX}")
    while True:
        size = os.path.getsize(OUTBOX)
        if size < pos:          # file was cleared
            pos = 0
        if size > pos:
            with open(OUTBOX, "rb") as f:
                f.seek(pos)
                data = f.read()
            lines = [l.strip() for l in data.decode("utf-8", "replace").splitlines()]
            try:
                for line in lines:
                    if line:
                        set_tx_text(line)
                pos += len(data)
            except OSError as e:
                print(f"CGCall not reachable ({e}); retrying")
                time.sleep(2)
                continue
        time.sleep(0.5)


if __name__ == "__main__":
    main()
