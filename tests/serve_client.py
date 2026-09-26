"""Minimal client for `qfv serve` (newline-delimited JSON over stdio)."""
import json
import os
import subprocess
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


class Serve:
    def __init__(self, work):
        env = {**os.environ, "PATH": os.path.join(ROOT, "tools", "oss-cad-suite", "bin") + ":" + os.environ["PATH"]}
        self.p = subprocess.Popen([os.path.join(ROOT, "build", "qfv"), "serve", "--work", work],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
        self.n = 0

    def call(self, method, **params):
        self.n += 1
        t0 = time.time()
        self.p.stdin.write(json.dumps({"id": self.n, "method": method, "params": params}) + "\n")
        self.p.stdin.flush()
        events = []
        while True:
            msg = json.loads(self.p.stdout.readline())
            if "event" in msg:
                events.append(msg["event"])
                continue
            return msg["result"], events, (time.time() - t0) * 1000

    def close(self):
        self.p.stdin.close()
        self.p.wait()
