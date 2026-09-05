"""Manual-test IPC harness for keygnosys-core. Scratch tool, not product code.

Envelope per docs/SPEC.md 5.2:  {"v":1,"t":"command","n":<name>,"id":<str>,"d":{}}
Replies:                        {"v":1,"t":"reply","id":<str>,"ok":<bool>,"d":{}}

  kgn.py send <command> [json-data]     send one command, print the reply
  kgn.py watch <seconds> [outfile]      stream every event line for N seconds
  kgn.py ping                           is the core up?

Reads happen on a daemon thread so a blocking pipe read can never outlive the
deadline; the process exits hard when the deadline passes.
"""
import json
import os
import sys
import threading
import time

PIPE = chr(92) * 2 + chr(46) + chr(92) + "pipe" + chr(92) + "keygnosys"


def connect():
    return open(PIPE, "r+b", buffering=0)


def reader(f, sink):
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
            if line.strip():
                sink(line.decode("utf-8", "replace"))


def cmd_send(name, data, timeout=5.0):
    ident = "m%d" % (int(time.time() * 1000) % 1000000)
    msg = {"v": 1, "t": "command", "n": name, "id": ident, "d": data or {}}
    f = connect()
    got = threading.Event()
    result = {"ok": False}

    def sink(line):
        try:
            m = json.loads(line)
        except ValueError:
            return
        if m.get("t") == "event" and m.get("n") == "hello":
            d = m.get("d", {})
            print("HELLO backends=%s capabilities=%s"
                  % (d.get("backends"), d.get("capabilities")))
        elif m.get("t") == "reply" and m.get("id") == ident:
            print("REPLY " + json.dumps(m))
            result["ok"] = bool(m.get("ok"))
            got.set()

    threading.Thread(target=reader, args=(f, sink), daemon=True).start()
    f.write((json.dumps(msg) + "\n").encode())
    if not got.wait(timeout):
        print("NO REPLY within %.1fs" % timeout)
        sys.stdout.flush()
        os._exit(5)
    sys.stdout.flush()
    os._exit(0 if result["ok"] else 1)


def cmd_watch(seconds, outfile):
    f = connect()
    out = open(outfile, "w", encoding="utf-8") if outfile else None
    lock = threading.Lock()

    def sink(line):
        with lock:
            print(line, flush=True)
            if out:
                out.write(line + "\n")
                out.flush()

    threading.Thread(target=reader, args=(f, sink), daemon=True).start()
    time.sleep(seconds)
    if out:
        out.flush()
        out.close()
    sys.stdout.flush()
    os._exit(0)


if __name__ == "__main__":
    a = sys.argv[1:]
    try:
        if not a or a[0] == "ping":
            cmd_send("ping", None)
        elif a[0] == "send":
            cmd_send(a[1], json.loads(a[2]) if len(a) > 2 else None)
        elif a[0] == "watch":
            cmd_watch(float(a[1]) if len(a) > 1 else 10.0,
                      a[2] if len(a) > 2 else None)
        else:
            print(__doc__)
            sys.exit(2)
    except FileNotFoundError:
        print("CORE NOT RUNNING (pipe not found)")
        sys.exit(4)
