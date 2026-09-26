"""Logged session driver for the case study: every call and its timing is
appended to agent_log.jsonl so the report can show the agent loop."""
import json, os, sys, time
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from serve_client import Serve
LOG = os.path.join(os.path.dirname(__file__), "agent_log.jsonl")

class Agent:
    def __init__(self, design, work):
        self.s = Serve(work)
        self.design = design
    def call(self, why, method, **params):
        r, ev, ms = self.s.call(method, **params)
        with open(LOG, "a") as f:
            f.write(json.dumps({"design": self.design, "why": why, "method": method, "params": params,
                                "ms": round(ms, 1), "result": r, "events": ev}) + "\n")
        return r, ev, ms
