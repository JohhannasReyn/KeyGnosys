"""Session recorder for the M3 manual matrix.

Records every core event with a timestamp, and uses F12 as a row separator so
the operator can run a batch of rows at their own pace without a round-trip per
row.

    record.py --out FILE [--timeout 7200]

  Press F12  -> closes the current segment and opens the next one
  Segment 0 is whatever happens before the first F12 (setup noise).

Written after a capture client produced five false negatives by going deaf
after its first line: this one records a heartbeat so liveness is checkable
while it runs, and its segment boundaries come from the event stream itself
rather than from anyone's wall clock.
"""
import json
import os
import sys
import threading
import time

PIPE = chr(92) * 2 + chr(46) + chr(92) + "pipe" + chr(92) + "keygnosys"
TRIGGER = "F12"


def main():
    out = None
    timeout = 7200.0
    a = sys.argv[1:]
    i = 0
    while i < len(a):
        if a[i] == "--out":
            out = a[i + 1]; i += 2
        elif a[i] == "--timeout":
            timeout = float(a[i + 1]); i += 2
        else:
            i += 1
    if not out:
        print(__doc__)
        return 2

    f = open(PIPE, "r+b", buffering=0)
    fh = open(out, "w", encoding="utf-8")
    state = {"segment": 0, "events": 0, "keys": 0, "t0": time.time()}
    lock = threading.Lock()

    def reader():
        buf = b""
        while True:
            try:
                chunk = f.read(4096)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    msg = json.loads(line.decode("utf-8", "replace"))
                except ValueError:
                    continue
                now = time.time()
                with lock:
                    d = msg.get("d") or {}
                    if (msg.get("n") == "key" and d.get("code") == TRIGGER
                            and d.get("state") == "up"):
                        state["segment"] += 1
                        rec = {"t": round(now - state["t0"], 3),
                               "marker": state["segment"]}
                    else:
                        state["events"] += 1
                        if msg.get("n") == "key":
                            state["keys"] += 1
                        rec = {"t": round(now - state["t0"], 3),
                               "seg": state["segment"],
                               "n": msg.get("n"), "d": d}
                    fh.write(json.dumps(rec) + "\n")
                    fh.flush()

    def heartbeat():
        while True:
            try:
                with open(out + ".hb", "w", encoding="utf-8") as h:
                    with lock:
                        h.write("segment=%d events=%d keys=%d elapsed=%ds\n"
                                % (state["segment"], state["events"],
                                   state["keys"], int(time.time() - state["t0"])))
            except OSError:
                pass
            time.sleep(2)

    threading.Thread(target=reader, daemon=True).start()
    threading.Thread(target=heartbeat, daemon=True).start()
    print("recording to %s; F12 advances the segment" % out, flush=True)
    time.sleep(timeout)
    fh.flush(); fh.close()
    os._exit(0)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except FileNotFoundError:
        print("CORE NOT RUNNING")
        sys.exit(4)
